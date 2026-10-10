// Scratch demo, kept for parity with file_sender.py: push one file, then quit.
// The real client is sync_push.
#include "framing.hpp"
#include "socket.hpp"
#include "util.hpp"

#include <print>

int main(int argc, char** argv)
{
    using namespace lfs;

    std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    std::string path = argc > 2 ? argv[2] : "bigfile.bin";
    constexpr int port = 8765;

    try {
        Socket sock = tcp_connect(host, port, 8);
        if (send_file(sock, ".", path) == SendResult::Changed)
            std::print("  ! {} changed while being sent; the copy is padded or clipped\n", path);
        std::print("Sent {}\n", path);
    } catch (const std::exception& e) {
        die(e.what());
    }
    return 0;
}
