import socket
import sys
import json
from framing import send_msg, recv_msg, build_manifest, diff_manifests, send_file

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = 8765
ROOT_DIR = sys.argv[2] if len(sys.argv) > 2 else "source"

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect((HOST, PORT))

local = build_manifest(ROOT_DIR)

send_msg(sock, json.dumps({"op": "MANIFEST"}).encode("utf-8"))
reply = recv_msg(sock)
remote = json.loads(reply.decode("utf-8"))["files"]

to_put, to_delete = diff_manifests(local, remote)

print(f"Local: {len(local)} files | Peer: {len(remote)} files")
print(f"Sending {len(to_put)} file(s)...")
for path in to_put:
    send_file(sock, ROOT_DIR, path)
    print(f"   PUT    {path}")
if to_delete:
    print(f"Skipping {len(to_delete)} delete(s) — need --delete:")
    for path in to_delete:
        print(f"   DELETE {path}")

send_msg(sock, json.dumps({"op": "BYE"}).encode("utf-8"))
sock.close()
