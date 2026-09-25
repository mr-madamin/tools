// Self-contained: framing over a socketpair, no server. Mirrors nasty_test and
// the C port's truncate_test.
import Foundation
import SyncCore
import Testing

@Suite("Framing boundaries")
struct FramingTests {
    @Test("a payload far bigger than the socket buffer survives partial reads")
    func bigPayload() throws {
        let (a, b) = socketPair()
        let big = [UInt8](repeating: UInt8(ascii: "A"), count: 1_000_000)
        // sendMsg blocks once the kernel buffer fills, so the reader must drain
        // concurrently; recvExactly then reassembles many partial recv()s.
        let reader = Background { try recvMsg(b) }
        try sendMsg(a, big)
        #expect(try reader.wait() == big)
    }

    @Test("two frames coalesced into one write split at the right boundary")
    func coalesced() throws {
        let (a, b) = socketPair()
        try a.sendAll([0, 0, 0, 5] + Array("hello".utf8) + [0, 0, 0, 3] + Array("bye".utf8))
        #expect(try recvMsg(b) == Array("hello".utf8))
        #expect(try recvMsg(b) == Array("bye".utf8))
    }

    @Test("a frame is refused on its length prefix alone")
    func oversized() throws {
        let (a, b) = socketPair()
        try a.sendAll([0xFF, 0xFF, 0xFF, 0xFF])
        #expect(throws: FrameTooLarge.self) { try recvMsg(b, maxBytes: Limits.maxControlFrame) }
    }

    @Test("EOF before a frame reads as nil, not an error")
    func eof() throws {
        let (a, b) = socketPair()
        a.close()
        #expect(try recvMsg(b) == nil)
    }
}

@Suite("sendFile keeps its size promise")
struct SendFileTests {
    /// Truncate the file while it is mid-transfer. The receiver must still get
    /// exactly `size` body bytes, and the NEXT frame must still parse — one byte
    /// short and it would read that frame's header as file content.
    @Test func truncatedMidTransfer() throws {
        let dir = makeTempDir()
        defer { removeTree(dir) }
        let size = 8 * 1024 * 1024 // far more than a socketpair buffers
        try Data(repeating: 0x42, count: size).write(to: URL(fileURLWithPath: dir + "/shrinks.bin"))

        let (a, b) = socketPair()
        let sender = Background {
            let changed = try sendFile(a, root: dir, relPath: "shrinks.bin")
            try sendJSON(a, ["op": "BYE"])
            return changed
        }

        let header = try decodeHeader(try #require(try recvMsg(b)))
        #expect(header["size"] as? Int == size)

        _ = try #require(try b.recvExactly(256 * 1024)) // the sender is now blocked mid-file
        #expect(truncate(dir + "/shrinks.bin", 0) == 0)
        _ = try #require(try b.recvExactly(size - 256 * 1024))

        let next = try decodeHeader(try #require(try recvMsg(b)))
        #expect(next["op"] as? String == "BYE", "the stream desynced after a truncated file")
        #expect(try sender.wait(), "sendFile didn't report that the file changed")
    }

    @Test func unchangedFileReportsNoChange() throws {
        let dir = makeTempDir()
        defer { removeTree(dir) }
        writeFile(dir + "/steady.txt", "steady\n")

        let (a, b) = socketPair()
        let reader = Background { () -> [UInt8]? in
            _ = try recvMsg(b)
            return try b.recvExactly(7)
        }
        #expect(try sendFile(a, root: dir, relPath: "steady.txt") == false)
        #expect(try reader.wait() == Array("steady\n".utf8))
    }
}
