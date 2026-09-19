# LAN Folder Sync — Swift

A Swift port of [`lan_folder_sync/`](../lan_folder_sync), written to be compared
against it and against the [C port](../lan_folder_sync_c). It's the same one-way
folder mirror over raw TCP, with the same wire protocol, the same CLI and the
same safety guards.

- **Receiver** runs the server and holds the copy.
- **Source** runs the push and holds the truth.

> As in the other two, the **server is the passive side**: it receives and obeys.
> The **push client is the brain**: it reads its folder, diffs against the peer,
> and decides every PUT and DELETE.

**All three implementations interoperate**, and this is checked, not assumed.
The Python suite and the C suite both pass against the Swift server, the C
suite drives the Swift pusher, and the Swift pusher syncs into the Python server
(see §6).

## 1. Build

```
make          # swift build -c release, then copies the four programs into bin/
make test     # swift test — self-contained, needs no running server
make clean
```

You need Swift 6 (Xcode 16 or later). There are no package dependencies:
Foundation and Darwin only. Everything runs from this directory
(`lan_folder_sync_swift/`).

The binaries are copied into `bin/` rather than run from `.build/release/`, so
the paths match the C port (`./bin/sync_server`). That's also what lets the C
test runners, which call `./bin/sync_push`, drive this build.

## 2. One-time setup (do this on *both* machines)

```
cp config.example.json config.json
```

It's the same file, with the same rules, as the Python version:

- **`token`**: the **same random string on both machines**.
- **`shared_dir`**: an **absolute path**. `~` is not expanded.
- **`peer`**: `{ "host": "<receiver's LAN IP>", "port": 8765 }`, needed on the source.

`config.json` is gitignored because it carries the token.

The program looks for the config in the same order as the C port: first
`$LAN_FOLDER_SYNC_CONFIG`, then `./config.json`, then next to the executable,
then its parent. A value of the wrong type is reported by name. For example,
`"port": "9999"` says `peer.port must be a number, not in quotes` rather than
silently falling back to 8765.

## 3. Use

On the **receiver**:

```
ipconfig getifaddr en0        # this machine's LAN IP
./bin/sync_server --lan       # binds that IP only, never 0.0.0.0
```

On the **source**:

```
./bin/sync_push --dry-run            # preview: show what WOULD change, touch nothing
./bin/sync_push                      # push new/changed files
./bin/sync_push --delete             # also remove files the source no longer has
./bin/sync_push --dry-run --delete   # preview a mirror, including the deletions
```

Positional overrides work as in Python:

- `sync_server <shared_dir> [port] [--lan]`
- `sync_push <peer_host> <root_dir> [--flags]`

Both programs accept `--help`, and both **reject** unknown flags.

When a push can't connect, it fails in 8 s with a diagnosis: no answer
(firewall/router), refused (server not running, or not `--lan`), or unreachable
(no route/VPN). This is identical to §8 of the Python README.

## 4. What's the same, exactly

- **Wire protocol:** a 4-byte big-endian length prefix, UTF-8 JSON, and raw PUT
  bodies. The ops are `HELLO`, `MANIFEST`, `PUT`, `DELETE`, `BYE` and `ERROR`,
  and the frame caps are the same (1 MB control, 64 MB manifest).
- **Error messages:** the server's `ERROR` texts match the Python and C servers
  word for word (`bad token`, `unknown op: 'MANFEST'`,
  `PUT header has an out-of-range 'size'`, and so on).
- **Every guard:**
  - path confinement for PUT and DELETE, including symlink escapes
  - atomic temp-then-rename receive
  - `size`/`mtime` bounds
  - the declared-size contract when a file changes mid-send
  - no `--delete` from an empty or partial source listing
  - connect/protocol/transfer timeouts

## 5. Where it follows the C port rather than `framing.py`

The C README lists these as fixes "worth porting back" to Python. Here they're
simply in place:

| | Python | C and Swift |
|---|---|---|
| mtime window in the diff | 2 s (misses same-size edits) | 1 ms |
| server idle session | waits forever | dropped after 300 s |
| unknown server flag (`--lann`) | ignored, serves loopback | refused |
| skipped symlinked dirs | silent | listed by the push, counted by the server |
| mistyped `config.json` value | falls back to a default | named |

## 6. Tests

```
make test             # Swift Testing: 35 tests, in-process, ~1 s
make interop-python   # Python suite  -> Swift server
make interop-c        # C suite       -> Swift server + Swift pusher
make check            # all of the above
```

`make test` runs the real `serveSession` on an **ephemeral port** against a
temp directory, so it needs no server, no `config.json` and no free port 8765.
It covers:

- HELLO gating
- 13 malformed-frame cases
- the pre-auth frame cap
- an idle-peer drop
- a PUT round trip, including mtime
- aborted PUT atomicity
- PUT and DELETE escapes (`..`, absolute paths, an out-of-tree symlink, the root itself)
- symlink and partial walks
- the diff window
- config typing
- a mid-transfer truncation that must not desync the stream

The `interop` targets start `./bin/sync_server` on 8765, run the sibling
project's runners against it, and stop it. They refuse to start if 8765 is
already taken, so they can never test the wrong server by accident. The Python
run serves `../lan_folder_sync/sandbox/received` **explicitly**, because that
project's `config.json` may point `shared_dir` at a real folder.

## 7. Swift-specific notes

These are the places where the port needed more than transliteration:

- **String equality isn't byte equality.** Swift compares `String`s by Unicode
  canonical equivalence (`"é"` precomposed `==` `"e\u{301}"`), and `hasPrefix`
  works on grapheme clusters. The filesystem, Python and C all compare bytes.
  So the path-confinement check and the token check compare `.utf8` views
  explicitly. There's a test that a canonically-equal but byte-different token
  is refused.
- **Traps can't be caught.** Python needed a catch-all around each session so
  one bad frame couldn't kill the server. In Swift, an out-of-range
  `Int64(someDouble)` or an integer overflow traps, and no `catch` can stop that.
  So every number that comes off the wire is range-checked before conversion.
  The server's robustness comes from having no trap sites, not from catching
  them.
- **`JSONSerialization` is safe on hostile input.** Checked before relying on
  it: 200 KB of `[` gives an ordinary parse error, where Python raised
  `RecursionError` and the C parser overflowed its stack. `1e999` and `NaN` are
  rejected, and `true` is distinguishable from `1` via `CFBoolean`. So there's
  no hand-written parser like `json.c`.
- **Typed throws.** `Socket.connect` throws `ConnectFailure` (timeout, refused,
  resolve, unreachable). The pusher `switch`es on it, so adding a failure case
  won't compile until it has an explanation. Swift 6 doesn't check
  exhaustiveness across several `catch` clauses, which is why it's a single
  `catch` plus a `switch`.
- **Blocking BSD sockets, not Network.framework.** The protocol is strictly
  sequential ("read 4 bytes, then N, then exactly `size`"). A thin blocking
  wrapper keeps the three ports readable side by side. Network.framework would
  be the natural next experiment, as an async rewrite of `Socket.swift`.
- **`opendir`/`readdir`, not `FileManager.enumerator`.** The enumerator skips
  unreadable directories unless you remember to give it an error handler. That's
  the same silent hole `os.walk(onerror=None)` had, which let `--delete` remove
  files the walk couldn't see.

## 8. Layout

```
Sources/SyncCore/       everything shared: Socket, Framing, Paths, Manifest,
                        Config, NetUtil, Server (the session loop), Util
Sources/sync_server/    argv, bind, accept loop
Sources/sync_push/      argv, preflight checks, diff, transfer, diagnostics
Sources/file_sender/    the one-file experiments, as in Python
Sources/file_receiver/
Tests/SyncCoreTests/    Swift Testing suites
```
