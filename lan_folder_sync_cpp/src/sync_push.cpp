// The brain: read the local folder, ask the peer for its manifest, diff the
// two, and decide every PUT and DELETE. --dry-run prints the plan and sends
// nothing; --delete turns "the peer has extras" into actual removals.
#include "config.hpp"
#include "framing.hpp"
#include "json.hpp"
#include "manifest.hpp"
#include "socket.hpp"
#include "util.hpp"

#include <csignal>
#include <cstdio>
#include <format>
#include <print>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace lfs;

// Without these the program blocks in the kernel with nothing on screen: a
// dropped SYN takes ~75s to fail on macOS, and a peer that accepts and then
// says nothing waits forever. Same three phases as sync_push.py.
constexpr int CONNECT_TIMEOUT = 8;    // TCP handshake
constexpr int PROTOCOL_TIMEOUT = 15;  // HELLO / MANIFEST reply
constexpr int TRANSFER_TIMEOUT = 300; // any single stalled send during the transfer

constexpr std::string_view USAGE =
    "usage: sync_push [peer_host] [root_dir] [--delete | --dry-run]";

constexpr std::string_view HELP =
    "  --dry-run   print the plan (puts and deletes), send nothing\n"
    "  --delete    also remove files the peer has and the source\n"
    "              doesn't, making the peer an exact mirror\n"
    "  --help      this message\n\n"
    "peer_host defaults to config.json peer.host, root_dir to\n"
    "shared_dir. The port always comes from config.json peer.port.";

// Python's hint(): a warning that isn't fatal, set off from the normal log.
void hint(std::string_view text)
{
    std::print("  ! {}\n", text);
}

// The TCP connection is up but the peer never answered -- a different failure
// from "can't get there", and it points at the other end, not the network.
[[noreturn]] void die_silent_peer(const std::string& host, int port, std::string_view stage)
{
    die(std::format(
        "Connected to {}:{}, but the peer never answered {} within {}s.\n\n"
        "The TCP connection is up, so this is the other end misbehaving:\n"
        "  - Is something ELSE listening on that port (not sync_server)?\n"
        "  - Is the server wedged in an earlier session? Restart it.",
        host, port, stage, PROTOCOL_TIMEOUT));
}

// Each way of failing to connect gets its own page, because they need opposite
// fixes. ConnectFailure carries the kind as a value, so this switch is the one
// place that has to know them all -- adding a kind breaks the build here
// rather than silently printing the wrong advice.
[[noreturn]] void die_connect_failure(const ConnectFailure& failure, const std::string& host,
                                      int port)
{
    switch (failure.kind) {
    case ConnectFailure::Kind::Resolve:
        die(std::format("can't resolve peer host '{}': {}", host, failure.what()));
    case ConnectFailure::Kind::Timeout:
        die(std::format(
            "No answer from {}:{} after {}s.\n\n"
            "The connection request went out and nothing came back -- not even\n"
            "a refusal. Something is DROPPING the packet:\n"
            "  1. Firewall on the receiver (most common). Stealth mode drops\n"
            "     silently, exactly like this.\n"
            "  2. Client isolation on the router (guest network / AP isolation).\n"
            "  3. Wrong or stale IP -- DHCP moved the receiver.\n"
            "  4. The server isn't running at all.\n\n"
            "Narrow it down from HERE:\n"
            "  ping -c 3 {}      # no replies -> network/firewall, not the port\n"
            "  nc -vz {} {}      # 'succeeded' -> port is open, rerun the push\n\n"
            "The server prints 'Connected from ...' the instant a peer arrives.\n"
            "If it stays quiet while this side waits, the packet never reached\n"
            "it -- look at the firewall and the router, not at this program.",
            host, port, CONNECT_TIMEOUT, host, host, port));
    case ConnectFailure::Kind::Refused:
        die(std::format(
            "Connection refused by {}:{}.\n\n"
            "Good news: the host is reachable and answered. Nothing is listening\n"
            "on that port for that address.\n\n"
            "  - Is the server running on the receiver?\n"
            "  - Was it started with --lan? Without it the server binds\n"
            "    127.0.0.1 only, and refuses connections on the LAN IP.\n"
            "  - Do the ports match? This side is using {} (config.json\n"
            "    peer.port).",
            host, port, port));
    case ConnectFailure::Kind::Unreachable:
        die(std::format(
            "Can't reach {}:{} -- {}.\n\n"
            "The route doesn't exist: this Mac has no path to that address.\n"
            "  - Is this Mac on Wi-Fi at all?  ipconfig getifaddr en0\n"
            "  - Same network as the receiver, and the same subnet?\n"
            "  - A VPN can take the LAN route away; turn it off and retry.",
            host, port, failure.what()));
    }
    die("unreachable");
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::signal(SIGPIPE, SIG_IGN);

    Args args = parse_args(argc, argv, {"--dry-run", "--delete", "--help"});
    if (args.has("--help")) {
        std::print("{}\n\n{}\n", USAGE, HELP);
        return 0;
    }
    if (!args.unknown.empty())
        die_unknown_flags(args.unknown, USAGE);

    const bool dry_run = args.has("--dry-run");
    const bool delete_extras = args.has("--delete");

    Config cfg = load_config(argv[0]);

    std::string host = args.at(0)      ? *args.at(0)
                       : cfg.peer_host ? *cfg.peer_host
                                       : "127.0.0.1";
    int port = cfg.peer_port != 0 ? cfg.peer_port : 8765;
    std::string root_dir = args.at(1)       ? *args.at(1)
                           : cfg.shared_dir ? *cfg.shared_dir
                                            : "sandbox/source";

    std::print("Push {}  ->  {}:{}\n", root_dir, host, port);

    // Both of these end in an empty local manifest, which with --delete reads
    // as "the source has nothing, so remove everything" -- check before we
    // connect.
    if (root_dir.starts_with("~"))
        die(std::format("root_dir '{}' starts with '~', which is NOT expanded here --\n"
                        "it would walk an empty folder and push nothing. Use an absolute path.",
                        root_dir));

    struct stat root_st{};
    if (stat(root_dir.c_str(), &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        char cwd[PATH_MAX];
        die(std::format("root_dir does not exist: {}\n"
                        "(cwd: {}) -- check the path, or mkdir -p it.",
                        path_abs(root_dir), getcwd(cwd, sizeof(cwd)) != nullptr ? cwd : "?"));
    }

    std::print("Connecting (timeout {}s) ...\n", CONNECT_TIMEOUT);
    std::optional<Socket> sock;
    try {
        sock = tcp_connect(host, port, CONNECT_TIMEOUT);
    } catch (const ConnectFailure& e) {
        die_connect_failure(e, host, port);
    }

    // Connected. From here a wedged peer is the risk, not an unreachable one.
    sock->set_timeout(PROTOCOL_TIMEOUT);

    // Which phase we are in, so a timeout can name the thing that went quiet:
    // "the peer never answered HELLO" and "the peer stopped reading mid-send"
    // need opposite fixes.
    enum class Stage { Handshake, Manifest, Transfer };
    Stage stage = Stage::Handshake;

    try {
        auto reply = handshake(*sock, cfg.token);
        if (!reply)
            die("peer closed during handshake (wrong token?)");

        const std::string* op = reply->str("op");
        if (op == nullptr || *op != "OK") {
            const std::string* message = reply->str("message");
            die(std::format("handshake refused: {}", message != nullptr ? *message : "None"));
        }

        stage = Stage::Manifest;
        Manifest local = Manifest::scan(root_dir);

        send_json(*sock, R"({"op": "MANIFEST"})");
        auto payload = recv_msg(*sock); // the manifest: MAX_FRAME, not control
        if (!payload)
            die("Connection closed by peer before manifest was received");

        auto message = json::parse(*payload);
        if (!message)
            die("peer sent a manifest we can't parse");

        auto remote = Manifest::from_json(message->find("files"));
        if (!remote) {
            const std::string* text = message->str("message");
            die(std::format("peer's manifest has no \"files\": {}",
                            text != nullptr ? *text : "None"));
        }

        Plan plan = diff_manifests(local, *remote);

        if (dry_run)
            std::print("DRY RUN - no files will be sent\n");
        std::print("Local: {} files | Peer: {} files\n", local.files.size(), remote->files.size());

        // Not an error -- we chose not to follow them -- but a user who
        // symlinked a folder in here is otherwise told nothing about why it
        // never syncs.
        if (!local.skipped_links.empty()) {
            hint(std::format("skipped {} symlink{} -- a link to a directory isn't followed",
                             local.skipped_links.size(),
                             local.skipped_links.size() == 1 ? "" : "s"));
            hint("(same as Python's os.walk), and a broken link can't be read:");
            for (const auto& link : local.skipped_links)
                hint(std::format("    {}", link));
            hint("copy the folder in, or sync it separately, if you want it mirrored");
        }

        // A directory we could not read is not the same as a directory with
        // nothing in it: the files are there, we just can't see them. Deleting
        // on the peer from a picture we know is incomplete is the empty-source
        // wipe wearing a different hat.
        if (!local.complete) {
            hint(std::format("could not read every directory under {}", root_dir));
            hint("the file list above is INCOMPLETE -- anything unreadable is");
            hint("missing from it (paths past PATH_MAX do this silently)");
            if (delete_extras && !dry_run)
                die("refusing to run --delete from a partial source listing: files\n"
                    "this scan could not see look identical to files you deleted,\n"
                    "and would be removed from the peer.");
        }

        if (local.files.empty()) {
            hint(std::format("no files found under {}", path_abs(root_dir)));
            hint("is that the right folder? nothing will be sent");
            if (delete_extras && !dry_run)
                die("refusing to run --delete from an empty source: it would wipe the peer.");
        }

        // A big file legitimately takes a while; only a fully stalled send is a
        // failure, so the transfer phase gets a much longer leash.
        stage = Stage::Transfer;
        sock->set_timeout(TRANSFER_TIMEOUT);

        std::print("{} {} file(s):\n", dry_run ? "Would send" : "Sending", plan.to_put.size());
        for (const auto& rel_path : plan.to_put) {
            std::print("   PUT    {}\n", rel_path);
            if (dry_run)
                continue;
            try {
                if (send_file(*sock, root_dir, rel_path) == SendResult::Changed) {
                    hint(std::format("{} changed while it was being sent -- the copy on the "
                                     "peer is",
                                     rel_path));
                    hint("padded or clipped to the size we announced. Re-run to fix it.");
                    // The stream is still in sync; keep going.
                }
            } catch (const LocalFileError& e) {
                // Nothing has been sent for this file, so the stream is still
                // in sync: report it and keep going rather than abandon the
                // whole push over one file that vanished mid-run.
                hint(std::format("{} -- skipping it", e.what()));
            } catch (const SocketTimeout&) {
                die(std::format("stalled for {}s while sending '{}'.\n"
                                "The peer stopped reading -- server killed, or the Wi-Fi dropped.",
                                TRANSFER_TIMEOUT, rel_path));
            } catch (const SocketError& e) {
                die(std::format("peer went away while sending '{}' ({}).\n"
                                "Check the server's terminal for the error it printed.",
                                rel_path, e.what()));
            }
        }

        if (!plan.to_delete.empty()) {
            std::string_view verb = dry_run          ? "Would delete"
                                    : delete_extras  ? "Deleting"
                                                     : "Skipping (need --delete):";
            std::print("{} {} file(s):\n", verb, plan.to_delete.size());
            for (const auto& rel_path : plan.to_delete) {
                std::print("   DELETE {}\n", rel_path);
                if (!dry_run && delete_extras)
                    send_delete(*sock, rel_path);
            }
        }

        send_json(*sock, R"({"op": "BYE"})");
        sock->close();
    } catch (const SocketTimeout&) {
        if (stage == Stage::Handshake)
            die_silent_peer(host, port, "HELLO");
        if (stage == Stage::Manifest)
            die_silent_peer(host, port, "MANIFEST");
        // A per-file stall is caught in the loop, with the filename; this is a
        // stall while sending DELETEs or the closing BYE.
        die(std::format("stalled for {}s finishing the session.\n"
                        "The peer stopped reading -- server killed, or the Wi-Fi dropped.",
                        TRANSFER_TIMEOUT));
    } catch (const FrameTooLarge&) {
        die(std::format("peer's manifest frame exceeds {} bytes -- refusing to allocate it.\n"
                        "That's not a folder listing; check what's actually on {}:{}.",
                        MAX_FRAME, host, port));
    } catch (const SocketError& e) {
        die(std::format("peer went away: {}", e.what()));
    }

    return 0;
}
