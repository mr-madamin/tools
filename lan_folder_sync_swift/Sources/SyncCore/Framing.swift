// Wire protocol: a 4-byte big-endian length prefix, then a UTF-8 JSON payload.
// A PUT frame is followed by exactly `size` raw body bytes. Byte-for-byte the
// protocol framing.py and the C port speak, so all three interoperate.
import Darwin
import Foundation

public enum Limits {
    /// size and mtime arrive as JSON numbers from the peer. A negative size
    /// skipped the write loop and left an empty file where a real one had been,
    /// and a huge one could fill the disk. Bound both.
    public static let maxFileSize: Int64 = 64 * 1024 * 1024 * 1024 // 64 GiB per file
    public static let maxMtime: Double = 1e15 // far past any real clock

    /// The length prefix can claim up to 4 GB before a byte of payload arrives,
    /// and it is read before the HELLO. Two ceilings, because the directions
    /// carry very different frames: a MANIFEST lists every file in the folder,
    /// everything else is a short header. The server only reads the short kind.
    public static let maxFrame = 64 * 1024 * 1024 // a manifest of a huge folder
    public static let maxControlFrame = 1024 * 1024 // HELLO / PUT / DELETE / BYE

    /// A file counts as unchanged when size AND mtime match. The window only
    /// absorbs timestamp round-trip error, which the C port measured at under a
    /// microsecond. framing.py still uses 2 seconds, which silently swallows a
    /// same-size edit made within 2 s of the peer's copy — permanently.
    public static let mtimeTolerance = 0.001
}

/// A peer declared a frame bigger than we are willing to read.
public struct FrameTooLarge: Error, CustomStringConvertible {
    public let length: UInt32
    public var description: String { "\(length)" }
}

/// Frame and send: 4-byte big-endian length prefix, then the payload.
public func sendMsg(_ sock: Socket, _ payload: some Collection<UInt8>) throws {
    var frame: [UInt8] = []
    frame.reserveCapacity(4 + payload.count)
    withUnsafeBytes(of: UInt32(payload.count).bigEndian) { frame.append(contentsOf: $0) }
    frame.append(contentsOf: payload)
    try sock.sendAll(frame)
}

public func sendJSON(_ sock: Socket, _ object: [String: Any]) throws {
    try sendMsg(sock, encodeJSON(object))
}

public func sendError(_ sock: Socket, _ message: String) throws {
    try sendJSON(sock, ["op": "ERROR", "message": message])
}

/// Read one framed message. Returns the payload, or nil on EOF.
///
/// Throws FrameTooLarge if the peer declares more than maxBytes. The payload is
/// then left unread, so the stream has no boundary left to resync on and every
/// caller must end the session.
public func recvMsg(_ sock: Socket, maxBytes: Int = Limits.maxFrame) throws -> [UInt8]? {
    guard let header = try sock.recvExactly(4) else { return nil }
    let length = header.withUnsafeBytes { UInt32(bigEndian: $0.loadUnaligned(as: UInt32.self)) }
    if Int(length) > maxBytes { throw FrameTooLarge(length: length) }
    return try sock.recvExactly(Int(length))
}
