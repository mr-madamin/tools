// Package netx wraps net.Conn with the timeout and error-classification
// behaviour this protocol needs.
//
// The protocol is strictly sequential -- read 4 bytes, then N, then exactly
// `size` -- so there is nothing for goroutines to do inside a session. What Go
// changes relative to the C and C++ ports is two things:
//
//  1. net.Conn deadlines are ABSOLUTE, where setsockopt(SO_RCVTIMEO) is
//     per-call. Setting a deadline once at the start of a session would give
//     the whole session 300 seconds rather than giving each read 300 seconds,
//     so a long legitimate transfer would be cut off mid-file. Conn therefore
//     holds a duration and refreshes the deadline before every operation,
//     which reproduces the socket-option semantics the other ports get from
//     the kernel.
//  2. Errors are values with structure already, so "the peer is wedged",
//     "the peer hung up" and "there is no route" don't need inventing -- they
//     need classifying, which is what ConnectError and the sentinels below do.
package netx

import (
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"strings"
	"syscall"
	"time"
)

// ErrTimeout marks "the deadline expired with the peer neither sending nor
// closing". Worth distinguishing: "the peer is wedged" and "the peer hung up"
// need opposite advice, and the pusher prints a different page for each.
var ErrTimeout = errors.New("timed out")

// ErrPeerClosed marks a peer that closed in the middle of something it had
// promised to finish -- notably a file body, where the length was declared up
// front.
var ErrPeerClosed = errors.New("peer closed the connection")

// Conn is a net.Conn with per-operation timeouts.
type Conn struct {
	conn    net.Conn
	timeout time.Duration
}

func NewConn(conn net.Conn) *Conn {
	return &Conn{conn: conn}
}

// SetTimeout bounds every later read and write on this socket, the way
// Python's sock.settimeout() does. Zero clears it.
func (c *Conn) SetTimeout(d time.Duration) {
	c.timeout = d
}

// refresh turns the stored duration into the absolute deadline net.Conn wants.
// Called before each operation: this is the per-call/absolute mismatch being
// bridged, and skipping it on any path is how a session acquires a deadline
// that silently expires mid-transfer.
func (c *Conn) refresh() {
	if c.timeout <= 0 {
		_ = c.conn.SetDeadline(time.Time{})
		return
	}
	_ = c.conn.SetDeadline(time.Now().Add(c.timeout))
}

func (c *Conn) Close() error { return c.conn.Close() }

// RemoteAddr is used only for the server's "Connected from" line.
func (c *Conn) RemoteAddr() net.Addr { return c.conn.RemoteAddr() }

// SendAll writes every byte or returns an error. net.Conn.Write already
// promises to write all of p or fail, unlike POSIX send(), so there is no loop
// here -- but the timeout still has to be refreshed first.
func (c *Conn) SendAll(data []byte) error {
	c.refresh()
	if _, err := c.conn.Write(data); err != nil {
		return classify("send", err)
	}
	return nil
}

// RecvExactly fills buf. It reports false when the peer closed before the
// first byte, which is the one clean way a session ends. A peer that closes
// PART way through is an error: once a frame arrives half-read the stream has
// no boundary left to resync on.
//
// io.ReadFull makes that distinction for us -- io.EOF for nothing at all,
// io.ErrUnexpectedEOF for a short read -- which is the whole of C's
// recv_exactly loop.
func (c *Conn) RecvExactly(buf []byte) (bool, error) {
	c.refresh()
	_, err := io.ReadFull(c.conn, buf)
	switch {
	case err == nil:
		return true, nil
	case errors.Is(err, io.EOF):
		return false, nil // the peer vanished between frames
	case errors.Is(err, io.ErrUnexpectedEOF):
		return false, fmt.Errorf("%w mid-frame", ErrPeerClosed)
	default:
		return false, classify("recv", err)
	}
}

// RecvSome reads at least one byte, returning 0 when the peer closed. The
// body-receive loop wants this rather than RecvExactly so it can write each
// chunk as it arrives instead of buffering a whole file.
func (c *Conn) RecvSome(buf []byte) (int, error) {
	c.refresh()
	n, err := c.conn.Read(buf)
	if n > 0 {
		return n, nil
	}
	switch {
	case err == nil:
		return 0, nil
	case errors.Is(err, io.EOF):
		return 0, nil
	default:
		return 0, classify("recv", err)
	}
}

// classify folds net's error types into our two sentinels, so callers can ask
// errors.Is(err, ErrTimeout) without knowing about os.ErrDeadlineExceeded.
func classify(op string, err error) error {
	if errors.Is(err, os.ErrDeadlineExceeded) {
		return fmt.Errorf("%s: %w", op, ErrTimeout)
	}
	var netErr net.Error
	if errors.As(err, &netErr) && netErr.Timeout() {
		return fmt.Errorf("%s: %w", op, ErrTimeout)
	}
	// A write to a peer that has gone is EPIPE/ECONNRESET. Go returns these as
	// errors rather than raising SIGPIPE, so unlike the C and C++ ports there
	// is no signal handler to install.
	if errors.Is(err, syscall.EPIPE) || errors.Is(err, syscall.ECONNRESET) {
		return fmt.Errorf("%w: %v", ErrPeerClosed, err)
	}
	return fmt.Errorf("%s: %w", op, err)
}

// ---- connecting -----------------------------------------------------------

// ConnectKind is why a connect failed, as a value the pusher can switch on.
// Each kind needs a different page of advice, and "nothing came back at all"
// versus "it answered, refusing" is the single most useful distinction in this
// program.
type ConnectKind int

const (
	ConnectTimeout ConnectKind = iota
	ConnectRefused
	ConnectUnreachable
	ConnectResolve
)

type ConnectError struct {
	Kind ConnectKind
	Err  error
}

func (e *ConnectError) Error() string { return e.Err.Error() }
func (e *ConnectError) Unwrap() error { return e.Err }

// Dial connects with a timeout. net.DialTimeout supplies the timeout that
// POSIX connect() lacks -- the macOS default is ~75 s of silence against a
// dropped SYN -- so the non-blocking-connect-then-poll dance the C port needs
// is one call here.
func Dial(host string, port int, timeout time.Duration) (*Conn, error) {
	address := net.JoinHostPort(host, strconv.Itoa(port))
	conn, err := net.DialTimeout("tcp", address, timeout)
	if err != nil {
		return nil, &ConnectError{Kind: classifyConnect(err), Err: err}
	}
	return NewConn(conn), nil
}

func classifyConnect(err error) ConnectKind {
	var dnsErr *net.DNSError
	if errors.As(err, &dnsErr) {
		return ConnectResolve
	}
	var netErr net.Error
	if errors.As(err, &netErr) && netErr.Timeout() {
		return ConnectTimeout
	}
	switch {
	case errors.Is(err, syscall.ECONNREFUSED):
		return ConnectRefused
	case errors.Is(err, syscall.EHOSTUNREACH), errors.Is(err, syscall.ENETUNREACH),
		errors.Is(err, syscall.ENETDOWN), errors.Is(err, syscall.EHOSTDOWN):
		return ConnectUnreachable
	}
	return ConnectUnreachable
}

// ---- listening ------------------------------------------------------------

// Listener accepts one peer at a time. Port 0 asks the kernel for an ephemeral
// one, which is what lets the tests run real sessions without needing 8765.
type Listener struct {
	listener net.Listener
}

func Listen(host string, port int) (*Listener, error) {
	listener, err := net.Listen("tcp", net.JoinHostPort(host, strconv.Itoa(port)))
	if err != nil {
		return nil, err
	}
	return &Listener{listener: listener}, nil
}

func (l *Listener) Port() int {
	if addr, ok := l.listener.Addr().(*net.TCPAddr); ok {
		return addr.Port
	}
	return 0
}

func (l *Listener) Close() error { return l.listener.Close() }

// Accept blocks for the next peer. Closing the listener makes a blocked Accept
// return immediately, so the test harness can shut a server down without a
// peer ever arriving -- the job C++ needed a poll() loop with a 50 ms timeout
// for.
func (l *Listener) Accept() (*Conn, string, int, error) {
	conn, err := l.listener.Accept()
	if err != nil {
		return nil, "", 0, err
	}
	host, portText, splitErr := net.SplitHostPort(conn.RemoteAddr().String())
	if splitErr != nil {
		return NewConn(conn), conn.RemoteAddr().String(), 0, nil
	}
	port, _ := strconv.Atoi(portText)
	return NewConn(conn), host, port, nil
}

// ---- host facts -----------------------------------------------------------
//
// Nothing here is part of the wire protocol: it exists so a failed push can
// say WHY it failed instead of hanging silently. Python shells out to
// `ipconfig getifaddr en0` and `ifconfig`; net.Interfaces() is the same
// answers without a subprocess, so unlike Python these can't be defeated by a
// missing binary or a 2-second timeout.

// LanIP is this Mac's en0 IPv4 (Wi-Fi, usually), or "" if offline / not on en0.
func LanIP() string {
	iface, err := net.InterfaceByName("en0")
	if err != nil {
		return ""
	}
	if iface.Flags&net.FlagUp == 0 {
		return ""
	}
	addresses, err := iface.Addrs()
	if err != nil {
		return ""
	}
	for _, address := range addresses {
		if ipNet, ok := address.(*net.IPNet); ok {
			if ipv4 := ipNet.IP.To4(); ipv4 != nil {
				return ipv4.String()
			}
		}
	}
	return ""
}

// SameSubnet reports whether two addresses share a /24. Rough, but right for
// home LANs. True when either side isn't a literal IPv4 address: we can't
// tell, so we don't cry wolf.
func SameSubnet(a, b string) bool {
	ipA, ipB := net.ParseIP(a).To4(), net.ParseIP(b).To4()
	if ipA == nil || ipB == nil {
		return true
	}
	return ipA[0] == ipB[0] && ipA[1] == ipB[1] && ipA[2] == ipB[2]
}

// TunnelInterfaces lists up tunnel interfaces carrying IPv4 -- i.e. a VPN is
// probably running. A VPN that claims the default route can swallow LAN
// traffic, which looks exactly like a firewall drop from the push side.
func TunnelInterfaces() []string {
	interfaces, err := net.Interfaces()
	if err != nil {
		return nil
	}
	var found []string
	for _, iface := range interfaces {
		if iface.Flags&net.FlagUp == 0 {
			continue
		}
		if !strings.HasPrefix(iface.Name, "utun") &&
			!strings.HasPrefix(iface.Name, "ppp") &&
			!strings.HasPrefix(iface.Name, "ipsec") {
			continue
		}
		addresses, err := iface.Addrs()
		if err != nil {
			continue
		}
		for _, address := range addresses {
			if ipNet, ok := address.(*net.IPNet); ok && ipNet.IP.To4() != nil {
				found = append(found, iface.Name)
				break
			}
		}
	}
	return found
}
