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

`mtime` is seconds as a JSON float; the diff treats a difference of ≤ 2 s as
unchanged, so the two languages' clock precision doesn't cause spurious resends.

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

  **This is the main deliberate behaviour difference.** `sync_server.py` runs its
  confinement guard on `DELETE` only; `recv_file_body` joins the incoming path
  onto `shared_dir` and writes it unchecked, so a hand-crafted `PUT` with
  `"path": "../../x"` writes outside the shared folder on the Python receiver.
  The C server refuses it. Worth porting back.

## 8. Running the tests (developers)

Same rig as the Python suite: the runners dial `127.0.0.1:8765` and plant into /
read from `sandbox/received`.

- Keep the local `config.json` at the test values: `shared_dir` =
  `sandbox/received`, `peer.host` = `127.0.0.1`, `peer.port` = `8765`.
- Start the test server **without `--lan`** — the tests dial loopback, and `--lan`
  binds the LAN IP *only*.

```
mkdir -p sandbox/source sandbox/received
make tests
./bin/sync_server            # loopback, in one terminal
./bin/hello_test             # in another
./bin/bad_frame_test
./bin/dry_run_test
./bin/delete_test
./bin/nasty_test             # self-contained — needs no server
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
