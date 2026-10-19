package manifest_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"lanfoldersync/internal/jsonval"
	"lanfoldersync/internal/manifest"
)

func write(t *testing.T, path, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o777); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestScanListsFilesByRelativePath(t *testing.T) {
	dir := t.TempDir()
	write(t, filepath.Join(dir, "top.txt"), "one")
	write(t, filepath.Join(dir, "sub/nested.txt"), "two!")
	write(t, filepath.Join(dir, "sub/deeper/leaf.txt"), "three")

	m := manifest.Scan(dir)
	if !m.Complete {
		t.Fatal("a readable tree was reported as partial")
	}

	want := map[string]int64{
		"top.txt":             3,
		"sub/nested.txt":      4,
		"sub/deeper/leaf.txt": 5,
	}
	if len(m.Files) != len(want) {
		t.Fatalf("got %d files, want %d: %v", len(m.Files), len(want), m.Paths())
	}
	for path, size := range want {
		entry, found := m.Files[path]
		if !found {
			t.Fatalf("%s missing from the manifest: %v", path, m.Paths())
		}
		if entry.Size != size {
			t.Fatalf("%s: size %d, want %d", path, entry.Size, size)
		}
		// Separators are forward slashes, as the wire format requires. On
		// macOS that is also what the OS uses, so the check is cheap
		// insurance rather than a conversion.
		if strings.Contains(path, "\\") {
			t.Fatalf("%s: wrong separator", path)
		}
	}
}

func TestScanIncludesSymlinkedFilesAndSkipsSymlinkedDirectories(t *testing.T) {
	dir := t.TempDir()
	write(t, filepath.Join(dir, "real.txt"), "content")

	outside := t.TempDir()
	write(t, filepath.Join(outside, "elsewhere.txt"), "far away")
	write(t, filepath.Join(outside, "folder/inside.txt"), "not followed")

	// A symlinked FILE is sent, stat()ed through the link.
	if err := os.Symlink(filepath.Join(outside, "elsewhere.txt"),
		filepath.Join(dir, "link-to-file.txt")); err != nil {
		t.Fatal(err)
	}
	// A symlinked DIRECTORY is not followed -- but it is reported.
	if err := os.Symlink(filepath.Join(outside, "folder"),
		filepath.Join(dir, "link-to-dir")); err != nil {
		t.Fatal(err)
	}
	// A broken link is neither.
	if err := os.Symlink(filepath.Join(outside, "gone.txt"),
		filepath.Join(dir, "broken-link")); err != nil {
		t.Fatal(err)
	}

	m := manifest.Scan(dir)

	if _, found := m.Files["link-to-file.txt"]; !found {
		t.Fatalf("a symlinked file was skipped: %v", m.Paths())
	}
	if m.Files["link-to-file.txt"].Size != int64(len("far away")) {
		t.Fatal("a symlinked file was stat'ed as the link, not the target")
	}
	for _, unwanted := range []string{"link-to-dir", "link-to-dir/inside.txt", "broken-link"} {
		if _, found := m.Files[unwanted]; found {
			t.Fatalf("%s should not be in the manifest", unwanted)
		}
	}

	// Skipping in silence is the bug: a user who symlinks a folder in here has
	// to be told why it never syncs.
	if len(m.SkippedLinks) != 2 {
		t.Fatalf("expected 2 skipped links, got %v", m.SkippedLinks)
	}
	if !m.Complete {
		t.Fatal("symlinks are not a reason to call the listing partial")
	}
}

// The important one. A directory we cannot read is NOT a directory with
// nothing in it, and the difference decides whether --delete is allowed to
// run: a file the walk could not see looks exactly like a file you deleted.
// os.walk(onerror=None) swallowed this, and a chmod 000 on a subdirectory was
// enough to make --delete wipe its contents from the peer.
func TestUnreadableDirectoryMakesTheScanPartialButKeepsWalking(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("running as root: permissions are not enforced")
	}

	dir := t.TempDir()
	write(t, filepath.Join(dir, "visible.txt"), "seen")
	write(t, filepath.Join(dir, "locked/hidden.txt"), "unseeable")
	write(t, filepath.Join(dir, "zzz-after/also-visible.txt"), "also seen")

	locked := filepath.Join(dir, "locked")
	if err := os.Chmod(locked, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.Chmod(locked, 0o755) })

	m := manifest.Scan(dir)

	if m.Complete {
		t.Fatal("an unreadable directory did not make the listing partial")
	}
	if _, found := m.Files["locked/hidden.txt"]; found {
		t.Fatal("the unreadable file should not be listed")
	}
	// Keep going AND report: the walk must not stop at the hole. "zzz-after"
	// sorts after "locked", so it is only found if the walk continued.
	if _, found := m.Files["zzz-after/also-visible.txt"]; !found {
		t.Fatalf("the walk abandoned the tree at the first hole: %v", m.Paths())
	}
	if _, found := m.Files["visible.txt"]; !found {
		t.Fatalf("visible.txt went missing: %v", m.Paths())
	}
}

func TestScanOfAMissingDirectoryIsPartialNotAPanic(t *testing.T) {
	m := manifest.Scan(filepath.Join(t.TempDir(), "does-not-exist"))
	if m.Complete {
		t.Fatal("a missing root should make the listing partial")
	}
	if len(m.Files) != 0 {
		t.Fatalf("expected no files, got %v", m.Paths())
	}
}

// ---- the diff -------------------------------------------------------------

func TestDiffPutsWhatIsMissingOrDifferent(t *testing.T) {
	local := &manifest.Manifest{Files: map[string]manifest.Entry{
		"same.txt":       {Size: 10, Mtime: 1000},
		"bigger.txt":     {Size: 20, Mtime: 1000},
		"newer.txt":      {Size: 10, Mtime: 1000.5},
		"only-local.txt": {Size: 10, Mtime: 1000},
	}}
	remote := &manifest.Manifest{Files: map[string]manifest.Entry{
		"same.txt":        {Size: 10, Mtime: 1000},
		"bigger.txt":      {Size: 10, Mtime: 1000},
		"newer.txt":       {Size: 10, Mtime: 1000},
		"only-remote.txt": {Size: 10, Mtime: 1000},
	}}

	plan := manifest.Diff(local, remote, manifest.MtimeTolerance)

	wantPut := []string{"bigger.txt", "newer.txt", "only-local.txt"}
	if strings.Join(plan.ToPut, ",") != strings.Join(wantPut, ",") {
		t.Fatalf("ToPut = %v, want %v", plan.ToPut, wantPut)
	}
	wantDelete := []string{"only-remote.txt"}
	if strings.Join(plan.ToDelete, ",") != strings.Join(wantDelete, ",") {
		t.Fatalf("ToDelete = %v, want %v", plan.ToDelete, wantDelete)
	}
}

// The window is 1 ms, not the 2 SECONDS framing.py uses. Two seconds silently
// swallows real edits: change a file without changing its size, land the new
// mtime within 2 s of the peer's copy, and the diff calls it unchanged forever
// -- the two mtimes never drift further apart, so it is never resent.
func TestDiffMtimeWindowIsOneMillisecondInBothDirections(t *testing.T) {
	cases := []struct {
		label string
		drift float64
		want  bool // should it be resent?
	}{
		{"identical", 0, false},
		{"half a millisecond later", 0.0005, false},
		{"half a millisecond earlier", -0.0005, false},
		{"two milliseconds later", 0.002, true},
		{"two milliseconds earlier", -0.002, true},
		// The case the 2-second window loses outright.
		{"one second later, same size", 1.0, true},
		{"one second earlier, same size", -1.0, true},
	}

	for _, c := range cases {
		t.Run(c.label, func(t *testing.T) {
			local := &manifest.Manifest{Files: map[string]manifest.Entry{
				"f.txt": {Size: 100, Mtime: 1_600_000_000 + c.drift},
			}}
			remote := &manifest.Manifest{Files: map[string]manifest.Entry{
				"f.txt": {Size: 100, Mtime: 1_600_000_000},
			}}

			plan := manifest.Diff(local, remote, manifest.MtimeTolerance)
			resent := len(plan.ToPut) == 1
			if resent != c.want {
				t.Fatalf("drift %gs: resent=%v, want %v", c.drift, resent, c.want)
			}
		})
	}
}

// ---- the wire form --------------------------------------------------------

// Go randomises map iteration, so the order has to be imposed. Without it the
// same folder produces a different byte string on every run and two
// implementations stop being comparable line by line.
func TestToJSONIsStableAndKeepsSixDecimals(t *testing.T) {
	m := &manifest.Manifest{Files: map[string]manifest.Entry{
		"b.txt":     {Size: 2, Mtime: 1_600_000_000.5},
		"a.txt":     {Size: 1, Mtime: 1_600_000_000.25},
		"sub/c.txt": {Size: 3, Mtime: 1_600_000_000},
	}}

	want := `{"a.txt": {"size": 1, "mtime": 1600000000.250000}, ` +
		`"b.txt": {"size": 2, "mtime": 1600000000.500000}, ` +
		`"sub/c.txt": {"size": 3, "mtime": 1600000000.000000}}`

	for i := 0; i < 20; i++ {
		if got := m.ToJSON(); got != want {
			t.Fatalf("run %d:\n got %s\nwant %s", i, got, want)
		}
	}
}

func TestFromJSONRoundTrip(t *testing.T) {
	original := &manifest.Manifest{Files: map[string]manifest.Entry{
		"a.txt":     {Size: 1, Mtime: 1_600_000_000.25},
		"sub/c.txt": {Size: 300000, Mtime: 1_600_000_000},
	}}

	document := `{"files": ` + original.ToJSON() + `}`
	value, err := jsonval.Parse([]byte(document))
	if err != nil {
		t.Fatalf("our own output didn't parse: %v", err)
	}
	object, _ := jsonval.AsObject(value)
	files, present := object.Find("files")

	parsed, ok := manifest.FromJSON(files, present)
	if !ok {
		t.Fatal("FromJSON rejected our own output")
	}
	if len(parsed.Files) != len(original.Files) {
		t.Fatalf("got %d files, want %d", len(parsed.Files), len(original.Files))
	}
	for path, want := range original.Files {
		got := parsed.Files[path]
		if got.Size != want.Size {
			t.Fatalf("%s: size %d, want %d", path, got.Size, want.Size)
		}
		// Six decimals is enough to land well inside the 1 ms window.
		if diff := got.Mtime - want.Mtime; diff > 0.0001 || diff < -0.0001 {
			t.Fatalf("%s: mtime drifted by %g", path, diff)
		}
	}
}

func TestFromJSONRejectsWhatIsNotAManifest(t *testing.T) {
	cases := []struct {
		label    string
		document string
	}{
		{"no files key", `{"op": "ERROR", "message": "bad token"}`},
		{"files is a string", `{"files": "nope"}`},
		{"files is an array", `{"files": [1, 2]}`},
	}

	for _, c := range cases {
		t.Run(c.label, func(t *testing.T) {
			value, err := jsonval.Parse([]byte(c.document))
			if err != nil {
				t.Fatal(err)
			}
			object, _ := jsonval.AsObject(value)
			files, present := object.Find("files")
			if _, ok := manifest.FromJSON(files, present); ok {
				t.Fatalf("%s was accepted as a manifest", c.label)
			}
		})
	}
}

// An entry with a mistyped size is skipped rather than defaulted to zero: a
// zero would make the diff think the peer has an empty copy.
func TestFromJSONSkipsUnusableEntries(t *testing.T) {
	document := `{"files": {"good.txt": {"size": 5, "mtime": 1},` +
		`"bad-size.txt": {"size": "5", "mtime": 1},` +
		`"infinite.txt": {"size": 1e999, "mtime": 1},` +
		`"no-mtime.txt": {"size": 5}}}`

	value, err := jsonval.Parse([]byte(document))
	if err != nil {
		t.Fatal(err)
	}
	object, _ := jsonval.AsObject(value)
	files, present := object.Find("files")
	m, ok := manifest.FromJSON(files, present)
	if !ok {
		t.Fatal("FromJSON rejected the whole manifest over one bad entry")
	}
	if len(m.Files) != 1 {
		t.Fatalf("expected only the good entry, got %v", m.Paths())
	}
	if _, found := m.Files["good.txt"]; !found {
		t.Fatalf("the good entry was dropped: %v", m.Paths())
	}
}
