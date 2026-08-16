import socket
import struct

from framing import recv_msg, send_msg

HOST, PORT = "127.0.0.1", 8765

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect((HOST, PORT))

# --- Test 1: payload WAY bigger than one TCP segment ---
# Forces recv() to return partial reads; recv_exactly must loop.
big = b"A" * 1_000_000
send_msg(sock, big)
reply = recv_msg(sock)
assert reply == big, f"MISMATCH: sent {len(big)}, got {len(reply) if reply else reply}"
print(f"OK big payload: {len(big)} bytes survived the round trip intact")

# --- Test 2: two frames shoved into ONE sendall (coalesced on the wire) ---
# Bypass send_msg on purpose so both frames land back-to-back in the
# server's kernel buffer. recv_msg must split them at the right boundary.
frame1 = struct.pack("!I", 5) + b"hello"
frame2 = struct.pack("!I", 3) + b"bye"
sock.sendall(frame1 + frame2)
r1 = recv_msg(sock)
r2 = recv_msg(sock)
assert r1 == b"hello", f"frame 1 wrong: {r1!r}"
assert r2 == b"bye", f"frame 2 wrong: {r2!r}"
print(f"OK coalesced: split cleanly into {r1!r} then {r2!r}")

sock.close()
print("\nAll boundaries held. Framing works. 🎉")
