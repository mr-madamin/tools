import Foundation
import SyncCore

/// A fresh directory under $TMPDIR, removed by the caller's `defer`.
func makeTempDir() -> String {
    let path = NSTemporaryDirectory() + "lan_folder_sync_tests_" + UUID().uuidString
    try! FileManager.default.createDirectory(atPath: path, withIntermediateDirectories: true)
    return path
}

func removeTree(_ path: String) {
    try? FileManager.default.removeItem(atPath: path)
}

func writeFile(_ path: String, _ text: String) {
    try! FileManager.default.createDirectory(atPath: dirName(path), withIntermediateDirectories: true)
    try! Data(text.utf8).write(to: URL(fileURLWithPath: path))
}

func readFile(_ path: String) -> String? {
    FileManager.default.contents(atPath: path).map { String(decoding: $0, as: UTF8.self) }
}

func socketPair() -> (Socket, Socket) {
    var fds: [Int32] = [0, 0]
    precondition(socketpair(AF_UNIX, SOCK_STREAM, 0, &fds) == 0)
    return (Socket(fd: fds[0]), Socket(fd: fds[1]))
}

/// Run `body` on another thread; `wait()` returns its result.
final class Background<T>: @unchecked Sendable {
    private var result: Result<T, Error>?
    private let done = DispatchSemaphore(value: 0)

    init(_ body: @escaping @Sendable () throws -> T) {
        Thread { [self] in
            result = Result { try body() }
            done.signal()
        }.start()
    }

    func wait() throws -> T {
        done.wait()
        return try result!.get()
    }
}

/// A real server session loop on an ephemeral loopback port, serving a temp
/// directory. The same serveSession sync_server runs — just in-process.
final class TestServer: @unchecked Sendable {
    let dir: String
    let token = "test-token-🔑"
    private let listener: Listener

    init(idleTimeout: Int = 10) throws {
        dir = makeTempDir()
        listener = try Listener(host: "127.0.0.1", port: 0)
        let listener = self.listener, dir = self.dir, token = self.token
        Thread {
            while let accepted = try? listener.accept() {
                serveSession(accepted.socket, sharedDir: dir, token: token, idleTimeout: idleTimeout, log: { _ in })
                accepted.socket.close()
            }
        }.start()
    }

    deinit { removeTree(dir) }

    func connect() throws -> Socket {
        let sock = try Socket.connect(host: "127.0.0.1", port: listener.port, timeout: 5)
        sock.setTimeout(seconds: 10) // a wedged server should fail the test, not hang it
        return sock
    }

    func connectAuthed() throws -> Socket {
        let sock = try connect()
        let reply = try handshake(sock, token: token)
        precondition(reply?["op"] as? String == "OK", "handshake failed: \(String(describing: reply))")
        return sock
    }

    /// Send one raw frame and return the reply, or nil if the server closed.
    func exchange(_ sock: Socket, _ payload: String) throws -> [String: Any]? {
        try sendMsg(sock, Array(payload.utf8))
        return try recvMsg(sock).map { try decodeHeader($0) }
    }

    func manifest() throws -> [String: Any] {
        let sock = try connectAuthed()
        defer { sock.close() }
        return try exchange(sock, #"{"op": "MANIFEST"}"#)?["files"] as! [String: Any]
    }
}
