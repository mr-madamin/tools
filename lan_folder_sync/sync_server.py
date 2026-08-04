import socket
import json

from framing import send_error, send_msg, recv_msg, build_manifest, recv_file_body

SHARED_DIR = "received"
PORT = 8765

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("127.0.0.1", PORT))
sock.listen(1)
print(f"Serving {SHARED_DIR}/ on {PORT} ... (Ctrl-C to stop)")

while True:
    conn, addr = sock.accept()
    print(f"Connected from {addr[0]}:{addr[1]}")
    try:
        while True:
            header_bytes = recv_msg(conn)
            if header_bytes is None:
                break  # peer vanished

            try:
                header = json.loads(header_bytes.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                send_error(conn, "malformed header: not valid UTF-8 JSON")
                break

            op = header["op"]
            if op == "MANIFEST":
                manifest = build_manifest(SHARED_DIR)
                reply = json.dumps({"op": "MANIFEST", "files": manifest}).encode(
                    "utf-8"
                )
                send_msg(conn, reply)
                print(f"Sent manifest ({len(manifest)} files)")
            elif op == "PUT":
                try:
                    rel_path = recv_file_body(conn, SHARED_DIR, header)
                except KeyError as e:
                    send_error(conn, f"PUT header missing field: {e}")
                    break  # body size unknown - can't resync the stream
                print(f"    received {rel_path}")
            elif op == "BYE":
                print("Peer said BYE")
                break
            else:
                send_error(conn, f"unknown op: {op!r}")
                break  # unknown frame
    except ConnectionError as e:
        print(f"Session error: {e}")
    finally:
        conn.close()
        print("Session ended, waiting for next peer")
