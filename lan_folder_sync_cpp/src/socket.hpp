// Blocking BSD sockets behind RAII. The protocol is strictly sequential ("read
// 4 bytes, then N, then exactly `size`"), so there is nothing for async to do
// here -- and keeping it blocking keeps this file readable beside the C port's
// framing.c.
//
// What C++ adds is that a descriptor now closes itself. The C receive path had
// six early returns, each repeating close/unlink/free by hand; one missed line
// there is a leaked fd or a stray .tmp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lfs {

// The socket timeout expired with the peer neither sending nor closing. Worth
// its own type: "the peer is wedged" and "the peer hung up" need opposite
// advice, and the pusher prints a different page for each.
struct SocketTimeout : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct SocketError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A file descriptor that closes itself. Move-only, so ownership is never in
// doubt -- and never double-closed.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { close(); }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Fd& operator=(Fd&& other) noexcept
    {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    int release()
    {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void close();

private:
    int fd_ = -1;
};

class Socket {
public:
    explicit Socket(Fd fd) : fd_(std::move(fd)) {}

    int fd() const { return fd_.get(); }

    // Bound every later blocking recv/send on this socket. Python spells the
    // whole of this sock.settimeout(); 0 seconds clears it.
    void set_timeout(int seconds);
    void close() { fd_.close(); }

    void send_all(const void* data, std::size_t n);
    void send_all(std::string_view data) { send_all(data.data(), data.size()); }

    // false when the peer closed first. Partial data is abandoned: once a frame
    // arrives half-read the stream has no boundary left to resync on, so every
    // caller ends the session.
    bool recv_exactly(void* buf, std::size_t n);

    // 0 when the peer closed. Throws on timeout or I/O error.
    std::size_t recv_some(void* buf, std::size_t n);

private:
    Fd fd_;
};

// Why a connect failed, as a value the pusher can switch on: each kind gets a
// different page of advice, and "nothing came back at all" versus "it answered,
// refusing" is the single most useful distinction in this program.
struct ConnectFailure : std::runtime_error {
    enum class Kind { Timeout, Refused, Unreachable, Resolve };
    ConnectFailure(Kind kind, const std::string& what) : std::runtime_error(what), kind(kind) {}
    Kind kind;
};

// connect() has no timeout of its own, and the macOS default is ~75s of silence
// against a dropped SYN.
Socket tcp_connect(const std::string& host, int port, int timeout_seconds);

struct Accepted {
    Socket socket;
    std::string peer_ip;
    int peer_port;
};

class Listener {
public:
    // port 0 asks the kernel for an ephemeral one; port() reports what it gave,
    // which is what lets the tests run real sessions without needing 8765.
    Listener(const std::string& host, int port);

    int port() const { return port_; }
    int fd() const { return fd_.get(); } // for poll(): the test server needs a
                                          // loop it can leave without a peer

    Accepted accept();

private:
    Fd fd_;
    int port_ = 0;
};

// This Mac's en0 IPv4 (Wi-Fi, usually), or nullopt if offline / not on en0.
// Python shells out to `ipconfig getifaddr en0`; getifaddrs() is the same
// answer without a subprocess.
std::optional<std::string> lan_ip();

} // namespace lfs
