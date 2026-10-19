package netx_test

import (
	"errors"
	"testing"
	"time"

	"lanfoldersync/internal/netx"
	"lanfoldersync/internal/testsupport"
)

// The subtlest difference between Go and the other ports. net.Conn deadlines
// are ABSOLUTE; setsockopt(SO_RCVTIMEO), which C, C++ and Python all use, is
// PER-CALL. Setting the deadline once at the start of a session would give the
// whole session 300 seconds instead of giving each read 300 seconds -- so a
// long, perfectly healthy transfer would be cut off mid-file. Conn refreshes
// the deadline before every operation; this is the test that says so.
func TestTimeoutIsPerOperationNotPerSession(t *testing.T) {
	a, b := testsupport.ConnPair(t)

	const timeout = 300 * time.Millisecond
	const gap = 150 * time.Millisecond
	const sends = 6 // 900ms total: three times the timeout

	b.SetTimeout(timeout)

	errs := make(chan error, 1)
	go func() {
		for i := 0; i < sends; i++ {
			time.Sleep(gap)
			if err := a.SendAll([]byte{byte(i)}); err != nil {
				errs <- err
				return
			}
		}
		errs <- nil
	}()

	// Each read waits less than the timeout, so each must succeed -- even
	// though the total elapsed time is well past it.
	for i := 0; i < sends; i++ {
		buf := make([]byte, 1)
		if _, err := b.RecvExactly(buf); err != nil {
			t.Fatalf("read %d failed after %v total: %v -- the deadline is not being refreshed",
				i, time.Duration(i+1)*gap, err)
		}
		if buf[0] != byte(i) {
			t.Fatalf("read %d got %d", i, buf[0])
		}
	}

	if err := <-errs; err != nil {
		t.Fatalf("writer: %v", err)
	}
}

// ...and the timeout still fires when the peer really does go quiet.
func TestTimeoutFiresOnASilentPeer(t *testing.T) {
	_, b := testsupport.ConnPair(t)
	b.SetTimeout(100 * time.Millisecond)

	started := time.Now()
	buf := make([]byte, 1)
	_, err := b.RecvExactly(buf)
	if !errors.Is(err, netx.ErrTimeout) {
		t.Fatalf("expected ErrTimeout, got %v", err)
	}
	if elapsed := time.Since(started); elapsed > time.Second {
		t.Fatalf("the timeout took %v to fire", elapsed)
	}
}

// A zero timeout means "no deadline", as Python's settimeout(None) does.
func TestZeroTimeoutClearsTheDeadline(t *testing.T) {
	a, b := testsupport.ConnPair(t)

	b.SetTimeout(50 * time.Millisecond)
	b.SetTimeout(0) // cleared

	go func() {
		time.Sleep(200 * time.Millisecond) // well past the old deadline
		a.SendAll([]byte{42})
	}()

	buf := make([]byte, 1)
	if _, err := b.RecvExactly(buf); err != nil {
		t.Fatalf("a cleared deadline still expired: %v", err)
	}
	if buf[0] != 42 {
		t.Fatalf("got %d", buf[0])
	}
}

// Connecting to a port nobody is listening on is REFUSED, not dropped -- and
// the pusher prints opposite advice for the two, so the classification matters
// more than the error text.
func TestDialClassifiesARefusedConnection(t *testing.T) {
	// Take a port, then drop it, so we know nothing is behind it.
	listener, err := netx.Listen("127.0.0.1", 0)
	if err != nil {
		t.Fatal(err)
	}
	port := listener.Port()
	listener.Close()

	_, err = netx.Dial("127.0.0.1", port, 2*time.Second)
	var connectErr *netx.ConnectError
	if !errors.As(err, &connectErr) {
		t.Fatalf("expected a ConnectError, got %v", err)
	}
	if connectErr.Kind != netx.ConnectRefused {
		t.Fatalf("expected ConnectRefused, got kind %d (%v)", connectErr.Kind, err)
	}
}

func TestDialClassifiesAnUnresolvableHost(t *testing.T) {
	// .invalid is reserved by RFC 2606 and never resolves.
	_, err := netx.Dial("nothing.invalid", 8765, 2*time.Second)
	var connectErr *netx.ConnectError
	if !errors.As(err, &connectErr) {
		t.Fatalf("expected a ConnectError, got %v", err)
	}
	if connectErr.Kind != netx.ConnectResolve {
		t.Fatalf("expected ConnectResolve, got kind %d (%v)", connectErr.Kind, err)
	}
}

func TestSameSubnet(t *testing.T) {
	same := [][2]string{
		{"192.168.1.7", "192.168.1.42"},
		{"10.0.0.1", "10.0.0.2"},
		// Not literal IPv4 on one side: we can't tell, so we don't cry wolf.
		{"localhost", "192.168.1.42"},
		{"192.168.1.7", "receiver.local"},
	}
	for _, pair := range same {
		if !netx.SameSubnet(pair[0], pair[1]) {
			t.Errorf("SameSubnet(%q, %q) = false", pair[0], pair[1])
		}
	}

	different := [][2]string{
		{"192.168.1.7", "192.168.2.42"},
		{"192.168.1.7", "10.0.0.1"},
	}
	for _, pair := range different {
		if netx.SameSubnet(pair[0], pair[1]) {
			t.Errorf("SameSubnet(%q, %q) = true", pair[0], pair[1])
		}
	}
}

// The ephemeral port is what lets the suites run real sessions without needing
// 8765 free.
func TestListenOnPortZeroReportsTheRealPort(t *testing.T) {
	listener, err := netx.Listen("127.0.0.1", 0)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()

	if listener.Port() <= 0 {
		t.Fatalf("Port() = %d", listener.Port())
	}
}
