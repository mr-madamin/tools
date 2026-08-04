import struct
import json
import os


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


def send_error(conn, message):
    reply = json.dumps({"op": "ERROR", "message": message}).encode("utf-8")
    send_msg(conn, reply)


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


def send_file(conn, root_dir, rel_path):
    """Send one file as a PUT: JSON header {path, size, mtime}, then raw body."""
    full_path = os.path.join(root_dir, rel_path)
    size = os.path.getsize(full_path)
    mtime = os.path.getmtime(full_path)
    meta = json.dumps(
        {"op": "PUT", "path": rel_path, "size": size, "mtime": mtime}
    ).encode("utf-8")

    send_msg(conn, meta)

    with open(full_path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if chunk == b"":
                break
            conn.sendall(chunk)


def recv_file_body(conn, dest_dir, header):
    """Write the raw body that follows a PUT header. header already parsed."""
    rel_path = header["path"]
    size = header["size"]
    mtime = header["mtime"]

    full_path = os.path.join(dest_dir, rel_path)
    os.makedirs(os.path.dirname(full_path), exist_ok=True)

    bytes_received = 0
    with open(full_path, "wb") as f:
        while bytes_received < size:
            chunk = conn.recv(min(65536, size - bytes_received))
            if chunk == b"":
                raise ConnectionError("peer closed mid-file - truncated transfer")
            f.write(chunk)
            bytes_received += len(chunk)

    os.utime(full_path, (mtime, mtime))
    return rel_path


def recv_file(conn, dest_dir):
    """Read a PUT header, then its body."""
    header_bytes = recv_msg(conn)
    if header_bytes is None:
        return None
    header = json.loads(header_bytes.decode("utf-8"))
    return recv_file_body(conn, dest_dir, header)


def build_manifest(root_dir):
    """Walk root_dir and return a manifest:
    { relative_path: {"size": int, "mtime": float}, ... }
    keyed by each file's path *relative to root_dir*."""
    manifest = {}
    for dirpath, dirnames, filenames in os.walk(root_dir):
        for filename in filenames:
            full_path = os.path.join(dirpath, filename)
            rel_path = os.path.relpath(full_path, root_dir)
            info = os.stat(full_path)
            manifest[rel_path] = {"size": info.st_size, "mtime": info.st_mtime}
    return manifest


def diff_manifests(local, remote, mtime_tolerance=2):
    """Compare local vs remote manifest. Return (to_put, to_delete).

    to_put      = paths we should send: missing on remote, or size/mtime differ.
    to_delete   = paths on remote but not local (deletion candidates).
    """
    to_put = []
    for path, info in local.items():
        remote_info = remote.get(path)
        if remote_info is None:
            to_put.append(path)
        elif info["size"] != remote_info["size"]:
            to_put.append(path)
        elif abs(info["mtime"] - remote_info["mtime"]) > mtime_tolerance:
            to_put.append(path)

    to_delete = [path for path in remote if path not in local]
    return to_put, to_delete
