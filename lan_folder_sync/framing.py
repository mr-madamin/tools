import errno
import json
import math
import os
import struct

# size and mtime arrive as JSON numbers from the peer and were used unchecked:
# a negative size skipped the write loop and left an empty file where a real one
# had been, while "size": 1e999 decodes to inf. Bound both. (The C port carries
# the same two constants.)
MAX_FILE_SIZE = 64 * 1024 * 1024 * 1024  # 64 GiB per file
MAX_MTIME = 1e15  # far past any real clock

# The length prefix is 4 bytes, so a peer can claim up to 4 GB before sending a
# single byte of payload - and this ran before the HELLO. Two ceilings, because
# the two directions carry very different frames: a MANIFEST lists every file in
# the folder, while everything else is a short header. The server only ever
# reads the short kind.
MAX_FRAME = 64 * 1024 * 1024  # a manifest of a huge folder
MAX_CONTROL_FRAME = 1024 * 1024  # HELLO / PUT / DELETE / BYE


class FrameTooLarge(Exception):
    """A peer declared a frame bigger than we are willing to read."""


def recv_exactly(conn, n):
    chunks = []
    bytes_received = 0

    while bytes_received < n:
        chunk = conn.recv(n - bytes_received)
        if chunk == b"":
            return None
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


def recv_msg(conn, max_bytes=MAX_FRAME):
    """Read one framed message. Returns the payload bytes, or None on EOF.

    Raises FrameTooLarge if the peer declares more than max_bytes. The payload
    is then left unread, so the stream has no boundary left to resync on and
    every caller must end the session.
    """
    header = recv_exactly(conn, 4)
    if header is None:
        return None
    (length,) = struct.unpack("!I", header)
    if length > max_bytes:
        raise FrameTooLarge(length)
    return recv_exactly(conn, length)


def send_file(conn, root_dir, rel_path):
    """Send one file as a PUT: JSON header {path, size, mtime}, then raw body.

    Returns True if the file changed size while we were reading it.

    The body is the only unframed part of the stream - the receiver finds the
    next frame by counting exactly `size` bytes. Send one byte too few or too
    many and it reads the following header as file content, silently corrupting
    every later file in the session. The file can change underneath us at any
    point, so the declared size is the contract and this honours it even when
    the file stops matching.
    """
    full_path = os.path.join(root_dir, rel_path)

    # Open first, then fstat the handle we actually hold: getsize()-then-open()
    # lets the path be replaced in between, and we would declare one file's size
    # while sending another's bytes.
    with open(full_path, "rb") as f:
        info = os.fstat(f.fileno())
        size = info.st_size
        mtime = info.st_mtime

        meta = json.dumps(
            {"op": "PUT", "path": rel_path, "size": size, "mtime": mtime}
        ).encode("utf-8")
        send_msg(conn, meta)

        sent = 0
        truncated = False
        while sent < size:
            chunk = f.read(min(65536, size - sent))
            if chunk == b"":
                # Truncated mid-transfer. We already promised `size` bytes, so
                # pad - aborting would leave the receiver waiting for bytes that
                # never come and kill the whole session over one file. The
                # padded copy loses the mtime comparison on the next push and
                # gets resent, so this self-heals.
                chunk = b"\0" * (size - sent)
                truncated = True
            conn.sendall(chunk)
            sent += len(chunk)

        # If the file GREW we simply stop at `size` and never read the tail -
        # the frame is still exactly as long as advertised.
        grew = os.fstat(f.fileno()).st_size != size

    return truncated or grew


def _checked_number(header, field, low, high):
    """A JSON number we can actually use, or ValueError naming the field."""
    value = header[field]  # KeyError here means "missing", handled separately
    # bool is a subclass of int, so `true` would sail through the isinstance.
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(field)
    if not math.isfinite(value) or not low <= value <= high:
        raise ValueError(field)
    return value


class UnsafePath(Exception):
    """A PUT or DELETE path that would land outside shared_dir."""


def path_is_lexically_safe(rel_path):
    """No absolute paths, no ".." component. Cheap, and runs before anything
    touches the disk — os.makedirs on "../evil/x" would create directories
    outside dest_dir before any resolution got a look in."""
    if not rel_path or rel_path.startswith("/"):
        return False
    return ".." not in rel_path.split("/")


def safe_path(base, rel_path):
    """Resolve rel_path under base, or None if it would escape.

    realpath() on the whole path is what the DELETE guard used to do, and it
    only works because the file already exists. A PUT names a file that usually
    does NOT exist yet, and realpath of a missing path resolves differently. So
    walk the path a component at a time, resolve any symlink as we meet it, and
    re-check containment each time — the one escape "..-free and relative"
    still leaves open is a symlink pointing out of the tree.
    """
    base_real = os.path.realpath(base)
    if not path_is_lexically_safe(rel_path):
        return None

    current = base_real
    for comp in rel_path.split("/"):
        if comp in ("", "."):
            continue
        nxt = os.path.join(current, comp)
        if os.path.islink(nxt):
            if not os.path.exists(nxt):
                return None  # broken link - refuse it
            nxt = os.path.realpath(nxt)
        if nxt != base_real and not nxt.startswith(base_real + os.sep):
            return None
        current = nxt

    if current == base_real:
        return None  # the path named the root itself
    return current


def recv_file_body(conn, dest_dir, header):
    """Write the raw body that follows a PUT header. header already parsed."""
    rel_path = header["path"]
    size = int(_checked_number(header, "size", 0, MAX_FILE_SIZE))
    mtime = _checked_number(header, "mtime", -MAX_MTIME, MAX_MTIME)

    # Check the path BEFORE creating anything: a hand-crafted "../../x" used to
    # be joined onto dest_dir and written unchecked, giving any authenticated
    # peer an arbitrary file write on the receiver.
    if not path_is_lexically_safe(rel_path):
        raise UnsafePath(repr(rel_path))

    full_path = os.path.join(dest_dir, rel_path)
    os.makedirs(os.path.dirname(full_path), exist_ok=True)

    # The parent exists now, so the symlink-aware guard can resolve it.
    full_path = safe_path(dest_dir, rel_path)
    if full_path is None:
        raise UnsafePath(repr(rel_path))

    # Land the bytes beside the target, then rename. Opening full_path directly
    # truncates the existing copy the moment the transfer starts, so a peer that
    # dies mid-file leaves a stub where a good file used to be. os.replace is
    # atomic within a filesystem, which is why the temp is a sibling.
    tmp_path = f"{full_path}.{os.getpid()}.tmp"
    try:
        bytes_received = 0
        with open(tmp_path, "wb") as f:
            while bytes_received < size:
                chunk = conn.recv(min(65536, size - bytes_received))
                if chunk == b"":
                    raise ConnectionError("peer closed mid-file - truncated transfer")
                f.write(chunk)
                bytes_received += len(chunk)

        # Stamp the mtime before the rename, so the file is never briefly
        # visible at its final path with the wrong timestamp.
        os.utime(tmp_path, (mtime, mtime))
        os.replace(tmp_path, full_path)
    except BaseException:
        try:
            os.remove(tmp_path)
        except OSError:
            pass
        raise

    return rel_path


def recv_file(conn, dest_dir):
    """Read a PUT header, then its body."""
    header_bytes = recv_msg(conn, MAX_CONTROL_FRAME)
    if header_bytes is None:
        return None
    header = json.loads(header_bytes.decode("utf-8"))
    return recv_file_body(conn, dest_dir, header)


def build_manifest(root_dir, errors=None):
    """Walk root_dir and return a manifest:
    { relative_path: {"size": int, "mtime": float}, ... }
    keyed by each file's path *relative to root_dir*.

    Pass `errors` (a list) to find out whether the walk was COMPLETE. os.walk
    defaults to onerror=None, which silently swallows every directory it can't
    read, and os.stat was called unguarded. Either way files vanish from the
    manifest while it still looks authoritative — and to the diff, a file we
    couldn't see is indistinguishable from one the user deleted, so --delete
    removes it from the peer. A chmod 000 on a subdirectory was enough.
    """
    manifest = {}

    def on_error(exc):
        if errors is not None:
            errors.append(exc)

    for dirpath, dirnames, filenames in os.walk(root_dir, onerror=on_error):
        for filename in filenames:
            full_path = os.path.join(dirpath, filename)
            rel_path = os.path.relpath(full_path, root_dir)
            try:
                info = os.stat(full_path)
            except OSError as e:
                # ENOENT is a benign race: it really is gone. Anything else (a
                # path past PATH_MAX, a permissions hole) means the file EXISTS
                # and we simply cannot see it.
                if e.errno != errno.ENOENT and errors is not None:
                    errors.append(e)
                continue
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
        if (
            remote_info is None
            or info["size"] != remote_info["size"]
            or abs(info["mtime"] - remote_info["mtime"]) > mtime_tolerance
        ):
            to_put.append(path)

    to_delete = [path for path in remote if path not in local]
    return to_put, to_delete


def send_delete(conn, rel_path):
    """Ask the peer to remove one file (no body)."""
    msg = json.dumps({"op": "DELETE", "path": rel_path}).encode("utf-8")
    send_msg(conn, msg)


def handshake(conn, token):
    """Client side of HELLO: send the token, return the server's reply dict
    (e.g. {"op": "OK"} or {"op": "ERROR", ...}), or None if the peer closed.
    Caller decides what to do with a non-OK reply"""
    send_msg(conn, json.dumps({"op": "HELLO", "token": token}).encode("utf-8"))
    reply = recv_msg(conn, MAX_CONTROL_FRAME)  # OK or ERROR
    if reply is None:
        return None
    return json.loads(reply.decode("utf-8"))
