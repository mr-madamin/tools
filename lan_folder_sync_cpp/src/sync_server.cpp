// The passive side: bind, accept, authenticate, then do exactly what the peer
// asks -- serve a manifest, accept a PUT, remove a DELETE. It never decides
// anything about the sync; the pusher is the brain.
#include "config.hpp"
#include "server.hpp"
#include "socket.hpp"
#include "util.hpp"

#include <csignal>
#include <cstdio>
#include <format>
#include <iostream>
#include <print>

namespace {

constexpr std::string_view USAGE = "usage: sync_server [shared_dir] [port] [--lan]";

constexpr std::string_view HELP =
    "  --lan    bind this Mac's en0 IPv4 instead of 127.0.0.1, so\n"
    "           peers on the LAN can reach it. Exits if en0 has no\n"
    "           address rather than quietly serving loopback.\n"
    "  --help   this message\n\n"
    "shared_dir defaults to config.json shared_dir, port to\n"
    "peer.port. Without --lan the server binds 127.0.0.1.";

} // namespace

int main(int argc, char** argv)
{
    using namespace lfs;

    // Line-buffered: with stdout redirected to a log or a service manager,
    // block buffering keeps the file completely empty -- startup banner
    // included -- until the process exits.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::signal(SIGPIPE, SIG_IGN); // a vanished peer is an error code, not a death

    Args args = parse_args(argc, argv, {"--lan", "--help"});
    if (args.has("--help")) {
        std::print("{}\n\n{}\n", USAGE, HELP);
        return 0;
    }
    if (!args.unknown.empty())
        die_unknown_flags(args.unknown, USAGE);

    Config cfg = load_config(argv[0]);

    std::string shared_dir = args.at(0)              ? *args.at(0)
                             : cfg.shared_dir        ? *cfg.shared_dir
                                                     : "sandbox/received";
    int port = args.at(1)        ? parse_port(*args.at(1), "port argument")
               : cfg.peer_port   ? cfg.peer_port
                                 : 8765;

    std::string bind_host = "127.0.0.1";
    if (args.has("--lan")) {
        auto ip = lan_ip();
        if (!ip)
            die("--lan: no en0 IPv4 found (offline, or Wi-Fi isn't en0). "
                "Join the LAN, or drop --lan to bind 127.0.0.1");
        bind_host = *ip;
    }

    try {
        Listener listener(bind_host, port);

        std::print("Serving {}/ on {}:{} ... (Ctrl-C to stop)\n", shared_dir, bind_host, port);
        if (bind_host != "127.0.0.1")
            std::print("  → peers: set config.json peer.host to '{}'\n", bind_host);

        for (;;) {
            Accepted peer = listener.accept();
            std::print("Connected from {}:{}\n", peer.peer_ip, peer.peer_port);

            serve_session(peer.socket, shared_dir, cfg.token);
            peer.socket.close();
            std::print("Session ended, waiting for next peer\n");
        }
    } catch (const SocketError& e) {
        die(e.what());
    }
}
