import Foundation

/// Python's sys.exit("message"): the message to stderr, exit status 1.
public func die(_ message: String) -> Never {
    fputs(message + "\n", stderr)
    exit(1)
}

/// Line-buffer stdout: redirected to a file or a service manager, stdio
/// block-buffers and the log stays empty until the process exits. Also ignore
/// SIGPIPE, so a vanished peer is an error we can report rather than a death.
public func setUpProcess() {
    setvbuf(stdout, nil, _IOLBF, 0)
    signal(SIGPIPE, SIG_IGN)
}

public struct SystemError: Error, CustomStringConvertible {
    public let what: String
    public let code: Int32

    public init(_ what: String, _ code: Int32 = errno) {
        self.what = what
        self.code = code
    }

    public var description: String { "\(what): \(errorString(code))" }
}

// MARK: - argv

public struct Arguments {
    public let flags: Set<String>
    public let positional: [String]

    /// Split argv into --flags and positionals, and exit listing any unknown
    /// flag. A silently ignored typo like --dryrun is how a preview becomes a
    /// real push, and --lann would serve loopback while you believed it was the
    /// LAN.
    public static func parse(_ argv: [String], known: Set<String>, usage: String) -> Arguments {
        let flags = Set(argv.filter { $0.hasPrefix("--") })
        let unknown = flags.subtracting(known)
        if !unknown.isEmpty {
            die("unknown flag(s): \(unknown.sorted().joined(separator: " "))\n\(usage)")
        }
        return Arguments(flags: flags, positional: argv.filter { !$0.hasPrefix("--") })
    }
}

/// A TCP port, or exit explaining why not.
public func parsePort(_ text: String, where context: String) -> Int {
    guard let value = Int(text) else { die("\(context): '\(text)' is not a number") }
    guard (1...65535).contains(value) else { die("\(context): port \(text) is outside 1-65535") }
    return value
}

// MARK: - JSON

public enum HeaderError: Error {
    case notUTF8JSON
    case notObject
}

/// Decode one frame's payload as a JSON object.
///
/// JSONSerialization refuses deep nesting with an ordinary error (Python's
/// parser raised RecursionError, which escaped the server's handlers; the C
/// parser blew its stack), and refuses 1e999 / NaN outright, so neither needs
/// special handling here. Fragments are allowed on purpose: "123" parses, and
/// then gets the more useful "not a JSON object" message.
public func decodeHeader(_ bytes: [UInt8]) throws(HeaderError) -> [String: Any] {
    // JSONSerialization sniffs UTF-16/32 too; the protocol says UTF-8.
    guard String(bytes: bytes, encoding: .utf8) != nil,
          let value = try? JSONSerialization.jsonObject(with: Data(bytes), options: [.fragmentsAllowed])
    else { throw .notUTF8JSON }
    guard let object = value as? [String: Any] else { throw .notObject }
    return object
}

public func encodeJSON(_ object: [String: Any]) -> Data {
    // Only ever called with strings and finite numbers, which cannot fail.
    try! JSONSerialization.data(withJSONObject: object, options: [.sortedKeys, .withoutEscapingSlashes])
}

/// A JSON number that isn't a bool. NSNumber bridges `true` as 1, so without
/// this check "size": true would pass as a one-byte file.
func jsonNumber(_ value: Any?) -> Double? {
    guard let number = value as? NSNumber,
          CFGetTypeID(number) != CFBooleanGetTypeID()
    else { return nil }
    return number.doubleValue
}

/// Absent and JSON null both count as missing, as with Python's dict.get().
func isMissing(_ value: Any?) -> Bool {
    value == nil || value is NSNull
}

/// How Python's !r renders a value in an error message, closely enough that the
/// three servers' errors read the same.
func pyRepr(_ value: Any?) -> String {
    if isMissing(value) { return "None" }
    if let s = value as? String { return "'\(s)'" }
    return "<non-string>"
}
