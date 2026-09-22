// Path plumbing, and the guard that keeps every PUT and DELETE inside shared_dir.
//
// A note on Swift strings: String's == and hasPrefix compare by Unicode
// canonical equivalence and grapheme clusters, while the kernel (and Python and
// C) compare bytes. A containment check is exactly the place where "equal
// enough" is wrong, so everything here that decides *where a path lands* works
// on the UTF-8 bytes instead.
import Darwin
import Foundation

/// os.path.join, for the cases this project uses.
public func joinPath(_ a: String, _ b: String) -> String {
    if b.hasPrefix("/") || a.isEmpty { return b }
    return a.hasSuffix("/") ? a + b : a + "/" + b
}

/// os.path.dirname.
public func dirName(_ path: String) -> String {
    guard let slash = path.utf8.lastIndex(of: UInt8(ascii: "/")) else { return "" }
    let head = String(path[..<slash])
    return head.isEmpty ? "/" : head
}

/// os.path.abspath — lexical only; for messages.
public func absolutePath(_ path: String) -> String {
    URL(fileURLWithPath: path).standardized.path
}

/// os.path.realpath. libc's realpath() needs the path to exist; when it
/// doesn't, fall back to the lexical absolute path.
public func realPath(_ path: String) -> String {
    guard let resolved = realpath(path, nil) else { return absolutePath(path) }
    defer { free(resolved) }
    return String(cString: resolved)
}

public func isDirectory(_ path: String) -> Bool {
    var st = stat()
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR
}

private func isSymlink(_ path: String) -> Bool {
    var st = stat()
    return lstat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFLNK
}

private func exists(_ path: String) -> Bool {
    var st = stat()
    return stat(path, &st) == 0
}

/// The '/'-separated components, split on the byte, as the kernel splits them.
private func components(_ path: String) -> [Substring.UTF8View.SubSequence] {
    path.utf8.split(separator: UInt8(ascii: "/"), omittingEmptySubsequences: false)
}

private let dotDot = Array("..".utf8)

/// No absolute paths, no ".." component. Cheap, and runs before anything
/// touches the disk — creating the parent directories of "../evil/x" would make
/// directories outside dest_dir before any resolution got a look in.
public func pathIsLexicallySafe(_ relPath: String) -> Bool {
    if relPath.isEmpty || relPath.hasPrefix("/") { return false }
    return !components(relPath).contains { $0.elementsEqual(dotDot) }
}

/// True if `path` is `base` itself or somewhere under it — by bytes.
private func isInside(_ path: String, _ base: String) -> Bool {
    path.utf8.elementsEqual(base.utf8) || path.utf8.starts(with: (base + "/").utf8)
}

/// Resolve relPath under base, or nil if it would escape.
///
/// realpath() on the whole path only works for a file that already exists, and
/// a PUT usually names one that doesn't. So walk a component at a time, resolve
/// any symlink as we meet it, and re-check containment each time — the one
/// escape that "..-free and relative" still leaves open is a symlink pointing
/// out of the tree.
public func safePath(base: String, relPath: String) -> String? {
    let baseReal = realPath(base)
    guard pathIsLexicallySafe(relPath) else { return nil }

    var current = baseReal
    for component in components(relPath) {
        let name = String(Substring(component))
        if name.isEmpty || name == "." { continue }
        var next = joinPath(current, name)
        if isSymlink(next) {
            guard exists(next) else { return nil } // broken link — refuse it
            next = realPath(next)
        }
        guard isInside(next, baseReal) else { return nil }
        current = next
    }

    // The path named the root itself.
    if current.utf8.elementsEqual(baseReal.utf8) { return nil }
    return current
}
