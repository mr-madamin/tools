// Real sessions against serve_session, in-process. These cover the ground of
// the C port's hello_test, bad_frame_test, atomic_test and the path-safety
// half of delete_test -- without needing a server running on 8765.
#include "support.hpp"
#include "test.hpp"

#include <filesystem>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

using namespace support;
namespace fs = std::filesystem;

namespace {

void expect_error(const std::optional<json::Value>& reply, std::string_view contains,
                  std::string_view label)
{
    CHECK(reply.has_value(), "{}: server closed without sending an ERROR", label);
    const std::string* op = reply->str("op");
    CHECK(op != nullptr && *op == "ERROR", "{}: expected ERROR, got op={}", label,
          json::py_repr(reply->find("op")));
    const std::string* message = reply->str("message");
    CHECK(message != nullptr && message->find(contains) != std::string::npos,
          "{}: message was '{}', expected it to contain '{}'", label,
          message != nullptr ? *message : "", contains);
}

} // namespace

// ---- HELLO gates every op --------------------------------------------------

TEST(HELLO, rejects_the_unauthenticated)
{
    const std::vector<std::pair<std::string, std::string>> cases = {
        {R"({"op": "HELLO", "token": "definitely-not-it"})", "bad token"},
        {R"({"op": "HELLO"})", "bad token"},
        {R"({"op": "MANIFEST"})", "auth required"},
        {R"({"op": "DELETE", "path": "anything.txt"})", "auth required"},
    };

    for (const auto& [frame, expected] : cases) {
        TestServer server;
        Socket sock = server.connect();
        expect_error(server.exchange(sock, frame), expected, frame);
        CHECK(!recv_msg(sock).has_value(), "{}: server did not close after rejecting", frame);
    }
}

TEST(HELLO, token_is_compared_as_bytes)
{
    // Canonically equivalent to the real token (NFD instead of NFC), and a
    // different sequence of bytes. Swift's String == would accept this one.
    TestServer server;
    Socket sock = server.connect();
    auto reply = server.exchange(
        sock, R"({"op": "HELLO", "token": "café-token-🔑"})");
    expect_error(reply, "bad token", "canonically-equal token");
}

TEST(HELLO, correct_token_serves)
{
    TestServer server;
    json::Value files = server.manifest();
    CHECK(files.as_object() != nullptr && files.as_object()->empty(),
          "a fresh server served a non-empty manifest");
}

// ---- Malformed frames ------------------------------------------------------

TEST(Malformed_frames, are_refused_after_auth)
{
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"not json at all", "not valid UTF-8 JSON"},
        {"123", "not a JSON object"},
        {R"({"foo": 1})", "unknown op: None"},
        {R"({"op": "MANFEST"})", "unknown op: 'MANFEST'"},
        {R"({"op": "PUT", "path": "x"})", "missing field: 'size'"},
        {R"({"op": "PUT", "size": 1, "mtime": 0})", "missing field: 'path'"},
        {R"({"op": "PUT", "path": "x", "size": -1, "mtime": 0})", "out-of-range 'size'"},
        {R"({"op": "PUT", "path": "x", "size": true, "mtime": 0})", "out-of-range 'size'"},
        {R"({"op": "PUT", "path": "x", "size": "5", "mtime": 0})", "out-of-range 'size'"},
        {R"({"op": "PUT", "path": "x", "size": 1, "mtime": 1e20})", "out-of-range 'mtime'"},
        {R"({"op": "PUT", "path": "x", "size": 1e999, "mtime": 0})", "out-of-range 'size'"},
        {R"({"op": "DELETE"})", "DELETE missing 'path'"},
    };

    for (const auto& [frame, expected] : cases) {
        TestServer server;
        Socket sock = server.connect_authed();
        expect_error(server.exchange(sock, frame), expected, frame);
    }
}

TEST(Malformed_frames, invalid_utf8_is_named_as_such)
{
    TestServer server;
    Socket sock = server.connect();
    const char bytes[] = {0x7B, '\xFF', '\xFE', 0x7D};
    send_msg(sock, std::string_view(bytes, sizeof(bytes)));

    auto payload = recv_msg(sock);
    CHECK(payload.has_value(), "server closed without answering invalid UTF-8");
    expect_error(json::parse(*payload), "UTF-8", "invalid UTF-8");
}

TEST(Malformed_frames, an_oversized_length_prefix_is_refused_before_the_hello)
{
    // Four bytes claiming 4 GB, from a peer that has not authenticated. The
    // server must refuse on the header alone: allocating first is a four-byte
    // way to exhaust memory.
    TestServer server;
    Socket sock = server.connect();
    const unsigned char huge[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    sock.send_all(huge, sizeof(huge));

    auto payload = recv_msg(sock);
    CHECK(payload.has_value(), "server closed without an ERROR (or tried the allocation)");
    expect_error(json::parse(*payload), "frame too large", "oversized frame");
}

TEST(Malformed_frames, deep_nesting_is_refused_before_the_hello)
{
    // Every '[' costs a stack frame in a recursive-descent parser, and this is
    // parsed before any authentication: 200 KB of them killed the C build.
    TestServer server;
    Socket sock = server.connect();
    expect_error(server.exchange(sock, std::string(200000, '[')), "not valid UTF-8 JSON",
                 "200k open brackets");

    // The cap has to reject the abusive case without rejecting real data.
    std::string shallow = std::string(20, '[') + "1" + std::string(20, ']');
    CHECK(json::parse(shallow).has_value(), "20-deep nesting was rejected -- the cap is too tight");
}

TEST(Malformed_frames, the_server_survives_all_of_them)
{
    TestServer server;
    for (const auto* frame : {"garbage", "[]", R"({"op": 5})"}) {
        Socket sock = server.connect();
        (void)server.exchange(sock, frame);
    }
    json::Value files = server.manifest();
    CHECK(files.as_object() != nullptr, "server stopped serving after bad frames");
}

TEST(Malformed_frames, an_idle_peer_is_dropped)
{
    TestServer server(1); // one-second idle timeout
    Socket sock = server.connect();
    CHECK(!recv_msg(sock).has_value(), "an idle session was never dropped");
}

// ---- PUT -------------------------------------------------------------------

TEST(PUT, round_trip_keeps_content_and_mtime)
{
    TestServer server;
    TempDir src;
    const std::string rel = "nested/dir/hello.txt";
    write_file(src.path() + "/" + rel, "hello, receiver\n");
    set_mtime(src.path() + "/" + rel, 1700000000.123456);

    {
        Socket sock = server.connect_authed();
        send_file(sock, src.path(), rel);
        send_json(sock, R"({"op": "BYE"})"); // one session at a time: end this one
    }

    // Serving a second session proves the first one is over: the server takes
    // one peer at a time, so this is the barrier that makes the read below
    // race-free.
    json::Value files = server.manifest();

    CHECK(read_file(server.dir() + "/" + rel) == "hello, receiver\n",
          "the received file's content does not match");
    const json::Value* info = files.find(rel);
    CHECK(info != nullptr, "{} is missing from the peer's manifest", rel);
    auto mtime = info->number("mtime");
    CHECK(mtime.has_value() && std::abs(*mtime - 1700000000.123456) < 1e-6,
          "mtime did not survive the round trip: {}", mtime.value_or(-1));
}

TEST(PUT, an_aborted_transfer_leaves_the_old_file_intact)
{
    TestServer server;
    write_file(server.dir() + "/probe.txt", "THE ORIGINAL GOOD CONTENT\n");

    {
        Socket sock = server.connect_authed();
        send_json(sock, R"({"op": "PUT", "path": "probe.txt", "size": 5242880, "mtime": 1.0})");
        std::string partial(102400, 'X');
        sock.send_all(partial);
        sock.close(); // vanish mid-body
    }

    (void)server.manifest(); // the aborted session is over once this one is served

    CHECK(read_file(server.dir() + "/probe.txt") == "THE ORIGINAL GOOD CONTENT\n",
          "an aborted PUT destroyed the file it was replacing");

    std::size_t leftovers = 0;
    for (const auto& entry : fs::directory_iterator(server.dir()))
        if (entry.path().string().find(".tmp") != std::string::npos)
            leftovers++;
    CHECK(leftovers == 0, "{} .tmp file(s) left behind", leftovers);
}

TEST(PUT, a_path_that_escapes_the_shared_folder_is_refused)
{
    for (const auto* path : {"../escaped.txt", "/etc/escaped", "a/../../escaped.txt", "."}) {
        TestServer server;
        std::string outside = fs::path(server.dir()).parent_path() / "escaped.txt";

        Socket sock = server.connect_authed();
        auto reply = server.exchange(
            sock, std::format(R"({{"op": "PUT", "path": "{}", "size": 0, "mtime": 0}})", path));
        expect_error(reply, "unsafe path refused", path);
        CHECK(!exists(outside), "a PUT wrote outside the shared folder: {}", outside);
    }
}

TEST(PUT, a_symlink_pointing_out_of_the_tree_cannot_be_written_through)
{
    TestServer server;
    TempDir outside;
    CHECK(::symlink(outside.path().c_str(), (server.dir() + "/out").c_str()) == 0,
          "could not plant the symlink");

    Socket sock = server.connect_authed();
    auto reply =
        server.exchange(sock, R"({"op": "PUT", "path": "out/evil.txt", "size": 0, "mtime": 0})");
    expect_error(reply, "unsafe path refused", "symlink escape");
    CHECK(!exists(outside.path() + "/evil.txt"), "the PUT was written through the symlink");
}

// ---- DELETE ----------------------------------------------------------------

TEST(DELETE, removes_a_nested_file)
{
    TestServer server;
    write_file(server.dir() + "/sub/doomed.txt", "delete me\n");

    Socket sock = server.connect_authed();
    send_delete(sock, "sub/doomed.txt");
    auto reply = server.exchange(sock, R"({"op": "MANIFEST"})"); // prove the session lives
    CHECK(reply.has_value(), "session died after a DELETE");
    CHECK(!exists(server.dir() + "/sub/doomed.txt"), "the file is still there");
}

TEST(DELETE, a_missing_file_is_idempotent)
{
    TestServer server;
    Socket sock = server.connect_authed();
    send_delete(sock, "never_existed.txt"); // no reply on success...

    auto reply = server.exchange(sock, R"({"op": "MANIFEST"})"); // ...so prove the session lives
    CHECK(reply.has_value() && reply->str("op") != nullptr && *reply->str("op") == "MANIFEST",
          "deleting a missing file ended the session");
}

TEST(DELETE, an_escaping_path_is_refused_and_the_victim_survives)
{
    TestServer server;
    std::string victim = fs::path(server.dir()).parent_path() /
                         std::format("victim-{}.txt", getpid());
    write_file(victim, "must survive\n");

    for (const auto& path : {std::format("../{}", fs::path(victim).filename().string()),
                             std::string("/etc/hosts"), std::string(".")}) {
        Socket sock = server.connect_authed();
        auto reply = server.exchange(
            sock, std::format(R"({{"op": "DELETE", "path": {}}})", json::escape(path)));
        expect_error(reply, "unsafe path refused", path);
    }

    CHECK(exists(victim), "a DELETE removed a file outside the shared folder");
    fs::remove(victim);
}

// ---- Fuzzing the session loop ----------------------------------------------

TEST(Malformed_frames, the_server_survives_a_few_hundred_random_frames)
{
    // The C port's worst bugs (a 4 GB allocation from four bytes, a stack
    // overflow from 200 KB of '[') were found this way rather than by reading.
    // A fixed seed, so a failure is reproducible.
    TestServer server;
    std::mt19937 rng(20260927);
    std::uniform_int_distribution<int> length(0, 200);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> shape(0, 3);

    for (int i = 0; i < 200; i++) {
        Socket sock = server.connect();
        std::string junk;
        for (int n = length(rng); n > 0; n--)
            junk += static_cast<char>(byte(rng));

        try {
            switch (shape(rng)) {
            case 0: // a well-formed frame full of garbage
                send_msg(sock, junk);
                break;
            case 1: // a length prefix that lies: more promised than sent
                sock.send_all(std::string("\x00\x00\x10\x00", 4) + junk);
                sock.close(); // ...then vanish, rather than let the server wait
                continue;     // out the idle timeout for bytes we never send
            case 2: // raw bytes with no framing at all
                sock.send_all(junk);
                sock.close();
                continue;
            case 3: // a plausible-looking header with random values
                send_msg(sock, std::format(R"({{"op": "PUT", "path": {}, "size": {}}})",
                                           json::escape(junk), byte(rng) - 128));
                break;
            }
            (void)recv_msg(sock);
        } catch (const std::exception&) {
            // The peer end erroring is fine; the server staying up is the point.
        }
    }

    json::Value files = server.manifest();
    CHECK(files.as_object() != nullptr, "the server stopped serving after 200 random frames");
}
