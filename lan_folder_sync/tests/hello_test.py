import json
import socket

from config import load_config
from framing import recv_msg, send_msg
from tests._report import done, ok, section

HOST, PORT = "127.0.0.1", 8765
TOKEN = load_config()["token"]


def connect():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    return s


def expect_rejected(label, first_frame):
    """Send `first_frame` as the opening frame; expect an error, then a closed connection (the server must reject AND hand up before serving anything)."""
    s = connect()
    send_msg(s, json.dumps(first_frame).encode("utf-8"))
    reply = recv_msg(s)
    assert reply is not None, f"{label}: server closed without sending an error"
    msg = json.loads(reply.decode("utf-8"))
    assert msg.get("op") == "ERROR", f"{label} expected ERROR, got {msg!r}"
    # the session must be gone - a second read returns EOF (None)
    assert recv_msg(s) is None, f"{label}: server did not close after rejecting"
    ok(f"{label}: rejected → {msg.get('message')!r}, then closed")
    s.close()


section("HELLO rejects the unauthenticated")

# --- Case 1: wrong token → ERROR + close, before anything is served ---
expect_rejected("wrong-token", {"op": "HELLO", "token": "definitely-not-it"})

# --- Case 2: missing token → same treatment ---
expect_rejected("missing-token", {"op": "HELLO"})

# --- Case 3: skipping HELLO entirely (MANIFEST first) → refused ---
expect_rejected("no-hello", {"op": "MANIFEST"})

# a mutating op with no HELLO must NOT act - refused before dispatch
expect_rejected("delete-before-hello", {"op": "DELETE", "path": "anything.txt"})

section("HELLO with the right token proceeds")

# --- Case 4: correct token → OK, then the session actually servers ---
s = connect()
send_msg(s, json.dumps({"op": "HELLO", "token": TOKEN}).encode("utf-8"))
reply = recv_msg(s)
assert reply is not None, "server closed instead of accepting a valid token"
msg = json.loads(reply.decode("utf-8"))
assert msg.get("op") == "OK", f"expected OK, got {msg!r}"
ok("correct token accepted → OK")

# same authenticated connection can now do real work
send_msg(s, json.dumps({"op": "MANIFEST"}).encode("utf-8"))
reply = recv_msg(s)
assert reply is not None, "authenticated session died before serving a manifest"
msg = json.loads(reply.decode("utf-8"))
assert msg.get("op") == "MANIFEST", f"expected MANIFEST after HELLO, got {msg!r}"
ok(f"authenticated session served a manifest ({len(msg['files'])} files)")
s.close()

done("HELLO gates every op; wrong/missing token is rejected and closed 🔐")
