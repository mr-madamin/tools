import json
import os
import socket
import sys

from config import load_config
from framing import (
    MAX_CONTROL_FRAME,
    FrameTooLarge,
    UnsafePath,
    build_manifest,
    recv_file_body,
    recv_msg,
    safe_path,
    send_error,
    send_msg,
)
from netutil import lan_ip

# Line-buffer stdout: redirected to a file or a service manager, Python
# block-buffers and the log stays empty until the process exits. The C
# port spells this setvbuf(stdout, NULL, _IOLBF, 0).
sys.stdout.reconfigure(line_buffering=True)

cfg = load_config()
TOKEN = cfg["token"]

flags = {a for a in sys.argv[1:] if a.startswith("--")}
positional = [a for a in sys.argv[1:] if not a.startswith("--")]

SHARED_DIR = (
    positional[0] if len(positional) > 0 else cfg.get("shared_dir", "sandbox/received")
)
PORT = int(
    positional[1] if len(positional) > 1 else cfg.get("peer", {}).get("port", 8765)
)

if "--lan" in flags:
    BIND_HOST = lan_ip()
    if not BIND_HOST:
        sys.exit(
            "--lan: no en0 IPv4 found (offline, or Wi-Fi isn't en0). "
            "Join the LAN, or drop --lan to bind 127.0.0.1"
        )
else:
    BIND_HOST = "127.0.0.1"

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind((BIND_HOST, PORT))
sock.listen(1)
print(f"Serving {SHARED_DIR}/ on {BIND_HOST}:{PORT} ... (Ctrl-C to stop)")
if BIND_HOST != "127.0.0.1":
    print(f"  → peers: set config.json peer.host to {BIND_HOST!r}")

while True:
    conn, addr = sock.accept()
    print(f"Connected from {addr[0]}:{addr[1]}")
    authenticated = False
    try:
        while True:
            # Everything a client sends us is a short header; the big frame
            # in this protocol only ever travels the other way.
            try:
                header_bytes = recv_msg(conn, MAX_CONTROL_FRAME)
            except FrameTooLarge as e:
                print(f"Refused oversized frame ({e} bytes, cap {MAX_CONTROL_FRAME})")
                send_error(conn, "frame too large")
                break
            if header_bytes is None:
                break  # peer vanished

            try:
                header = json.loads(header_bytes.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                send_error(conn, "malformed header: not valid UTF-8 JSON")
                break
            except RecursionError:
                # json's parser recurses per '[' or '{'. 200 KB of '[' exhausts
                # the interpreter's stack BEFORE the HELLO, and RecursionError
                # is neither a JSONDecodeError nor a ConnectionError, so it
                # escaped both handlers and killed the whole server.
                send_error(conn, "malformed header: nested too deeply")
                break

            # json.loads("123") is a perfectly valid int, and header.get() then
            # raised AttributeError straight out of the accept loop — another
            # unauthenticated four-byte kill. The header must be an object.
            if not isinstance(header, dict):
                send_error(conn, "malformed header: not a JSON object")
                break

            op = header.get("op")

            if not authenticated:
                if op != "HELLO":
                    send_error(conn, "auth required: send HELLO first")
                    break
                if header.get("token") != TOKEN:
                    send_error(conn, "bad token")
                    break
                authenticated = True
                send_msg(conn, json.dumps({"op": "OK"}).encode("utf-8"))
                print("     HELLO ok, session authenticated")
                continue

            if op == "MANIFEST":
                # Partial here is far less dangerous than partial on the
                # pusher - files we fail to list just get re-sent, never
                # deleted - but say so, it's the same underlying problem.
                walk_errors = []
                manifest = build_manifest(SHARED_DIR, walk_errors)
                reply = json.dumps({"op": "MANIFEST", "files": manifest}).encode(
                    "utf-8"
                )
                send_msg(conn, reply)
                if walk_errors:
                    print(
                        f"Sent manifest ({len(manifest)} files) - WARNING: some "
                        f"directories under {SHARED_DIR} could not be read; "
                        f"the list is incomplete"
                    )
                else:
                    print(f"Sent manifest ({len(manifest)} files)")
            elif op == "PUT":
                try:
                    rel_path = recv_file_body(conn, SHARED_DIR, header)
                except KeyError as e:
                    send_error(conn, f"PUT header missing field: {e}")
                    break  # body size unknown - can't resync the stream
                except UnsafePath as e:
                    send_error(conn, f"unsafe path refused: {e}")
                    break  # body still queued - the stream can't be resynced
                except ValueError as e:
                    # size/mtime present but unusable (negative, inf, NaN).
                    # Same wording as the C server.
                    send_error(conn, f"PUT header has an out-of-range '{e}'")
                    break  # body length is nonsense - can't find the next frame
                print(f"    received {rel_path}")
            elif op == "BYE":
                print("Peer said BYE")
                break
            elif op == "DELETE":
                rel_path = header.get("path")
                if rel_path is None:
                    send_error(conn, "DELETE missing 'path'")
                    break

                # One shared guard for PUT and DELETE. The old check here
                # realpath'd the whole path, which also let a path resolving to
                # the shared folder ITSELF through (target == base passed).
                target = safe_path(SHARED_DIR, rel_path)
                if target is None:
                    send_error(conn, f"unsafe path refused: {rel_path!r}")
                    break

                try:
                    os.remove(target)
                    print(f"    deleted {rel_path}")
                except FileNotFoundError:
                    print(f"    already gone {rel_path}")  # delete is idempotent
            else:
                send_error(conn, f"unknown op: {op!r}")
                break  # unknown frame
    except ConnectionError as e:
        print(f"Session error: {e}")
    except Exception as e:  # noqa: BLE001 - a session must never kill the server
        # Last line of defence. One peer's bad frame is a session-level problem;
        # before this, anything unanticipated propagated out of the accept loop
        # and took the process with it. KeyboardInterrupt/SystemExit derive from
        # BaseException, so Ctrl-C still stops the server.
        print(f"Session error: {type(e).__name__}: {e}")
    finally:
        conn.close()
        print("Session ended, waiting for next peer")
