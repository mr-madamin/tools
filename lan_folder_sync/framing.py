import struct


def recv_exactly(conn, n):
    """Read exactly n bytes from conn, or return None if the peer
    closes the connection before n bytes arrive."""
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
    """Frame and send: 4-byte big-endian length prefix, then the payload."""
    header = struct.pack("!I", len(payload))
    conn.sendall(header + payload)


def recv_msg(conn):
    """Read one framed message. Returns the payload bytes, or None on EOF."""
    header = recv_exactly(conn, 4)
    if header is None:
        return None
    (length,) = struct.unpack("!I", header)
    return recv_exactly(conn, length)
