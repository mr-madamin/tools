package framing_test

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"lanfoldersync/internal/framing"
	"lanfoldersync/internal/netx"
	"lanfoldersync/internal/testsupport"
)

// A payload far bigger than the socket buffer, which forces the reader to
// reassemble it over many partial reads. SendAll blocks once the kernel buffer
// fills, so the reader has to be draining concurrently -- that is the point.
func TestBigPayloadSurvivesPartialReads(t *testing.T) {
	a, b := testsupport.ConnPair(t)

	want := strings.Repeat("A", 1_000_000)

	received := make(chan []byte, 1)
	go func() {
		payload, ok, err := framing.RecvMsg(b, framing.MaxFrame)
		if err != nil || !ok {
			received <- nil
			return
		}
		received <- payload
	}()

	if err := framing.SendMsg(a, []byte(want)); err != nil {
		t.Fatalf("send: %v", err)
	}

	got := <-received
	if string(got) != want {
		t.Fatalf("mismatch: sent %d bytes, got %d", len(want), len(got))
	}
}

// Two frames shoved into ONE write, so they land back-to-back in the
// receiver's buffer. RecvMsg must split them at the right boundary.
func TestCoalescedFramesSplitCleanly(t *testing.T) {
	a, b := testsupport.ConnPair(t)

	frame1 := testsupport.RawFrame(5, []byte("hello"))
	frame2 := testsupport.RawFrame(3, []byte("bye"))
	if err := a.SendAll(append(frame1, frame2...)); err != nil {
		t.Fatalf("send: %v", err)
	}

	for _, want := range []string{"hello", "bye"} {
		payload, ok, err := framing.RecvMsg(b, framing.MaxFrame)
		if err != nil || !ok {
			t.Fatalf("recv %q: ok=%v err=%v", want, ok, err)
		}
		if string(payload) != want {
			t.Fatalf("got %q, want %q", payload, want)
		}
	}
}

// A zero-length frame is legal and is not EOF. Conflating the two would make a
// peer's empty frame look like a hang-up.
func TestEmptyFrameIsNotEOF(t *testing.T) {
	a, b := testsupport.ConnPair(t)

	if err := framing.SendMsg(a, nil); err != nil {
		t.Fatalf("send: %v", err)
	}
	payload, ok, err := framing.RecvMsg(b, framing.MaxFrame)
	if err != nil || !ok {
		t.Fatalf("an empty frame read as EOF: ok=%v err=%v", ok, err)
	}
	if len(payload) != 0 {
		t.Fatalf("expected an empty payload, got %q", payload)
	}
}

// The cap is checked against the length prefix, so an oversized frame costs us
// nothing: no allocation, and the payload is never read.
func TestFrameCapIsEnforcedOnThePrefix(t *testing.T) {
	a, b := testsupport.ConnPair(t)

	if err := a.SendAll(testsupport.RawFrame(2_000_000, nil)); err != nil {
		t.Fatalf("send: %v", err)
	}

	_, _, err := framing.RecvMsg(b, framing.MaxControlFrame)
	var tooLarge *framing.FrameTooLargeError
	if !errors.As(err, &tooLarge) {
		t.Fatalf("expected FrameTooLargeError, got %v", err)
	}
	if tooLarge.Length != 2_000_000 {
		t.Fatalf("the error lost the declared length: %d", tooLarge.Length)
	}
}

// A peer that closes between frames is the clean end of a session.
func TestClosedPeerBetweenFramesIsNotAnError(t *testing.T) {
	a, b := testsupport.ConnPair(t)
	a.Close()

	payload, ok, err := framing.RecvMsg(b, framing.MaxFrame)
	if err != nil {
		t.Fatalf("a clean hang-up was reported as an error: %v", err)
	}
	if ok || payload != nil {
		t.Fatalf("expected EOF, got %q", payload)
	}
}

// A peer that closes PART way through a frame is an error: the stream has no
// boundary left to resync on.
func TestClosedPeerMidFrameIsAnError(t *testing.T) {
	a, b := testsupport.ConnPair(t)

	// Declare 100 bytes, send 4, hang up.
	if err := a.SendAll(testsupport.RawFrame(100, []byte("abcd"))); err != nil {
		t.Fatalf("send: %v", err)
	}
	a.Close()

	if _, ok, err := framing.RecvMsg(b, framing.MaxFrame); err == nil || ok {
		t.Fatalf("a half-read frame was accepted: ok=%v err=%v", ok, err)
	} else if !errors.Is(err, netx.ErrPeerClosed) {
		t.Fatalf("expected ErrPeerClosed, got %v", err)
	}
}

// ---- the declared-size contract -------------------------------------------
//
// The body is the only unframed part of the stream. These three tests are the
// reason send_file is written the way it is: whatever the file does
// underneath us, exactly `size` bytes go out, so the NEXT frame still lines up.

func TestFileTruncatedMidSendIsPaddedToTheDeclaredSize(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "shrinking.bin")

	// Bigger than the socket buffers, so SendAll BLOCKS partway through and the
	// truncation below is guaranteed to land mid-transfer. At 200 KB the whole
	// file fits in the buffer, the send finishes before the reader has even
	// read the header, and the test passes or fails on a race.
	if err := os.WriteFile(path, []byte(strings.Repeat("x", 4_000_000)), 0o644); err != nil {
		t.Fatal(err)
	}

	a, b := testsupport.ConnPair(t)

	// Read the header, then truncate the file, then drain the body.
	done := make(chan error, 1)
	go func() {
		payload, ok, err := framing.RecvMsg(b, framing.MaxControlFrame)
		if err != nil || !ok {
			done <- fmt.Errorf("header: ok=%v err=%v", ok, err)
			return
		}
		header, err := framing.DecodeHeader(payload)
		if err != nil {
			done <- err
			return
		}
		size, _ := header.Number("size")

		// Shrink it now, after the size was declared.
		if err := os.Truncate(path, 1000); err != nil {
			done <- err
			return
		}

		// The sender must still produce exactly `size` bytes.
		body := make([]byte, int(size))
		if _, err := b.RecvExactly(body); err != nil {
			done <- fmt.Errorf("body: %w", err)
			return
		}

		// ...and the next frame must line up exactly where it should.
		next, ok, err := framing.RecvMsg(b, framing.MaxControlFrame)
		if err != nil || !ok {
			done <- fmt.Errorf("next frame: ok=%v err=%v", ok, err)
			return
		}
		if string(next) != `{"op": "BYE"}` {
			done <- fmt.Errorf("the stream lost its boundary: next frame was %q", next)
			return
		}
		done <- nil
	}()

	changed, err := framing.SendFile(a, dir, "shrinking.bin")
	if err != nil {
		t.Fatalf("send: %v", err)
	}
	if !changed {
		t.Fatal("a file truncated mid-send was not reported as changed")
	}
	if err := framing.SendJSON(a, `{"op": "BYE"}`); err != nil {
		t.Fatalf("send BYE: %v", err)
	}

	if err := <-done; err != nil {
		t.Fatal(err)
	}
}

func TestFileGrownMidSendIsClippedToTheDeclaredSize(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "growing.bin")
	// Bigger than the socket buffers, for the same reason as the truncation
	// test: the sender has to still be mid-loop when the file changes.
	if err := os.WriteFile(path, []byte(strings.Repeat("x", 4_000_000)), 0o644); err != nil {
		t.Fatal(err)
	}

	a, b := testsupport.ConnPair(t)

	done := make(chan error, 1)
	go func() {
		payload, ok, err := framing.RecvMsg(b, framing.MaxControlFrame)
		if err != nil || !ok {
			done <- fmt.Errorf("header: ok=%v err=%v", ok, err)
			return
		}
		header, _ := framing.DecodeHeader(payload)
		size, _ := header.Number("size")

		// Append to it, after the size was declared.
		file, err := os.OpenFile(path, os.O_APPEND|os.O_WRONLY, 0o644)
		if err != nil {
			done <- err
			return
		}
		file.WriteString(strings.Repeat("y", 50_000))
		file.Close()

		body := make([]byte, int(size))
		if _, err := b.RecvExactly(body); err != nil {
			done <- fmt.Errorf("body: %w", err)
			return
		}
		next, ok, err := framing.RecvMsg(b, framing.MaxControlFrame)
		if err != nil || !ok {
			done <- fmt.Errorf("next frame: ok=%v err=%v", ok, err)
			return
		}
		if string(next) != `{"op": "BYE"}` {
			done <- fmt.Errorf("the stream lost its boundary: next frame was %q", next)
			return
		}
		done <- nil
	}()

	if _, err := framing.SendFile(a, dir, "growing.bin"); err != nil {
		t.Fatalf("send: %v", err)
	}
	if err := framing.SendJSON(a, `{"op": "BYE"}`); err != nil {
		t.Fatalf("send BYE: %v", err)
	}

	if err := <-done; err != nil {
		t.Fatal(err)
	}
}

// A file that vanished between the scan and the send puts NOTHING on the wire,
// so the stream is still in sync and the pusher can skip it and carry on. (The
// C port treats this as fatal and abandons the rest of the push; sync_push.py
// lets the FileNotFoundError escape as a traceback.)
func TestVanishedFileSendsNothingAndIsRecoverable(t *testing.T) {
	dir := t.TempDir()
	a, b := testsupport.ConnPair(t)

	_, err := framing.SendFile(a, dir, "never-existed.txt")
	var localErr *framing.LocalFileError
	if !errors.As(err, &localErr) {
		t.Fatalf("expected LocalFileError, got %v", err)
	}

	// Nothing reached the socket: the next frame we send is the first thing
	// the peer sees.
	if err := framing.SendJSON(a, `{"op": "BYE"}`); err != nil {
		t.Fatalf("send BYE: %v", err)
	}
	payload, ok, err := framing.RecvMsg(b, framing.MaxControlFrame)
	if err != nil || !ok {
		t.Fatalf("recv: ok=%v err=%v", ok, err)
	}
	if string(payload) != `{"op": "BYE"}` {
		t.Fatalf("a failed send put bytes on the wire: %q", payload)
	}
}

// An unchanged file reports changed=false, so the pusher's warning means
// something.
func TestUnchangedFileIsNotReportedAsChanged(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "steady.txt"), []byte("hello\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	a, b := testsupport.ConnPair(t)

	done := make(chan struct{})
	go func() {
		defer close(done)
		payload, _, _ := framing.RecvMsg(b, framing.MaxControlFrame)
		header, _ := framing.DecodeHeader(payload)
		size, _ := header.Number("size")
		body := make([]byte, int(size))
		b.RecvExactly(body)
	}()

	changed, err := framing.SendFile(a, dir, "steady.txt")
	if err != nil {
		t.Fatalf("send: %v", err)
	}
	if changed {
		t.Fatal("a file nobody touched was reported as changed")
	}
	<-done
}

// The PUT header's spelling is part of the protocol: `size` an integer,
// `mtime` six decimals. A generic marshaller would render the float in Go's
// shortest round-trip form and silently stop matching the other ports.
func TestPutHeaderSpelling(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "spelling.txt")
	if err := os.WriteFile(path, []byte("12345"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(path, nowish(), nowish()); err != nil {
		t.Fatal(err)
	}

	a, b := testsupport.ConnPair(t)

	done := make(chan string, 1)
	go func() {
		payload, _, _ := framing.RecvMsg(b, framing.MaxControlFrame)
		body := make([]byte, 5)
		b.RecvExactly(body)
		done <- string(payload)
	}()

	if _, err := framing.SendFile(a, dir, "spelling.txt"); err != nil {
		t.Fatalf("send: %v", err)
	}
	header := <-done

	if !strings.HasPrefix(header, `{"op": "PUT", "path": "spelling.txt", "size": 5, "mtime": `) {
		t.Fatalf("header spelling changed: %s", header)
	}
	if !strings.Contains(header, "1600000000.500000") {
		t.Fatalf("mtime lost its six decimals: %s", header)
	}
}

func nowish() time.Time {
	// A fixed, obviously-synthetic timestamp with a fraction, so the six
	// decimals in the header are checkable.
	return time.Unix(1_600_000_000, 500_000_000)
}
