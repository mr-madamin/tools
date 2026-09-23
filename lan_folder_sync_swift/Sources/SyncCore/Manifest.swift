import Darwin
import Foundation

public struct FileInfo: Equatable, Sendable {
    public var size: Int64
    public var mtime: Double

    public init(size: Int64, mtime: Double) {
        self.size = size
        self.mtime = mtime
    }
}

/// { relative_path: {size, mtime} } for every file under a root, keyed by the
/// path relative to that root.
public struct Manifest {
    public var files: [String: FileInfo] = [:]

    /// Symlinked directories are deliberately not followed (os.walk doesn't
    /// either), but skipping them in silence means a user who symlinks a folder
    /// into the sync directory sees nothing sync and is told nothing. Broken
    /// links land here too.
    public var skippedLinks: [String] = []

    /// Everything the walk could not read. Non-empty means the manifest is
    /// PARTIAL — and to the diff, a file we couldn't see is indistinguishable
    /// from one the user deleted, so --delete would remove it from the peer.
    public var errors: [String] = []

    public var isComplete: Bool { errors.isEmpty }

    public init() {}

    /// Walk root and list every file.
    ///
    /// opendir/readdir rather than FileManager.enumerator: the enumerator skips
    /// what it can't read unless you remember to pass an error handler — the
    /// same silent hole os.walk(onerror=None) had in framing.py.
    public init(scanning root: String) {
        walk(root, prefix: "")
    }

    private mutating func walk(_ directory: String, prefix: String) {
        guard let dir = opendir(directory) else {
            errors.append(SystemError("can't read \(directory)").description)
            return
        }
        defer { closedir(dir) }

        while true {
            errno = 0
            guard let entry = readdir(dir) else {
                if errno != 0 { errors.append(SystemError("can't list \(directory)").description) }
                return
            }
            let name = withUnsafeBytes(of: entry.pointee.d_name) {
                String(cString: $0.bindMemory(to: CChar.self).baseAddress!)
            }
            if name == "." || name == ".." { continue }

            let child = joinPath(directory, name)
            let rel = prefix.isEmpty ? name : prefix + "/" + name

            var lst = stat()
            guard lstat(child, &lst) == 0 else {
                // ENOENT is a benign race: it really is gone. Anything else (a
                // path past PATH_MAX, a permissions hole) means the entry
                // EXISTS and we simply cannot see it.
                if errno != ENOENT { errors.append(SystemError("can't stat \(child)").description) }
                continue
            }

            switch lst.st_mode & S_IFMT {
            case S_IFDIR:
                walk(child, prefix: rel)
            case S_IFLNK:
                // A symlinked file is still synced, stat()ed through the link.
                var st = stat()
                if stat(child, &st) != 0 || (st.st_mode & S_IFMT) == S_IFDIR {
                    skippedLinks.append(rel)
                } else {
                    files[rel] = FileInfo(size: Int64(st.st_size), mtime: mtimeSeconds(st))
                }
            default:
                files[rel] = FileInfo(size: Int64(lst.st_size), mtime: mtimeSeconds(lst))
            }
        }
    }

    /// The value of a MANIFEST reply's "files" key.
    public var jsonObject: [String: Any] {
        files.mapValues { ["size": $0.size, "mtime": $0.mtime] }
    }

    /// Parse the peer's "files" object. Entries without usable numbers are
    /// skipped, as in the C port.
    public init?(json: Any?) {
        guard let object = json as? [String: Any] else { return nil }
        for (path, value) in object {
            guard let info = value as? [String: Any],
                  let size = jsonNumber(info["size"]),
                  let mtime = jsonNumber(info["mtime"]),
                  size.isFinite, mtime.isFinite
            else { continue }
            // Int64(Double) traps when out of range, and this number is the peer's.
            let clamped = min(max(size, -9e18), 9e18)
            files[path] = FileInfo(size: Int64(clamped), mtime: mtime)
        }
    }
}

/// Compare local vs remote. toPut = missing on remote, or size/mtime differ.
/// toDelete = on remote but not local (deletion candidates). Both sorted, so
/// the plan prints in a stable order.
public func diffManifests(local: Manifest, remote: Manifest, tolerance: Double = Limits.mtimeTolerance)
    -> (toPut: [String], toDelete: [String])
{
    let toPut = local.files.compactMap { path, info -> String? in
        guard let theirs = remote.files[path] else { return path }
        let changed = info.size != theirs.size || abs(info.mtime - theirs.mtime) > tolerance
        return changed ? path : nil
    }
    let toDelete = remote.files.keys.filter { local.files[$0] == nil }
    return (toPut.sorted(), toDelete.sorted())
}
