package cli_test

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"lanfoldersync/internal/cli"
)

// The guard that matters: a mistyped flag must land in Unknown, where the
// caller turns it into an exit. If --dryrun were silently ignored, a preview
// would become a real push.
func TestParseArgsCatchesMistypedFlags(t *testing.T) {
	known := []string{"--dry-run", "--delete", "--help"}

	args := cli.ParseArgs([]string{"--dryrun"}, known, 2)
	if len(args.Unknown) != 1 || args.Unknown[0] != "--dryrun" {
		t.Fatalf("--dryrun was not caught: %+v", args)
	}
	if args.Has("--dry-run") {
		t.Fatal("--dryrun was treated as --dry-run")
	}

	// Single-dash is not an alias. The other ports accept only the long form,
	// and the "flag" package would have accepted both.
	for _, typo := range []string{"-dry-run", "--DRY-RUN", "--delete=true", "--lann"} {
		args := cli.ParseArgs([]string{typo}, known, 2)
		if typo == "-dry-run" {
			// A single dash is not a "--" flag at all, so it is positional --
			// which is caught by root_dir not existing rather than by the flag
			// check. Either way it does not enable anything.
			if args.Has("--dry-run") {
				t.Fatalf("%s enabled --dry-run", typo)
			}
			continue
		}
		if len(args.Unknown) == 0 {
			t.Errorf("%s was not reported as unknown", typo)
		}
	}
}

// Flags are recognised wherever they appear, including AFTER the positional
// arguments. The "flag" package stops parsing at the first non-flag, which
// would make `sync_push 192.168.1.42 /tmp/x --dry-run` push for real.
func TestParseArgsAcceptsFlagsAfterPositionals(t *testing.T) {
	args := cli.ParseArgs([]string{"192.168.1.42", "/tmp/x", "--dry-run"},
		[]string{"--dry-run", "--delete"}, 2)

	if !args.Has("--dry-run") {
		t.Fatal("a flag after the positional arguments was ignored -- a dry run would have pushed")
	}
	if host, ok := args.At(0); !ok || host != "192.168.1.42" {
		t.Fatalf("At(0) = %q, %v", host, ok)
	}
	if root, ok := args.At(1); !ok || root != "/tmp/x" {
		t.Fatalf("At(1) = %q, %v", root, ok)
	}
	if _, ok := args.At(2); ok {
		t.Fatal("At(2) should not be set")
	}
}

func TestParseArgsCapsPositionals(t *testing.T) {
	args := cli.ParseArgs([]string{"a", "b", "c", "d"}, nil, 2)
	if len(args.Positional) != 2 {
		t.Fatalf("expected 2 positionals, got %v", args.Positional)
	}
}

func TestParseArgsHandlesNoArguments(t *testing.T) {
	args := cli.ParseArgs(nil, []string{"--lan"}, 2)
	if len(args.Positional) != 0 || len(args.Flags) != 0 || len(args.Unknown) != 0 {
		t.Fatalf("expected everything empty, got %+v", args)
	}
	if _, ok := args.At(0); ok {
		t.Fatal("At(0) should not be set")
	}
}

// PathJoin is os.path.join, and the "absolute right side wins" rule is
// deliberate: it preserves a hostile absolute path for the guard to refuse,
// where filepath.Join would quietly make it relative and filepath.Clean would
// fold away a "..".
func TestPathJoinPreservesWhatTheGuardNeedsToSee(t *testing.T) {
	cases := []struct{ a, b, want string }{
		{"base", "file.txt", "base/file.txt"},
		{"base/", "file.txt", "base/file.txt"},
		{"/abs/base", "sub/file.txt", "/abs/base/sub/file.txt"},
		{"", "file.txt", "file.txt"},
		// The two that matter.
		{"base", "/etc/hosts", "/etc/hosts"},
		{"base", "../escape", "base/../escape"},
	}
	for _, c := range cases {
		if got := cli.PathJoin(c.a, c.b); got != c.want {
			t.Errorf("PathJoin(%q, %q) = %q, want %q", c.a, c.b, got, c.want)
		}
	}
}

func TestPathDirname(t *testing.T) {
	cases := []struct{ in, want string }{
		{"a/b/c.txt", "a/b"},
		{"a/b.txt", "a"},
		{"b.txt", ""},
		{"/b.txt", "/"},
		{"/a/b.txt", "/a"},
	}
	for _, c := range cases {
		if got := cli.PathDirname(c.in); got != c.want {
			t.Errorf("PathDirname(%q) = %q, want %q", c.in, got, c.want)
		}
	}
}

func TestPathAbsLeavesAbsolutePathsAlone(t *testing.T) {
	if got := cli.PathAbs("/already/absolute"); got != "/already/absolute" {
		t.Errorf("PathAbs = %q", got)
	}
	// A relative path gains the cwd, and is not resolved through symlinks:
	// this only ever feeds an error message.
	if got := cli.PathAbs("relative/path"); !strings.HasSuffix(got, "/relative/path") ||
		!strings.HasPrefix(got, "/") {
		t.Errorf("PathAbs(relative) = %q", got)
	}
}

// The mtime round trip has to survive at better than the diff's 1 ms window,
// or every push would resend files that are already correct.
func TestMtimeRoundTrip(t *testing.T) {
	for _, mtime := range []float64{0, 1_600_000_000, 1_600_000_000.5, 1_600_000_000.123456, -1} {
		stamp := cli.MtimeToTime(mtime)
		got := float64(stamp.Unix()) + float64(stamp.Nanosecond())/1e9
		if diff := got - mtime; diff > 0.000001 || diff < -0.000001 {
			t.Errorf("MtimeToTime(%f) round-tripped to %f (drift %g)", mtime, got, diff)
		}
	}
}

// Mtime reads a real file's timestamp back as the float the wire carries.
func TestMtimeReadsAFilesTimestamp(t *testing.T) {
	path := filepath.Join(t.TempDir(), "stamped.txt")
	if err := os.WriteFile(path, []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}

	const want = 1_600_000_000.5
	stamp := cli.MtimeToTime(want)
	if err := os.Chtimes(path, stamp, stamp); err != nil {
		t.Fatal(err)
	}

	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if got := cli.Mtime(info); got != want {
		t.Fatalf("Mtime = %f, want %f", got, want)
	}
}
