// A thin, blocking wrapper over BSD sockets.
//
// Network.framework is the Apple-native networking API, but it is callback /
// async shaped, and this protocol is a strictly sequential byte stream: read 4
// bytes, read N bytes, read exactly `size` body bytes. Plain blocking sockets
// are the straightest port of Python's `socket` module and of the C version,
// and keep the three implementations readable side by side.
import Darwin

public enum SocketError: Error, CustomStringConvertible {
    /// SO_RCVTIMEO / SO_SNDTIMEO expired: the peer is neither sending nor closing.
    case timeout
    /// EOF in the middle of a PUT body.
    case closedMidFile
    case system(String, Int32)

    public var description: String {
        switch self {
        case .timeout: return "timed out"
        case .closedMidFile: return "peer closed mid-file - truncated transfer"
        case .system(let op, let code): return "\(op): \(errorString(code))"
        }
    }

    /// The peer is gone rather than slow — Python's BrokenPipeError /
    /// ConnectionResetError.
    public var isPeerGone: Bool {
        if case .system(_, let code) = self { return code == EPIPE || code == ECONNRESET }
        return false
    }
}

public enum ConnectFailure: Error {
    case timeout
    case refused
    case resolve(String)
    case unreachable(String)
}

public func errorString(_ code: Int32) -> String {
    String(cString: strerror(code))
}

public final class Socket: @unchecked Sendable {
    public private(set) var fd: Int32

    public init(fd: Int32) {
        self.fd = fd
        // A vanished peer should be an EPIPE we can report, not a SIGPIPE that
        // kills the process. (main() also ignores SIGPIPE; this is per-socket.)
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
    }

    deinit { close() }

    public func close() {
        if fd >= 0 {
            Darwin.close(fd)
            fd = -1
        }
    }

    /// Bound every later blocking recv/send. Python spells this sock.settimeout();
    /// 0 clears it.
    public func setTimeout(seconds: Int) {
        var tv = timeval(tv_sec: seconds, tv_usec: 0)
        let len = socklen_t(MemoryLayout<timeval>.size)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, len)
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, len)
    }

    public func sendAll(_ bytes: UnsafeRawBufferPointer) throws {
        var offset = 0
        while offset < bytes.count {
            let n = Darwin.send(fd, bytes.baseAddress! + offset, bytes.count - offset, 0)
            if n < 0 {
                let code = errno
                if code == EINTR { continue }
                if code == EAGAIN || code == EWOULDBLOCK { throw SocketError.timeout }
                throw SocketError.system("send", code)
            }
            offset += n
        }
    }

    public func sendAll(_ bytes: [UInt8]) throws {
        try bytes.withUnsafeBytes { try sendAll($0) }
    }

    /// One recv(). Returns 0 on EOF.
    public func recv(into buffer: UnsafeMutableRawBufferPointer) throws -> Int {
        while true {
            let n = Darwin.recv(fd, buffer.baseAddress, buffer.count, 0)
            if n >= 0 { return n }
            let code = errno
            if code == EINTR { continue }
            if code == EAGAIN || code == EWOULDBLOCK { throw SocketError.timeout }
            throw SocketError.system("recv", code)
        }
    }

    /// Exactly n bytes, or nil if the peer closed first.
    ///
    /// Grows as bytes arrive instead of allocating n up front: n comes off the
    /// wire, and preallocating it let a four-byte length prefix claim memory
    /// before the peer had sent (or authenticated) anything.
    public func recvExactly(_ n: Int) throws -> [UInt8]? {
        var out: [UInt8] = []
        out.reserveCapacity(min(n, 65536))
        var chunk = [UInt8](repeating: 0, count: min(max(n, 1), 65536))
        while out.count < n {
            let want = min(chunk.count, n - out.count)
            let got = try chunk.withUnsafeMutableBytes {
                try recv(into: UnsafeMutableRawBufferPointer(rebasing: $0[0..<want]))
            }
            if got == 0 { return nil }
            out.append(contentsOf: chunk[0..<got])
        }
        return out
    }

    /// Connect with a deadline. connect() ignores SO_SNDTIMEO, and a dropped SYN
    /// takes ~75 s to fail on macOS, so: non-blocking connect, then poll.
    public static func connect(host: String, port: Int, timeout seconds: Int) throws(ConnectFailure) -> Socket {
        var hints = addrinfo()
        hints.ai_family = AF_INET
        hints.ai_socktype = SOCK_STREAM
        var result: UnsafeMutablePointer<addrinfo>?
        let rc = getaddrinfo(host, String(port), &hints, &result)
        guard rc == 0, let info = result else {
            throw .resolve(String(cString: gai_strerror(rc)))
        }
        defer { freeaddrinfo(result) }

        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw .unreachable(errorString(errno)) }
        let sock = Socket(fd: fd)

        let flags = fcntl(fd, F_GETFL, 0)
        _ = fcntl(fd, F_SETFL, flags | O_NONBLOCK)

        if Darwin.connect(fd, info.pointee.ai_addr, info.pointee.ai_addrlen) != 0 {
            let code = errno
            guard code == EINPROGRESS else { throw classify(code) }

            var pfd = pollfd(fd: fd, events: Int16(POLLOUT), revents: 0)
            var ready: Int32
            repeat {
                ready = poll(&pfd, 1, Int32(seconds * 1000))
            } while ready < 0 && errno == EINTR
            if ready == 0 { throw .timeout }

            var soError: Int32 = 0
            var len = socklen_t(MemoryLayout<Int32>.size)
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len)
            if soError != 0 { throw classify(soError) }
        }

        _ = fcntl(fd, F_SETFL, flags)
        return sock
    }

    private static func classify(_ code: Int32) -> ConnectFailure {
        switch code {
        case ECONNREFUSED: return .refused
        case ETIMEDOUT: return .timeout
        default: return .unreachable(errorString(code))
        }
    }
}

public final class Listener: @unchecked Sendable {
    public let fd: Int32
    /// The port actually bound — differs from the one asked for when that was 0.
    public let port: Int

    /// host "" binds every interface (file_receiver); anything else must be a
    /// literal IPv4 address.
    public init(host: String, port: Int) throws {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw SocketError.system("socket", errno) }

        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))

        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = in_port_t(UInt16(port).bigEndian)
        if !host.isEmpty && inet_pton(AF_INET, host, &addr.sin_addr) != 1 {
            Darwin.close(fd)
            throw SocketError.system("bad bind address \(host)", EINVAL)
        }

        let bound = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bound == 0 else {
            let code = errno
            Darwin.close(fd)
            throw SocketError.system("bind \(host):\(port)", code)
        }
        guard listen(fd, 1) == 0 else {
            let code = errno
            Darwin.close(fd)
            throw SocketError.system("listen", code)
        }

        var actual = sockaddr_in()
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        _ = withUnsafeMutablePointer(to: &actual) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &len) }
        }
        self.fd = fd
        self.port = Int(UInt16(bigEndian: actual.sin_port))
    }

    deinit { Darwin.close(fd) }

    public func accept() throws -> (socket: Socket, host: String, port: Int) {
        while true {
            var peer = sockaddr_in()
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            let conn = withUnsafeMutablePointer(to: &peer) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.accept(fd, $0, &len) }
            }
            if conn < 0 {
                if errno == EINTR { continue }
                throw SocketError.system("accept", errno)
            }
            return (Socket(fd: conn), ipv4String(peer.sin_addr), Int(UInt16(bigEndian: peer.sin_port)))
        }
    }
}

func ipv4String(_ addr: in_addr) -> String {
    var addr = addr
    var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
    guard inet_ntop(AF_INET, &addr, &buf, socklen_t(INET_ADDRSTRLEN)) != nil else { return "?" }
    return buf.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
}
