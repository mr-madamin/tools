import struct


def recv_exactly(conn, n):
    chunks = []
    bytes_received = 0

    while bytes_received < n:
        chunk = conn.recv(n - bytes_received)
        if chunk == b"":
            return None  # peer closed the connection (EOF)
        chunks.append(chunk)
        bytes_received += len(chunk)
    return b"".join(chunks)


def send_msg(conn, payload: bytes):
    header = struct.pack("!I", len(payload))
    conn.sendall(header + payload)
