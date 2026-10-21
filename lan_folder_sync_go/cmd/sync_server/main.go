// The passive side: bind, accept, authenticate, then do exactly what the peer
// asks -- serve a manifest, accept a PUT, remove a DELETE. It never decides
// anything about the sync; the pusher is the brain.
package main

import (
	"fmt"
	"os"

	"lanfoldersync/internal/cli"
	"lanfoldersync/internal/config"
	"lanfoldersync/internal/netx"
	"lanfoldersync/internal/server"
)

const usage = "usage: sync_server [shared_dir] [port] [--lan]"

const help = `  --lan    bind this Mac's en0 IPv4 instead of 127.0.0.1, so
           peers on the LAN can reach it. Exits if en0 has no
           address rather than quietly serving loopback.
  --help   this message

shared_dir defaults to config.json shared_dir, port to
peer.port. Without --lan the server binds 127.0.0.1.`

func main() {
	// No stdout buffering to arrange: Go's os.Stdout is unbuffered, so the
	// startup banner and every session line reach a redirected log file as
	// they are printed. Python needs sys.stdout.reconfigure(line_buffering=True)
	// here and C needs setvbuf(stdout, NULL, _IOLBF, 0), both for the same
	// reason -- a block-buffered stdout leaves the log completely empty until
	// the process exits.
	//
	// Nor is there a SIGPIPE handler: a write to a vanished peer comes back as
	// an EPIPE error value rather than killing the process.

	args := cli.ParseArgs(os.Args[1:], []string{"--lan", "--help"}, 2)
	if args.Has("--help") {
		fmt.Printf("%s\n\n%s\n", usage, help)
		return
	}
	if len(args.Unknown) > 0 {
		cli.DieUnknownFlags(args.Unknown, usage)
	}

	cfg := config.Load(os.Args[0])

	sharedDir := "sandbox/received"
	if given, ok := args.At(0); ok {
		sharedDir = given
	} else if cfg.SharedDir != "" {
		sharedDir = cfg.SharedDir
	}

	port := 8765
	if given, ok := args.At(1); ok {
		port = cli.ParsePort(given, "port argument")
	} else if cfg.PeerPort != 0 {
		port = cfg.PeerPort
	}

	bindHost := "127.0.0.1"
	if args.Has("--lan") {
		ip := netx.LanIP()
		if ip == "" {
			cli.Die("--lan: no en0 IPv4 found (offline, or Wi-Fi isn't en0). " +
				"Join the LAN, or drop --lan to bind 127.0.0.1")
		}
		bindHost = ip
	}

	listener, err := netx.Listen(bindHost, port)
	if err != nil {
		cli.Die(err.Error())
	}

	fmt.Printf("Serving %s/ on %s:%d ... (Ctrl-C to stop)\n", sharedDir, bindHost, port)
	if bindHost != "127.0.0.1" {
		fmt.Printf("  → peers: set config.json peer.host to '%s'\n", bindHost)
	}

	for {
		conn, peerIP, peerPort, err := listener.Accept()
		if err != nil {
			cli.Die(err.Error())
		}
		fmt.Printf("Connected from %s:%d\n", peerIP, peerPort)

		serve(conn, sharedDir, cfg.Token)
		conn.Close()
		fmt.Println("Session ended, waiting for next peer")
	}
}

// serve runs one session with a panic guard around it. ServeSession returns
// its failures rather than throwing, so this is not where protocol errors are
// handled -- it is the last line of defence against a bug in our own code
// taking the whole server down with one peer's unusual frame. The accept loop
// must outlive every session.
func serve(conn *netx.Conn, sharedDir, token string) {
	defer func() {
		if recovered := recover(); recovered != nil {
			fmt.Printf("Session error: panic: %v\n", recovered)
		}
	}()
	server.ServeSession(conn, sharedDir, token, server.SessionTimeout, nil)
}
