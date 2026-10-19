// Package framing speaks the wire protocol: a 4-byte big-endian length prefix,
// then a UTF-8 JSON payload. A PUT frame is followed by exactly `size` raw
// body bytes. Byte for byte the protocol framing.py speaks, so a Go peer and a
// Python (or C, or C++, or Swift) peer interoperate.
package framing

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"

	"lanfoldersync/internal/cli"
	"lanfoldersync/internal/jsonval"
	"lanfoldersync/internal/netx"
	"lanfoldersync/internal/paths"
)

// The length prefix is 4 bytes, so a peer can claim up to 4 GB before sending
// a single byte of payload -- and this runs before the HELLO. Two ceilings,
// because the two directions carry very different frames: a MANIFEST lists
// every file in the folder, while everything else is a short header. The
// server only ever reads the short kind.
const (
	MaxFrame        = 64 * 1024 * 1024 // a manifest of a huge folder
	MaxControlFrame = 1 * 1024 * 1024  // HELLO / PUT / DELETE / BYE
)

// size and mtime arrive as JSON numbers from the peer. A negative size skips
// the write loop and leaves an empty file where a real one had been, and
// "size": 1e999 decodes to +Inf -- which in C and C++ is undefined behaviour
// on conversion and in Go is a defined but meaningless integer. Bound both
// before anything is cast, opened or written.
const (
	MaxFileSize = 64 * 1024 * 1024 * 1024 // 64 GiB per file
	MaxMtime    = 1e15                    // far past any real clock
)

const chunkSize = 65536

// FrameTooLargeError means the peer declared a frame bigger than we are
// willing to read. The payload is left unread, so the stream has no boundary
// to resync on: every caller ends the session.
type FrameTooLargeError struct {
	Length uint32
}

func (e *FrameTooLargeError) Error() string {
	return fmt.Sprintf("frame too large: %d bytes", e.Length)
}

// SendMsg frames and sends: 4-byte big-endian length prefix, then the payload.
func SendMsg(conn *netx.Conn, payload []byte) error {
	// One buffer, one write -- as Python's sendall(header + payload) does. It
	// keeps the length prefix from ever landing without its body.
	frame := make([]byte, 4+len(payload))
	binary.BigEndian.PutUint32(frame[:4], uint32(len(payload)))
	copy(frame[4:], payload)
	return conn.SendAll(frame)
}

// SendJSON is SendMsg for an already-spelled JSON document.
func SendJSON(conn *netx.Conn, document string) error {
	return SendMsg(conn, []byte(document))
}

func SendError(conn *netx.Conn, message string) error {
	return SendJSON(conn, `{"op": "ERROR", "message": `+jsonval.Escape(message)+`}`)
}

// RecvMsg reads one framed message. ok is false when the peer closed between
// frames, which is the clean way a session ends; err covers everything else,
// including *FrameTooLargeError.
func RecvMsg(conn *netx.Conn, max uint32) (payload []byte, ok bool, err error) {
	var header [4]byte
	got, err := conn.RecvExactly(header[:])
	if err != nil || !got {
		return nil, false, err
	}

	length := binary.BigEndian.Uint32(header[:])

	// Refuse before allocating: without this an unauthenticated peer could
	// make us reserve 4 GB with four bytes.
	if length > max {
		return nil, false, &FrameTooLargeError{Length: length}
	}
	if length == 0 {
		return []byte{}, true, nil
	}

	payload = make([]byte, length)
	got, err = conn.RecvExactly(payload)
	if err != nil || !got {
		return nil, false, err
	}
	return payload, true, nil
}

// ---- headers --------------------------------------------------------------

type HeaderErrorKind int

const (
	// HeaderNotUtf8JSON: it didn't decode, or didn't parse.
	HeaderNotUtf8JSON HeaderErrorKind = iota
	// HeaderNotObject: it parsed, but "123" and "[1,2]" are not headers.
	// Worth its own message -- the parse succeeded, so blaming UTF-8 sends you
	// hunting in the wrong place.
	HeaderNotObject
)

type HeaderError struct {
	Kind HeaderErrorKind
}

func (e *HeaderError) Error() string {
	if e.Kind == HeaderNotObject {
		return "malformed header: not a JSON object"
	}
	return "malformed header: not valid UTF-8 JSON"
}

// DecodeHeader parses one control frame into the object every op reads from.
func DecodeHeader(payload []byte) (jsonval.Object, error) {
	value, err := jsonval.Parse(payload)
	if err != nil {
		return nil, &HeaderError{Kind: HeaderNotUtf8JSON}
	}
	object, ok := jsonval.AsObject(value)
	if !ok {
		return nil, &HeaderError{Kind: HeaderNotObject}
	}
	return object, nil
}

// ---- sending a file -------------------------------------------------------

// LocalFileError is a local file we could not read. It is only ever returned
// BEFORE the PUT header goes out, so the stream is still intact and the pusher
// can skip this one file and carry on. The common case is a file deleted
// between the scan and the send. (The C port dies here instead, which throws
// away the rest of the push; sync_push.py is worse still -- it catches only
// TimeoutError and the two connection errors, so a FileNotFoundError escapes
// as an uncaught traceback.)
type LocalFileError struct {
	Err error
}

func (e *LocalFileError) Error() string { return e.Err.Error() }
func (e *LocalFileError) Unwrap() error { return e.Err }

// SendFile sends one file as a PUT: JSON header {path, size, mtime}, then the
// raw body. changed reports that the file changed size while we were reading
// it -- a warning, not an error: exactly `size` body bytes still went out, so
// the stream is intact and the session can continue. Only that one file's
// content is suspect.
//
// The body is the only unframed part of the stream: the receiver finds the
// next frame by counting exactly `size` bytes. Send one byte too few or too
// many and it reads the following header as file content, silently corrupting
// every later file in the session. The file can change underneath us at any
// point, so the declared size is the contract and this honours it even when
// the file stops matching.
func SendFile(conn *netx.Conn, rootDir, relPath string) (changed bool, err error) {
	// Open first, then stat the handle we actually hold: os.Stat-then-open
	// lets the path be replaced in between, and we would declare one file's
	// size while sending another's bytes. os.File.Stat is fstat on the
	// descriptor, which is the distinction that matters.
	file, err := os.Open(cli.PathJoin(rootDir, relPath))
	if err != nil {
		return false, &LocalFileError{Err: err}
	}
	defer file.Close()

	info, err := file.Stat()
	if err != nil {
		return false, &LocalFileError{Err: err}
	}
	size := info.Size()

	header := fmt.Sprintf(`{"op": "PUT", "path": %s, "size": %d, "mtime": %.6f}`,
		jsonval.Escape(relPath), size, cli.Mtime(info))
	if err := SendJSON(conn, header); err != nil {
		return false, err
	}

	buf := make([]byte, chunkSize)
	var sent int64
	truncated := false // the file shrank; we padded the rest

	for sent < size {
		want := min(int64(len(buf)), size-sent)
		n, readErr := file.Read(buf[:want])
		if n == 0 {
			if readErr != nil && !errors.Is(readErr, io.EOF) {
				return false, fmt.Errorf("read %s: %w", relPath, readErr)
			}
			// Truncated mid-transfer. We already promised `size` bytes, so pad
			// -- aborting would leave the receiver waiting for bytes that never
			// come and kill the whole session over one file. The padded copy
			// loses the mtime comparison on the next push and gets resent, so
			// this self-heals.
			clear(buf[:want])
			n = int(want)
			truncated = true
		}
		if err := conn.SendAll(buf[:n]); err != nil {
			return false, err
		}
		sent += int64(n)
	}

	// If the file GREW we simply stop at `size` and never read the tail -- the
	// frame is still exactly as long as advertised.
	grew := false
	if after, err := file.Stat(); err == nil && after.Size() != size {
		grew = true
	}

	return truncated || grew, nil
}

// SendDelete asks the peer to remove one file (no body).
func SendDelete(conn *netx.Conn, relPath string) error {
	return SendJSON(conn, `{"op": "DELETE", "path": `+jsonval.Escape(relPath)+`}`)
}

// ---- receiving a file -----------------------------------------------------

type PutErrorKind int

const (
	PutMissingField PutErrorKind = iota // Detail names the field
	PutOutOfRange                       // Detail names the field
	PutUnsafePath                       // Detail is the path, as the peer spelled it
)

// PutError is a PUT we refuse on its header. Each kind ends the session,
// because the body length is either unknown or not to be trusted -- there is
// no way to find the next frame boundary.
type PutError struct {
	Kind    PutErrorKind
	Detail  string
	Message string
}

func (e *PutError) Error() string { return e.Message }

func missingField(field string) *PutError {
	return &PutError{
		Kind:    PutMissingField,
		Detail:  field,
		Message: fmt.Sprintf("PUT header missing field: '%s'", field),
	}
}

func outOfRange(field string) *PutError {
	return &PutError{
		Kind:    PutOutOfRange,
		Detail:  field,
		Message: fmt.Sprintf("PUT header has an out-of-range '%s'", field),
	}
}

func unsafePath(relPath string) *PutError {
	return &PutError{
		Kind:    PutUnsafePath,
		Detail:  relPath,
		Message: fmt.Sprintf("unsafe path refused: '%s'", relPath),
	}
}

// RecvFileBody writes the raw body that follows an already-parsed PUT header
// and returns the relative path it landed at.
func RecvFileBody(conn *netx.Conn, destDir string, header jsonval.Object) (string, error) {
	relPath, ok := header.Str("path")
	if !ok {
		return "", missingField("path")
	}

	// Absent and present-but-unusable are different bugs, so they get
	// different messages: a peer that forgot the field, versus one that sent
	// `true`, "5" or 1e999. Go's type switch makes the second case fall out
	// for free -- a bool simply isn't a json.Number -- where Python needed an
	// explicit isinstance(bool) exclusion, because its bool is a subclass of
	// int and `true` compared equal to 1.
	for _, field := range []string{"size", "mtime"} {
		if _, present := header.Find(field); !present {
			return "", missingField(field)
		}
	}

	sizeFloat, sizeIsNumber := header.Number("size")
	mtime, mtimeIsNumber := header.Number("mtime")

	if !sizeIsNumber || !jsonval.Finite(sizeFloat) || sizeFloat < 0 || sizeFloat > MaxFileSize {
		return "", outOfRange("size")
	}
	if !mtimeIsNumber || !jsonval.Finite(mtime) || mtime < -MaxMtime || mtime > MaxMtime {
		return "", outOfRange("mtime")
	}
	size := int64(sizeFloat)

	// Check the path BEFORE creating anything: a hand-crafted "../../x" used
	// to be joined onto destDir and written unchecked, giving any
	// authenticated peer an arbitrary file write on the receiver. MkdirAll on
	// "../evil/x" would make directories outside destDir before any
	// resolution got a look in.
	if !paths.IsLexicallySafe(relPath) {
		return "", unsafePath(relPath)
	}

	fullPath := cli.PathJoin(destDir, relPath)
	if dir := cli.PathDirname(fullPath); dir != "" {
		if err := os.MkdirAll(dir, 0o777); err != nil {
			return "", fmt.Errorf("mkdir %s: %w", dir, err)
		}
	}

	// The parent exists now, so the symlink-aware guard can resolve it.
	safe := paths.Safe(destDir, relPath)
	if safe == "" {
		return "", unsafePath(relPath)
	}

	// Land the bytes beside the target, then rename. Opening the target
	// directly truncates the existing copy the moment the transfer starts, so
	// a peer that dies mid-file leaves a stub where a good file used to be.
	// os.Rename is atomic within a filesystem, which is why the temp is a
	// sibling.
	//
	// os.CreateTemp would pick the name for us and is deliberately not used:
	// the name has to be a sibling of the target for the rename to stay
	// atomic, and keeping the other ports' "<target>.<pid>.tmp" spelling means
	// a stray temp left by any implementation is recognisable.
	tmpPath := fmt.Sprintf("%s.%d.tmp", safe, os.Getpid())
	tmp, err := os.OpenFile(tmpPath, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o666)
	if err != nil {
		return "", fmt.Errorf("open %s: %w", tmpPath, err)
	}

	// defer is doing what C++ needed a TempFile class with a commit() for, and
	// what C spelled out as close/unlink at six separate early returns: every
	// failure path below removes the temp, including a panic, because the
	// cleanup is attached to the function rather than to each exit.
	committed := false
	defer func() {
		tmp.Close()
		if !committed {
			os.Remove(tmpPath)
		}
	}()

	buf := make([]byte, chunkSize)
	var received int64
	for received < size {
		want := min(int64(len(buf)), size-received)
		n, err := conn.RecvSome(buf[:want])
		if err != nil {
			return "", err
		}
		if n == 0 {
			return "", errors.New("peer closed mid-file - truncated transfer")
		}
		if _, err := tmp.Write(buf[:n]); err != nil {
			return "", fmt.Errorf("write %s: %w", tmpPath, err)
		}
		received += int64(n)
	}

	if err := tmp.Close(); err != nil {
		return "", fmt.Errorf("write %s: %w", tmpPath, err)
	}

	// Stamp the mtime before the rename, so the file is never briefly visible
	// at its final path with the wrong timestamp (which the next diff would
	// read as "changed" and resend).
	stamp := cli.MtimeToTime(mtime)
	if err := os.Chtimes(tmpPath, stamp, stamp); err != nil {
		return "", fmt.Errorf("utimes %s: %w", tmpPath, err)
	}

	if err := os.Rename(tmpPath, safe); err != nil {
		return "", fmt.Errorf("rename onto %s: %w", safe, err)
	}
	committed = true

	return relPath, nil
}

// RecvFile reads a PUT header, then its body. Only file_receiver uses it; the
// server has already decoded the header by the time it dispatches.
func RecvFile(conn *netx.Conn, destDir string) (string, error) {
	payload, ok, err := RecvMsg(conn, MaxControlFrame)
	if err != nil {
		return "", err
	}
	if !ok {
		return "", io.EOF
	}
	header, err := DecodeHeader(payload)
	if err != nil {
		return "", err
	}
	return RecvFileBody(conn, destDir, header)
}

// ---- handshake ------------------------------------------------------------

// Handshake is the client side of HELLO: send the token and return the
// server's reply object (e.g. {"op": "OK"} or {"op": "ERROR", ...}). ok is
// false when the peer closed instead of replying. The caller decides what to
// do with a non-OK reply.
func Handshake(conn *netx.Conn, token string) (reply jsonval.Object, ok bool, err error) {
	if err := SendJSON(conn, `{"op": "HELLO", "token": `+jsonval.Escape(token)+`}`); err != nil {
		return nil, false, err
	}

	payload, got, err := RecvMsg(conn, MaxControlFrame) // OK or ERROR
	if err != nil || !got {
		return nil, false, err
	}

	value, err := jsonval.Parse(payload)
	if err != nil {
		return nil, true, err
	}
	object, isObject := jsonval.AsObject(value)
	if !isObject {
		return nil, true, errors.New("handshake reply is not a JSON object")
	}
	return object, true, nil
}
