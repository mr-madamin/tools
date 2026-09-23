import Foundation
import SyncCore

@main
struct SyncPush {
    // Without these the program blocks in the kernel with nothing on screen: a
    // dropped SYN takes ~75 s to fail on macOS, and a peer that never replies
    // waits forever.
    static let connectTimeout = 8 // TCP handshake
    static let protocolTimeout = 15 // HELLO / MANIFEST reply
    static let transferTimeout = 300 // any single stalled send during the transfer

    static let usage = "usage: sync_push [peer_host] [root_dir] [--delete | --dry-run]"
    static let loopback: Set<String> = ["127.0.0.1", "localhost", "::1"]

    static let help = """
        \(usage)

          --dry-run   print the plan (puts and deletes), send nothing
          --delete    also remove files the peer has and the source
                      doesn't, making the peer an exact mirror
          --help      this message

        peer_host defaults to config.json peer.host, root_dir to
        shared_dir. The port always comes from config.json peer.port.
        """

    static func main() {
        setUpProcess()

        let args = Arguments.parse(
            Array(CommandLine.arguments.dropFirst()),
            known: ["--dry-run", "--delete", "--help"],
            usage: usage)
        if args.flags.contains("--help") {
            print(help)
            return
        }

        let config = Config.load()
        let push = Push(
            host: args.positional.first ?? config.peerHost ?? "127.0.0.1",
            port: config.peerPort ?? 8765,
            rootDir: args.positional.count > 1 ? args.positional[1] : config.sharedDir ?? "sandbox/source",
            token: config.token,
            dryRun: args.flags.contains("--dry-run"),
            delete: args.flags.contains("--delete"))
        push.run()
    }
}

func hint(_ message: String) {
    print("  ! \(message)")
}

func human(_ bytes: Double) -> String {
    let units = ["B", "KB", "MB", "GB"]
    var n = bytes
    var index = 0
    while n >= 1024 && index < units.count - 1 {
        n /= 1024
        index += 1
    }
    return index == 0 ? String(format: "%.0f B", n) : String(format: "%.1f ", n) + units[index]
}

func seconds(_ duration: Duration) -> Double {
    let (whole, atto) = duration.components
    return Double(whole) + Double(atto) / 1e18
}

struct Push {
    let host: String
    let port: Int
    let rootDir: String
    let token: String
    let dryRun: Bool
    let delete: Bool

    func run() {
        print("Push \(rootDir)  ->  \(host):\(port)")

        // Both of these end in an empty local manifest, which with --delete reads
        // as "the source has nothing, so remove everything" — check before we
        // connect.
        if rootDir.hasPrefix("~") {
            die("root_dir '\(rootDir)' starts with '~', which is NOT expanded here --\n"
                + "it would walk an empty folder and push nothing. Use an absolute path.")
        }
        if !isDirectory(rootDir) {
            die("root_dir does not exist: \(absolutePath(rootDir))\n"
                + "(cwd: \(FileManager.default.currentDirectoryPath)) -- check the path, or mkdir -p it.")
        }

        if !SyncPush.loopback.contains(host) {
            if let me = lanIP() {
                if me == host {
                    hint("peer host \(host) is THIS machine's own IP -- pushing to yourself?")
                    hint("the peer IP must come from the RECEIVER's 'ipconfig getifaddr en0'")
                } else if !sameSubnet(me, host) {
                    hint("this Mac is \(me), the peer is \(host) -- different subnets")
                    hint("same Wi-Fi network on both? (a guest network is a separate subnet)")
                } else {
                    print("  this Mac: \(me) (same subnet as the peer)")
                }
            } else {
                hint("this Mac has no en0 IPv4 (offline, or Wi-Fi isn't en0) -- a LAN push")
                hint("will not get anywhere until it's on the network")
            }
        }

        print("Connecting (timeout \(SyncPush.connectTimeout)s) ...")
        let clock = ContinuousClock()
        let started = clock.now
        let sock: Socket
        do throws(ConnectFailure) {
            sock = try Socket.connect(host: host, port: port, timeout: SyncPush.connectTimeout)
        } catch {
            // `error` is a ConnectFailure here, not `any Error`, so this switch
            // is checked for exhaustiveness: a new failure case won't compile
            // until it has an explanation.
            switch error {
            case .timeout: die(noAnswerHelp())
            case .refused: die(refusedHelp())
            case .resolve(let reason): die("can't resolve peer host '\(host)': \(reason)")
            case .unreachable(let reason): die(unreachableHelp(reason))
            }
        }
        print(String(format: "  connected in %.2fs", seconds(clock.now - started)))

        // Connected. From here a wedged peer is the risk, not an unreachable one.
        sock.setTimeout(seconds: SyncPush.protocolTimeout)

        let reply: [String: Any]?
        do {
            reply = try handshake(sock, token: token)
        } catch SocketError.timeout {
            die(silentPeerHelp("HELLO"))
        } catch {
            die("handshake failed: \(error)")
        }
        guard let reply else { die("peer closed during handshake (wrong token?)") }
        guard reply["op"] as? String == "OK" else {
            die("handshake refused: \(reply["message"] as? String ?? "None")")
        }
        print("  handshake ok (token accepted)")

        let local = Manifest(scanning: rootDir)

        let remote: Manifest
        do {
            try sendJSON(sock, ["op": "MANIFEST"])
            // The manifest: MAX_FRAME, not the control cap.
            guard let bytes = try recvMsg(sock, maxBytes: Limits.maxFrame) else {
                die("Connection closed by peer before manifest was received")
            }
            let message = try decodeHeader(bytes)
            guard let files = Manifest(json: message["files"]) else {
                die("peer's manifest has no \"files\": \(message["message"] as? String ?? "None")")
            }
            remote = files
        } catch SocketError.timeout {
            die(silentPeerHelp("MANIFEST"))
        } catch let e as FrameTooLarge {
            die("peer's manifest frame is \(e.length) bytes, over the \(Limits.maxFrame) cap --\n"
                + "refusing to read it. That's not a folder listing; check what is\n"
                + "actually listening on \(host):\(port).")
        } catch is HeaderError {
            die("peer sent a manifest we can't parse")
        } catch {
            die("manifest request failed: \(error)")
        }

        let (toPut, toDelete) = diffManifests(local: local, remote: remote)

        if dryRun { print("DRY RUN - no files will be sent") }
        print("Local: \(local.files.count) files | Peer: \(remote.files.count) files")

        // Not an error — we chose not to follow them — but a user who symlinked
        // a folder in here is otherwise told nothing about why it never syncs.
        if !local.skippedLinks.isEmpty {
            let n = local.skippedLinks.count
            hint("skipped \(n) symlink\(n == 1 ? "" : "s") -- a link to a directory isn't followed")
            hint("and a broken link can't be read:")
            for link in local.skippedLinks.sorted() { hint("    \(link)") }
        }

        // A directory we couldn't read is not a directory with nothing in it:
        // the files are there, we just can't see them. Deleting on the peer from
        // a picture we KNOW has holes is the empty-source wipe in a different hat.
        if !local.isComplete {
            hint("could not read every directory under \(rootDir)")
            hint("the file list below is INCOMPLETE -- anything unreadable is missing")
            hint("from it (\(local.errors[0]))")
            if delete && !dryRun {
                die("refusing to run --delete from a partial source listing: files this\n"
                    + "scan could not see look identical to files you deleted, and would\n"
                    + "be removed from the peer.")
            }
        }

        if local.files.isEmpty {
            hint("no files found under \(absolutePath(rootDir))")
            hint("is that the right folder? nothing will be sent")
            if delete && !dryRun {
                die("refusing to run --delete from an empty source: it would wipe the peer.")
            }
        }

        sock.setTimeout(seconds: SyncPush.transferTimeout)

        print("\(dryRun ? "Would send" : "Sending") \(toPut.count) file(s):")
        var sentBytes: Int64 = 0
        let transferStarted = clock.now
        for path in toPut {
            let size = local.files[path]!.size
            print("   PUT    \(path) (\(human(Double(size))))")
            if dryRun { continue }
            do {
                if try sendFile(sock, root: rootDir, relPath: path) {
                    hint("\(path) changed while it was being sent -- the copy on the")
                    hint("peer is padded or clipped to the size we announced.")
                    hint("Re-run to fix it.")
                }
            } catch SocketError.timeout {
                die("stalled for \(SyncPush.transferTimeout)s while sending '\(path)'.\n"
                    + "The peer stopped reading -- server killed, or the Wi-Fi dropped.")
            } catch let e as SocketError where e.isPeerGone {
                die("peer went away while sending '\(path)' (\(e)).\n"
                    + "Check the server's terminal for the error it printed.")
            } catch {
                die("failed sending '\(path)': \(error)")
            }
            sentBytes += size
        }

        if !toDelete.isEmpty {
            let verb = dryRun ? "Would delete" : delete ? "Deleting" : "Skipping (need --delete):"
            print("\(verb) \(toDelete.count) file(s):")
            for path in toDelete {
                print("   DELETE \(path)")
                if !dryRun && delete {
                    do {
                        try sendDelete(sock, path)
                    } catch {
                        die("peer went away while deleting '\(path)' (\(error)).")
                    }
                }
            }
        }

        try? sendJSON(sock, ["op": "BYE"])
        sock.close()

        if !dryRun && sentBytes > 0 {
            let elapsed = seconds(clock.now - transferStarted)
            let rate = human(Double(sentBytes) / max(elapsed, 0.001))
            print("Done: \(human(Double(sentBytes))) in \(String(format: "%.1f", elapsed))s (\(rate)/s)")
        }
    }

    // MARK: - explaining failures

    /// Nothing came back from the SYN: the packet is being dropped, not refused.
    func noAnswerHelp() -> String {
        let pingProbe = "  ping -c 3 \(host)"
        let ncProbe = "  nc -vz \(host) \(port)"
        let width = max(pingProbe.count, ncProbe.count) + 2
        func padded(_ s: String) -> String { s.padding(toLength: width, withPad: " ", startingAt: 0) }

        var lines = [
            "No answer from \(host):\(port) after \(SyncPush.connectTimeout)s.",
            "",
            "The connection request went out and nothing came back -- not even a",
            "refusal. That means something is DROPPING the packet, in this order of",
            "likelihood:",
            "",
            "  1. Firewall on the receiver (most common). System Settings -> Network",
            "     -> Firewall. Allow incoming connections for sync_server, or turn the",
            "     firewall off while you test. Stealth mode drops silently, exactly",
            "     like this.",
            "  2. Client isolation on the router (guest network / 'AP isolation').",
            "     Both Macs reach the internet, neither can reach the other.",
            "  3. Wrong or stale IP -- DHCP moved the receiver.",
            "  4. The server isn't running at all.",
            "",
            "Narrow it down from HERE:",
            padded(pingProbe) + "# no replies -> network/firewall, not the port",
            padded(ncProbe) + "# 'succeeded' -> port is open, rerun the push",
            "",
            "...and on the RECEIVER:",
            "  ipconfig getifaddr en0        # the IP you should be pushing to",
            "  ./bin/sync_server --lan       # must print 'Serving ... on <that IP>'",
            "",
            "The server prints 'Connected from ...' the instant a peer arrives. If it",
            "stays quiet while this side waits, the packet never reached it -- look at",
            "the firewall and the router, not at this program.",
        ]
        let tunnels = tunnelInterfaces()
        if !tunnels.isEmpty {
            lines += [
                "",
                "Also: a VPN/tunnel looks active here (\(tunnels.joined(separator: ", "))). If it grabs",
                "the default route it can swallow LAN traffic. Try again with it off.",
            ]
        }
        return lines.joined(separator: "\n")
    }

    func refusedHelp() -> String {
        """
        Connection refused by \(host):\(port).

        Good news: the host is reachable and answered. Nothing is listening on
        that port for that address.

          - Is the server running on the receiver?
          - Was it started with --lan? Without it the server binds 127.0.0.1
            only, and refuses connections arriving on the LAN IP.
          - Do the ports match? This side is using \(port) (config.json peer.port).
        """
    }

    func unreachableHelp(_ reason: String) -> String {
        """
        Can't reach \(host):\(port) -- \(reason).

        The route doesn't exist: this Mac has no path to that address.
          - Is this Mac on Wi-Fi at all?  ipconfig getifaddr en0
          - Same network as the receiver, and the same subnet?
          - A VPN can take the LAN route away; turn it off and retry.
        """
    }

    func silentPeerHelp(_ stage: String) -> String {
        """
        Connected to \(host):\(port), but the peer never answered \(stage) within \(SyncPush.protocolTimeout)s.

        The TCP connection is up, so this is the other end misbehaving:
          - Is something ELSE listening on that port (not sync_server)?
          - Is the server wedged in an earlier session? Restart it.
        """
    }
}
