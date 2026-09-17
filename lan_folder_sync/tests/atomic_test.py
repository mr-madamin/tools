"""An aborted PUT must not damage the copy already on the receiver.

Opening the destination with "wb" truncates the good file the moment the
transfer starts, so a peer that dies mid-body leaves a stub where a complete
file used to be. The receiver writes to a temp beside the target and
os.replace()s it into place, so the old copy survives until the new one is
whole. A negative size must not blank it either.

Mirrors lan_folder_sync_c/tests/atomic_test.c. Needs the server running.
"""

import json
import os
import socket
import struct
import time

from config import load_config
from framing import recv_msg, send_msg
from tests._report import done, ok, section

HOST, PORT = "127.0.0.1", 8765
TOKEN = load_config()["token"]
# Hardcoded like the other runners, NOT cfg["shared_dir"]: that points at the
# real folder this machine syncs, and a test must never plant files there.
# Start the server against sandbox/received, as the README says.
PEER_DIR = os.path.join("sandbox", "received")

GOOD_TEXT = "THE ORIGINAL GOOD CONTENT\n"
CLAIMED = 5 * 1024 * 1024  # announced
DELIVERED = 102400  # actually sent before we vanish

PROBE = f"_atomic_probe_{os.getpid()}.txt"
PROBE_PATH = os.path.join(PEER_DIR, PROBE)


def connect_authed():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect((HOST, PORT))
    send_msg(s, json.dumps({"op": "HELLO", "token": TOKEN}).encode("utf-8"))
    reply = json.loads(recv_msg(s).decode("utf-8"))
    assert reply.get("op") == "OK", f"handshake failed: {reply!r}"
    return s


def stray_temp_files():
    """A leftover .tmp would show up in the manifest as if it were a real file."""
    return [f for f in os.listdir(PEER_DIR) if ".tmp" in f]


try:
    os.makedirs(PEER_DIR, exist_ok=True)
    with open(PROBE_PATH, "w") as f:
        f.write(GOOD_TEXT)

    section("Aborted PUT leaves the old file alone")

    # --- announce a big file, deliver a sliver, then drop the connection ---
    s = connect_authed()
    send_msg(
        s,
        json.dumps(
            {"op": "PUT", "path": PROBE, "size": CLAIMED, "mtime": 1.0}
        ).encode("utf-8"),
    )
    s.sendall(b"X" * DELIVERED)
    s.close()  # vanish mid-body
    time.sleep(0.3)

    with open(PROBE_PATH) as f:
        now = f.read()
    assert now == GOOD_TEXT, (
        f"{PROBE} is {len(now)} bytes, was {len(GOOD_TEXT)} "
        "- the aborted transfer overwrote it"
    )
    ok(f"the original {len(GOOD_TEXT)}-byte file survived intact")

    leftovers = stray_temp_files()
    assert not leftovers, f"an aborted PUT left {leftovers} behind in {PEER_DIR}"
    ok("no stray .tmp left in the shared folder")

    # --- a negative size must not blank it either ---
    s = connect_authed()
    raw = b'{"op": "PUT", "path": "%s", "size": -1, "mtime": 0}' % PROBE.encode()
    s.sendall(struct.pack("!I", len(raw)) + raw)
    reply = recv_msg(s)
    s.close()
    assert reply is not None, "server accepted a negative size without an ERROR"
    msg = json.loads(reply.decode("utf-8"))
    assert msg.get("op") == "ERROR", f"expected ERROR for a negative size, got {msg!r}"

    with open(PROBE_PATH) as f:
        after = f.read()
    assert after == GOOD_TEXT, "a negative size truncated the file"
    ok("a negative size was refused and left the file intact")

    # --- and the server is still serving ---
    s = connect_authed()
    send_msg(s, json.dumps({"op": "MANIFEST"}).encode("utf-8"))
    reply = recv_msg(s)
    s.close()
    assert reply is not None, "server died after the aborted transfer"
    assert json.loads(reply.decode("utf-8")).get("op") == "MANIFEST"
    ok("server kept serving after the aborted transfer")

    done("an interrupted PUT cannot damage the file it was replacing 💾")
finally:
    if os.path.exists(PROBE_PATH):
        os.remove(PROBE_PATH)
