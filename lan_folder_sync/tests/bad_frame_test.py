import json
import socket

from framing import recv_msg, send_msg

HOST, PORT = "127.0.0.1", 8765


def connect():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    return s


def expect_error(label, payload: bytes):
    """Send one bad frame on a fresh connection; expect an ERROR frame back."""
    s = connect()
    send_msg(s, payload)
    reply = recv_msg(s)
    assert reply is not None, f"{label}: server closed WITHOUT sending an ERROR"
    msg = json.loads(reply.decode("utf-8"))
    assert msg.get("op") == "ERROR", f"{label}: expected ERROR, got {msg!r}"
    print(f"OK {label}: got ERROR -> {msg.get('message')!r}")
    s.close()


# --- Case 1: not JSON at all (malformed parse) ---
expect_error("bad-json", b"not json at all")

# --- Case 2: valid JSON, but no "op" ---
expect_error("missing-op", json.dumps({"foo": 1}).encode("utf-8"))

# --- Case 3: well-formed frame, unknown op (a typo) ---
expect_error("unknown-op", json.dumps({"op": "MANFEST"}).encode("utf-8"))

# --- Case 4: PUT header missing size/mtime ---
expect_error(
    "put-missing-fields", json.dumps({"op": "PUT", "path": "x"}).encode("utf-8")
)

# --- The real pass condition: server is still alive and speaks the protocol ---
s = connect()
send_msg(s, json.dumps({"op": "MANIFEST"}).encode("utf-8"))
reply = recv_msg(s)
assert reply is not None, "server died — no reply after four bad frames"
msg = json.loads(reply.decode("utf-8"))
assert msg.get("op") == "MANIFEST", f"expected MANIFEST, got {msg!r}"
print(f"OK survived: server still served a manifest ({len(msg['files'])} files)")
s.close()

print("\nServer shrugged off every bad frame and kept serving. 🛡️")
