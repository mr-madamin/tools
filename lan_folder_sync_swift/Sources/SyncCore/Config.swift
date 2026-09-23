import Foundation

public struct Config {
    public var sharedDir: String?
    public var peerHost: String?
    public var peerPort: Int?
    public var token: String

    /// Find config.json and load it, or exit explaining what's wrong.
    ///
    /// The binaries live in bin/ (or .build/), so look a little wider than
    /// Python's "next to the source file" — the same order as the C port:
    /// $LAN_FOLDER_SYNC_CONFIG, ./config.json, next to the executable, its parent.
    public static func load(argv0: String = CommandLine.arguments[0]) -> Config {
        guard let path = find(argv0: argv0),
              let data = FileManager.default.contents(atPath: path)
        else {
            die("config.json not found. Copy config.example.json to config.json and set "
                + "shared_dir, peer host/port, and a shared token (same on both machines).")
        }
        do {
            return try parse(data, path: path)
        } catch {
            die(error.message)
        }
    }

    private static func find(argv0: String) -> String? {
        if let env = ProcessInfo.processInfo.environment["LAN_FOLDER_SYNC_CONFIG"], !env.isEmpty {
            return env
        }
        let dir = dirName(argv0)
        let candidates = ["config.json", joinPath(dir, "config.json"), joinPath(dir, "../config.json")]
        return candidates.first { FileManager.default.isReadableFile(atPath: $0) }
    }

    public struct ParseError: Error {
        public let message: String
    }

    /// Absent is fine — the defaults are the point. Present but the wrong type
    /// is a typo worth naming: quoting the port by mistake used to give you 8765
    /// and a "connection refused" with nothing pointing at config.json.
    public static func parse(_ data: Data, path: String) throws(ParseError) -> Config {
        guard let root = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else {
            throw ParseError(message: "\(path) is not valid JSON")
        }

        func string(_ object: [String: Any], _ key: String, _ label: String) throws(ParseError) -> String? {
            let value = object[key]
            if isMissing(value) { return nil }
            guard let s = value as? String else {
                throw ParseError(message: "\(path): \"\(label)\" must be a string, in double quotes")
            }
            return s
        }

        guard let token = try string(root, "token", "token") else {
            throw ParseError(message: "\(path): missing \"token\"")
        }
        var config = Config(sharedDir: try string(root, "shared_dir", "shared_dir"), token: token)

        let peerValue = root["peer"]
        if !isMissing(peerValue) {
            guard let peer = peerValue as? [String: Any] else {
                throw ParseError(message: "\(path): \"peer\" must be an object, like "
                    + "{\"host\": \"192.168.1.42\", \"port\": 8765}")
            }
            config.peerHost = try string(peer, "host", "peer.host")

            let portValue = peer["port"]
            if !isMissing(portValue) {
                guard let port = jsonNumber(portValue) else {
                    throw ParseError(message: "\(path): peer.port must be a number, not in quotes")
                }
                guard port >= 1 && port <= 65535 else {
                    throw ParseError(message: "\(path): peer.port must be 1-65535")
                }
                config.peerPort = Int(port)
            }
        }
        return config
    }
}
