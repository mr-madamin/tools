import json
import os
import socket
import sys
import time

from config import load_config
from framing import (
    build_manifest,
    diff_manifests,
    handshake,
    recv_msg,
    send_delete,
    send_file,
    send_msg,
)
from netutil import lan_ip, same_subnet, tunnel_interfaces

# Without these the script blocks in the kernel with nothing on screen: a dropped
# SYN takes ~75s to fail on macOS, and a peer that never replies waits forever.
CONNECT_TIMEOUT = 8  # TCP handshake
PROTOCOL_TIMEOUT = 15  # HELLO / MANIFEST reply
TRANSFER_TIMEOUT = 300  # any single stalled send during the transfer

KNOWN_FLAGS = {"--dry-run", "--delete"}
LOOPBACK = {"127.0.0.1", "localhost", "::1"}

flags = {a for a in sys.argv[1:] if a.startswith("--")}
positional = [a for a in sys.argv[1:] if not a.startswith("--")]

unknown = flags - KNOWN_FLAGS
if unknown:
    # A typo like --dryrun would otherwise push for real, silently.
    sys.exit(
        f"unknown flag(s): {' '.join(sorted(unknown))}\n"
        "usage: sync_push.py [peer_host] [root_dir] "
        f"[{' | '.join(sorted(KNOWN_FLAGS))}]"
    )

cfg = load_config()
TOKEN = cfg["token"]

peer = cfg.get("peer", {})

HOST = positional[0] if len(positional) > 0 else peer.get("host", "127.0.0.1")
PORT = peer.get("port", 8765)
ROOT_DIR = (
    positional[1] if len(positional) > 1 else cfg.get("shared_dir", "sandbox/source")
)

dry_run = "--dry-run" in flags
delete = "--delete" in flags


def hint(msg):
    print(f"  ! {msg}")


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024


def no_answer_help():
    """Nothing came back from the SYN: the packet is being dropped, not refused."""
    ping_probe = f"  ping -c 3 {HOST}"
    nc_probe = f"  nc -vz {HOST} {PORT}"
    probe_width = max(len(ping_probe), len(nc_probe)) + 2
    lines = [
        f"No answer from {HOST}:{PORT} after {CONNECT_TIMEOUT}s.",
        "",
        "The connection request went out and nothing came back -- not even a",
        "refusal. That means something is DROPPING the packet, in this order of",
        "likelihood:",
        "",
        "  1. Firewall on the receiver (most common). System Settings -> Network",
        "     -> Firewall. Allow incoming connections for Python, or turn the",
        "     firewall off while you test. Stealth mode drops silently, exactly",
        "     like this.",
        "  2. Client isolation on the router (guest network / 'AP isolation').",
        "     Both Macs reach the internet, neither can reach the other.",
        "  3. Wrong or stale IP -- DHCP moved the receiver.",
        "  4. The server isn't running at all.",
        "",
        "Narrow it down from HERE:",
        f"{ping_probe:<{probe_width}}# no replies -> network/firewall, not the port",
        f"{nc_probe:<{probe_width}}# 'succeeded' -> port is open, rerun the push",
        "",
        "...and on the RECEIVER:",
        "  ipconfig getifaddr en0        # the IP you should be pushing to",
        "  python3 sync_server.py --lan  # must print 'Serving ... on <that IP>'",
        "",
        "The server prints 'Connected from ...' the instant a peer arrives. If it",
        "stays quiet while this side waits, the packet never reached it -- look at",
        "the firewall and the router, not at this script.",
    ]
    tunnels = tunnel_interfaces()
    if tunnels:
        lines += [
            "",
            f"Also: a VPN/tunnel looks active here ({', '.join(tunnels)}). If it grabs",
            "the default route it can swallow LAN traffic. Try again with it off.",
        ]
    return "\n".join(lines)


def refused_help():
    return "\n".join(
        [
            f"Connection refused by {HOST}:{PORT}.",
            "",
            "Good news: the host is reachable and answered. Nothing is listening on",
            "that port for that address.",
            "",
            "  - Is the server running on the receiver?",
            "  - Was it started with --lan? Without it the server binds 127.0.0.1",
            "    only, and refuses connections arriving on the LAN IP.",
            f"  - Do the ports match? This side is using {PORT}"
            " (config.json peer.port).",
        ]
    )


def unreachable_help(err):
    return "\n".join(
        [
            f"Can't reach {HOST}:{PORT} -- {err.strerror or err}.",
            "",
            "The route doesn't exist: this Mac has no path to that address.",
            "  - Is this Mac on Wi-Fi at all?  ipconfig getifaddr en0",
            "  - Same network as the receiver, and the same subnet?",
            "  - A VPN can take the LAN route away; turn it off and retry.",
        ]
    )


def silent_peer_help(stage):
    return "\n".join(
        [
            f"Connected to {HOST}:{PORT}, but the peer never answered {stage}"
            f" within {PROTOCOL_TIMEOUT}s.",
            "",
            "The TCP connection is up, so this is the other end misbehaving:",
            "  - Is something ELSE listening on that port (not sync_server.py)?",
            "  - Is the server wedged in an earlier session? Restart it.",
        ]
    )


print(f"Push {ROOT_DIR}  ->  {HOST}:{PORT}")

if ROOT_DIR.startswith("~"):
    sys.exit(
        f"root_dir {ROOT_DIR!r} starts with '~', which is NOT expanded here --\n"
        "it would walk an empty folder and push nothing. Use an absolute path."
    )
if not os.path.isdir(ROOT_DIR):
    sys.exit(
        f"root_dir does not exist: {os.path.abspath(ROOT_DIR)}\n"
        f"(cwd: {os.getcwd()}) -- check the path, or mkdir -p it."
    )

if HOST not in LOOPBACK:
    my_ip = lan_ip()
    if not my_ip:
        hint("this Mac has no en0 IPv4 (offline, or Wi-Fi isn't en0) -- a LAN push")
        hint("will not get anywhere until it's on the network")
    elif my_ip == HOST:
        hint(f"peer host {HOST} is THIS machine's own IP -- pushing to yourself?")
        hint("the peer IP must come from the RECEIVER's 'ipconfig getifaddr en0'")
    elif not same_subnet(my_ip, HOST):
        hint(f"this Mac is {my_ip}, the peer is {HOST} -- different subnets")
        hint("same Wi-Fi network on both? (a guest network is a separate subnet)")
    else:
        print(f"  this Mac: {my_ip} (same subnet as the peer)")

print(f"Connecting (timeout {CONNECT_TIMEOUT}s) ...", flush=True)
started = time.monotonic()
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(CONNECT_TIMEOUT)
try:
    sock.connect((HOST, PORT))
except TimeoutError:
    sys.exit(no_answer_help())
except ConnectionRefusedError:
    sys.exit(refused_help())
except socket.gaierror as e:
    sys.exit(f"can't resolve peer host {HOST!r}: {e}")
except OSError as e:
    sys.exit(unreachable_help(e))
print(f"  connected in {time.monotonic() - started:.2f}s")

sock.settimeout(PROTOCOL_TIMEOUT)

try:
    reply = handshake(sock, TOKEN)
except TimeoutError:
    sys.exit(silent_peer_help("HELLO"))
if reply is None:
    sys.exit("peer closed during handshake (wrong token?)")
if reply.get("op") != "OK":
    sys.exit(f"handshake refused: {reply.get('message')}")
print("  handshake ok (token accepted)")

local = build_manifest(ROOT_DIR)

send_msg(sock, json.dumps({"op": "MANIFEST"}).encode("utf-8"))
try:
    reply = recv_msg(sock)
except TimeoutError:
    sys.exit(silent_peer_help("MANIFEST"))
if reply is None:
    sys.exit("Connection closed by peer before manifest was received")
remote = json.loads(reply.decode("utf-8"))["files"]

to_put, to_delete = diff_manifests(local, remote)

label = "DRY RUN - no files will be sent\n" if dry_run else ""
print(f"{label}Local: {len(local)} files | Peer: {len(remote)} files")

if not local:
    hint(f"no files found under {os.path.abspath(ROOT_DIR)}")
    hint("is that the right folder? nothing will be sent")
    if delete and not dry_run:
        sys.exit(
            "refusing to run --delete from an empty source: it would wipe the peer."
        )

sock.settimeout(TRANSFER_TIMEOUT)

verb = "Would send" if dry_run else "Sending"
print(f"{verb} {len(to_put)} file(s):")
sent_bytes = 0
transfer_started = time.monotonic()
for path in to_put:
    size = local[path]["size"]
    print(f"   PUT    {path} ({human(size)})", flush=True)
    if not dry_run:
        try:
            send_file(sock, ROOT_DIR, path)
        except TimeoutError:
            sys.exit(
                f"stalled for {TRANSFER_TIMEOUT}s while sending {path!r}.\n"
                "The peer stopped reading -- server killed, or the Wi-Fi dropped."
            )
        except (BrokenPipeError, ConnectionResetError) as e:
            sys.exit(
                f"peer went away while sending {path!r} ({e}).\n"
                "Check the server's terminal for the error it printed."
            )
        sent_bytes += size

if to_delete:
    if dry_run:
        verb = "Would delete"
    elif delete:
        verb = "Deleting"
    else:
        verb = "Skipping (need --delete):"
    print(f"{verb} {len(to_delete)} file(s):")
    for path in to_delete:
        print(f"   DELETE {path}")
        if not dry_run and delete:
            send_delete(sock, path)

send_msg(sock, json.dumps({"op": "BYE"}).encode("utf-8"))
sock.close()

if not dry_run and sent_bytes:
    elapsed = time.monotonic() - transfer_started
    rate = human(sent_bytes / max(elapsed, 0.001))
    print(f"Done: {human(sent_bytes)} in {elapsed:.1f}s ({rate}/s)")
