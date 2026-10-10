// What a folder currently holds, and what has to change for the peer to match.
#pragma once

#include "json.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lfs {

struct Entry {
    long long size;
    double mtime;
};

class Manifest {
public:
    // std::map, not unordered_map: the wire form and the diff then come out in
    // a stable byte order, which makes two runs (and two implementations)
    // comparable line by line.
    std::map<std::string, Entry, std::less<>> files;

    // Symlinked directories are deliberately not followed (os.walk doesn't
    // either), but skipping them in silence means a user who symlinks a folder
    // into their sync directory sees nothing sync and is told nothing.
    std::vector<std::string> skipped_links;

    // false when at least one directory could not be read, so this is a
    // PARTIAL picture. Callers that delete based on the difference must not
    // treat the two the same: a file we cannot see looks exactly like a file
    // you deleted.
    bool complete = true;

    static Manifest scan(const std::string& root_dir);

    // The {"files": ...} value, spelled by hand so `size` stays an integer and
    // `mtime` keeps six decimals -- the diff's 1 ms window depends on the
    // second one surviving the round trip.
    std::string to_json() const;
    static std::optional<Manifest> from_json(const json::Value* files);
};

// A file counts as unchanged when size AND mtime match. The window exists only
// to absorb timestamp round-trip error, and that error is tiny: measured at
// under 1 microsecond. It used to be 2 SECONDS, which silently swallowed real
// edits -- change a file without changing its size, land the new mtime within
// 2 s of the peer's copy, and the diff called it unchanged forever, because the
// two mtimes never drift further apart. Not zero, because a filesystem that
// stores coarser timestamps would then resend every file on every run.
constexpr double MTIME_TOLERANCE = 0.001;

struct Plan {
    std::vector<std::string> to_put;    // we have it, the peer doesn't (or differs)
    std::vector<std::string> to_delete; // the peer has it, we don't
};

Plan diff_manifests(const Manifest& local, const Manifest& remote,
                    double tolerance = MTIME_TOLERANCE);

} // namespace lfs
