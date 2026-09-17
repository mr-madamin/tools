# LAN Folder Sync — C

A C port of [`lan_folder_sync/`](../lan_folder_sync), written to be compared
against it. Same one-way folder mirror over raw TCP, same wire protocol, same
CLI, same test scenarios — so the two can be swapped for each other and read
side by side.

- **Receiver** — runs the server, holds the copy.
- **Source** — runs the push, holds the truth.

> Same counterintuitive shape as the Python version: the **server is the passive
> side** — it just receives and obeys. The **push client is the brain** — it
> reads its folder, diffs against the peer, and decides every PUT and DELETE.

**The two implementations interoperate.** A C pusher syncs into a Python server
and vice versa, and each project's test suite passes against the other project's
server unmodified. That's the pass condition for "is this really the same thing".

## 1. Build

```
make          # builds bin/sync_server, bin/sync_push, bin/file_sender, bin/file_receiver
make tests    # builds the runners in tests/
make clean
```

No dependencies — C11 and libc. Everything runs from this directory
(`lan_folder_sync_c/`).

## 2. One-time setup (do this on *both* machines)

```
cp config.example.json config.json
```

Then edit `config.json`:

- **`token`** — set the **same random string on both machines**. Every connection
  must present it (`HELLO`); a mismatch is refused. Make it long.
- **`shared_dir`** — the folder this machine syncs (the truth on the source, the
  copy on the receiver). **Use an absolute path** — `/Users/you/folder`,
  not `~/Desktop/folder`. `~` is **not** expanded; a `~`-path silently walks an
  empty folder (pushes nothing — and with `--delete`, can look like "everything
  was deleted"). Make sure the folder exists first: `mkdir -p <shared_dir>`.

`config.json` is **per-machine and gitignored** — it carries the token, so it's
never committed. `config.example.json` is the committed template you copy from.

The binaries live in `bin/`, so config lookup is a little wider than Python's
"next to the source file": `$LAN_FOLDER_SYNC_CONFIG`, then `./config.json`, then
next to the executable, then its parent. Run from this directory and the second
one wins.

## 3. On the receiver — find its LAN IP and serve

```
ipconfig getifaddr en0        # prints this machine's LAN IP, e.g. 192.168.1.42
./bin/sync_server --lan       # binds that IP and prints it
```

`--lan` reads the `en0` address (via `getifaddrs`, no subprocess) and binds
*only* that IP — never `0.0.0.0`. If you're offline or Wi-Fi isn't `en0`, it
exits loudly rather than silently falling back to loopback. **Copy the IP it
prints** — you need it on the source.

(Without `--lan` the server binds `127.0.0.1` for local dev and the test suite.)

## 4. On the source — point at the receiver

Paste the receiver's IP into this machine's `config.json`:

```json
"peer": { "host": "192.168.1.42", "port": 8765 }
```

## 5. Sync

On the **receiver**, leave the server running:

```
./bin/sync_server --lan
```

On the **source**, push:

```
./bin/sync_push --dry-run            # preview: show what WOULD change, touch nothing
./bin/sync_push                       # push new/changed files
./bin/sync_push --delete              # also remove files the source no longer has
./bin/sync_push --dry-run --delete    # preview a mirror, including the deletions
```

- **`--dry-run`** — read-only. Prints the plan (puts *and* deletes) and transfers
  nothing. Always run this first.
- **`--delete`** — makes the receiver an exact mirror by removing files that are
  gone from the source. Without it, extra files on the receiver are reported but
  left in place.

Positional overrides work exactly as in the Python version:

- `sync_server <shared_dir> [port] [--lan]`
- `sync_push <peer_host> <root_dir> [--flags]` (push port still comes from
  `config.json` `peer.port`)

Both accept `--help`, and both **reject** an unrecognised `--flag` instead of
ignoring it — a typo like `--dryrun` would otherwise push for real, and
`--lann` would bind loopback while you believed you were serving the LAN.

## 6. The protocol (identical to the Python version)

A frame is a **4-byte big-endian length prefix** followed by a **UTF-8 JSON
payload**. A `PUT` frame is followed by exactly `size` raw body bytes — the only
part of the stream that isn't framed, which is why a `PUT` header missing its
`size` has to end the session: there's no way to find the next frame boundary.

| op | direction | payload |
|----|-----------|---------|
| `HELLO` | client → server | `{"op":"HELLO","token":…}` → `{"op":"OK"}` or `ERROR` |
| `MANIFEST` | client → server | request; reply is `{"op":"MANIFEST","files":{path:{size,mtime}}}` |
| `PUT` | client → server | `{"op":"PUT","path":…,"size":…,"mtime":…}` + body |
| `DELETE` | client → server | `{"op":"DELETE","path":…}`, no reply on success |
| `BYE` | client → server | ends the session |
| `ERROR` | server → client | `{"op":"ERROR","message":…}`, then close |

`mtime` is seconds as a JSON float. The diff treats a file as unchanged when
size *and* mtime match, with a 1 ms window to absorb timestamp round-trip error.

That window used to be **2 seconds**, and it silently swallowed real edits:
change a file without changing its size, land the new mtime within 2 s of the
peer's copy, and the diff called it unchanged — permanently, because the two
mtimes never drift further apart. Measured, the actual round-trip error is under
one microsecond (both C→C and C→Python), so 2 s was six orders of magnitude
wider than the thing it was there for. It isn't zero, because a filesystem that
stores coarser timestamps would then resend every file on every run — a worse
failure than a 1 ms gap.

**What size + mtime cannot see.** A file whose content changes while its size
*and* mtime both stay the same is invisible to the diff, permanently. That
isn't a narrow float problem like the one above — it's inherent to the
heuristic, and `rsync` behaves the same way (hence its `--checksum`). Reaching
it takes a tool that deliberately preserves timestamps: `touch -r`, `cp -p`,
or extracting an archive over the folder, since `tar` and `unzip` restore the
stored mtime. Ordinary editing always moves the mtime, so normal use is safe.
If you've done something like that, the fix today is to touch the affected
files so their mtime moves, and re-push.

## 7. Safety notes

Same guarantees as the Python version, plus one:

- **Dry-run first.** `--dry-run` shows the full plan before you commit to it.
- **`--delete` mirrors deletions** — it removes files on the receiver. A mistyped
  or empty source could delete a lot; preview with `--dry-run --delete` first.
- **An empty source can't wipe the peer.** `sync_push` refuses three ways of
  arriving at "the source has nothing, so delete everything": a `root_dir`
  starting with `~` (never expanded), a `root_dir` that isn't a directory, and a
  real folder that walks to zero files while `--delete` is set. The first two
  exit before connecting. `--dry-run --delete` still prints the full plan, so
  you can see what the guard just stopped.
- **The token is required on every connection.** No token, or the wrong one, and
  the server refuses the session.
- **Paths are confined to `shared_dir`** — for `DELETE` *and* for `PUT`. Absolute
  paths and any `..` component are rejected before anything touches the disk, and
  each surviving component is re-checked after symlink resolution.

- **Unknown flags are refused, on both programs.** `sync_push.py` already does
  this; `sync_server.py` does not, so a typo'd `--lann` silently serves loopback
  there. The second thing worth porting back.

- **Nothing blocks forever.** `sync_push` bounds each phase the way
  `sync_push.py` does — 8 s to connect, 15 s for a HELLO or MANIFEST reply,
  300 s for a stalled send — and says which one expired, since "the packet never
  arrived" and "the peer accepted and went quiet" need opposite fixes. A dropped
  SYN now fails in 8 s instead of macOS's ~75 s default.

  `sync_server` bounds a session at 300 s of silence. It serves one peer at a
  time, so before this a single client that connected and said nothing locked
  out everyone else until the process was killed. **`sync_server.py` still has
  that wedge** — it has no timeouts at all. The third thing worth porting back.

- **A frame is refused on its length prefix alone.** Those 4 bytes can claim up
  to 4 GB, and the C code used to `malloc` that on the spot — before reading any
  payload, and before the HELLO, so an unauthenticated peer could try to exhaust
  memory with four bytes and kill the process (`xmalloc` dies on failure).
  There are two ceilings, because the two directions
  carry different frames: `MAX_CONTROL_FRAME` (1 MB) for everything a client
  sends, and `MAX_FRAME` (64 MB) for the MANIFEST reply, which lists every file
  in the folder and is legitimately large — a 15,000-file folder already
  produces ~1 MB. Over the cap the server answers `ERROR: frame too large` and
  ends the session; it can't resync a stream whose payload it never read.
  `bad_frame_test` covers this pre-auth.

  `framing.py` had no cap either, though it was a milder bug there: its
  `recv_exactly` appends chunks as they arrive instead of preallocating, so a
  claimed length cost nothing until the attacker actually sent the bytes — no
  four-byte amplification, and no `die()` on a failed allocation. It now
  carries the same two ceilings and raises `FrameTooLarge`, refusing with the
  same `frame too large` message.

- **A file that changes while it's being sent can't corrupt the files after
  it.** `send_file` opens the file and `fstat`s *that descriptor* (rather than
  `stat`-then-`open`, which lets the path be swapped in between), then writes
  exactly the number of body bytes it announced — padding if the file was
  truncated, stopping early if it grew. That matters because the body is the
  only unframed part of the stream: one byte off and the receiver reads the next
  header as file content, silently corrupting every later file in the session.
  The push reports `SEND_CHANGED` as a warning and keeps going, since the stream
  is still intact and the affected file loses the mtime comparison next time and
  gets resent. `truncate_test` pins this down without needing a server.
  **`send_file` in `framing.py` had the same desync, and now carries the same
  contract.** Its version had both races: `os.path.getsize(full_path)` followed
  by a separate `open(full_path)`, then a read-to-EOF loop with no length
  agreement. Reproduced over a socketpair — declared 5,242,880 bytes, delivered
  65,558, and the following frame never arrived. It now `os.fstat`s the open
  handle and writes exactly the size it announced, returning True when the file
  changed so the pusher can say so.

- **An interrupted PUT can't damage the file it was replacing.** The receiver
  writes the body to `<target>.<pid>.tmp` in the same directory and `rename()`s
  it into place — atomic within a filesystem, which is exactly why the temp is a
  sibling rather than somewhere in `/tmp`. Opening the destination directly with
  `O_TRUNC` destroys the good copy the instant the transfer begins: aborting a
  5 MB PUT after 100 KB turned a 26-byte file into 102,400 bytes of padding.
  Now the old copy stays until the new one is whole, and every failure path
  removes the temp. The mtime is stamped before the rename, so the file is never
  briefly visible with the wrong timestamp (which the next diff would read as
  "changed" and resend). `atomic_test` covers it. **`recv_file_body` in
  `framing.py` wrote the destination directly, and has now been fixed the same
  way** — `os.replace()` onto a sibling temp, with the temp removed on every
  failure path.

  This buys atomic *visibility*, not durability: there's no `fsync` before the
  rename, so a power cut can still lose a just-written file. That's a different
  failure, and paying an fsync per file to close it isn't obviously worth it
  for a LAN folder sync.

- **The numbers in a PUT header are validated before they're used.** `size` and
  `mtime` arrive as JSON doubles and were cast straight to `long long` and
  `time_t`. A double outside the target's range makes that cast *undefined* —
  `"size": 1e999` is a one-line frame, and UBSan reports `inf is outside the
  range of representable values of type 'long long'`. Worse in practice,
  `"size": -1` skipped the write loop entirely and renamed an empty temp over
  the destination, blanking a real file while the log said `received`:

  ```
  before: 23 bytes -> 'IMPORTANT REAL CONTENT'
  after:   0 bytes -> ''
  ```

  Both are now rejected with `ERROR: PUT header has an out-of-range 'size'`
  before anything is cast, opened or written. `size` must be finite and within
  `0 .. MAX_FILE_SIZE` (64 GiB, which also stops a peer from claiming a file
  large enough to fill the disk); `mtime` must be finite and sane. Covered by
  `bad_frame_test` and `atomic_test`.

  `framing.py` now carries the same two constants and the same check, wording
  the refusal identically. Its version also has to exclude `bool`, which is a
  subclass of `int` in Python — `"size": true` would otherwise pass an
  `isinstance` test and then compare as 1.

- **A source listing that isn't complete can't drive deletions.** `walk()`
  skips whatever it can't read — a directory without permission, or a path past
  `PATH_MAX` — and those entries used to vanish without a trace: the recursive
  call's return value was discarded, and an `lstat` failure just `continue`d.
  The manifest came back short and looked authoritative. To the diff, a file we
  couldn't see is indistinguishable from a file you deleted, so `--delete`
  removed it from the peer. One `chmod 000` on a subdirectory was enough to
  delete its contents from the other machine while the originals sat there
  untouched.

  A partial walk is now reported and, on the pusher, refuses `--delete` — the
  empty-source guard above, generalised: don't mirror a picture you know has
  holes in it. `ENOENT` is exempt, since a file disappearing mid-walk really is
  gone rather than hidden. `--dry-run` still previews, and a fully readable tree
  is never flagged. `partial_test` covers it.

  **`build_manifest` in `framing.py` had the same hole by a different route,
  and now carries the same guard.** `os.walk` defaults to `onerror=None`, which
  swallows the error and walks on, and `os.stat` was called unguarded on top of
  that. It takes an optional `errors` list now; `sync_push.py` passes one and
  refuses `--delete` when it comes back non-empty, naming the directory it
  couldn't read. Same `ENOENT` exemption, since a file that vanished mid-walk
  really is gone.

- **Both servers line-buffer stdout.** The C side has always called
  `setvbuf(stdout, NULL, _IOLBF, 0)`; `sync_server.py` did not, so with its
  output redirected to a file — a log, a service manager — Python block-buffered
  and the log stayed *completely empty*, startup banner included, until the
  process exited. Found while checking that the frame-cap refusal was logged: it
  was, and nothing was visible. Both now reconfigure stdout at startup.

- **A skipped symlink is reported, not swallowed.** Symlinked directories
  aren't followed — `os.walk` doesn't either, and that parity is deliberate —
  but skipping them in silence is its own failure: symlink a folder into your
  sync directory and the push says `Local: 1 files`, transfers nothing from it,
  and never mentions why. The manifest now carries the names it passed over
  (broken links too, which can't be read at all), the push lists them, and the
  server notes the count. A symlink to a *file* is still synced, stat'd through
  the link, exactly as before. `symlink_test` covers all four cases.

- **A mistyped `config.json` value names itself.** `json_get_str()` returns
  NULL both when a key is absent and when it's present but the wrong type, so
  every mistyped value used to fall through to a default in silence. Quoting the
  port by mistake — `"port": "9999"` — simply gave you 8765 and then a
  "connection refused" with nothing pointing at the config; `"token": 12345`
  reported `missing "token"`, sending you to look for a key that was right
  there. Absent is still fine (the defaults are the point); wrong type now
  stops with the key named. `config_test` covers it — `config.c` is
  hand-edited by every user and had no test at all before.

- **Editing a header rebuilds what uses it.** The Makefile listed only `.c`
  files as prerequisites, so touching `framing.h` recompiled *nothing* —
  `make check` could pass against stale objects that didn't contain the change
  under test. It bit during this very session: the mtime fix above appeared not
  to work, because the binary still held the old constant. `-MMD -MP` now emits
  a `.d` per object and the Makefile `-include`s them; touching `framing.h`
  rebuilds all five dependants.

- **Smaller sharp edges.** `safe_path` uses `strtok_r` rather than `strtok`,
  whose cursor lives in one static slot shared by every caller. Ports are parsed
  with `strtol` and range-checked on both the command line and in `config.json`
  — `atoi` returned 0 for `http` and truncated anything over 65535, so a typo
  bound a port nobody meant, and the server looked like it had started fine.

- **Nested JSON is bounded.** `json.c` is a recursive-descent parser, so every
  `[` or `{` costs a C stack frame — and the header is parsed *before* the
  HELLO. 200 KB of `[`, from an unauthenticated peer and comfortably under the
  frame cap, walked the release build off the end of its stack and killed the
  server outright. Depth is now capped at 64, which is an order of magnitude
  past anything real (a manifest nests three deep).

  **`sync_server.py` died on the same input, and has now been fixed too.** Its
  parser raises `RecursionError` rather than smashing the stack, but the server
  caught only `(UnicodeDecodeError, json.JSONDecodeError)` around `json.loads`
  and `ConnectionError` around the session, so it escaped both and took the
  process down with a traceback. Fixing that turned up a *second*
  unauthenticated kill in the same spot: `json.loads("123")` is a valid `int`,
  and `header.get("op")` then raised `AttributeError` straight out of the accept
  loop. A four-byte frame stopped the server. Both are refused now, and a
  catch-all around the session means no future frame can kill the process
  either — `KeyboardInterrupt` derives from `BaseException`, so Ctrl-C still
  works.

  Found by `make asan` and a few hundred malformed frames, not by reading.

  A header that parses but isn't an object now gets its own message —
  `malformed header: not a JSON object` — on both sides. Blaming UTF-8 for
  `123` sent you hunting in the wrong place.

  **This used to be the main deliberate behaviour difference, and no longer is.**
  `sync_server.py` ran its confinement guard on `DELETE` only; `recv_file_body`
  joined the incoming path onto `shared_dir` and wrote it unchecked, so an
  authenticated peer had an arbitrary file write on the receiver — a `PUT` with
  `"path": "../../ESCAPED.txt"` landed two directories outside the shared
  folder, verified. `framing.py` now carries `path_is_lexically_safe` and
  `safe_path`, the same two-stage guard as the C server: reject absolute paths
  and `..` before `os.makedirs` can create anything, then resolve component by
  component so a symlink pointing out of the tree is caught too.

  `DELETE` now shares that implementation rather than keeping its own. The old
  DELETE check `realpath`'d the whole path, which worked only because the file
  already existed — a PUT names a file that usually doesn't — and its
  `target != base and not target.startswith(base + os.sep)` let a path
  resolving to the shared folder *itself* through. `safe_path` refuses that, as
  C always did.

## 8. Running the tests (developers)

Same rig as the Python suite: the runners dial `127.0.0.1:8765` and plant into /
read from `sandbox/received`.

- Keep the local `config.json` at the test values: `shared_dir` =
  `sandbox/received`, `peer.host` = `127.0.0.1`, `peer.port` = `8765`.
- Start the test server **without `--lan`** — the tests dial loopback, and `--lan`
  binds the LAN IP *only*.

```
make check    # starts a loopback server, runs every runner, tears it down
make asan     # the same, rebuilt with AddressSanitizer + UBSan
```

`make check` is the one command that proves the tree. `make asan` builds into
`bin-asan/` so it never mixes objects with the normal build; note that
LeakSanitizer is unavailable on macOS, so it catches memory *errors* and
undefined behaviour, not leaks.

To drive the runners by hand instead:

```
mkdir -p sandbox/source sandbox/received
make tests
./bin/sync_server            # loopback, in one terminal
./bin/hello_test             # in another
./bin/bad_frame_test
./bin/dry_run_test
./bin/delete_test
./bin/nasty_test             # self-contained — needs no server
./bin/truncate_test          # self-contained
./bin/symlink_test           # self-contained
./bin/config_test            # self-contained
./bin/atomic_test
./bin/partial_test
```

### Cross-checking against the Python implementation

The strongest equivalence check is to point one language's tests at the other
language's server. From this directory, with `bin/sync_server` running:

```
ASDF_PYTHON_VERSION=3.12.13 PYTHONPATH=../lan_folder_sync \
  python3 -m tests.hello_test        # Python's suite, C's server
```

and the reverse — run `python3 sync_server.py "$PWD/sandbox/received" 8765` from
`../lan_folder_sync`, then run `./bin/hello_test` here. Both directions pass,
with byte-identical error messages.

## 9. What the port actually cost

| | Python | C |
|---|---|---|
| core (`framing`, `config`, both entry points, the two demos) | 394 lines | 1,960 lines |
| test suite | 446 lines | 664 lines |
| dependencies | stdlib | libc |
| binary | — | 54 KB per program |

The 5× is not distributed evenly. `sync_push.c` is 151 lines against
`sync_push.py`'s 76 — the *logic* barely grew. The bulk is machinery Python ships
in its standard library:

- **`json.c` (353 lines)** — a whole JSON reader/writer, including `\uXXXX`
  escapes and surrogate pairs, for what Python spells `import json`. This is the
  single biggest chunk of the port.
- **`util.c` (228 lines)** — a growable byte buffer (`str + str`), a list of
  strings (`list.append`), `os.path.join`, `os.path.dirname`, `os.makedirs`, and
  a UTF-8 validator to reproduce what `bytes.decode("utf-8")` rejects for free.
- **`framing.c` (552 lines vs `framing.py`'s 139)** — the protocol itself is a
  near-line-for-line translation. The growth is `os.walk` as an `opendir`
  recursion, and the manifest: Python compares two dicts by key, so C either
  needs a hash table or, as here, sorts both sides and merges.

Things C made easier or better, for the sake of an honest table:

- **`--lan`** is `getifaddrs()` instead of shelling out to `ipconfig` and parsing
  stdout — fewer moving parts and no subprocess.
- **The error paths are visible.** Python's `recv_exactly` returning `None` is
  easy to skim past; `FRAME_OK` / `FRAME_EOF` / `FRAME_ERR` at every call site
  makes "the peer vanished" impossible to forget to handle.
- **The path guard got stronger**, mostly because writing `safe_path` by hand
  forced the question of what `realpath` does with a file that doesn't exist yet
  — a question `os.path.realpath` answers silently, and differently.

What Python keeps, unambiguously: memory. Every `free` in this tree is a line
Python never had to write, and every one of them is a chance to be wrong.
