import Foundation
import SyncCore
import Testing

@Suite("Manifest walk")
struct ManifestTests {
    @Test("symlinked dirs and broken links are skipped and reported; file links are synced")
    func symlinks() throws {
        let root = makeTempDir()
        let elsewhere = makeTempDir()
        defer { removeTree(root); removeTree(elsewhere) }

        writeFile(root + "/plain.txt", "plain\n")
        writeFile(elsewhere + "/buried.txt", "not followed\n")
        #expect(symlink(elsewhere, root + "/linked_dir") == 0)
        #expect(symlink("/nonexistent/nowhere", root + "/broken_link") == 0)
        #expect(symlink("plain.txt", root + "/file_link") == 0)

        let manifest = Manifest(scanning: root)
        #expect(manifest.isComplete)
        #expect(Set(manifest.files.keys) == ["plain.txt", "file_link"])
        #expect(manifest.skippedLinks.sorted() == ["broken_link", "linked_dir"])
    }

    @Test("an unreadable directory makes the walk partial, not silently short")
    func partialWalk() throws {
        let root = makeTempDir()
        defer { chmod(root + "/locked", 0o755); removeTree(root) }
        writeFile(root + "/visible.txt", "seen\n")
        writeFile(root + "/locked/hidden.txt", "exists, can't be seen\n")
        #expect(chmod(root + "/locked", 0) == 0)

        let manifest = Manifest(scanning: root)
        #expect(!manifest.isComplete)
        #expect(manifest.errors.first?.contains("locked") == true)

        chmod(root + "/locked", 0o755)
        #expect(Manifest(scanning: root).isComplete)
    }

    @Test func missingRootIsPartial() {
        #expect(!Manifest(scanning: "/nonexistent-\(UUID().uuidString)").isComplete)
    }

    @Test func survivesJSONRoundTrip() throws {
        let root = makeTempDir()
        defer { removeTree(root) }
        writeFile(root + "/a/b.txt", "bee\n")
        let manifest = Manifest(scanning: root)

        let data = encodeJSON(["files": manifest.jsonObject])
        let decoded = try #require(Manifest(json: try decodeHeader(Array(data))["files"]))
        #expect(decoded.files == manifest.files)
    }
}

@Suite("Diff")
struct DiffTests {
    func manifest(_ files: [String: FileInfo]) -> Manifest {
        var m = Manifest()
        m.files = files
        return m
    }

    @Test func putsNewAndChangedDeletesExtras() {
        let local = manifest(["same": FileInfo(size: 1, mtime: 100), "new": FileInfo(size: 1, mtime: 100),
                              "bigger": FileInfo(size: 2, mtime: 100)])
        let remote = manifest(["same": FileInfo(size: 1, mtime: 100), "bigger": FileInfo(size: 1, mtime: 100),
                               "extra": FileInfo(size: 1, mtime: 100)])
        let (toPut, toDelete) = diffManifests(local: local, remote: remote)
        #expect(toPut == ["bigger", "new"])
        #expect(toDelete == ["extra"])
    }

    @Test("a same-size edit 0.5 s later is seen (the 2 s window hid it)")
    func narrowWindow() {
        let local = manifest(["f": FileInfo(size: 10, mtime: 1000.5)])
        let remote = manifest(["f": FileInfo(size: 10, mtime: 1000.0)])
        #expect(diffManifests(local: local, remote: remote).toPut == ["f"])
    }

    @Test("float round-trip noise is not a change")
    func roundTripNoise() {
        let local = manifest(["f": FileInfo(size: 10, mtime: 1_700_000_000.123456)])
        let remote = manifest(["f": FileInfo(size: 10, mtime: 1_700_000_000.1234561)])
        #expect(diffManifests(local: local, remote: remote).toPut.isEmpty)
    }
}

@Suite("Paths")
struct PathTests {
    @Test(arguments: ["a.txt", "a/b/c.txt", "a/./b", ".hidden", "..dots", "a..b/c"])
    func lexicallySafe(path: String) {
        #expect(pathIsLexicallySafe(path))
    }

    @Test(arguments: ["", "/etc/passwd", "..", "../x", "a/../b", "a/.."])
    func lexicallyUnsafe(path: String) {
        #expect(!pathIsLexicallySafe(path))
    }

    @Test func safePathResolvesInside() throws {
        let base = makeTempDir()
        defer { removeTree(base) }
        writeFile(base + "/sub/f.txt", "x")
        let resolved = try #require(safePath(base: base, relPath: "sub/f.txt"))
        #expect(resolved == realPath(base) + "/sub/f.txt")
        #expect(safePath(base: base, relPath: "new/not-yet.txt") != nil, "a PUT names files that don't exist yet")
    }

    @Test func safePathAllowsInternalSymlink() throws {
        let base = makeTempDir()
        defer { removeTree(base) }
        writeFile(base + "/real/f.txt", "x")
        #expect(symlink(base + "/real", base + "/alias") == 0)
        #expect(safePath(base: base, relPath: "alias/f.txt") == realPath(base) + "/real/f.txt")
    }
}

@Suite("config.json validation")
struct ConfigTests {
    func parse(_ json: String) throws(Config.ParseError) -> Config {
        try Config.parse(Data(json.utf8), path: "config.json")
    }

    @Test(arguments: [
        (#"{"token": 12345}"#, "must be a string"),
        (#"{"shared_dir": "x"}"#, "missing \"token\""),
        (#"{"token":"t","shared_dir":["/Users/me"]}"#, "must be a string"),
        (#"{"token":"t","peer":"nope"}"#, "\"peer\" must be an object"),
        (#"{"token":"t","peer":{"host":42}}"#, "\"peer.host\" must be a string"),
        (#"{"token":"t","peer":{"port":"9999"}}"#, "must be a number, not in quotes"),
        (#"{"token":"t","peer":{"port":true}}"#, "must be a number, not in quotes"),
        (#"{"token":"t","peer":{"port":0}}"#, "must be 1-65535"),
        ("hello", "is not valid JSON"),
    ])
    func mistypedValueNamesItself(json: String, message: String) {
        #expect {
            try parse(json)
        } throws: { error in
            (error as? Config.ParseError)?.message.contains(message) == true
        }
    }

    @Test func validConfigLoads() throws {
        let config = try parse(#"{"token":"t","shared_dir":"/x","peer":{"host":"10.0.0.2","port":9999}}"#)
        #expect(config.token == "t")
        #expect(config.sharedDir == "/x")
        #expect(config.peerHost == "10.0.0.2")
        #expect(config.peerPort == 9999)
    }

    @Test("absent optional keys are not the same as mistyped")
    func absentKeysDefault() throws {
        let config = try parse(#"{"token":"t","peer":{"host":null}}"#)
        #expect(config.sharedDir == nil && config.peerHost == nil && config.peerPort == nil)
    }
}
