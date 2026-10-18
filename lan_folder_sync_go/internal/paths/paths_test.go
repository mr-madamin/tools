package paths_test

import (
	"os"
	"path/filepath"
	"testing"

	"lanfoldersync/internal/paths"
)

func TestIsLexicallySafe(t *testing.T) {
	safe := []string{
		"file.txt",
		"sub/file.txt",
		"sub/deeper/file.txt",
		"..hidden.txt",     // starts with dots, is not ".."
		"sub/..hidden.txt", //
		"a..b/file.txt",    // contains dots, is not a traversal
		"./file.txt",       // redundant, not dangerous
	}
	for _, relPath := range safe {
		if !paths.IsLexicallySafe(relPath) {
			t.Errorf("%q should be lexically safe", relPath)
		}
	}

	unsafe := []string{
		"",
		"/etc/hosts",
		"/",
		"..",
		"../file.txt",
		"sub/../../file.txt",
		"sub/..",
		"a/../../b",
	}
	for _, relPath := range unsafe {
		if paths.IsLexicallySafe(relPath) {
			t.Errorf("%q should NOT be lexically safe", relPath)
		}
	}
}

func TestSafeResolvesLegitimatePaths(t *testing.T) {
	base := t.TempDir()
	if err := os.MkdirAll(filepath.Join(base, "sub/deeper"), 0o777); err != nil {
		t.Fatal(err)
	}

	// A path that does not exist yet is the normal case for a PUT, and must
	// resolve. This is what filepath.EvalSymlinks refuses to do.
	got := paths.Safe(base, "sub/deeper/not-yet.txt")
	if got == "" {
		t.Fatal("a PUT to a path that doesn't exist yet was refused")
	}
	real, err := filepath.EvalSymlinks(base)
	if err != nil {
		t.Fatal(err)
	}
	if want := filepath.Join(real, "sub/deeper/not-yet.txt"); got != want {
		t.Fatalf("got %q, want %q", got, want)
	}
}

func TestSafeRefusesEscapes(t *testing.T) {
	parent := t.TempDir()
	base := filepath.Join(parent, "shared")
	if err := os.MkdirAll(base, 0o777); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(parent, "victim.txt"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}

	// The escape that "relative and ..-free" does not catch: a symlink
	// pointing out of the tree. This is why the guard walks component by
	// component instead of resolving once at the end.
	if err := os.Symlink(parent, filepath.Join(base, "out")); err != nil {
		t.Fatal(err)
	}
	// A link to a file outside, too.
	if err := os.Symlink(filepath.Join(parent, "victim.txt"),
		filepath.Join(base, "link-to-victim")); err != nil {
		t.Fatal(err)
	}
	// And a broken one, which is refused rather than guessed at.
	if err := os.Symlink(filepath.Join(parent, "gone.txt"),
		filepath.Join(base, "broken")); err != nil {
		t.Fatal(err)
	}

	escapes := []string{
		"",
		".",
		"..",
		"../victim.txt",
		"sub/../../victim.txt",
		"/etc/hosts",
		"out/victim.txt",    // through the directory symlink
		"out/shared/ok.txt", // out and back in: still left the tree on the way
		"link-to-victim",
		"broken",
	}
	for _, relPath := range escapes {
		if got := paths.Safe(base, relPath); got != "" {
			t.Errorf("Safe(%q) = %q, expected a refusal", relPath, got)
		}
	}
}

// A path naming the shared folder itself is refused: "." resolves to the root,
// and a DELETE of the root is not a file operation. The guard this replaced
// resolved the whole path at once and let target == base through.
func TestSafeRefusesTheRootItself(t *testing.T) {
	base := t.TempDir()
	for _, relPath := range []string{".", "./", "./."} {
		if got := paths.Safe(base, relPath); got != "" {
			t.Errorf("Safe(%q) = %q, expected a refusal", relPath, got)
		}
	}
}

// Byte comparison, deliberately. Two filenames that are canonically
// equivalent but byte-different are two different files to the filesystem, and
// the guard must agree with the filesystem rather than with Unicode. Go's
// string == is already a byte compare; this test is what stops a later
// "normalise the path" change from quietly breaking it.
func TestSafeComparesBytesNotUnicode(t *testing.T) {
	base := t.TempDir()

	const nfc = "café"  // é as one rune
	const nfd = "café" // e + combining acute

	if err := os.MkdirAll(filepath.Join(base, nfc), 0o777); err != nil {
		t.Fatal(err)
	}

	// Both spellings are inside the tree, so both must be allowed -- the point
	// is that resolution does not fold one into the other or reject either.
	for _, spelling := range []string{nfc + "/file.txt", nfd + "/file.txt"} {
		if got := paths.Safe(base, spelling); got == "" {
			t.Errorf("Safe(%q) was refused; the guard should confine, not normalise", spelling)
		}
	}
}

// A symlink that stays inside the tree is fine: the guard confines, it does
// not ban links.
func TestSafeAllowsLinksThatStayInside(t *testing.T) {
	base := t.TempDir()
	if err := os.MkdirAll(filepath.Join(base, "real"), 0o777); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(filepath.Join(base, "real"), filepath.Join(base, "alias")); err != nil {
		t.Fatal(err)
	}

	if got := paths.Safe(base, "alias/file.txt"); got == "" {
		t.Fatal("a symlink that stays inside the tree was refused")
	}
}
