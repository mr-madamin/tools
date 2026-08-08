import socket
import sys
import json

from framing import send_msg, recv_msg, build_manifest, diff_manifests, send_file

flags = {a for a in sys.argv[1:] if a.startswith("--")}
positional = [a for a in sys.argv[1:] if not a.startswith("--")]

HOST = positional[0] if len(positional) > 0 else "127.0.0.1"
PORT = 8765
ROOT_DIR = positional[1] if len(positional) > 1 else "source"

dry_run = "--dry-run" in flags

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect((HOST, PORT))

local = build_manifest(ROOT_DIR)

send_msg(sock, json.dumps({"op": "MANIFEST"}).encode("utf-8"))
reply = recv_msg(sock)
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
    print(f"Skipping {len(to_delete)} delete(s) — need --delete:")
    for path in to_delete:
        print(f"   DELETE {path}")

send_msg(sock, json.dumps({"op": "BYE"}).encode("utf-8"))
sock.close()
