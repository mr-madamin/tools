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


def send_file(conn, path):
    size = os.path.getsize(path)
    meta = json.dumps({"name": os.path.basename(path), "size": size}).encode("utf-8")

    send_msg(conn, meta)

    with open(path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if chunk == b"":
                break
            conn.sendall(chunk)


def recv_file(conn, dest_dir):
    meta_bytes = recv_msg(conn)
    if meta_bytes is None:
        return None

    meta = json.loads(meta_bytes.decode("utf-8"))
    name = meta["name"]
    size = meta["size"]
    file_path = os.path.join(dest_dir, name)

    bytes_received = 0
    with open(file_path, "wb") as f:
        while bytes_received < size:
            chunk = conn.recv(min(65536, size - bytes_received))
            if chunk == b"":
                raise ConnectionError("peer closed mid-file - truncated transfer")
            f.write(chunk)
            bytes_received += len(chunk)

    return file_path


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
