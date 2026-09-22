// The passive side: authenticate, then do exactly what the peer asks — serve a
// manifest, accept a PUT, remove a DELETE. It never decides anything about the
// sync; the pusher is the brain.
//
// Lives in the library rather than in sync_server's main so the tests can run
// real sessions in-process, on an ephemeral port.
import Darwin
import Foundation

public enum ServerDefaults {
    /// The server handles one session at a time, so a peer that connects and
    /// then says nothing locks out everyone else. Bound the wait — generously,
    /// since a legitimate peer can be busy walking a large folder between frames.
    public static let sessionTimeout = 300
}

/// Serve one connection until the peer says BYE, errs, or goes quiet.
///
/// Nothing a peer sends can take the process down: every refusal is an ERROR
/// frame plus the end of *this* session. (Python needed a catch-all for this;
/// in Swift a trap can't be caught at all, so the code below simply mustn't
/// have any — every conversion of a peer-supplied number is range-checked first.)
public func serveSession(
    _ conn: Socket,
    sharedDir: String,
    token: String,
    idleTimeout: Int = ServerDefaults.sessionTimeout,
    log: (String) -> Void = { print($0) }
) {
    conn.setTimeout(seconds: idleTimeout)
    var authenticated = false

    // Refuse, then end the session. The ERROR is best-effort: the peer may
    // already be gone, and there's nothing more to do about that.
    func refuse(_ message: String) {
        try? sendError(conn, message)
    }

    do {
        while true {
            // Everything a client sends us is a short header; the big frame in
            // this protocol only ever travels the other way.
            let bytes: [UInt8]
            do {
                guard let frame = try recvMsg(conn, maxBytes: Limits.maxControlFrame) else { return }
                bytes = frame
            } catch let e as FrameTooLarge {
                log("Refused oversized frame (\(e.length) bytes, cap \(Limits.maxControlFrame))")
                return refuse("frame too large")
            }

            let header: [String: Any]
            do throws(HeaderError) {
                header = try decodeHeader(bytes)
            } catch {
                switch error {
                case .notUTF8JSON: return refuse("malformed header: not valid UTF-8 JSON")
                // "123" is valid JSON but not a header; blaming UTF-8 for it
                // would send you hunting in the wrong place.
                case .notObject: return refuse("malformed header: not a JSON object")
                }
            }

            let op = header["op"] as? String

            if !authenticated {
                guard op == "HELLO" else { return refuse("auth required: send HELLO first") }
                // Compare bytes: String == would accept a differently-normalised
                // Unicode spelling of the token.
                guard let got = header["token"] as? String, Array(got.utf8) == Array(token.utf8) else {
                    return refuse("bad token")
                }
                authenticated = true
                try sendJSON(conn, ["op": "OK"])
                log("     HELLO ok, session authenticated")
                continue
            }

            switch op {
            case "MANIFEST":
                // Partial here is far less dangerous than partial on the pusher
                // — files we fail to list just get re-sent, never deleted — but
                // say so, it's the same underlying problem.
                let manifest = Manifest(scanning: sharedDir)
                try sendJSON(conn, ["op": "MANIFEST", "files": manifest.jsonObject])
                if !manifest.isComplete {
                    log("Sent manifest (\(manifest.files.count) files) - WARNING: some directories "
                        + "under \(sharedDir) could not be read; the list is incomplete")
                } else if !manifest.skippedLinks.isEmpty {
                    let n = manifest.skippedLinks.count
                    log("Sent manifest (\(manifest.files.count) files, \(n) symlink\(n == 1 ? "" : "s") skipped)")
                } else {
                    log("Sent manifest (\(manifest.files.count) files)")
                }

            case "PUT":
                do {
                    let relPath = try recvFileBody(conn, destDir: sharedDir, header: header)
                    log("    received \(relPath)")
                } catch PutError.missingField(let field) {
                    return refuse("PUT header missing field: '\(field)'") // body size unknown
                } catch PutError.outOfRange(let field) {
                    return refuse("PUT header has an out-of-range '\(field)'")
                } catch PutError.unsafePath(let path) {
                    return refuse("unsafe path refused: \(path)") // body still queued
                }

            case "BYE":
                log("Peer said BYE")
                return

            case "DELETE":
                let raw = header["path"]
                if isMissing(raw) { return refuse("DELETE missing 'path'") }

                // One shared guard for PUT and DELETE.
                guard let relPath = raw as? String, let target = safePath(base: sharedDir, relPath: relPath) else {
                    return refuse("unsafe path refused: \(pyRepr(raw))")
                }
                if unlink(target) == 0 {
                    log("    deleted \(relPath)")
                } else if errno == ENOENT {
                    log("    already gone \(relPath)") // delete is idempotent
                } else {
                    throw SystemError("can't delete \(relPath)")
                }

            default:
                return refuse("unknown op: \(pyRepr(header["op"]))")
            }
        }
    } catch SocketError.timeout {
        log("Session timed out: peer idle for \(idleTimeout)s, dropping it")
    } catch {
        log("Session error: \(error)")
    }
}
