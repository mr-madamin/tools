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

public func mtimeSeconds(_ st: stat) -> Double {
    Double(st.st_mtimespec.tv_sec) + Double(st.st_mtimespec.tv_nsec) / 1e9
}

/// Send one file as a PUT: JSON header {path, size, mtime}, then the raw body.
/// Returns true if the file changed size while we were reading it.
///
/// The body is the only unframed part of the stream — the receiver finds the
/// next frame by counting exactly `size` bytes. One byte too few or too many and
/// it reads the following header as file content, silently corrupting every
/// later file in the session. The file can change underneath us at any point,
/// so the declared size is the contract, honoured even when the file stops
/// matching it.
public func sendFile(_ sock: Socket, root: String, relPath: String) throws -> Bool {
    let fullPath = joinPath(root, relPath)

    // Open first, then fstat the descriptor we actually hold: stat-then-open
    // lets the path be replaced in between, and we would declare one file's
    // size while sending another's bytes.
    let fd = open(fullPath, O_RDONLY)
    guard fd >= 0 else { throw SystemError("can't open \(fullPath)") }
    defer { close(fd) }

    var st = stat()
    guard fstat(fd, &st) == 0 else { throw SystemError("can't stat \(fullPath)") }
    let size = Int64(st.st_size)

    try sendJSON(sock, ["op": "PUT", "path": relPath, "size": size, "mtime": mtimeSeconds(st)])

    var buffer = [UInt8](repeating: 0, count: 65536)
    var sent: Int64 = 0
    var truncated = false
    while sent < size {
        let want = Int(min(Int64(buffer.count), size - sent))
        var got = 0
        if !truncated {
            got = buffer.withUnsafeMutableBytes { read(fd, $0.baseAddress, want) }
            if got < 0 && errno == EINTR { continue }
        }
        if got <= 0 {
            // Truncated mid-transfer (or unreadable). We already promised `size`
            // bytes, so pad — aborting would leave the receiver waiting for
            // bytes that never come and kill the whole session over one file.
            // The padded copy loses the mtime comparison on the next push and
            // gets resent, so this self-heals.
            if !truncated {
                truncated = true
                buffer.withUnsafeMutableBytes { _ = memset($0.baseAddress, 0, $0.count) }
            }
            got = want
        }
        try buffer.withUnsafeBytes { try sock.sendAll(UnsafeRawBufferPointer(rebasing: $0[0..<got])) }
        sent += Int64(got)
    }

    // If the file GREW we simply stopped at `size` and never read the tail —
    // the frame is still exactly as long as advertised.
    var after = stat()
    let grew = fstat(fd, &after) == 0 && Int64(after.st_size) != size
    return truncated || grew
}

public enum PutError: Error {
    /// A field is absent. The body length is then unknown, so the stream can't
    /// be resynced.
    case missingField(String)
    /// size/mtime present but unusable (negative, huge, a bool, a string).
    case outOfRange(String)
    /// The path would land outside dest_dir.
    case unsafePath(String)
}

/// A JSON number we can actually use. Missing and malformed are different
/// errors, because they get different messages.
private func checkedNumber(_ header: [String: Any], _ field: String, _ range: ClosedRange<Double>) throws(PutError) -> Double {
    guard let raw = header[field] else { throw .missingField(field) }
    guard let value = jsonNumber(raw), value.isFinite, range.contains(value) else {
        throw .outOfRange(field)
    }
    return value
}

private func makeDirectories(_ path: String) throws {
    do {
        try FileManager.default.createDirectory(atPath: path, withIntermediateDirectories: true)
    } catch {
        throw SystemError("can't create \(path)", (error as NSError).underlyingPOSIXCode ?? EIO)
    }
}

extension NSError {
    var underlyingPOSIXCode: Int32? {
        if domain == NSPOSIXErrorDomain { return Int32(code) }
        return (userInfo[NSUnderlyingErrorKey] as? NSError)?.underlyingPOSIXCode
    }
}

private func timespecOf(_ seconds: Double) -> timespec {
    var whole = seconds.rounded(.down)
    var nanos = ((seconds - whole) * 1e9).rounded()
    if nanos >= 1e9 {
        whole += 1
        nanos -= 1e9
    }
    return timespec(tv_sec: Int(whole), tv_nsec: Int(nanos))
}

/// Write the raw body that follows a PUT header. Returns the relative path.
public func recvFileBody(_ sock: Socket, destDir: String, header: [String: Any]) throws -> String {
    // Same order as framing.py, so the same bad header gets the same message.
    guard let rawPath = header["path"] else { throw PutError.missingField("path") }
    let size = try Int64(checkedNumber(header, "size", 0...Double(Limits.maxFileSize)))
    let mtime = try checkedNumber(header, "mtime", -Limits.maxMtime...Limits.maxMtime)

    // Check the path BEFORE creating anything: a hand-crafted "../../x" joined
    // onto dest_dir would otherwise be an arbitrary file write on the receiver.
    guard let relPath = rawPath as? String, pathIsLexicallySafe(relPath) else {
        throw PutError.unsafePath(pyRepr(rawPath))
    }

    try makeDirectories(dirName(joinPath(destDir, relPath)))

    // The parent exists now, so the symlink-aware guard can resolve it.
    guard let fullPath = safePath(base: destDir, relPath: relPath) else {
        throw PutError.unsafePath(pyRepr(relPath))
    }

    // Land the bytes beside the target, then rename. Writing fullPath directly
    // truncates the existing copy the moment the transfer starts, so a peer that
    // dies mid-file leaves a stub where a good file used to be. rename() is
    // atomic within a filesystem, which is why the temp is a sibling.
    let tmpPath = "\(fullPath).\(getpid()).tmp"
    let fd = open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC, 0o666)
    guard fd >= 0 else { throw SystemError("can't create \(tmpPath)") }

    var renamed = false
    defer { if !renamed { unlink(tmpPath) } }

    do {
        defer { close(fd) }
        var buffer = [UInt8](repeating: 0, count: 65536)
        var received: Int64 = 0
        while received < size {
            let want = Int(min(Int64(buffer.count), size - received))
            let got = try buffer.withUnsafeMutableBytes {
                try sock.recv(into: UnsafeMutableRawBufferPointer(rebasing: $0[0..<want]))
            }
            if got == 0 { throw SocketError.closedMidFile }
            try buffer.withUnsafeBytes { try writeAll(fd, UnsafeRawBufferPointer(rebasing: $0[0..<got]), tmpPath) }
            received += Int64(got)
        }
    }

    // Stamp the mtime before the rename, so the file is never briefly visible
    // at its final path with the wrong timestamp.
    var times = [timespecOf(mtime), timespecOf(mtime)]
    guard utimensat(AT_FDCWD, tmpPath, &times, 0) == 0 else { throw SystemError("can't set mtime on \(tmpPath)") }
    guard rename(tmpPath, fullPath) == 0 else { throw SystemError("can't rename onto \(fullPath)") }
    renamed = true
    return relPath
}

private func writeAll(_ fd: Int32, _ bytes: UnsafeRawBufferPointer, _ path: String) throws {
    var offset = 0
    while offset < bytes.count {
        let n = write(fd, bytes.baseAddress! + offset, bytes.count - offset)
        if n < 0 {
            if errno == EINTR { continue }
            throw SystemError("can't write \(path)")
        }
        offset += n
    }
}

/// Read a PUT header, then its body.
public func recvFile(_ sock: Socket, destDir: String) throws -> String? {
    guard let bytes = try recvMsg(sock, maxBytes: Limits.maxControlFrame) else { return nil }
    return try recvFileBody(sock, destDir: destDir, header: decodeHeader(bytes))
}

/// Ask the peer to remove one file (no body, no reply on success).
public func sendDelete(_ sock: Socket, _ relPath: String) throws {
    try sendJSON(sock, ["op": "DELETE", "path": relPath])
}

/// Client side of HELLO: send the token, return the server's reply (e.g.
/// {"op": "OK"} or {"op": "ERROR", ...}), or nil if the peer closed. The caller
/// decides what to do with a non-OK reply.
public func handshake(_ sock: Socket, token: String) throws -> [String: Any]? {
    try sendJSON(sock, ["op": "HELLO", "token": token])
    guard let reply = try recvMsg(sock, maxBytes: Limits.maxControlFrame) else { return nil }
    return try decodeHeader(reply)
}
