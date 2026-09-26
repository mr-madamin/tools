// Real sessions against serveSession, in-process. These cover the ground of
// hello_test, bad_frame_test, atomic_test and the path-safety half of
// delete_test — without needing a server running on 8765.
import Foundation
import SyncCore
import Testing

private func expectError(_ reply: [String: Any]?, containing text: String = "", _ label: String) {
    #expect(reply?["op"] as? String == "ERROR", "\(label): expected ERROR, got \(String(describing: reply))")
    if !text.isEmpty {
        let message = reply?["message"] as? String ?? ""
        #expect(message.contains(text), "\(label): message was \(message)")
    }
}

@Suite("HELLO gates every op")
struct HelloTests {
    @Test(arguments: [
        (#"{"op": "HELLO", "token": "definitely-not-it"}"#, "bad token"),
        (#"{"op": "HELLO"}"#, "bad token"),
        (#"{"op": "MANIFEST"}"#, "auth required"),
        (#"{"op": "DELETE", "path": "anything.txt"}"#, "auth required"),
    ])
    func rejectedThenClosed(frame: String, message: String) throws {
        let server = try TestServer()
        let sock = try server.connect()
        expectError(try server.exchange(sock, frame), containing: message, frame)
        #expect(try recvMsg(sock) == nil, "server did not close after rejecting")
    }

    @Test func tokenComparedByBytes() throws {
        // Swift's String == would call these equal (canonical equivalence);
        // the token check must not.
        let server = try TestServer()
        let sock = try server.connect()
        let decomposed = server.token.decomposedStringWithCanonicalMapping + "\u{301}"
        expectError(try server.exchange(sock, #"{"op": "HELLO", "token": "\#(decomposed)"}"#), containing: "bad token", "near-miss token")
    }

    @Test func correctTokenServes() throws {
        let server = try TestServer()
        let files = try server.manifest()
        #expect(files.isEmpty)
    }
}

@Suite("Malformed frames")
struct BadFrameTests {
    @Test(arguments: [
        ("not json at all", "not valid UTF-8 JSON"),
        ("123", "not a JSON object"),
        (String(repeating: "[", count: 200_000), "not valid UTF-8 JSON"),
        (#"{"foo": 1}"#, "unknown op: None"),
        (#"{"op": "MANFEST"}"#, "unknown op: 'MANFEST'"),
        (#"{"op": "PUT", "path": "x"}"#, "missing field: 'size'"),
        (#"{"op": "PUT", "size": 1, "mtime": 0}"#, "missing field: 'path'"),
        (#"{"op": "PUT", "path": "x", "size": -1, "mtime": 0}"#, "out-of-range 'size'"),
        (#"{"op": "PUT", "path": "x", "size": true, "mtime": 0}"#, "out-of-range 'size'"),
        (#"{"op": "PUT", "path": "x", "size": "5", "mtime": 0}"#, "out-of-range 'size'"),
        (#"{"op": "PUT", "path": "x", "size": 1, "mtime": 1e20}"#, "out-of-range 'mtime'"),
        (#"{"op": "PUT", "path": "x", "size": 1e999, "mtime": 0}"#, "not valid UTF-8 JSON"),
        (#"{"op": "DELETE"}"#, "DELETE missing 'path'"),
    ])
    func refusedAfterAuth(frame: String, message: String) throws {
        let server = try TestServer()
        let sock = try server.connectAuthed()
        expectError(try server.exchange(sock, frame), containing: message, String(frame.prefix(40)))
    }

    @Test func invalidUTF8() throws {
        let server = try TestServer()
        let sock = try server.connect()
        try sendMsg(sock, [0x7B, 0xFF, 0xFE, 0x7D])
        expectError(try recvMsg(sock).map { try decodeHeader($0) }, containing: "UTF-8", "invalid UTF-8")
    }

    @Test("an oversized length prefix is refused before the HELLO")
    func oversizedBeforeHello() throws {
        let server = try TestServer()
        let sock = try server.connect()
        try sock.sendAll([0x00, 0x20, 0x00, 0x00]) // claims 2 MB, sends nothing
        expectError(try recvMsg(sock).map { try decodeHeader($0) }, containing: "frame too large", "oversized")
    }

    @Test func serverSurvivesEveryBadFrame() throws {
        let server = try TestServer()
        for frame in ["garbage", "[]", #"{"op": 5}"#] {
            let sock = try server.connect()
            _ = try server.exchange(sock, frame)
        }
        #expect(try server.manifest().isEmpty)
    }

    @Test func idlePeerIsDropped() throws {
        let server = try TestServer(idleTimeout: 1)
        let sock = try server.connect()
        #expect(try recvMsg(sock) == nil, "an idle session was never dropped")
    }
}

@Suite("PUT")
struct PutTests {
    @Test func roundTripKeepsContentAndMtime() throws {
        let server = try TestServer()
        let src = makeTempDir()
        defer { removeTree(src) }
        writeFile(src + "/nested/dir/hello.txt", "hello, receiver\n")
        var times = [timespec(tv_sec: 1_700_000_000, tv_nsec: 123_456_000), timespec(tv_sec: 1_700_000_000, tv_nsec: 123_456_000)]
        utimensat(AT_FDCWD, src + "/nested/dir/hello.txt", &times, 0)

        let sock = try server.connectAuthed()
        _ = try sendFile(sock, root: src, relPath: "nested/dir/hello.txt")
        try sendJSON(sock, ["op": "BYE"]) // one session at a time: end this one first
        let files = try server.manifest() // a second session: the PUT above has landed

        #expect(readFile(server.dir + "/nested/dir/hello.txt") == "hello, receiver\n")
        let info = try #require(files["nested/dir/hello.txt"] as? [String: Any])
        let mtime = try #require(info["mtime"] as? Double)
        #expect(abs(mtime - 1_700_000_000.123456) < 1e-6)
    }

    @Test("an aborted PUT leaves the old file intact and no .tmp behind")
    func abortedPut() throws {
        let server = try TestServer()
        writeFile(server.dir + "/probe.txt", "THE ORIGINAL GOOD CONTENT\n")

        let sock = try server.connectAuthed()
        try sendJSON(sock, ["op": "PUT", "path": "probe.txt", "size": 5 * 1024 * 1024, "mtime": 1.0])
        try sock.sendAll([UInt8](repeating: UInt8(ascii: "X"), count: 102_400))
        sock.close() // vanish mid-body

        _ = try server.manifest() // the aborted session is over once this one is served
        #expect(readFile(server.dir + "/probe.txt") == "THE ORIGINAL GOOD CONTENT\n")
        let leftovers = try FileManager.default.contentsOfDirectory(atPath: server.dir).filter { $0.contains(".tmp") }
        #expect(leftovers.isEmpty)
    }

    @Test(arguments: ["../escaped.txt", "/etc/escaped", "a/../../escaped.txt", "."])
    func escapingPathRefused(path: String) throws {
        let server = try TestServer()
        let sock = try server.connectAuthed()
        let reply = try server.exchange(sock, #"{"op": "PUT", "path": "\#(path)", "size": 0, "mtime": 0}"#)
        expectError(reply, containing: "unsafe path refused", path)
        #expect(!FileManager.default.fileExists(atPath: dirName(server.dir) + "/escaped.txt"))
    }

    @Test("a symlink pointing out of the tree can't be written through")
    func symlinkEscape() throws {
        let server = try TestServer()
        let outside = makeTempDir()
        defer { removeTree(outside) }
        #expect(symlink(outside, server.dir + "/out") == 0)

        let sock = try server.connectAuthed()
        let reply = try server.exchange(sock, #"{"op": "PUT", "path": "out/evil.txt", "size": 0, "mtime": 0}"#)
        expectError(reply, containing: "unsafe path refused", "symlink escape")
        #expect(!FileManager.default.fileExists(atPath: outside + "/evil.txt"))
    }
}

@Suite("DELETE")
struct DeleteTests {
    @Test func removesNestedFile() throws {
        let server = try TestServer()
        writeFile(server.dir + "/sub/doomed.txt", "delete me\n")
        let sock = try server.connectAuthed()
        try sendDelete(sock, "sub/doomed.txt")
        _ = try server.exchange(sock, #"{"op": "MANIFEST"}"#)
        #expect(!FileManager.default.fileExists(atPath: server.dir + "/sub/doomed.txt"))
    }

    @Test func missingFileIsIdempotent() throws {
        let server = try TestServer()
        let sock = try server.connectAuthed()
        try sendDelete(sock, "never_existed.txt") // no reply on success...
        let reply = try server.exchange(sock, #"{"op": "MANIFEST"}"#) // ...so prove the session lives
        #expect(reply?["op"] as? String == "MANIFEST")
    }

    @Test(arguments: ["../victim.txt", "/etc/hosts", "."])
    func escapeRefused(path: String) throws {
        let server = try TestServer()
        let victim = dirName(server.dir) + "/victim-\(UUID().uuidString).txt"
        writeFile(victim, "must survive\n")
        defer { removeTree(victim) }

        let sock = try server.connectAuthed()
        let target = path == "../victim.txt" ? "../" + URL(fileURLWithPath: victim).lastPathComponent : path
        expectError(try server.exchange(sock, #"{"op": "DELETE", "path": "\#(target)"}"#), containing: "unsafe path refused", path)
        #expect(FileManager.default.fileExists(atPath: victim))
    }
}
