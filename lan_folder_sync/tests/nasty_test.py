import socket
import struct
import threading

from framing import recv_msg, send_msg
from tests._report import done, ok, section

section("Framing boundaries — self-contained, no server")

a, b = socket.socketpair()

# --- Test 1: payload far bigger than the socket buffer (forces partial reads) ---
# send_msg does one big sendall; on a socketpair that blocks once the kernel
# buffer fills, so a reader thread must drain concurrently. recv_exactly on the
# reader side then loops over many partial recv()s to reassemble all 1 MB.
big = b"A" * 1_000_000
out = []
reader = threading.Thread(target=lambda: out.append(recv_msg(b)))
reader.start()
send_msg(a, big)  # blocks/unblocks as the reader drains — that's the point
reader.join()

got = out[0] if out else None
assert got == big, f"MISMATCH: sent {len(big)}, got {len(got) if got else got}"
ok(f"big payload: {len(big)} bytes survived the round trip intact")

# --- Test 2: two frames shoved into ONE sendall (coalesced on the wire) ---
# Bypass send_msg on purpose so both frames land back-to-back in the receiver's
# buffer. recv_msg must split them at the right boundary. Two small frames fit,
# so no thread needed here.
frame1 = struct.pack("!I", 5) + b"hello"
frame2 = struct.pack("!I", 3) + b"bye"
a.sendall(frame1 + frame2)
r1 = recv_msg(b)
r2 = recv_msg(b)
assert r1 == b"hello", f"frame 1 wrong: {r1!r}"
assert r2 == b"bye", f"frame 2 wrong: {r2!r}"
ok(f"coalesced: split cleanly into {r1!r} then {r2!r}")

a.close()
b.close()
done("all boundaries held — framing works")
