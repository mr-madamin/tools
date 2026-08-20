import json
import socket
import sys

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

flags = {a for a in sys.argv[1:] if a.startswith("--")}
positional = [a for a in sys.argv[1:] if not a.startswith("--")]

cfg = load_config()
TOKEN = cfg["token"]

HOST = positional[0] if len(positional) > 0 else "127.0.0.1"
PORT = 8765
ROOT_DIR = positional[1] if len(positional) > 1 else "sandbox/source"

dry_run = "--dry-run" in flags
delete = "--delete" in flags

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect((HOST, PORT))

reply = handshake(sock, TOKEN)
if reply is None:
    sys.exit("peer closed during handshake (wrong token?)")
if reply.get("op") != "OK":
    sys.exit(f"handshake refused: {reply.get('message')}")

local = build_manifest(ROOT_DIR)

send_msg(sock, json.dumps({"op": "MANIFEST"}).encode("utf-8"))
reply = recv_msg(sock)
if reply is None:
    sys.exit("Connection closed by peer before manifest was received")
remote = json.loads(reply.decode("utf-8"))["files"]

to_put, to_delete = diff_manifests(local, remote)

label = "DRY RUN - no files will be sent\n" if dry_run else ""
print(f"{label}Local: {len(local)} files | Peer: {len(remote)} files")

verb = "Would send" if dry_run else "Sending"
print(f"{verb} {len(to_put)} file(s):")
for path in to_put:
    print(f"   PUT    {path}")
    if not dry_run:
        send_file(sock, ROOT_DIR, path)

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
