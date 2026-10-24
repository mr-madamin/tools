// The one-file experiment, as in Python: connect, send one file as a PUT, hang
// up. No handshake and no manifest -- its peer is file_receiver, not
// sync_server, which would refuse an unauthenticated PUT.
package main

import (
	"fmt"
	"os"
	"time"

	"lanfoldersync/internal/cli"
	"lanfoldersync/internal/framing"
	"lanfoldersync/internal/netx"
)

const usage = "usage: file_sender [host] [path]"

func main() {
	args := cli.ParseArgs(os.Args[1:], []string{"--help"}, 2)
	if args.Has("--help") {
		fmt.Println(usage)
		return
	}
	if len(args.Unknown) > 0 {
		cli.DieUnknownFlags(args.Unknown, usage)
	}

	host := "127.0.0.1"
	if given, ok := args.At(0); ok {
		host = given
	}
	path := "bigfile.bin"
	if given, ok := args.At(1); ok {
		path = given
	}

	conn, err := netx.Dial(host, 8765, 8*time.Second)
	if err != nil {
		cli.Die(err.Error())
	}
	defer conn.Close()
	conn.SetTimeout(300 * time.Second)

	changed, err := framing.SendFile(conn, ".", path)
	if err != nil {
		cli.Die(err.Error())
	}
	if changed {
		fmt.Printf("  ! %s changed while being sent; the copy is padded or clipped\n", path)
	}
	fmt.Printf("Sent %s\n", path)
}
