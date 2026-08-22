# LAN Folder Sync

A one-way folder mirror over the local network, built on raw TCP sockets. One
machine holds the **source of truth** and *pushes*; the other **receives** and
becomes an exact copy — including deletions when you ask for them.

- **Receiver** — runs the server, holds the copy.
- **Source** — runs the push, holds the truth.

> Counterintuitive but important: the **server is the passive side** — it just
> receives and obeys. The **push client is the brain** — it reads its folder,
> diffs against the peer, and decides every PUT and DELETE.

Everything runs from this directory (`lan_folder_sync/`).

## 1. One-time setup (do this on *both* machines)

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

## 2. On the receiver — find its LAN IP and serve

```
ipconfig getifaddr en0        # prints this machine's LAN IP, e.g. 192.168.1.42
python3 sync_server.py --lan  # binds that IP and prints it
```

`--lan` auto-detects the `en0` address and binds *only* that IP (never `0.0.0.0`).
If you're offline or Wi-Fi isn't `en0`, it exits loudly rather than silently
falling back to loopback. **Copy the IP it prints** — you need it on the source.

(Without `--lan` the server binds `127.0.0.1` for local dev and the test suite.)

## 3. On the source — point at the receiver

Paste the receiver's IP into this machine's `config.json`:

```json
"peer": { "host": "192.168.1.42", "port": 8765 }
```

## 4. Sync

On the **receiver**, leave the server running:

```
python3 sync_server.py --lan
```

On the **source**, push:

```
python3 sync_push.py --dry-run            # preview: show what WOULD change, touch nothing
python3 sync_push.py                       # push new/changed files
python3 sync_push.py --delete              # also remove files the source no longer has (mirror)
python3 sync_push.py --dry-run --delete    # preview a mirror, including the deletions
```

- **`--dry-run`** — read-only. Prints the plan (puts *and* deletes) and transfers
  nothing. Always run this first.
- **`--delete`** — makes the receiver an exact mirror by removing files that are
  gone from the source. Without it, extra files on the receiver are reported but
  left in place.

## 5. Real two-machine example (and how to sync without touching the test config)

Say **A** is the source at `/Users/alice/folder` and **B** is the
receiver at `/Users/bob/folder`, with B's LAN IP `192.168.1.42`.
Same token on both.

**On B (receiver):**

```
mkdir -p /Users/bob/folder
python3 sync_server.py /Users/bob/folder --lan
```

**On A (source):**

```
python3 sync_push.py 192.168.1.42 /Users/alice/folder --dry-run
python3 sync_push.py 192.168.1.42 /Users/alice/folder
python3 sync_push.py 192.168.1.42 /Users/alice/folder --delete
```

Those trailing paths/IP are **positional overrides**:

- `sync_server.py <shared_dir> [port] [--lan]` — the folder to serve.
- `sync_push.py <peer_host> <root_dir> [--flags]` — the peer's address, then the
  local folder to read. (Push port still comes from `config.json` `peer.port`.)

**Why override instead of editing `config.json`?** On the machine you run the test
suite from, `config.json` *is* the test rig (`sandbox/received`, `127.0.0.1` — see
§7). Editing its `shared_dir`/`peer.host` for a real sync would break the tests.
Passing the real folder and IP as arguments does the real sync while leaving the
test config untouched — the token is still read from `config.json`, so B just needs
that same token. (On a machine that never runs the tests, editing `config.json`
per §1–§3 is fine — use whichever is simpler.)

Note **where files land**: the pusher sends folder-*relative* paths; the receiver
joins them onto **its own `shared_dir`**. The source picks *what* and the relative
structure; the receiver's config decides the destination root (that's the path-
confinement guard, §6).

## 6. Safety notes

- **Dry-run first.** `--dry-run` shows the full plan before you commit to it.
- **`--delete` mirrors deletions** — it removes files on the receiver. A mistyped
  or empty source could delete a lot; preview with `--dry-run --delete` first.
- **The token is required on every connection.** No token, or the wrong one, and
  the server refuses the session.
- **Paths are confined to `shared_dir`.** The server resolves every incoming path
  and refuses anything that would escape the shared folder (`../…`, absolute
  paths, symlink tricks).

## 7. Running the tests (developers)

The integration tests in `tests/` dial `127.0.0.1` and plant into / read from
`sandbox/received`. So on the machine running the suite:

- Keep the **local `config.json`** at the test values: `shared_dir` =
  `sandbox/received`, `peer.host` = `127.0.0.1`, `peer.port` = `8765`. The test
  server takes its directory from config (no positional override in the test
  invocation), so these must match or the suite can't find its files.
- Start the test server **without `--lan`** — the tests dial loopback, and `--lan`
  binds the LAN IP *only* (it refuses `127.0.0.1`).

Run each test as a module, from this directory, with a server up in another
terminal:

```
python3 sync_server.py                 # loopback, in one terminal
python3 -m tests.hello_test            # in another  (NOT python3 tests/hello_test.py)
```
