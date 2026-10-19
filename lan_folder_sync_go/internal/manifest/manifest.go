// Package manifest answers what a folder currently holds, and what has to
// change for the peer to match.
package manifest

import (
	"errors"
	"fmt"
	"io/fs"
	"math"
	"os"
	"slices"
	"strings"

	"lanfoldersync/internal/cli"
	"lanfoldersync/internal/jsonval"
)

// MtimeTolerance: a file counts as unchanged when size AND mtime match. The
// window exists only to absorb timestamp round-trip error, and that error is
// tiny -- measured at under a microsecond. framing.py uses 2 SECONDS, which
// silently swallows real edits: change a file without changing its size, land
// the new mtime within 2 s of the peer's copy, and the diff calls it unchanged
// forever, because the two mtimes never drift further apart. Not zero, because
// a filesystem that stores coarser timestamps would then resend every file on
// every run.
const MtimeTolerance = 0.001

type Entry struct {
	Size  int64
	Mtime float64
}

type Manifest struct {
	Files map[string]Entry

	// SkippedLinks: symlinked directories are deliberately not followed (os.walk
	// doesn't either), but skipping them in silence means a user who symlinks a
	// folder into their sync directory sees nothing sync and is told nothing.
	SkippedLinks []string

	// Complete is false when at least one directory could not be read, so this
	// is a PARTIAL picture. Callers that delete based on the difference must
	// not treat the two the same: a file we cannot see looks exactly like a
	// file you deleted.
	Complete bool
}

// Paths returns the keys in sorted order. Go randomises map iteration, so
// every caller that puts paths on the wire or on screen goes through this:
// the wire form and the diff then come out in a stable byte order, which makes
// two runs -- and two implementations -- comparable line by line. (C++ gets
// this from std::map; Python's dict order is insertion order, and nothing in
// the protocol cares.)
func (m *Manifest) Paths() []string {
	list := make([]string, 0, len(m.Files))
	for path := range m.Files {
		list = append(list, path)
	}
	slices.Sort(list)
	return list
}

// Scan walks rootDir and records every file by its path relative to rootDir.
//
// filepath.WalkDir is used rather than hand-rolled opendir/readdir, because
// unlike C++'s recursive_directory_iterator it offers exactly the third
// behaviour this needs: the error callback can record an unreadable directory
// AND return nil to keep walking. That silence is the hole os.walk(onerror=None)
// had, which let --delete remove files the walk could not see.
//
// WalkDir does not follow symlinks, which is what we want: it hands us the
// link itself, and we decide. Note it reports the ROOT's own error through the
// same callback, so a missing rootDir makes the walk partial rather than
// panicking.
func Scan(rootDir string) *Manifest {
	m := &Manifest{Files: map[string]Entry{}, Complete: true}

	walkErr := fs.WalkDir(os.DirFS(rootDir), ".", func(relPath string, entry fs.DirEntry, err error) error {
		if err != nil {
			// ENOENT is a benign race: something was deleted while we walked,
			// and it genuinely isn.t there any more. Anything else (a path past
			// PATH_MAX, a directory we lack permission to enter) means the entry
			// EXISTS and we simply cannot see it -- exactly the silent hole that
			// made this listing look complete when it wasn.t.
			//
			// The ROOT is the exception: a shared_dir that is missing or
			// unreadable is not a vanished entry, it is the whole picture being
			// absent, and reporting that as a complete listing of nothing is how
			// --delete gets talked into wiping a peer.
			if relPath == "." || !errors.Is(err, fs.ErrNotExist) {
				m.Complete = false
			}
			// Returning nil keeps the walk going past the hole. Returning the
			// error would abandon the rest of the tree, which is the other
			// wrong answer.
			return nil
		}
		if relPath == "." {
			return nil
		}

		if entry.IsDir() {
			return nil
		}

		if entry.Type()&fs.ModeSymlink != 0 {
			// A symlinked FILE still shows up, stat()ed through the link. A
			// symlinked directory does not: we choose not to follow it, and say
			// so rather than let the user wonder.
			target, err := os.Stat(cli.PathJoin(rootDir, relPath))
			switch {
			case err != nil:
				// A broken link: not a file we can send, and not a directory we
				// failed to read. Not an error -- but worth reporting.
				m.SkippedLinks = append(m.SkippedLinks, relPath)
			case target.IsDir():
				m.SkippedLinks = append(m.SkippedLinks, relPath)
			default:
				m.Files[relPath] = Entry{Size: target.Size(), Mtime: cli.Mtime(target)}
			}
			return nil
		}

		info, err := entry.Info()
		if err != nil {
			if !errors.Is(err, fs.ErrNotExist) {
				m.Complete = false
			}
			return nil
		}
		m.Files[relPath] = Entry{Size: info.Size(), Mtime: cli.Mtime(info)}
		return nil
	})

	// WalkDir only returns an error when a callback does, and ours never does
	// -- so this is belt and braces rather than a path we expect to take.
	if walkErr != nil {
		m.Complete = false
	}

	slices.Sort(m.SkippedLinks)
	return m
}

// ToJSON is the {"files": ...} value, spelled by hand so `size` stays an
// integer and `mtime` keeps six decimals. The diff's 1 ms window depends on
// the second one surviving the round trip, and json.Marshal would render the
// float in Go's shortest round-trip form instead.
func (m *Manifest) ToJSON() string {
	var out strings.Builder
	out.WriteByte('{')
	for i, path := range m.Paths() {
		if i > 0 {
			out.WriteString(", ")
		}
		entry := m.Files[path]
		fmt.Fprintf(&out, `%s: {"size": %d, "mtime": %.6f}`,
			jsonval.Escape(path), entry.Size, entry.Mtime)
	}
	out.WriteByte('}')
	return out.String()
}

// FromJSON reads the peer's listing. An entry whose size or mtime is missing
// or the wrong type is skipped rather than defaulted: a zero would make the
// diff think the peer has an empty copy and resend the file, which is wasteful
// but safe, while a default mtime could make it skip a file that differs.
func FromJSON(files any, present bool) (*Manifest, bool) {
	if !present {
		return nil, false
	}
	object, ok := jsonval.AsObject(files)
	if !ok {
		return nil, false
	}

	m := &Manifest{Files: map[string]Entry{}, Complete: true}
	for path, raw := range object {
		info, ok := jsonval.AsObject(raw)
		if !ok {
			continue
		}
		size, sizeOK := info.Number("size")
		mtime, mtimeOK := info.Number("mtime")
		if !sizeOK || !mtimeOK || !jsonval.Finite(size) || !jsonval.Finite(mtime) {
			continue
		}
		m.Files[path] = Entry{Size: int64(size), Mtime: mtime}
	}
	return m, true
}

// Plan is what the pusher decided.
type Plan struct {
	ToPut    []string // we have it, the peer doesn't (or it differs)
	ToDelete []string // the peer has it, we don't
}

func Diff(local, remote *Manifest, tolerance float64) Plan {
	var plan Plan

	for _, path := range local.Paths() {
		mine := local.Files[path]
		theirs, found := remote.Files[path]
		if !found {
			plan.ToPut = append(plan.ToPut, path) // the peer doesn't have it
			continue
		}
		if mine.Size != theirs.Size || math.Abs(mine.Mtime-theirs.Mtime) > tolerance {
			plan.ToPut = append(plan.ToPut, path)
		}
	}

	for _, path := range remote.Paths() {
		if _, found := local.Files[path]; !found {
			plan.ToDelete = append(plan.ToDelete, path) // we don't have it
		}
	}

	return plan
}
