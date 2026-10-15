// The parts that need no peer: the declared-size contract, the frame cap, and
// the lexical half of the path guard. The C port tests these with standalone
// runners (truncate_test, nasty_test); a socketpair does the same job here.
#include "paths.hpp"
#include "support.hpp"
#include "test.hpp"

#include <chrono>
#include <fstream>
#include <thread>
#include <unistd.h>

using namespace support;

namespace {

constexpr long long BIG = 5 * 1024 * 1024;

// Read the PUT header frame and return the size it declared.
long long read_put_header(Socket& sock, std::string& path_out)
{
    auto payload = recv_msg(sock);
    CHECK(payload.has_value(), "no PUT header arrived");
    auto header = json::parse(*payload);
    CHECK(header.has_value(), "PUT header was not JSON: {}", *payload);
    const std::string* path = header->str("path");
    CHECK(path != nullptr, "PUT header had no path");
    path_out = *path;
    auto size = header->number("size");
    CHECK(size.has_value(), "PUT header had no size");
    return static_cast<long long>(*size);
}

} // namespace

TEST(send_file, pads_a_file_that_shrinks_mid_transfer)
{
    // The body is the only unframed part of the stream, so the receiver finds
    // the next frame by counting exactly the bytes the header declared. A file
    // truncated halfway through must not shorten what we send: one byte off
    // and every later file in the session is silently corrupt.
    TempDir dir;
    const std::string rel = "big.bin";
    write_file(dir.path() + "/" + rel, std::string(BIG, 'A'));

    auto [sender_side, reader_side] = socket_pair();

    SendResult result = SendResult::Ok;
    std::jthread sender([&] {
        result = send_file(sender_side, dir.path(), rel);
        send_delete(sender_side, "after.txt"); // the next frame must still line up
    });

    // The socket buffer fills long before 5 MB, so the sender is blocked in
    // send() by the time we truncate.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(::truncate((dir.path() + "/" + rel).c_str(), 0) == 0, "truncate failed");

    std::string path;
    long long declared = read_put_header(reader_side, path);
    CHECK(declared == BIG, "declared {} bytes, expected {}", declared, BIG);

    std::string body(static_cast<std::size_t>(declared), '\0');
    CHECK(reader_side.recv_exactly(body.data(), body.size()),
          "the sender delivered fewer bytes than it declared");

    sender.join();
    CHECK(result == SendResult::Changed, "a file that shrank mid-send was not reported");

    // The pass condition: the stream still has a frame boundary where the
    // receiver expects one.
    auto next = recv_msg(reader_side);
    CHECK(next.has_value(), "the frame after the body never arrived -- the stream desynced");
    auto header = json::parse(*next);
    CHECK(header.has_value() && header->str("op") != nullptr && *header->str("op") == "DELETE",
          "the frame after the body was not the next frame: {}", *next);
}

TEST(send_file, clips_a_file_that_grows_mid_transfer)
{
    TempDir dir;
    const std::string rel = "growing.bin";
    write_file(dir.path() + "/" + rel, std::string(BIG, 'A'));

    auto [sender_side, reader_side] = socket_pair();

    SendResult result = SendResult::Ok;
    std::jthread sender([&] { result = send_file(sender_side, dir.path(), rel); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::ofstream more(dir.path() + "/" + rel, std::ios::binary | std::ios::app);
        more << std::string(1024 * 1024, 'B');
    }

    std::string path;
    long long declared = read_put_header(reader_side, path);
    std::string body(static_cast<std::size_t>(declared), '\0');
    CHECK(reader_side.recv_exactly(body.data(), body.size()), "short body");
    sender.join();

    CHECK(declared == BIG, "the header declared a size that changed under it");
    CHECK(result == SendResult::Changed, "a file that grew mid-send was not reported");
}

TEST(recv_msg, refuses_a_frame_larger_than_the_cap)
{
    auto [writer, reader] = socket_pair();

    const unsigned char claims_2mb[4] = {0x00, 0x20, 0x00, 0x00};
    writer.send_all(claims_2mb, sizeof(claims_2mb)); // and not one byte more

    bool refused = false;
    try {
        (void)recv_msg(reader, MAX_CONTROL_FRAME);
    } catch (const FrameTooLarge& e) {
        refused = true;
        CHECK(e.length == 2 * 1024 * 1024, "reported {} bytes", e.length);
    }
    CHECK(refused, "a 2 MB frame was accepted against a 1 MB cap");
}

TEST(recv_msg, reports_a_closed_peer_as_end_of_stream)
{
    auto [writer, reader] = socket_pair();
    writer.close();
    CHECK(!recv_msg(reader).has_value(), "a closed peer was not reported as EOF");
}

TEST(path_guard, refuses_traversal_lexically)
{
    const std::vector<std::pair<std::string, bool>> cases = {
        {"a.txt", true},      {"sub/dir/a.txt", true}, {"a/./b.txt", true},
        {"..", false},        {"../a.txt", false},     {"a/../../b.txt", false},
        {"/etc/passwd", false}, {"", false},           {"a/..", false},
        {"...", true}, // three dots is a legal filename, not a traversal
    };

    for (const auto& [path, expected] : cases)
        CHECK(path_is_lexically_safe(path) == expected, "path_is_lexically_safe('{}') should be {}",
              path, expected);
}

TEST(path_guard, refuses_the_shared_root_itself)
{
    TempDir dir;
    CHECK(!safe_path(dir.path(), ".").has_value(), "'.' resolved to the root and was allowed");
    CHECK(safe_path(dir.path(), "a/b.txt").has_value(), "a normal nested path was refused");
}

TEST(send_file, reports_a_file_that_vanished_without_touching_the_stream)
{
    // A file can disappear between the walk and the send. Nothing has been
    // written to the socket at that point, so the pusher skips it and carries
    // on -- and the proof is that the next frame still arrives intact.
    TempDir dir;
    auto [sender_side, reader_side] = socket_pair();

    bool reported = false;
    try {
        (void)send_file(sender_side, dir.path(), "never_existed.txt");
    } catch (const LocalFileError& e) {
        reported = true;
        CHECK(std::string(e.what()).find("never_existed.txt") != std::string::npos,
              "the error did not name the file: {}", e.what());
    }
    CHECK(reported, "a missing file was not reported as a local error");

    send_delete(sender_side, "next.txt");
    auto next = recv_msg(reader_side);
    CHECK(next.has_value(), "nothing arrived after the skipped file");
    auto header = json::parse(*next);
    CHECK(header.has_value() && header->str("op") != nullptr && *header->str("op") == "DELETE",
          "the skipped file put bytes on the wire: {}", *next);
}
