import SyncCore

@main
struct SyncServer {
    static let usage = "usage: sync_server [shared_dir] [port] [--lan]"

    static let help = """
        \(usage)

          --lan    bind this Mac's en0 IPv4 instead of 127.0.0.1, so
                   peers on the LAN can reach it. Exits if en0 has no
                   address rather than quietly serving loopback.
          --help   this message

        shared_dir defaults to config.json shared_dir, port to
        peer.port. Without --lan the server binds 127.0.0.1.
        """

    static func main() {
        setUpProcess()

        let args = Arguments.parse(Array(CommandLine.arguments.dropFirst()), known: ["--lan", "--help"], usage: usage)
        if args.flags.contains("--help") {
            print(help)
            return
        }

        let config = Config.load()
        let sharedDir = args.positional.first ?? config.sharedDir ?? "sandbox/received"
        let port = args.positional.count > 1
            ? parsePort(args.positional[1], where: "port argument")
            : config.peerPort ?? 8765

        let bindHost: String
        if args.flags.contains("--lan") {
            guard let ip = lanIP() else {
                die("--lan: no en0 IPv4 found (offline, or Wi-Fi isn't en0). "
                    + "Join the LAN, or drop --lan to bind 127.0.0.1")
            }
            bindHost = ip
        } else {
            bindHost = "127.0.0.1"
        }

        let listener: Listener
        do {
            listener = try Listener(host: bindHost, port: port)
        } catch {
            die("\(error)")
        }

        print("Serving \(sharedDir)/ on \(bindHost):\(port) ... (Ctrl-C to stop)")
        if bindHost != "127.0.0.1" {
            print("  → peers: set config.json peer.host to '\(bindHost)'")
        }

        while true {
            let (conn, host, peerPort): (Socket, String, Int)
            do {
                (conn, host, peerPort) = try listener.accept()
            } catch {
                die("\(error)")
            }
            print("Connected from \(host):\(peerPort)")
            serveSession(conn, sharedDir: sharedDir, token: config.token)
            conn.close()
            print("Session ended, waiting for next peer")
        }
    }
}
