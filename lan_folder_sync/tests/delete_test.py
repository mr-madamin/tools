import json
import os
import socket
import subprocess

from framing import recv_msg, send_msg

HOST, PORT = "127.0.0.1", 8765
SANDBOX = "sandbox"
ROOT_DIR = os.path.join(SANDBOX, "source")
PEER_DIR = os.path.join(SANDBOX, "received")  # what the server serves; same machine, so we can plant files
PROBE = f"_delete_probe_{os.getpid()}.txt"
VICTIM = f"_delete_victim_{os.getpid()}.txt"
VICTIM_PATH = os.path.join(SANDBOX, VICTIM)  # a sibling of received/, i.e. OUTSIDE it


def connect():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    return s


def remote_manifest():
    """Ask the live server what files its received/ holds — our oracle."""
    s = connect()
    send_msg(s, json.dumps({"op": "MANIFEST"}).encode("utf-8"))
    reply = recv_msg(s)
    s.close()
    assert reply is not None, "server closed without sending a manifest"
    return json.loads(reply.decode("utf-8"))["files"]


def push(*extra_flags):
    """Run the REAL sync_push.py with extra flags; return its CompletedProcess."""
    result = subprocess.run(
        ["python3", "sync_push.py", HOST, ROOT_DIR, *extra_flags],
        capture_output=True,
        text=True,
        check=False,
    )
    print(result.stdout, end="")
    assert result.returncode == 0, f"push crashed:\n{result.stderr}"
    return result


def plant_on_peer(name):
    """Simulate a file the peer HAS but the source does NOT (a delete candidate)."""
    path = os.path.join(PEER_DIR, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)  # handles nested names like sub/x
    with open(path, "w") as f:
        f.write("delete me\n")
    return path


try:
    # --- Case 1: --delete actually removes the file from the peer ---
    plant_on_peer(PROBE)
    assert PROBE in remote_manifest(), "setup failed: probe not on peer"
    push("--delete")
    assert PROBE not in remote_manifest(), f"{PROBE} still on peer after --delete"
    print(f"OK --delete removed {PROBE} from the peer")

    # --- Case 2: --dry-run --delete PREVIEWS the delete but does NOT remove ---
    plant_on_peer(PROBE)
    result = push("--dry-run", "--delete")
    assert f"DELETE {PROBE}" in result.stdout, "dry-run did not list the delete"
    assert PROBE in remote_manifest(), f"{PROBE} was deleted during a DRY RUN!"
    print(f"OK --dry-run --delete previewed but kept {PROBE}")

    # --- Case 3: a path escaping received/ is refused; the outside file survives ---
    # Hand-crafted frame: the honest client can never emit this path.
    with open(VICTIM_PATH, "w") as f:  # in sandbox/, one level above received/
        f.write("must survive\n")
    s = connect()
    send_msg(s, json.dumps({"op": "DELETE", "path": f"../{VICTIM}"}).encode("utf-8"))
    reply = recv_msg(s)
    s.close()
    assert reply is not None, "server closed without sending an ERROR"
    msg = json.loads(reply.decode("utf-8"))
    assert msg.get("op") == "ERROR", f"expected ERROR, got {msg!r}"
    assert os.path.exists(VICTIM_PATH), (
        "path-escape DELETE removed a file OUTSIDE received/!"
    )
    print(f"OK path escape refused ({msg.get('message')!r}); {VICTIM} survived")

    # absolute path is the other traversal vector — same guard must catch it
    s = connect()
    send_msg(s, json.dumps({"op": "DELETE", "path": "/etc/hosts"}).encode("utf-8"))
    reply = recv_msg(s)
    s.close()
    assert reply is not None and json.loads(reply.decode("utf-8")).get("op") == "ERROR", (
        "absolute-path DELETE was not refused"
    )
    print("OK absolute path also refused")

    # --- Case 4: a legit nested-subdir delete works (guard must not over-block) ---
    nested = os.path.join("sub", PROBE)
    plant_on_peer(nested)  # creates received/sub/<probe>
    assert nested in remote_manifest(), "setup failed: nested probe not on peer"
    push("--delete")
    assert nested not in remote_manifest(), f"{nested} survived --delete (guard too strict?)"
    print(f"OK --delete removed nested {nested}")

    # --- Case 5: deleting a missing file is idempotent (no ERROR, session lives) ---
    s = connect()
    gone = f"_never_existed_{os.getpid()}.txt"
    send_msg(s, json.dumps({"op": "DELETE", "path": gone}).encode("utf-8"))
    # server succeeds silently (no reply) — prove it by reusing the connection:
    send_msg(s, json.dumps({"op": "MANIFEST"}).encode("utf-8"))
    reply = recv_msg(s)
    s.close()
    assert reply is not None, "session died after deleting a missing file"
    msg = json.loads(reply.decode("utf-8"))
    assert msg.get("op") == "MANIFEST", f"expected MANIFEST, got {msg!r}"
    print("OK deleting a missing file is idempotent; session survived")

    print("\nDELETE removes, previews safely, and refuses to escape received/. 🗑️")
finally:
    nested = os.path.join(PEER_DIR, "sub", PROBE)
    for p in (os.path.join(PEER_DIR, PROBE), nested, VICTIM_PATH):
        if os.path.exists(p):
            os.remove(p)
    sub = os.path.join(PEER_DIR, "sub")
    if os.path.isdir(sub) and not os.listdir(sub):
        os.rmdir(sub)
