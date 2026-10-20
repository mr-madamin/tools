package server_test

import (
	"fmt"
	"math/rand"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"lanfoldersync/internal/framing"
	"lanfoldersync/internal/testsupport"
)

// ---- HELLO gating ---------------------------------------------------------

func TestHelloGatesEveryOp(t *testing.T) {
	s := testsupport.StartServer(t, t.TempDir(), 0)

	// Every one of these is the FIRST frame on a fresh connection. The server
	// must refuse and hang up before serving anything -- a mutating op least
	// of all.
	cases := []struct {
		label string
		frame string
	}{
		{"wrong-token", `{"op": "HELLO", "token": "definitely-not-it"}`},
		{"missing-token", `{"op": "HELLO"}`},
		{"null-token", `{"op": "HELLO", "token": null}`},
		{"numeric-token", `{"op": "HELLO", "token": 5}`},
		{"no-hello", `{"op": "MANIFEST"}`},
		{"delete-before-hello", `{"op": "DELETE", "path": "anything.txt"}`},
		{"put-before-hello", `{"op": "PUT", "path": "x", "size": 0, "mtime": 0}`},
	}

	for _, c := range cases {
		t.Run(c.label, func(t *testing.T) {
			conn := s.Dial(t)
			testsupport.SendJSON(t, conn, c.frame)
			message := testsupport.ExpectError(t, conn, c.label)
			if message == "" {
				t.Fatalf("%s: ERROR carried no message", c.label)
			}
			testsupport.ExpectClosed(t, conn, c.label)
		})
	}
}

// A token that is canonically equal but byte-different must NOT be accepted.
// This is the comparison Swift's String == gets wrong by default (it compares
// by Unicode canonical equivalence), and the one a well-meaning "normalise the
// input" change would break here. Go's string == is a byte compare, and this
// test is what keeps it that way.
func TestTokenIsComparedByBytes(t *testing.T) {
	// "é" as one precomposed rune (NFC) versus "e" plus a combining acute
	// (NFD). The two are canonically equivalent and are not the same bytes.
	const nfc = "café-token"
	const nfd = "café-token"
	if nfc == nfd {
		t.Fatal("precondition: these two spellings must differ byte-wise")
	}

	// Configure the server with the NFC spelling, then present the NFD one.
	s := testsupport.StartServerWithToken(t, t.TempDir(), nfc, 0)

	conn := s.Dial(t)
	testsupport.SendJSON(t, conn, fmt.Sprintf(`{"op": "HELLO", "token": %q}`, nfd))
	if message := testsupport.ExpectError(t, conn, "nfd-token"); message != "bad token" {
		t.Fatalf("expected 'bad token' for a canonically-equal token, got %q", message)
	}
	testsupport.ExpectClosed(t, conn, "nfd-token")

	// And the exact bytes still work, so this isn't just a broken token.
	good := s.Dial(t)
	testsupport.SendJSON(t, good, fmt.Sprintf(`{"op": "HELLO", "token": %q}`, nfc))
	reply, ok := testsupport.Recv(t, good)
	if !ok {
		t.Fatal("the server closed on the correct token")
	}
	if op, _ := reply.Str("op"); op != "OK" {
		t.Fatalf("expected OK for the exact token bytes, got %v", reply)
	}
}
func TestHelloWithTheRightTokenProceeds(t *testing.T) {
	s := testsupport.StartServer(t, t.TempDir(), 0)
	conn := s.Authed(t)

	// The same authenticated connection can now do real work.
	testsupport.SendJSON(t, conn, `{"op": "MANIFEST"}`)
	reply, ok := testsupport.Recv(t, conn)
	if !ok {
		t.Fatal("authenticated session died before serving a manifest")
	}
	if op, _ := reply.Str("op"); op != "MANIFEST" {
		t.Fatalf("expected MANIFEST, got %v", reply)
	}
}

// ---- malformed frames -----------------------------------------------------

func TestMalformedFramesAreRefusedAndTheServerSurvives(t *testing.T) {
	s := testsupport.StartServer(t, t.TempDir(), 0)

	cases := []struct {
		label string
		frame string
		want  string // the exact message, where the ports agree on it
	}{
		{"not-json", `not json at all`, "malformed header: not valid UTF-8 JSON"},
		{"empty-frame", ``, "malformed header: not valid UTF-8 JSON"},
		{"trailing-garbage", `{"op": "BYE"} and more`, "malformed header: not valid UTF-8 JSON"},
		{"bare-number", `123`, "malformed header: not a JSON object"},
		{"bare-array", `[1, 2]`, "malformed header: not a JSON object"},
		{"bare-string", `"MANIFEST"`, "malformed header: not a JSON object"},
		{"bare-null", `null`, "malformed header: not a JSON object"},
		{"missing-op", `{"foo": 1}`, "unknown op: None"},
		{"null-op", `{"op": null}`, "unknown op: None"},
		{"unknown-op", `{"op": "MANFEST"}`, "unknown op: 'MANFEST'"},
		{"numeric-op", `{"op": 5}`, "unknown op: 5"},
		{"boolean-op", `{"op": true}`, "unknown op: True"},
		{"put-missing-path", `{"op": "PUT", "size": 1, "mtime": 0}`, "PUT header missing field: 'path'"},
		{"put-missing-size", `{"op": "PUT", "path": "x", "mtime": 0}`, "PUT header missing field: 'size'"},
		{"put-missing-mtime", `{"op": "PUT", "path": "x", "size": 1}`, "PUT header missing field: 'mtime'"},
		{"put-negative-size", `{"op": "PUT", "path": "x", "size": -1, "mtime": 0}`, "PUT header has an out-of-range 'size'"},
		{"put-infinite-size", `{"op": "PUT", "path": "x", "size": 1e999, "mtime": 0}`, "PUT header has an out-of-range 'size'"},
		{"put-huge-size", `{"op": "PUT", "path": "x", "size": 1e18, "mtime": 0}`, "PUT header has an out-of-range 'size'"},
		// Present but the wrong type. A bool is not a number, and neither is a
		// quoted digit -- the distinction Python needed an isinstance(bool)
		// exclusion for, because its bool is a subclass of int.
		{"put-boolean-size", `{"op": "PUT", "path": "x", "size": true, "mtime": 0}`, "PUT header has an out-of-range 'size'"},
		{"put-string-size", `{"op": "PUT", "path": "x", "size": "5", "mtime": 0}`, "PUT header has an out-of-range 'size'"},
		{"put-infinite-mtime", `{"op": "PUT", "path": "x", "size": 0, "mtime": 1e999}`, "PUT header has an out-of-range 'mtime'"},
		{"put-numeric-path", `{"op": "PUT", "path": 5, "size": 0, "mtime": 0}`, "PUT header missing field: 'path'"},
		{"delete-missing-path", `{"op": "DELETE"}`, "DELETE missing 'path'"},
		{"delete-null-path", `{"op": "DELETE", "path": null}`, "DELETE missing 'path'"},
		{"delete-numeric-path", `{"op": "DELETE", "path": 5}`, "unsafe path refused: 5"},
		{"delete-empty-path", `{"op": "DELETE", "path": ""}`, "unsafe path refused: ''"},
	}

	for _, c := range cases {
		t.Run(c.label, func(t *testing.T) {
			conn := s.Authed(t)
			testsupport.SendJSON(t, conn, c.frame)
			if message := testsupport.ExpectError(t, conn, c.label); message != c.want {
				t.Fatalf("%s: got %q, want %q", c.label, message, c.want)
			}
		})
	}

	// The real pass condition: after all of that, the server still speaks the
	// protocol.
	conn := s.Authed(t)
	testsupport.SendJSON(t, conn, `{"op": "MANIFEST"}`)
	if reply, ok := testsupport.Recv(t, conn); !ok {
		t.Fatal("server died after the bad frames")
	} else if op, _ := reply.Str("op"); op != "MANIFEST" {
		t.Fatalf("expected MANIFEST, got %v", reply)
	}
}

// Python decodes every header with .decode("utf-8") and refuses what doesn't
// fit. Go's encoding/json would instead replace the bad bytes with U+FFFD and
// carry on, so jsonval checks utf8.Valid explicitly -- this is the test that
// pins it.
func TestInvalidUTF8HeaderIsRefused(t *testing.T) {
	s := testsupport.StartServer(t, t.TempDir(), 0)
	conn := s.Authed(t)

	// A lone continuation byte inside an otherwise valid header.
	payload := []byte(`{"op": "DELETE", "path": "a` + "\xff" + `b"}`)
	if err := framing.SendMsg(conn, payload); err != nil {
		t.Fatalf("send: %v", err)
	}
	message := testsupport.ExpectError(t, conn, "invalid-utf8")
	if message != "malformed header: not valid UTF-8 JSON" {
		t.Fatalf("got %q", message)
	}
}

// 200 KB of '[' is a frame nobody has authenticated. json's parser recurses
// per bracket in Python, which exhausted the interpreter's stack and killed
// the whole server; jsonval caps the depth on a byte scan before the parser
// ever sees it.
func TestDeeplyNestedHeaderIsRefused(t *testing.T) {
	s := testsupport.StartServer(t, t.TempDir(), 0)
	conn := s.Dial(t) // deliberately BEFORE the HELLO

	if err := framing.SendMsg(conn, []byte(strings.Repeat("[", 200_000))); err != nil {
		t.Fatalf("send: %v", err)
	}
	if message := testsupport.ExpectError(t, conn, "deep-nesting"); message == "" {
		t.Fatal("no ERROR for a deeply nested frame")
	}

	// And the server is still there.
	conn2 := s.Authed(t)
	testsupport.SendJSON(t, conn2, `{"op": "MANIFEST"}`)
	if _, ok := testsupport.Recv(t, conn2); !ok {
		t.Fatal("server died on a deeply nested frame")
	}
}

// The frame cap is enforced on the length prefix alone, before the payload is
// read and before the HELLO -- otherwise four bytes from anyone would make us
// reserve a gigabyte.
func TestOversizedFrameIsRefusedOnItsLengthPrefix(t *testing.T) {
	s := testsupport.StartServer(t, t.TempDir(), 0)
	raw := testsupport.DialRaw(t, s)

	// Claim 64 MB, send nothing. The cap for a client frame is 1 MB.
	if _, err := raw.Write(testsupport.RawFrame(64*1024*1024, nil)); err != nil {
		t.Fatalf("write: %v", err)
	}

	buf := make([]byte, 4096)
	n, err := raw.Read(buf)
	if err != nil || n == 0 {
		t.Fatalf("server closed without an ERROR: n=%d err=%v", n, err)
	}
	if !strings.Contains(string(buf[:n]), "frame too large") {
		t.Fatalf("expected 'frame too large', got %q", buf[:n])
	}

	// The log should carry the length the peer claimed -- the one number worth
	// having when you're looking at it.
	found := false
	for _, line := range s.Logs() {
		if strings.Contains(line, "Refused oversized frame") && strings.Contains(line, "67108864") {
			found = true
		}
	}
	if !found {
		t.Fatalf("the refusal was not logged with its length: %v", s.Logs())
	}
}

// 200 random, truncated and lying frames, with a fixed seed so a failure can
// be reproduced. Nothing here should take the server down.
func TestRandomFramesNeverKillTheServer(t *testing.T) {
	s := testsupport.StartServer(t, t.TempDir(), 0)
	random := rand.New(rand.NewSource(1))

	for i := 0; i < 200; i++ {
		raw := testsupport.DialRaw(t, s)

		switch i % 4 {
		case 0: // random bytes as a whole frame
			payload := make([]byte, random.Intn(200))
			random.Read(payload)
			raw.Write(testsupport.RawFrame(uint32(len(payload)), payload))
		case 1: // a length prefix that lies: more declared than sent
			payload := make([]byte, random.Intn(50)+1)
			random.Read(payload)
			raw.Write(testsupport.RawFrame(uint32(len(payload))+100, payload))
		case 2: // a truncated prefix, then a hang-up
			raw.Write([]byte{0, 0})
		case 3: // valid JSON, random op
			op := fmt.Sprintf(`{"op": "%d"}`, random.Int())
			raw.Write(testsupport.RawFrame(uint32(len(op)), []byte(op)))
		}
		raw.Close()
	}

	conn := s.Authed(t)
	testsupport.SendJSON(t, conn, `{"op": "MANIFEST"}`)
	if _, ok := testsupport.Recv(t, conn); !ok {
		t.Fatal("server died somewhere in 200 hostile frames")
	}
}

// A peer that connects and says nothing locks out everyone else, because the
// server handles one session at a time. It gets dropped.
func TestIdlePeerIsDropped(t *testing.T) {
	s := testsupport.StartServer(t, t.TempDir(), 300*time.Millisecond)
	conn := s.Authed(t)

	// Say nothing. The server's own read deadline should expire and close us.
	if _, ok := testsupport.Recv(t, conn); ok {
		t.Fatal("server replied to a peer that sent nothing")
	}

	logs := strings.Join(s.Logs(), "\n")
	if !strings.Contains(logs, "Session timed out") {
		t.Fatalf("the drop was not logged: %v", s.Logs())
	}
}

// ---- PUT and DELETE -------------------------------------------------------

func TestPutRoundTripIncludingMtime(t *testing.T) {
	dir := t.TempDir()
	s := testsupport.StartServer(t, dir, 0)
	conn := s.Authed(t)

	const body = "hello over the wire\n"
	const mtime = 1_600_000_000.5
	header := fmt.Sprintf(`{"op": "PUT", "path": "sub/dir/file.txt", "size": %d, "mtime": %.6f}`,
		len(body), mtime)
	testsupport.SendJSON(t, conn, header)
	if err := conn.SendAll([]byte(body)); err != nil {
		t.Fatalf("send body: %v", err)
	}

	// The session is still good, which is how we know the body was consumed
	// exactly: ask for something and get an answer.
	testsupport.SendJSON(t, conn, `{"op": "MANIFEST"}`)
	if _, ok := testsupport.Recv(t, conn); !ok {
		t.Fatal("session died after a PUT")
	}

	landed := filepath.Join(dir, "sub/dir/file.txt")
	got, err := os.ReadFile(landed)
	if err != nil {
		t.Fatalf("the file did not land: %v", err)
	}
	if string(got) != body {
		t.Fatalf("content mismatch: %q", got)
	}

	info, err := os.Stat(landed)
	if err != nil {
		t.Fatal(err)
	}
	// The mtime has to survive, to a millisecond: the diff's whole window is
	// 1 ms, so a timestamp that lands further off than that would make the
	// next push resend a file that is already correct.
	landedMtime := float64(info.ModTime().Unix()) + float64(info.ModTime().Nanosecond())/1e9
	if diff := landedMtime - mtime; diff > 0.001 || diff < -0.001 {
		t.Fatalf("mtime drifted by %gs (got %f, want %f)", diff, landedMtime, mtime)
	}

	// No temp file left behind.
	assertNoTempFiles(t, dir)
}

// A PUT that dies mid-body must leave the previous copy intact. Writing the
// destination directly would truncate it the instant the transfer started.
func TestAbortedPutLeavesTheOldFileIntact(t *testing.T) {
	dir := t.TempDir()
	s := testsupport.StartServer(t, dir, 0)

	const original = "the good copy, which must survive\n"
	target := filepath.Join(dir, "precious.txt")
	if err := os.WriteFile(target, []byte(original), 0o644); err != nil {
		t.Fatal(err)
	}

	conn := s.Authed(t)
	testsupport.SendJSON(t, conn,
		`{"op": "PUT", "path": "precious.txt", "size": 10000, "mtime": 1600000000}`)
	// Send a fraction of what we promised, then vanish.
	if err := conn.SendAll([]byte("partial")); err != nil {
		t.Fatalf("send: %v", err)
	}
	conn.Close()

	// Give the server a moment to notice the hang-up and run its cleanup.
	waitFor(t, func() bool {
		return len(assertableTempFiles(t, dir)) == 0
	})

	got, err := os.ReadFile(target)
	if err != nil {
		t.Fatalf("the original file is gone: %v", err)
	}
	if string(got) != original {
		t.Fatalf("the original was damaged by an aborted PUT: %q", got)
	}
	assertNoTempFiles(t, dir)
}

func TestPathEscapesAreRefusedForPutAndDelete(t *testing.T) {
	dir := t.TempDir()
	shared := filepath.Join(dir, "shared")
	if err := os.MkdirAll(shared, 0o777); err != nil {
		t.Fatal(err)
	}

	// A file OUTSIDE the shared folder, which no frame may reach.
	victim := filepath.Join(dir, "victim.txt")
	if err := os.WriteFile(victim, []byte("must survive\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	// A symlink inside the shared folder pointing out of it: the one escape
	// that "relative and ..-free" still leaves open.
	if err := os.Symlink(dir, filepath.Join(shared, "escape")); err != nil {
		t.Fatal(err)
	}

	s := testsupport.StartServer(t, shared, 0)

	escapes := []string{
		"../victim.txt",
		"../../victim.txt",
		"sub/../../victim.txt",
		"/etc/hosts",
		"escape/victim.txt", // through the symlink
		".",
		"",
	}

	for _, relPath := range escapes {
		t.Run("delete "+relPath, func(t *testing.T) {
			conn := s.Authed(t)
			testsupport.SendJSON(t, conn, fmt.Sprintf(`{"op": "DELETE", "path": %q}`, relPath))
			if message := testsupport.ExpectError(t, conn, relPath); !strings.Contains(message, "unsafe path refused") {
				t.Fatalf("DELETE %q: got %q", relPath, message)
			}
		})

		t.Run("put "+relPath, func(t *testing.T) {
			conn := s.Authed(t)
			testsupport.SendJSON(t, conn, fmt.Sprintf(
				`{"op": "PUT", "path": %q, "size": 5, "mtime": 0}`, relPath))
			if message := testsupport.ExpectError(t, conn, relPath); !strings.Contains(message, "unsafe path refused") {
				t.Fatalf("PUT %q: got %q", relPath, message)
			}
		})
	}

	if _, err := os.Stat(victim); err != nil {
		t.Fatalf("a path escape reached a file outside the shared folder: %v", err)
	}
}

// The guard must not over-block: a legitimate nested path still works, and a
// DELETE of a file that isn't there is idempotent rather than an error.
func TestDeleteWorksAndIsIdempotent(t *testing.T) {
	dir := t.TempDir()
	s := testsupport.StartServer(t, dir, 0)

	nested := filepath.Join(dir, "sub", "doomed.txt")
	if err := os.MkdirAll(filepath.Dir(nested), 0o777); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(nested, []byte("delete me\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	conn := s.Authed(t)
	testsupport.SendJSON(t, conn, `{"op": "DELETE", "path": "sub/doomed.txt"}`)
	// No reply on success -- prove the session lived by reusing it.
	testsupport.SendJSON(t, conn, `{"op": "MANIFEST"}`)
	if _, ok := testsupport.Recv(t, conn); !ok {
		t.Fatal("session died on a legitimate DELETE")
	}
	if _, err := os.Stat(nested); !os.IsNotExist(err) {
		t.Fatalf("the nested file survived DELETE (guard too strict?): %v", err)
	}

	// Again, now that it is gone.
	testsupport.SendJSON(t, conn, `{"op": "DELETE", "path": "sub/doomed.txt"}`)
	testsupport.SendJSON(t, conn, `{"op": "MANIFEST"}`)
	if _, ok := testsupport.Recv(t, conn); !ok {
		t.Fatal("session died deleting a file that was already gone")
	}
}

func TestByeEndsTheSessionCleanly(t *testing.T) {
	s := testsupport.StartServer(t, t.TempDir(), 0)
	conn := s.Authed(t)

	testsupport.SendJSON(t, conn, `{"op": "BYE"}`)
	testsupport.ExpectClosed(t, conn, "bye")

	if !strings.Contains(strings.Join(s.Logs(), "\n"), "Peer said BYE") {
		t.Fatalf("BYE was not logged: %v", s.Logs())
	}
}

// ---- helpers --------------------------------------------------------------

func assertableTempFiles(t *testing.T, dir string) []string {
	t.Helper()
	var found []string
	filepath.WalkDir(dir, func(path string, entry os.DirEntry, err error) error {
		if err == nil && !entry.IsDir() && strings.HasSuffix(path, ".tmp") {
			found = append(found, path)
		}
		return nil
	})
	return found
}

func assertNoTempFiles(t *testing.T, dir string) {
	t.Helper()
	if left := assertableTempFiles(t, dir); len(left) > 0 {
		t.Fatalf("temp files left behind: %v", left)
	}
}

// waitFor polls a condition for up to two seconds. The server runs in its own
// goroutine, so "it has noticed by now" is not instantaneous -- and a sleep
// long enough to be safe would be long enough to be slow.
func waitFor(t *testing.T, condition func() bool) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if condition() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("timed out waiting for the server to catch up")
}
