// Package paths is the two-stage confinement guard every incoming path goes
// through, for PUT and for DELETE alike.
package paths

import (
	"os"
	"path/filepath"
	"slices"
	"strings"

	"lanfoldersync/internal/cli"
)

// IsLexicallySafe is a cheap first pass: no absolute paths, no ".."
// component. It runs before we create any directories, so a traversal never
// gets as far as mkdir.
func IsLexicallySafe(relPath string) bool {
	if relPath == "" || strings.HasPrefix(relPath, "/") {
		return false
	}
	return !slices.Contains(strings.Split(relPath, "/"), "..")
}

// Safe resolves relPath under base and returns "" if it would land outside.
//
// ".." and absolute paths are gone by the time we get here, so the one
// remaining escape is a symlink: walk the path a component at a time and, each
// time a component *is* a link, resolve it and re-check containment.
//
// filepath.EvalSymlinks looks like this function and is not a substitute for
// it. It refuses a path that does not exist yet, which is the normal case for
// a PUT, and it resolves the whole path in one go -- so it answers "where does
// this end up" but never "did it leave the tree on the way", which is the
// question when an intermediate component is a link out and back. The same
// trap as C++'s weakly_canonical() and os.path.realpath() on a missing path.
//
// filepath.Clean is likewise not usable here: it folds ".." lexically, which
// would resolve the traversal rather than preserve it for stage one to refuse.
func Safe(base, relPath string) string {
	// Resolve the base itself once. The containment test is a prefix test
	// against the REAL root, so a symlinked shared_dir doesn't fail everything.
	root, err := filepath.EvalSymlinks(base)
	if err != nil {
		return ""
	}
	if !IsLexicallySafe(relPath) {
		return ""
	}

	current := root
	for _, component := range strings.Split(relPath, "/") {
		if component == "" || component == "." {
			continue
		}

		next := cli.PathJoin(current, component)

		// Lstat, not Stat: we need to know whether this component IS a link,
		// not what it points at.
		if info, err := os.Lstat(next); err == nil && info.Mode()&os.ModeSymlink != 0 {
			resolved, err := filepath.EvalSymlinks(next)
			if err != nil {
				return "" // broken link -- refuse it
			}
			next = resolved
		}

		// Byte comparison, which is what the filesystem means. Go's string ==
		// is a byte compare by default, so unlike Swift (whose String ==
		// is Unicode canonical equivalence, making "é" equal "é") there
		// is nothing to opt out of here. A test pins it so a later "cleanup"
		// can't introduce a normalising comparison.
		if next != root && !strings.HasPrefix(next, root+"/") {
			return ""
		}

		current = next
	}

	if current == root {
		return "" // the path named the root itself
	}
	return current
}
