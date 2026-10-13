// A real server session loop on an ephemeral loopback port, serving a temp
// directory -- the same serve_session sync_server runs, just in-process.
#pragma once

#include "framing.hpp"
#include "json.hpp"
#include "socket.hpp"

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace support {

using namespace lfs;

// std::filesystem is exactly right here and exactly wrong in src/paths.cpp:
// a test wants convenience over a path it made up itself, while the guards
// want byte semantics over a path a peer sent.
class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

void write_file(const std::string& path, std::string_view text);
std::optional<std::string> read_file(const std::string& path);
bool exists(const std::string& path);
void set_mtime(const std::string& path, double mtime);

std::pair<Socket, Socket> socket_pair();

class TestServer {
public:
    explicit TestServer(int idle_timeout = 10);
    ~TestServer();

    const std::string& dir() const { return dir_.path(); }
    const std::string& token() const { return token_; }
    int port() const { return listener_->port(); }

    Socket connect();
    Socket connect_authed();

    // Send one raw frame and return the reply, or nullopt if the server closed.
    std::optional<json::Value> exchange(Socket& sock, std::string_view payload);

    // What the server currently holds, over a fresh session.
    json::Value manifest();

private:
    TempDir dir_;
    // A token with non-ASCII in it, so "compared as bytes" is actually tested.
    // Precomposed "é" and an emoji: a Unicode-aware comparison would call the
    // decomposed spelling of this equal to it. The filesystem, and every other
    // implementation of this protocol, compare bytes.
    std::string token_ = "caf\xc3\xa9-token-\xf0\x9f\x94\x91";
    std::unique_ptr<Listener> listener_;
    std::atomic<bool> stop_{false};
    std::jthread thread_;
};

} // namespace support
