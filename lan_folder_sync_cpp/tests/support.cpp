#include "support.hpp"

#include "server.hpp"
#include "test.hpp"

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace support {
namespace fs = std::filesystem;

TempDir::TempDir()
{
    static std::atomic<int> counter{0};
    std::random_device rd;
    path_ = (fs::temp_directory_path() /
             std::format("lan_folder_sync_cpp_{}_{}_{}", getpid(), counter++, rd()))
                .string();
    fs::create_directories(path_);
}

TempDir::~TempDir()
{
    std::error_code ec;
    fs::remove_all(path_, ec);
}

void write_file(const std::string& path, std::string_view text)
{
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::optional<std::string> read_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool exists(const std::string& path)
{
    std::error_code ec;
    return fs::exists(fs::symlink_status(path, ec));
}

void set_mtime(const std::string& path, double mtime)
{
    struct timeval times[2];
    times[0].tv_sec = times[1].tv_sec = static_cast<time_t>(mtime);
    times[0].tv_usec = times[1].tv_usec =
        static_cast<suseconds_t>((mtime - static_cast<double>(static_cast<time_t>(mtime))) * 1e6);
    utimes(path.c_str(), times);
}

std::pair<Socket, Socket> socket_pair()
{
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
        throw std::runtime_error("socketpair failed");
    return {Socket(Fd(fds[0])), Socket(Fd(fds[1]))};
}

TestServer::TestServer(int idle_timeout)
{
    listener_ = std::make_unique<Listener>("127.0.0.1", 0);

    thread_ = std::jthread([this, idle_timeout] {
        while (!stop_.load()) {
            // poll rather than block in accept(), so the destructor can end
            // this loop without a peer ever arriving.
            struct pollfd pfd{};
            pfd.fd = listener_->fd();
            pfd.events = POLLIN;
            int n = ::poll(&pfd, 1, 50);
            if (n <= 0)
                continue;
            try {
                Accepted peer = listener_->accept();
                serve_session(peer.socket, dir_.path(), token_, idle_timeout,
                              [](const std::string&) {});
            } catch (const std::exception&) {
                return;
            }
        }
    });
}

TestServer::~TestServer()
{
    stop_.store(true);
    // jthread joins in its own destructor; the loop notices the flag within a
    // poll interval.
}

Socket TestServer::connect()
{
    Socket sock = tcp_connect("127.0.0.1", port(), 5);
    sock.set_timeout(10); // a wedged server should fail the test, not hang it
    return sock;
}

Socket TestServer::connect_authed()
{
    Socket sock = connect();
    auto reply = handshake(sock, token_);
    if (!reply || reply->str("op") == nullptr || *reply->str("op") != "OK")
        throw std::runtime_error("handshake failed");
    return sock;
}

std::optional<json::Value> TestServer::exchange(Socket& sock, std::string_view payload)
{
    send_msg(sock, payload);
    auto reply = recv_msg(sock);
    if (!reply)
        return std::nullopt;
    return json::parse(*reply);
}

json::Value TestServer::manifest()
{
    Socket sock = connect_authed();
    auto reply = exchange(sock, R"({"op": "MANIFEST"})");
    if (!reply)
        throw std::runtime_error("server closed without sending a manifest");
    const json::Value* files = reply->find("files");
    if (files == nullptr)
        throw std::runtime_error("manifest reply had no \"files\"");
    return *files;
}

} // namespace support
