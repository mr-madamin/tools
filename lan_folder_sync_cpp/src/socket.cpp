#include "socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace lfs {
namespace {

// SO_RCVTIMEO/SO_SNDTIMEO surface as EAGAIN (== EWOULDBLOCK on macOS). That's
// the timeout, not a real error -- the two get different advice upstream.
bool is_timeout(int e)
{
    return e == EAGAIN || e == EWOULDBLOCK;
}

} // namespace

void Fd::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void Socket::set_timeout(int seconds)
{
    struct timeval tv{};
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd_.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd_.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void Socket::send_all(const void* data, std::size_t n)
{
    const auto* p = static_cast<const unsigned char*>(data);
    std::size_t sent = 0;

    while (sent < n) {
        ssize_t w = ::send(fd_.get(), p + sent, n - sent, 0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (is_timeout(errno))
                throw SocketTimeout("send timed out");
            throw SocketError(std::format("send: {}", std::strerror(errno)));
        }
        sent += static_cast<std::size_t>(w);
    }
}

bool Socket::recv_exactly(void* buf, std::size_t n)
{
    auto* out = static_cast<unsigned char*>(buf);
    std::size_t got = 0;

    while (got < n) {
        std::size_t r = recv_some(out + got, n - got);
        if (r == 0)
            return false;
        got += r;
    }
    return true;
}

std::size_t Socket::recv_some(void* buf, std::size_t n)
{
    for (;;) {
        ssize_t r = ::recv(fd_.get(), buf, n, 0);
        if (r >= 0)
            return static_cast<std::size_t>(r);
        if (errno == EINTR)
            continue;
        if (is_timeout(errno))
            throw SocketTimeout("recv timed out");
        throw SocketError(std::format("recv: {}", std::strerror(errno)));
    }
}

namespace {

// Go non-blocking, poll for the deadline, then hand back a blocking socket so
// the rest of the code stays straight-line.
Fd connect_one(const struct addrinfo& ai, int seconds, int& failure_errno)
{
    Fd fd(::socket(ai.ai_family, ai.ai_socktype, ai.ai_protocol));
    if (!fd.valid()) {
        failure_errno = errno;
        return {};
    }

    int flags = fcntl(fd.get(), F_GETFL, 0);
    if (flags < 0 || fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK) < 0) {
        failure_errno = errno;
        return {};
    }

    if (::connect(fd.get(), ai.ai_addr, ai.ai_addrlen) != 0) {
        if (errno != EINPROGRESS) {
            failure_errno = errno;
            return {};
        }

        struct pollfd pfd{};
        pfd.fd = fd.get();
        pfd.events = POLLOUT;
        int n;
        do {
            n = poll(&pfd, 1, seconds * 1000);
        } while (n < 0 && errno == EINTR);

        if (n == 0) { // the SYN went out and nothing came back at all
            failure_errno = ETIMEDOUT;
            return {};
        }
        if (n < 0) {
            failure_errno = errno;
            return {};
        }

        // poll() says "done", not "succeeded" -- a refusal wakes it too.
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            failure_errno = err != 0 ? err : EIO;
            return {};
        }
    }

    fcntl(fd.get(), F_SETFL, flags); // blocking again; SO_RCVTIMEO bounds it now
    return fd;
}

} // namespace

Socket tcp_connect(const std::string& host, int port, int timeout_seconds)
{
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    int err = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (err != 0)
        throw ConnectFailure(ConnectFailure::Kind::Resolve, gai_strerror(err));

    int failure_errno = 0;
    Fd fd;
    for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = connect_one(*ai, timeout_seconds, failure_errno);
        if (fd.valid())
            break;
    }
    freeaddrinfo(res);

    if (!fd.valid()) {
        auto kind = failure_errno == ETIMEDOUT   ? ConnectFailure::Kind::Timeout
                    : failure_errno == ECONNREFUSED ? ConnectFailure::Kind::Refused
                                                    : ConnectFailure::Kind::Unreachable;
        throw ConnectFailure(kind, std::strerror(failure_errno));
    }
    return Socket(std::move(fd));
}

Listener::Listener(const std::string& host, int port)
{
    Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
    if (!fd.valid())
        throw SocketError(std::format("socket: {}", std::strerror(errno)));

    int one = 1;
    setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        throw SocketError(std::format("bad bind address: {}", host));

    if (::bind(fd.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0)
        throw SocketError(std::format("bind {}:{}: {}", host, port, std::strerror(errno)));
    if (::listen(fd.get(), 1) != 0)
        throw SocketError(std::format("listen: {}", std::strerror(errno)));

    socklen_t len = sizeof(addr);
    if (getsockname(fd.get(), reinterpret_cast<struct sockaddr*>(&addr), &len) == 0)
        port_ = ntohs(addr.sin_port);
    else
        port_ = port;

    fd_ = std::move(fd);
}

Accepted Listener::accept()
{
    for (;;) {
        struct sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        Fd conn(::accept(fd_.get(), reinterpret_cast<struct sockaddr*>(&peer), &len));
        if (!conn.valid()) {
            if (errno == EINTR)
                continue;
            throw SocketError(std::format("accept: {}", std::strerror(errno)));
        }

        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        return Accepted{Socket(std::move(conn)), std::string(ip), ntohs(peer.sin_port)};
    }
}

std::optional<std::string> lan_ip()
{
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0)
        return std::nullopt;

    std::optional<std::string> found;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (std::strcmp(ifa->ifa_name, "en0") != 0)
            continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
            continue;

        char buf[INET_ADDRSTRLEN];
        auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) != nullptr) {
            found = std::string(buf);
            break;
        }
    }
    freeifaddrs(ifaddr);
    return found;
}

} // namespace lfs
