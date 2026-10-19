// Package testsupport is the harness the suites share: a real server, in this
// process, on an ephemeral port.
//
// Nothing here needs a config.json, a free 8765, or a server started by hand.
// Go makes the whole arrangement cheap -- net.Listen(":0") for the port, a
// goroutine for the accept loop, t.TempDir() for a shared_dir that cleans
// itself up -- which is why these suites can test the real serve_session
// rather than a stub of it. (C++ needs a poll() loop with a 50 ms timeout to
// get an accept loop it can shut down; closing the listener does it here.)
package testsupport

import (
	"encoding/binary"
	"net"
	"strconv"
	"sync"
	"testing"
	"time"

	"lanfoldersync/internal/framing"
	"lanfoldersync/internal/jsonval"
	"lanfoldersync/internal/netx"
	"lanfoldersync/internal/server"
)

const Token = "test-token-not-a-secret"

// Server is a live sync_server serving Dir, with its log captured.
type Server struct {
	Dir  string
	Port int

	listener *netx.Listener
	done     sync.WaitGroup

	mu   sync.Mutex
	logs []string
}

// StartServer serves dir on an ephemeral port until the test ends, with the
// shared test token. idle is the per-operation timeout handed to ServeSession;
// pass 0 for a generous default.
func StartServer(t *testing.T, dir string, idle time.Duration) *Server {
	t.Helper()
	return StartServerWithToken(t, dir, Token, idle)
}

// StartServerWithToken is StartServer for the tests that care what the token
// is -- the byte-comparison one, which has to configure a token it then
// presents a different spelling of.
func StartServerWithToken(t *testing.T, dir, token string, idle time.Duration) *Server {
	t.Helper()
	if idle == 0 {
		idle = 10 * time.Second
	}

	listener, err := netx.Listen("127.0.0.1", 0)
	if err != nil {
		t.Fatalf("listen: %v", err)
	}

	s := &Server{Dir: dir, Port: listener.Port(), listener: listener}

	s.done.Add(1)
	go func() {
		defer s.done.Done()
		for {
			conn, _, _, err := listener.Accept()
			if err != nil {
				return // the listener was closed: the test is over
			}
			server.ServeSession(conn, dir, token, idle, s.record)
			conn.Close()
		}
	}()

	t.Cleanup(func() {
		listener.Close()
		s.done.Wait()
	})
	return s
}

func (s *Server) record(line string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.logs = append(s.logs, line)
}

// Logs returns a copy of what the server has narrated so far.
func (s *Server) Logs() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]string(nil), s.logs...)
}

// Dial opens a raw connection, with no handshake.
func (s *Server) Dial(t *testing.T) *netx.Conn {
	t.Helper()
	conn, err := netx.Dial("127.0.0.1", s.Port, 5*time.Second)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	conn.SetTimeout(5 * time.Second)
	t.Cleanup(func() { conn.Close() })
	return conn
}

// Authed opens a connection and completes the HELLO, failing the test if the
// server refuses it.
func (s *Server) Authed(t *testing.T) *netx.Conn {
	t.Helper()
	conn := s.Dial(t)
	reply, ok, err := framing.Handshake(conn, Token)
	if err != nil || !ok {
		t.Fatalf("handshake: ok=%v err=%v", ok, err)
	}
	if op, _ := reply.Str("op"); op != "OK" {
		t.Fatalf("handshake refused: %v", reply)
	}
	return conn
}

// SendJSON sends one framed JSON document.
func SendJSON(t *testing.T, conn *netx.Conn, document string) {
	t.Helper()
	if err := framing.SendJSON(conn, document); err != nil {
		t.Fatalf("send %s: %v", document, err)
	}
}

// Recv reads one frame and decodes it as a JSON object. ok is false when the
// peer closed instead of replying.
func Recv(t *testing.T, conn *netx.Conn) (reply jsonval.Object, ok bool) {
	t.Helper()
	payload, got, err := framing.RecvMsg(conn, framing.MaxFrame)
	if err != nil {
		t.Fatalf("recv: %v", err)
	}
	if !got {
		return nil, false
	}
	object, err := framing.DecodeHeader(payload)
	if err != nil {
		t.Fatalf("recv: undecodable reply %q: %v", payload, err)
	}
	return object, true
}

// ExpectError asserts the server answered with an ERROR frame, and returns its
// message.
func ExpectError(t *testing.T, conn *netx.Conn, label string) string {
	t.Helper()
	reply, ok := Recv(t, conn)
	if !ok {
		t.Fatalf("%s: server closed WITHOUT sending an ERROR", label)
	}
	op, _ := reply.Str("op")
	if op != "ERROR" {
		t.Fatalf("%s: expected ERROR, got %v", label, reply)
	}
	message, _ := reply.Str("message")
	return message
}

// ExpectClosed asserts the server hung up, which every refusal must do: the
// stream has no boundary left to resync on.
func ExpectClosed(t *testing.T, conn *netx.Conn, label string) {
	t.Helper()
	if _, ok := Recv(t, conn); ok {
		t.Fatalf("%s: server did not close after rejecting", label)
	}
}

// RawFrame builds a length prefix plus a payload, bypassing SendMsg so a test
// can lie about the length or send a partial frame.
func RawFrame(length uint32, payload []byte) []byte {
	frame := make([]byte, 4+len(payload))
	binary.BigEndian.PutUint32(frame[:4], length)
	copy(frame[4:], payload)
	return frame
}

// DialRaw returns a plain net.Conn, for the tests that write bytes no framing
// helper would produce.
func DialRaw(t *testing.T, s *Server) net.Conn {
	t.Helper()
	conn, err := net.DialTimeout("tcp", net.JoinHostPort("127.0.0.1", strconv.Itoa(s.Port)),
		5*time.Second)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	conn.SetDeadline(time.Now().Add(5 * time.Second))
	t.Cleanup(func() { conn.Close() })
	return conn
}

// ConnPair returns two connected sockets, for the send-side contracts that
// need no server at all. A real TCP pair rather than net.Pipe: net.Pipe is
// synchronous and unbuffered, so it would never produce the partial reads and
// coalesced frames these tests exist to provoke.
func ConnPair(t *testing.T) (*netx.Conn, *netx.Conn) {
	t.Helper()

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer listener.Close()

	type accepted struct {
		conn net.Conn
		err  error
	}
	incoming := make(chan accepted, 1)
	go func() {
		conn, err := listener.Accept()
		incoming <- accepted{conn, err}
	}()

	client, err := net.DialTimeout("tcp", listener.Addr().String(), 5*time.Second)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	server := <-incoming
	if server.err != nil {
		t.Fatalf("accept: %v", server.err)
	}

	a, b := netx.NewConn(client), netx.NewConn(server.conn)
	a.SetTimeout(5 * time.Second)
	b.SetTimeout(5 * time.Second)
	t.Cleanup(func() {
		a.Close()
		b.Close()
	})
	return a, b
}
