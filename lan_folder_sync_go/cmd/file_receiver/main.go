// The other half of the one-file experiment: accept one connection, receive
// one file, exit. file_sender's peer.
package main

import (
	"fmt"
	"os"
	"time"

	"lanfoldersync/internal/cli"
	"lanfoldersync/internal/framing"
	"lanfoldersync/internal/netx"
)

const dest = "received"

func main() {
	if err := os.MkdirAll(dest, 0o777); err != nil {
		cli.Die(err.Error())
	}

	// 127.0.0.1, where Python's file_receiver binds "" (every interface). This
	// program has no token check at all, so leaving it reachable from the LAN
	// would be an unauthenticated file write for anyone on the network. The
	// path guard still confines it to received/, but that is the second line
	// of defence, not the first.
	listener, err := netx.Listen("127.0.0.1", 8765)
	if err != nil {
		cli.Die(err.Error())
	}
	defer listener.Close()
	fmt.Println("Listening on 8765 ... waiting for a file")

	conn, peerIP, peerPort, err := listener.Accept()
	if err != nil {
		cli.Die(err.Error())
	}
	defer conn.Close()
	fmt.Printf("Connected from %s:%d\n", peerIP, peerPort)
	conn.SetTimeout(300 * time.Second)

	path, err := framing.RecvFile(conn, dest)
	if err != nil {
		cli.Die(err.Error())
	}
	fmt.Printf("Received -> %s\n", path)
}
