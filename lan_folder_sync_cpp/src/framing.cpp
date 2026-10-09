#include "framing.hpp"

#include "paths.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace lfs {
namespace {

constexpr std::size_t CHUNK = 65536;

} // namespace

void send_msg(Socket& sock, std::string_view payload)
{
    std::string frame;
    frame.reserve(payload.size() + 4);

    auto len = payload.size();
    frame += static_cast<char>((len >> 24) & 0xFF);
    frame += static_cast<char>((len >> 16) & 0xFF);
    frame += static_cast<char>((len >> 8) & 0xFF);
    frame += static_cast<char>(len & 0xFF);
    frame += payload;

    // One buffer, one write -- Python's sendall(header + payload) does the
    // same, and it keeps the length prefix from ever landing without its body.
    sock.send_all(frame);
}

void send_json(Socket& sock, std::string_view json)
{
    send_msg(sock, json);
}

void send_error(Socket& sock, std::string_view message)
{
    send_msg(sock, std::format(R"({{"op": "ERROR", "message": {}}})", json::escape(message)));
}

std::optional<std::string> recv_msg(Socket& sock, std::size_t max)
{
    std::array<unsigned char, 4> header{};
    if (!sock.recv_exactly(header.data(), header.size()))
        return std::nullopt;

    std::size_t len = (static_cast<std::size_t>(header[0]) << 24) |
                      (static_cast<std::size_t>(header[1]) << 16) |
                      (static_cast<std::size_t>(header[2]) << 8) |
                      static_cast<std::size_t>(header[3]);

    // Refuse before allocating: without this an unauthenticated peer could
    // exhaust memory with four bytes.
    if (len > max)
        throw FrameTooLarge(len);

    std::string payload(len, '\0');
    if (len > 0 && !sock.recv_exactly(payload.data(), len))
        return std::nullopt;
    return payload;
}

std::expected<json::Value, HeaderError> decode_header(std::string_view payload)
{
    if (!valid_utf8(payload))
        return std::unexpected(HeaderError::NotUtf8Json);

    auto parsed = json::parse(payload);
    if (!parsed)
        return std::unexpected(HeaderError::NotUtf8Json);
    if (!parsed->is_object())
        return std::unexpected(HeaderError::NotObject);
    return std::move(*parsed);
}

SendResult send_file(Socket& sock, const std::string& root_dir, const std::string& rel_path)
{
    // Open first, then fstat the descriptor we actually hold: stat-then-open
    // would let the path be replaced in between, and we'd declare one file's
    // size while sending another's bytes.
    Fd src(::open(path_join(root_dir, rel_path).c_str(), O_RDONLY));
    if (!src.valid())
        throw LocalFileError(std::format("can't open {}: {}", rel_path, std::strerror(errno)));

    struct stat st{};
    if (fstat(src.get(), &st) != 0)
        throw LocalFileError(std::format("can't stat {}: {}", rel_path, std::strerror(errno)));

    long long size = static_cast<long long>(st.st_size);
    send_msg(sock, std::format(R"({{"op": "PUT", "path": {}, "size": {}, "mtime": {:.6f}}})",
                               json::escape(rel_path), size, stat_mtime(st)));

    std::vector<char> buf(CHUNK);
    long long sent = 0;
    bool truncated = false; // the file shrank; we padded the rest

    while (sent < size) {
        auto want = static_cast<std::size_t>(std::min<long long>(size - sent,
                                                                 static_cast<long long>(buf.size())));
        ssize_t n = ::read(src.get(), buf.data(), want);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw SocketError(std::format("read {}: {}", rel_path, std::strerror(errno)));
        }
        if (n == 0) {
            // Short file: it was truncated mid-transfer. We already promised
            // `size` bytes, so pad -- aborting here would leave the receiver
            // waiting for bytes that never come, and kill the whole session
            // over one file. The padded copy loses the mtime race on the next
            // push and gets resent, so this self-heals.
            std::fill_n(buf.begin(), want, '\0');
            n = static_cast<ssize_t>(want);
            truncated = true;
        }
        sock.send_all(buf.data(), static_cast<std::size_t>(n));
        sent += n;
    }

    // If the file GREW we simply stop at `size` and never read the tail -- the
    // frame is still exactly as long as advertised.
    bool grew = false;
    struct stat after{};
    if (fstat(src.get(), &after) == 0 && static_cast<long long>(after.st_size) != size)
        grew = true;

    return (truncated || grew) ? SendResult::Changed : SendResult::Ok;
}

void send_delete(Socket& sock, std::string_view rel_path)
{
    send_msg(sock, std::format(R"({{"op": "DELETE", "path": {}}})", json::escape(rel_path)));
}

namespace {

// The bytes land beside the target and are renamed into place. Writing the
// destination directly with O_TRUNC destroys the existing copy the instant the
// transfer starts, so a peer that dies mid-file leaves a truncated file where a
// good one used to be. rename() is atomic within a filesystem, and the temp
// sits in the same directory precisely to guarantee that.
//
// This is where C++ pays for itself: the C version repeated close-unlink-free
// at six early returns, and every new failure path had to remember all three.
// Here the destructor is the cleanup, and commit() is the one way out that
// keeps the file.
class TempFile {
public:
    TempFile(std::string target, Fd fd) : target_(std::move(target)), fd_(std::move(fd)) {}

    ~TempFile()
    {
        if (!committed_) {
            fd_.close();
            ::unlink(target_.c_str());
        }
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    int fd() const { return fd_.get(); }
    const std::string& path() const { return target_; }

    void commit(const std::string& final_path)
    {
        fd_.close();
        if (::rename(target_.c_str(), final_path.c_str()) != 0)
            throw SocketError(std::format("rename onto {}: {}", final_path, std::strerror(errno)));
        committed_ = true;
    }

private:
    std::string target_;
    Fd fd_;
    bool committed_ = false;
};

} // namespace

std::string recv_file_body(Socket& sock, const std::string& dest_dir,
                           const json::Value& header)
{
    const std::string* rel_path = header.str("path");
    if (rel_path == nullptr)
        throw PutError(PutError::Kind::MissingField, "path", "PUT header missing field: 'path'");

    // Absent and present-but-unusable are different bugs, so they get different
    // messages: a peer that forgot the field, versus one that sent `true`, "5"
    // or 1e999. std::variant makes the second case fall out for free -- a bool
    // simply isn't the double alternative.
    for (const char* field : {"size", "mtime"}) {
        if (header.find(field) == nullptr)
            throw PutError(PutError::Kind::MissingField, field,
                           std::format("PUT header missing field: '{}'", field));
    }

    auto size_d = header.number("size");
    auto mtime = header.number("mtime");

    // isfinite() rejects both inf and NaN, which are what a JSON "1e999" and a
    // bare "nan" decode to. Do this BEFORE the cast: once the cast happens the
    // undefined behaviour has already happened.
    if (!size_d || !std::isfinite(*size_d) || *size_d < 0 ||
        *size_d > static_cast<double>(MAX_FILE_SIZE))
        throw PutError(PutError::Kind::OutOfRange, "size", "PUT header has an out-of-range 'size'");
    if (!mtime || !std::isfinite(*mtime) || *mtime < -MAX_MTIME || *mtime > MAX_MTIME)
        throw PutError(PutError::Kind::OutOfRange, "mtime", "PUT header has an out-of-range 'mtime'");

    auto size = static_cast<long long>(*size_d);

    // Check the path before creating anything: mkdir_p on "../evil/x" would
    // make directories outside dest_dir before realpath ever got a look in.
    auto unsafe = [&] {
        return PutError(PutError::Kind::UnsafePath, *rel_path,
                        std::format("unsafe path refused: '{}'", *rel_path));
    };
    if (!path_is_lexically_safe(*rel_path))
        throw unsafe();

    std::string full_path = path_join(dest_dir, *rel_path);
    if (!mkdir_p(path_dirname(full_path)))
        throw SocketError(std::format("mkdir {}: {}", path_dirname(full_path), std::strerror(errno)));

    // The parent exists now, so the traversal guard can resolve it. Unlike the
    // Python server as it was (which guarded DELETE only), we refuse escaping
    // PUTs too.
    auto safe = safe_path(dest_dir, *rel_path);
    if (!safe)
        throw unsafe();

    std::string tmp_path = std::format("{}.{}.tmp", *safe, getpid());
    Fd tmp_fd(::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666));
    if (!tmp_fd.valid())
        throw SocketError(std::format("open {}: {}", tmp_path, std::strerror(errno)));
    TempFile tmp(tmp_path, std::move(tmp_fd));

    std::vector<char> buf(CHUNK);
    long long received = 0;
    while (received < size) {
        auto want = static_cast<std::size_t>(std::min<long long>(size - received,
                                                                 static_cast<long long>(buf.size())));
        std::size_t n = sock.recv_some(buf.data(), want); // throws SocketTimeout if stalled
        if (n == 0)
            throw SocketError("peer closed mid-file - truncated transfer");
        if (::write(tmp.fd(), buf.data(), n) != static_cast<ssize_t>(n))
            throw SocketError(std::format("write {}: {}", tmp.path(), std::strerror(errno)));
        received += static_cast<long long>(n);
    }

    // Stamp the mtime before the rename, so the file is never briefly visible
    // at its final path with the wrong timestamp (which the next diff would
    // read as "changed" and resend).
    struct timeval times[2];
    times[0].tv_sec = times[1].tv_sec = static_cast<time_t>(*mtime);
    times[0].tv_usec = times[1].tv_usec =
        static_cast<suseconds_t>((*mtime - std::floor(*mtime)) * 1e6);
    utimes(tmp.path().c_str(), times);

    tmp.commit(*safe);
    return *rel_path;
}

std::optional<json::Value> handshake(Socket& sock, std::string_view token)
{
    send_msg(sock, std::format(R"({{"op": "HELLO", "token": {}}})", json::escape(token)));

    auto payload = recv_msg(sock, MAX_CONTROL_FRAME); // OK or ERROR
    if (!payload)
        return std::nullopt;
    return json::parse(*payload);
}

} // namespace lfs
