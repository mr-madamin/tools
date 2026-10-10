// Scratch demo, kept for parity with file_receiver.py: accept one file, then
// quit. The real server is sync_server.
#include "framing.hpp"
#include "json.hpp"
#include "socket.hpp"
#include "util.hpp"

#include <print>

int main()
{
    using namespace lfs;

    constexpr int port = 8765;
    const std::string dest = "received";
    mkdir_p(dest);

    try {
        Listener listener("0.0.0.0", port);
        std::print("Listening on {} ... waiting for a file\n", port);

        Accepted peer = listener.accept();
        std::print("Connected from {}:{}\n", peer.peer_ip, peer.peer_port);

        auto payload = recv_msg(peer.socket, MAX_CONTROL_FRAME);
        if (!payload)
            die("receive failed: peer closed before sending a header");
        auto header = decode_header(*payload);
        if (!header)
            die("receive failed: malformed header");

        std::print("Received -> {}\n", recv_file_body(peer.socket, dest, *header));
    } catch (const std::exception& e) {
        die(e.what());
    }
    return 0;
}
