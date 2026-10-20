// Package server is the passive side of the protocol. It lives here rather
// than in sync_server's main so the tests can run real sessions in-process, on
// an ephemeral port.
package server

import (
	"errors"
	"fmt"
	"os"
	"time"

	"lanfoldersync/internal/framing"
	"lanfoldersync/internal/jsonval"
	"lanfoldersync/internal/manifest"
	"lanfoldersync/internal/netx"
	"lanfoldersync/internal/paths"
)

// SessionTimeout: the server handles one session at a time, so a peer that
// connects and then says nothing isn't just its own problem -- it locks out
// everyone else until the process is killed. Bound the wait, generously: a
// legitimate peer can be busy walking a large folder between frames.
const SessionTimeout = 300 * time.Second

// Logger receives the session's narration. nil sends it to stdout.
type Logger func(string)

// ServeSession serves one connection until the peer says BYE, errs, or goes
// quiet.
//
// Nothing a peer sends can take the process down, and in Go that is a
// structural claim rather than a catch-all: this function returns errors up to
// here and handles them, and the one thing that could still unwind the stack
// -- a panic from a bug in our own code -- is recovered by the caller in
// sync_server, per session. (Python needs a bare `except Exception` at the
// accept loop for the same reason; C++ has a catch-all at the session
// boundary.)
func ServeSession(conn *netx.Conn, sharedDir, token string, idleTimeout time.Duration, logFn Logger) {
	log := func(line string) {
		if logFn != nil {
			logFn(line)
		} else {
			fmt.Println(line)
		}
	}
	logf := func(format string, args ...any) { log(fmt.Sprintf(format, args...)) }

	// Refuse, then end the session. The ERROR is best-effort: the peer may
	// already be gone, and there is nothing more to do about that.
	refuse := func(message string) {
		_ = framing.SendError(conn, message)
	}

	conn.SetTimeout(idleTimeout)
	authenticated := false

	for {
		// Everything a client sends us is a short header; the big frame in
		// this protocol only ever travels the other way.
		payload, ok, err := framing.RecvMsg(conn, framing.MaxControlFrame)
		if err != nil {
			var tooLarge *framing.FrameTooLargeError
			switch {
			case errors.As(err, &tooLarge):
				logf("Refused oversized frame (%d bytes, cap %d)",
					tooLarge.Length, framing.MaxControlFrame)
				refuse("frame too large")
			case errors.Is(err, netx.ErrTimeout):
				logf("Session timed out: peer idle for %.0fs, dropping it", idleTimeout.Seconds())
			default:
				logf("Session error: %v", err)
			}
			return
		}
		if !ok {
			return // peer closed
		}

		header, err := framing.DecodeHeader(payload)
		if err != nil {
			refuse(err.Error())
			return
		}

		op, opIsString := header.Str("op")

		if !authenticated {
			if !opIsString || op != "HELLO" {
				refuse("auth required: send HELLO first")
				return
			}
			got, tokenIsString := header.Str("token")
			// Go's string == is a byte comparison, which is what the
			// filesystem and every other implementation of this protocol
			// mean. (Swift's String == would accept a differently normalised
			// spelling of the same token; there is nothing to opt out of
			// here.)
			if !tokenIsString || got != token {
				refuse("bad token")
				return
			}

			authenticated = true
			if err := framing.SendJSON(conn, `{"op": "OK"}`); err != nil {
				logf("Session error: %v", err)
				return
			}
			log("     HELLO ok, session authenticated")
			continue
		}

		switch {
		case opIsString && op == "MANIFEST":
			// Partial here is far less dangerous than partial on the pusher --
			// files we fail to list just get re-sent, never deleted -- but say
			// so, because it is the same underlying problem.
			m := manifest.Scan(sharedDir)
			if err := framing.SendJSON(conn, `{"op": "MANIFEST", "files": `+m.ToJSON()+`}`); err != nil {
				logf("Session error: %v", err)
				return
			}

			switch {
			case !m.Complete:
				logf("Sent manifest (%d files) - WARNING: some directories under %s "+
					"could not be read; the list is incomplete", len(m.Files), sharedDir)
			case len(m.SkippedLinks) > 0:
				logf("Sent manifest (%d files, %d symlink%s skipped)",
					len(m.Files), len(m.SkippedLinks), plural(len(m.SkippedLinks)))
			default:
				logf("Sent manifest (%d files)", len(m.Files))
			}

		case opIsString && op == "PUT":
			relPath, err := framing.RecvFileBody(conn, sharedDir, header)
			if err != nil {
				var putErr *framing.PutError
				switch {
				case errors.As(err, &putErr):
					// Every kind ends the session: the body length is unknown
					// or untrustworthy, so there is no next frame boundary.
					refuse(putErr.Message)
				case errors.Is(err, netx.ErrTimeout):
					logf("Session timed out: peer stalled mid-file after %.0fs", idleTimeout.Seconds())
				default:
					logf("Session error: %v", err)
				}
				return
			}
			logf("    received %s", relPath)

		case opIsString && op == "BYE":
			log("Peer said BYE")
			return

		case opIsString && op == "DELETE":
			raw, present := header.Find("path")
			if !present || raw == nil {
				refuse("DELETE missing 'path'")
				return
			}

			// One shared guard for PUT and DELETE. The check this replaced
			// resolved the whole path at once, which also let a path resolving
			// to the shared folder ITSELF through.
			relPath, isString := header.Str("path")
			target := ""
			if isString {
				target = paths.Safe(sharedDir, relPath)
			}
			if target == "" {
				refuse(fmt.Sprintf("unsafe path refused: %s", jsonval.PyRepr(raw, true)))
				return
			}

			if err := os.Remove(target); err == nil {
				logf("    deleted %s", relPath)
			} else {
				logf("    already gone %s", relPath) // delete is idempotent
			}

		default:
			rawOp, present := header.Find("op")
			refuse(fmt.Sprintf("unknown op: %s", jsonval.PyRepr(rawOp, present)))
			return
		}
	}
}

func plural(n int) string {
	if n == 1 {
		return ""
	}
	return "s"
}
