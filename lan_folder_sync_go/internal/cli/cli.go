// Package cli holds the two argv guards and the byte-level path plumbing the
// standard library won't hand over as-is.
//
// The path helpers are deliberately NOT path/filepath. Every one of them is
// applied to bytes that arrived over the wire, and filepath.Join calls Clean,
// which folds ".." lexically -- it would quietly turn "../../etc/passwd" into
// a tidy relative path before the guard ever got to refuse it. Keeping these
// as string operations keeps the guards honest and matches Python's os.path.
package cli

import (
	"fmt"
	"math"
	"os"
	"slices"
	"strings"
	"time"
)

// Die prints to stderr and exits 1. Call sites format with fmt.Sprintf, so a
// peer-supplied string can never end up being used as a format string.
func Die(message string) {
	fmt.Fprintln(os.Stderr, message)
	os.Exit(1)
}

// Dief is Die with formatting, for the common case.
func Dief(format string, args ...any) {
	Die(fmt.Sprintf(format, args...))
}

// Args is the shape of command line both programs take: up to two positional
// arguments and a fixed set of --flags.
//
// The standard "flag" package is the obvious choice here and is the wrong one.
// It exits with its own usage text on an unknown flag -- no chance to print the
// four lines of advice that make the message useful -- it accepts -delete and
// --delete as the same thing where the other ports accept only the second, and
// it stops parsing flags at the first positional argument, so
// `sync_push 192.168.1.42 /tmp/x --dry-run` would silently ignore the flag and
// push for real. That last one is the exact bug these guards exist to prevent.
type Args struct {
	Positional []string // in order, capped by the caller
	Flags      []string // the recognised ones that were given
	Unknown    []string // everything else starting with --
}

func (a Args) Has(flag string) bool {
	return slices.Contains(a.Flags, flag)
}

// At returns the nth positional argument, or "" and false when it wasn't given.
func (a Args) At(index int) (string, bool) {
	if index < len(a.Positional) {
		return a.Positional[index], true
	}
	return "", false
}

// ParseArgs sorts argv[1:] into the three buckets. Anything starting with "--"
// that isn't in known is a typo, and a typo that gets ignored is how --dryrun
// becomes a real push.
func ParseArgs(argv []string, known []string, maxPositional int) Args {
	var args Args
	for _, arg := range argv {
		switch {
		case strings.HasPrefix(arg, "--"):
			if slices.Contains(known, arg) {
				args.Flags = append(args.Flags, arg)
			} else {
				args.Unknown = append(args.Unknown, arg)
			}
		case len(args.Positional) < maxPositional:
			args.Positional = append(args.Positional, arg)
		}
	}
	return args
}

// DieUnknownFlags exits listing the unknown --flags (sorted, deduped) above a
// usage line.
func DieUnknownFlags(unknown []string, usage string) {
	sorted := slices.Clone(unknown)
	slices.Sort(sorted)
	sorted = slices.Compact(sorted)
	Die("unknown flag(s): " + strings.Join(sorted, " ") + "\n" + usage)
}

// ParsePort returns a TCP port or dies explaining why not. strconv.Atoi would
// accept "+8765" and report "invalid syntax" for "http"; neither is a
// diagnosis, so the digits are checked by hand.
func ParsePort(text, where string) int {
	if text == "" {
		Dief("%s: '%s' is not a number", where, text)
	}
	for _, c := range []byte(text) {
		if c < '0' || c > '9' {
			Dief("%s: '%s' is not a number", where, text)
		}
	}

	// Accumulate with a ceiling rather than converting first: a long string of
	// digits must not overflow on its way to being rejected.
	value := 0
	for _, c := range []byte(text) {
		value = value*10 + int(c-'0')
		if value > 65535 {
			Dief("%s: port %s is outside 1-65535", where, text)
		}
	}
	if value < 1 {
		Dief("%s: port %s is outside 1-65535", where, text)
	}
	return value
}

// ---- paths ----------------------------------------------------------------

// PathJoin is os.path.join: an absolute right side wins outright. filepath.Join
// instead Cleans the result and discards nothing, so it would resolve the ".."
// in a hostile path rather than preserve it for the guard to reject.
func PathJoin(a, b string) string {
	if strings.HasPrefix(b, "/") {
		return b
	}
	if a == "" {
		return b
	}
	if strings.HasSuffix(a, "/") {
		return a + b
	}
	return a + "/" + b
}

// PathDirname is os.path.dirname, without Clean's normalisation.
func PathDirname(path string) string {
	i := strings.LastIndex(path, "/")
	switch {
	case i < 0:
		return ""
	case i == 0:
		return "/"
	default:
		return path[:i]
	}
}

// PathAbs is os.path.abspath: lexical, so it never touches the filesystem and
// never resolves a symlink. Only used for printing a clearer error.
func PathAbs(path string) string {
	if strings.HasPrefix(path, "/") {
		return path
	}
	cwd, err := os.Getwd()
	if err != nil {
		return path
	}
	return PathJoin(cwd, path)
}

// ---- time -----------------------------------------------------------------

// Mtime is st_mtime with sub-second precision, as the JSON float the protocol
// carries. time.Time is the better type everywhere except on the wire, where
// the format is fixed by the other four ports.
func Mtime(info os.FileInfo) float64 {
	t := info.ModTime()
	return float64(t.Unix()) + float64(t.Nanosecond())/1e9
}

// MtimeToTime is the inverse, for stamping a received file. Splitting on
// Floor keeps a negative timestamp from losing its fraction.
func MtimeToTime(mtime float64) time.Time {
	seconds := math.Floor(mtime)
	return time.Unix(int64(seconds), int64((mtime-seconds)*1e9))
}
