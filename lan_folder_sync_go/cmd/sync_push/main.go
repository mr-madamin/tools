// The brain: read the local folder, ask the peer for its manifest, diff the
// two, and decide every PUT and DELETE. --dry-run prints the plan and sends
// nothing; --delete turns "the peer has extras" into actual removals.
package main

import (
	"errors"
	"fmt"
	"os"
	"strings"
	"time"

	"lanfoldersync/internal/cli"
	"lanfoldersync/internal/config"
	"lanfoldersync/internal/framing"
	"lanfoldersync/internal/jsonval"
	"lanfoldersync/internal/manifest"
	"lanfoldersync/internal/netx"
)

// Without these the program blocks in the kernel with nothing on screen: a
// dropped SYN takes ~75s to fail on macOS, and a peer that accepts and then
// says nothing waits forever. Same three phases as sync_push.py.
const (
	connectTimeout  = 8 * time.Second   // TCP handshake
	protocolTimeout = 15 * time.Second  // HELLO / MANIFEST reply
	transferTimeout = 300 * time.Second // any single stalled send during the transfer
)

const usage = "usage: sync_push [peer_host] [root_dir] [--delete | --dry-run]"

const help = `  --dry-run   print the plan (puts and deletes), send nothing
  --delete    also remove files the peer has and the source
              doesn't, making the peer an exact mirror
  --help      this message

peer_host defaults to config.json peer.host, root_dir to
shared_dir. The port always comes from config.json peer.port.`

// hint is Python's hint(): a warning that isn't fatal, set off from the normal
// log.
func hint(text string) {
	fmt.Printf("  ! %s\n", text)
}

func hintf(format string, args ...any) {
	hint(fmt.Sprintf(format, args...))
}

func human(n float64) string {
	for _, unit := range []string{"B", "KB", "MB", "GB"} {
		if n < 1024 || unit == "GB" {
			if unit == "B" {
				return fmt.Sprintf("%.0f B", n)
			}
			return fmt.Sprintf("%.1f %s", n, unit)
		}
		n /= 1024
	}
	return ""
}

func main() {
	args := cli.ParseArgs(os.Args[1:], []string{"--dry-run", "--delete", "--help"}, 2)
	if args.Has("--help") {
		fmt.Printf("%s\n\n%s\n", usage, help)
		return
	}
	if len(args.Unknown) > 0 {
		// A typo like --dryrun would otherwise push for real, silently.
		cli.DieUnknownFlags(args.Unknown, usage)
	}

	dryRun := args.Has("--dry-run")
	deleteExtras := args.Has("--delete")

	cfg := config.Load(os.Args[0])

	host := "127.0.0.1"
	if given, ok := args.At(0); ok {
		host = given
	} else if cfg.PeerHost != "" {
		host = cfg.PeerHost
	}

	port := 8765
	if cfg.PeerPort != 0 {
		port = cfg.PeerPort
	}

	rootDir := "sandbox/source"
	if given, ok := args.At(1); ok {
		rootDir = given
	} else if cfg.SharedDir != "" {
		rootDir = cfg.SharedDir
	}

	fmt.Printf("Push %s  ->  %s:%d\n", rootDir, host, port)

	// Both of these end in an empty local manifest, which with --delete reads
	// as "the source has nothing, so remove everything" -- check before we
	// connect.
	if strings.HasPrefix(rootDir, "~") {
		cli.Dief("root_dir '%s' starts with '~', which is NOT expanded here --\n"+
			"it would walk an empty folder and push nothing. Use an absolute path.", rootDir)
	}
	if info, err := os.Stat(rootDir); err != nil || !info.IsDir() {
		cwd, cwdErr := os.Getwd()
		if cwdErr != nil {
			cwd = "?"
		}
		cli.Dief("root_dir does not exist: %s\n(cwd: %s) -- check the path, or mkdir -p it.",
			cli.PathAbs(rootDir), cwd)
	}

	// Preflight network sanity, before we spend 8 seconds finding out the hard
	// way. Loopback skips it: the answers would all be about the LAN.
	if !isLoopback(host) {
		myIP := netx.LanIP()
		switch {
		case myIP == "":
			hint("this Mac has no en0 IPv4 (offline, or Wi-Fi isn't en0) -- a LAN push")
			hint("will not get anywhere until it's on the network")
		case myIP == host:
			hintf("peer host %s is THIS machine's own IP -- pushing to yourself?", host)
			hint("the peer IP must come from the RECEIVER's 'ipconfig getifaddr en0'")
		case !netx.SameSubnet(myIP, host):
			hintf("this Mac is %s, the peer is %s -- different subnets", myIP, host)
			hint("same Wi-Fi network on both? (a guest network is a separate subnet)")
		default:
			fmt.Printf("  this Mac: %s (same subnet as the peer)\n", myIP)
		}
	}

	fmt.Printf("Connecting (timeout %.0fs) ...\n", connectTimeout.Seconds())
	started := time.Now()
	conn, err := netx.Dial(host, port, connectTimeout)
	if err != nil {
		var connectErr *netx.ConnectError
		if errors.As(err, &connectErr) {
			dieConnectFailure(connectErr, host, port)
		}
		cli.Die(err.Error())
	}
	defer conn.Close()
	fmt.Printf("  connected in %.2fs\n", time.Since(started).Seconds())

	// Connected. From here a wedged peer is the risk, not an unreachable one.
	conn.SetTimeout(protocolTimeout)

	reply, ok, err := framing.Handshake(conn, cfg.Token)
	if err != nil {
		if errors.Is(err, netx.ErrTimeout) {
			dieSilentPeer(host, port, "HELLO")
		}
		cli.Dief("peer went away during handshake: %v", err)
	}
	if !ok {
		cli.Die("peer closed during handshake (wrong token?)")
	}
	if op, _ := reply.Str("op"); op != "OK" {
		message, present := reply.Find("message")
		cli.Dief("handshake refused: %s", plainMessage(message, present))
	}
	fmt.Println("  handshake ok (token accepted)")

	local := manifest.Scan(rootDir)

	if err := framing.SendJSON(conn, `{"op": "MANIFEST"}`); err != nil {
		cli.Dief("peer went away asking for the manifest: %v", err)
	}
	payload, ok, err := framing.RecvMsg(conn, framing.MaxFrame) // the manifest: not the control cap
	if err != nil {
		var tooLarge *framing.FrameTooLargeError
		switch {
		case errors.Is(err, netx.ErrTimeout):
			dieSilentPeer(host, port, "MANIFEST")
		case errors.As(err, &tooLarge):
			cli.Dief("peer's manifest frame is %d bytes, over the %d cap --\n"+
				"refusing to read it. That's not a folder listing; check what is\n"+
				"actually listening on %s:%d.", tooLarge.Length, framing.MaxFrame, host, port)
		default:
			cli.Dief("peer went away before the manifest arrived: %v", err)
		}
	}
	if !ok {
		cli.Die("Connection closed by peer before manifest was received")
	}

	message, err := framing.DecodeHeader(payload)
	if err != nil {
		cli.Die("peer sent a manifest we can't parse")
	}
	rawFiles, present := message.Find("files")
	remote, isManifest := manifest.FromJSON(rawFiles, present)
	if !isManifest {
		text, textPresent := message.Find("message")
		cli.Dief("peer's manifest has no \"files\": %s", plainMessage(text, textPresent))
	}

	plan := manifest.Diff(local, remote, manifest.MtimeTolerance)

	if dryRun {
		fmt.Println("DRY RUN - no files will be sent")
	}
	fmt.Printf("Local: %d files | Peer: %d files\n", len(local.Files), len(remote.Files))

	// Not an error -- we chose not to follow them -- but a user who symlinked
	// a folder in here is otherwise told nothing about why it never syncs.
	if len(local.SkippedLinks) > 0 {
		hintf("skipped %d symlink%s -- a link to a directory isn't followed",
			len(local.SkippedLinks), plural(len(local.SkippedLinks)))
		hint("(same as Python's os.walk), and a broken link can't be read:")
		for _, link := range local.SkippedLinks {
			hintf("    %s", link)
		}
		hint("copy the folder in, or sync it separately, if you want it mirrored")
	}

	// A directory we could not read is not the same as a directory with
	// nothing in it: the files are there, we just can't see them. Deleting on
	// the peer from a picture we KNOW has holes is the empty-source wipe
	// wearing a different hat.
	if !local.Complete {
		hintf("could not read every directory under %s", rootDir)
		hint("the file list above is INCOMPLETE -- anything unreadable is")
		hint("missing from it (paths past PATH_MAX do this silently)")
		if deleteExtras && !dryRun {
			cli.Die("refusing to run --delete from a partial source listing: files\n" +
				"this scan could not see look identical to files you deleted,\n" +
				"and would be removed from the peer.")
		}
	}

	if len(local.Files) == 0 {
		hintf("no files found under %s", cli.PathAbs(rootDir))
		hint("is that the right folder? nothing will be sent")
		if deleteExtras && !dryRun {
			cli.Die("refusing to run --delete from an empty source: it would wipe the peer.")
		}
	}

	// A big file legitimately takes a while; only a fully stalled send is a
	// failure, so the transfer phase gets a much longer leash.
	conn.SetTimeout(transferTimeout)

	verb := "Sending"
	if dryRun {
		verb = "Would send"
	}
	fmt.Printf("%s %d file(s):\n", verb, len(plan.ToPut))

	var sentBytes int64
	transferStarted := time.Now()
	for _, relPath := range plan.ToPut {
		entry := local.Files[relPath]
		fmt.Printf("   PUT    %s (%s)\n", relPath, human(float64(entry.Size)))
		if dryRun {
			continue
		}

		changed, err := framing.SendFile(conn, rootDir, relPath)
		if err != nil {
			var localErr *framing.LocalFileError
			switch {
			case errors.As(err, &localErr):
				// Nothing has reached the socket for this file, so the stream
				// is still in sync: report it and keep going rather than
				// abandon the whole push over one file that vanished mid-run.
				hintf("%v -- skipping it", localErr)
				continue
			case errors.Is(err, netx.ErrTimeout):
				cli.Dief("stalled for %.0fs while sending '%s'.\n"+
					"The peer stopped reading -- server killed, or the Wi-Fi dropped.",
					transferTimeout.Seconds(), relPath)
			default:
				cli.Dief("peer went away while sending '%s' (%v).\n"+
					"Check the server's terminal for the error it printed.", relPath, err)
			}
		}
		if changed {
			hintf("%s changed while it was being sent -- the copy on the peer is", relPath)
			hint("padded or clipped to the size we announced. Re-run to fix it.")
			// The stream is still in sync; keep going.
		}
		sentBytes += entry.Size
	}

	if len(plan.ToDelete) > 0 {
		verb := "Skipping (need --delete):"
		switch {
		case dryRun:
			verb = "Would delete"
		case deleteExtras:
			verb = "Deleting"
		}
		fmt.Printf("%s %d file(s):\n", verb, len(plan.ToDelete))
		for _, relPath := range plan.ToDelete {
			fmt.Printf("   DELETE %s\n", relPath)
			if !dryRun && deleteExtras {
				if err := framing.SendDelete(conn, relPath); err != nil {
					cli.Dief("peer went away while deleting '%s' (%v).", relPath, err)
				}
			}
		}
	}

	if err := framing.SendJSON(conn, `{"op": "BYE"}`); err != nil {
		cli.Dief("peer went away before BYE: %v", err)
	}
	conn.Close()

	if !dryRun && sentBytes > 0 {
		elapsed := time.Since(transferStarted).Seconds()
		rate := float64(sentBytes) / max(elapsed, 0.001)
		fmt.Printf("Done: %s in %.1fs (%s/s)\n", human(float64(sentBytes)), elapsed, human(rate))
	}
}

func isLoopback(host string) bool {
	switch host {
	case "127.0.0.1", "localhost", "::1":
		return true
	}
	return false
}

func plural(n int) string {
	if n == 1 {
		return ""
	}
	return "s"
}

// plainMessage prints a peer-supplied "message" field the way Python's
// .get("message") would read in an f-string: the text itself, or None.
func plainMessage(value any, present bool) string {
	if text, ok := value.(string); ok && present {
		return text
	}
	return jsonval.PyRepr(value, present)
}

// dieSilentPeer: the TCP connection is up but the peer never answered -- a
// different failure from "can't get there", and it points at the other end,
// not the network.
func dieSilentPeer(host string, port int, stage string) {
	cli.Dief("Connected to %s:%d, but the peer never answered %s within %.0fs.\n\n"+
		"The TCP connection is up, so this is the other end misbehaving:\n"+
		"  - Is something ELSE listening on that port (not sync_server)?\n"+
		"  - Is the server wedged in an earlier session? Restart it.",
		host, port, stage, protocolTimeout.Seconds())
}

// dieConnectFailure: each way of failing to connect gets its own page, because
// they need opposite fixes. ConnectError carries the kind as a value, so this
// switch is the one place that has to know them all.
func dieConnectFailure(failure *netx.ConnectError, host string, port int) {
	switch failure.Kind {
	case netx.ConnectResolve:
		cli.Dief("can't resolve peer host '%s': %v", host, failure.Err)

	case netx.ConnectTimeout:
		// Nothing came back from the SYN: the packet is being dropped, not
		// refused.
		lines := []string{
			fmt.Sprintf("No answer from %s:%d after %.0fs.", host, port, connectTimeout.Seconds()),
			"",
			"The connection request went out and nothing came back -- not even a",
			"refusal. That means something is DROPPING the packet, in this order of",
			"likelihood:",
			"",
			"  1. Firewall on the receiver (most common). System Settings -> Network",
			"     -> Firewall. Allow incoming connections, or turn the firewall off",
			"     while you test. Stealth mode drops silently, exactly like this.",
			"  2. Client isolation on the router (guest network / 'AP isolation').",
			"     Both Macs reach the internet, neither can reach the other.",
			"  3. Wrong or stale IP -- DHCP moved the receiver.",
			"  4. The server isn't running at all.",
			"",
			"Narrow it down from HERE:",
			fmt.Sprintf("  ping -c 3 %-20s # no replies -> network/firewall, not the port", host),
			fmt.Sprintf("  nc -vz %s %-13d # 'succeeded' -> port is open, rerun the push", host, port),
			"",
			"...and on the RECEIVER:",
			"  ipconfig getifaddr en0        # the IP you should be pushing to",
			"  ./bin/sync_server --lan       # must print 'Serving ... on <that IP>'",
			"",
			"The server prints 'Connected from ...' the instant a peer arrives. If it",
			"stays quiet while this side waits, the packet never reached it -- look at",
			"the firewall and the router, not at this program.",
		}
		if tunnels := netx.TunnelInterfaces(); len(tunnels) > 0 {
			lines = append(lines, "",
				fmt.Sprintf("Also: a VPN/tunnel looks active here (%s). If it grabs",
					strings.Join(tunnels, ", ")),
				"the default route it can swallow LAN traffic. Try again with it off.")
		}
		cli.Die(strings.Join(lines, "\n"))

	case netx.ConnectRefused:
		cli.Dief("Connection refused by %s:%d.\n\n"+
			"Good news: the host is reachable and answered. Nothing is listening on\n"+
			"that port for that address.\n\n"+
			"  - Is the server running on the receiver?\n"+
			"  - Was it started with --lan? Without it the server binds 127.0.0.1\n"+
			"    only, and refuses connections arriving on the LAN IP.\n"+
			"  - Do the ports match? This side is using %d (config.json peer.port).",
			host, port, port)

	case netx.ConnectUnreachable:
		cli.Dief("Can't reach %s:%d -- %v.\n\n"+
			"The route doesn't exist: this Mac has no path to that address.\n"+
			"  - Is this Mac on Wi-Fi at all?  ipconfig getifaddr en0\n"+
			"  - Same network as the receiver, and the same subnet?\n"+
			"  - A VPN can take the LAN route away; turn it off and retry.",
			host, port, failure.Err)
	}
	cli.Die("unreachable")
}
