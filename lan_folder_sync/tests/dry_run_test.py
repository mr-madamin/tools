import json
import os
import socket
import subprocess

from framing import recv_msg, send_msg

HOST, PORT = "127.0.0.1", 8765
ROOT_DIR = "sandbox/source"
PROBE = f"_dryrun_probe_{os.getpid()}.txt"  # unique so it can't pre-exist on peer


def remote_manifest():
    """Ask the live server what files its received"""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    send_msg(s, json.dumps({"op": "MANIFEST"}).encode("utf-8"))
    reply = recv_msg(s)
    s.close()
    assert reply is not None, "server closed without sending a manifest"
    return json.loads(reply.decode("utf-8"))["files"]


# 1. Plant a file locally that the peer definitely does not have
os.makedirs(ROOT_DIR, exist_ok=True)
probe_path = os.path.join(ROOT_DIR, PROBE)
with open(probe_path, "w") as f:
    f.write("dry-run probe - must never reach the peer\n")

try:
    # 2. Sanity: the peer starts without it
    assert PROBE not in remote_manifest(), f"peer already has {PROBE}?!"

    # 3. Run the real push, in dry-run, and capture what it prints
    result = subprocess.run(
        ["python3", "sync_push.py", HOST, ROOT_DIR, "--dry-run"],
        capture_output=True,
        text=True,
        check=False,
    )
    print(result.stdout, end="")
    assert result.returncode == 0, f"push crashed:\n{result.stderr}"

    # 4. It should plan to send the probe...
    assert "DRY RUN" in result.stdout, "no DRY RUN banner in output"
    assert PROBE in result.stdout, f"{PROBE} not listed in the plan"

    # 5. ...but the peer must still not have it. The plan was talk, not action
    assert PROBE not in remote_manifest(), (
        f"{PROBE} reached the peer - dry-run transferred a file!"
    )

    print(f"\nOK dry-run planned PUT {PROBE} but transferred nothing")
finally:
    os.remove(probe_path)
