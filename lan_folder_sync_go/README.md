# LAN Folder Sync — Go

A Go port of [`lan_folder_sync/`](../lan_folder_sync), written to be compared
against it and against the [C](../lan_folder_sync_c),
[C++](../lan_folder_sync_cpp) and [Swift](../lan_folder_sync_swift) ports. Same
one-way folder mirror over raw TCP, same wire protocol, same CLI, same safety
guards.

- **Receiver** runs the server and holds the copy.
- **Source** runs the push and holds the truth.

> As in the other four, the **server is the passive side**: it receives and
> obeys. The **push client is the brain**: it reads its folder, diffs against
> the peer, and decides every PUT and DELETE.

**All five implementations interoperate**, and this is checked, not assumed:
the Python suite and the C suite both pass against this server, the C suite
drives this pusher, and this pusher syncs into the Python server byte for byte
(§6).

## 1. Build

```
make          # bin/sync_server, bin/sync_push, bin/file_sender, bin/file_receiver
make test     # the in-process suite — needs no server, no free port
make race     # the same suite under the race detector
make clean
```

No dependencies: the standard library only, no `go.sum`. The toolchain comes
from `.tool-versions` (Go 1.27.2 via asdf), and the Makefile prefers the asdf
shim so a fresh shell without `go` on `PATH` still builds. Everything runs from
this directory (`lan_folder_sync_go/`).

## 2. One-time setup (do this on *both* machines)

```
cp config.example.json config.json
```

It's the same file, with the same rules, as every other port:

- **`token`**: the **same random string on both machines**.
- **`shared_dir`**: an **absolute path**. `~` is not expanded.
- **`peer`**: `{ "host": "<receiver's LAN IP>", "port": 8765 }`, needed on the source.

`config.json` is gitignored because it carries the token.

The config is looked for in the same order as the C and C++ ports:
`$LAN_FOLDER_SYNC_CONFIG`, then `./config.json`, then next to the executable,
then its parent. A value of the wrong type is reported by name — `"port": "9999"`
says `peer.port must be a number, not in quotes` rather than silently falling
back to 8765.

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
- `sync_push <peer_host> <root_dir> [--flags]` (the push port always comes from
  `config.json` `peer.port`)

Both accept `--help`, and both **reject** unknown flags — a silently ignored
`--dryrun` is how a preview becomes a real push.

When a push can't connect it fails in 8 s with a diagnosis: no answer
(firewall/router), refused (server not running, or not `--lan`), or unreachable
(no route/VPN). A peer that accepts and then goes quiet is a *different* page,
after 15 s, because it points at the other end rather than the network.

## 4. What's the same, exactly

- **Wire protocol:** a 4-byte big-endian length prefix, UTF-8 JSON, and raw PUT
  bodies. The ops are `HELLO`, `MANIFEST`, `PUT`, `DELETE`, `BYE` and `ERROR`,
  and the frame caps are the same (1 MB control, 64 MB manifest).
- **Error messages:** the server's `ERROR` texts match the other four word for
  word (`bad token`, `unknown op: 'MANFEST'`,
  `PUT header has an out-of-range 'size'`, and so on), including Python's `repr`
  for a non-string op.
- **Every guard:**
  - path confinement for PUT *and* DELETE, including symlink escapes and the
    shared root itself
  - atomic temp-then-rename receive
  - `size`/`mtime` bounds checked before any conversion
  - the declared-size contract when a file changes mid-send
  - no `--delete` from an empty or partial source listing
  - connect / protocol / transfer timeouts, and a 300 s idle-session drop
  - a frame refused on its length prefix alone, before the HELLO
  - JSON nesting capped at 64

## 5. Where it follows the C and C++ ports rather than `framing.py`

The C README lists these as fixes "worth porting back" to Python. Here they are
simply in place:

| | Python | C, Swift, C++ and Go |
|---|---|---|
| mtime window in the diff | 2 s (misses same-size edits) | 1 ms |
| server idle session | waits forever | dropped after 300 s |
| unknown server flag (`--lann`) | ignored, serves loopback | refused |
| skipped symlinked dirs | silent | listed by the push, counted by the server |
| mistyped `config.json` value | falls back to a default | named |

It also keeps the four places where C++ is deliberately more specific than C: a
present-but-unusable PUT number says `out-of-range 'size'` rather than
`missing field`, `unknown op:` prints Python's repr for a non-string op, the
oversized-frame log line carries the length the peer claimed, and a file that
vanished between the scan and the send is reported and skipped rather than
killing the push.

One guard is **stricter here than in any other port**: `file_receiver` binds
`127.0.0.1`, where `file_receiver.py` binds `""` (every interface). That
program has no token check at all — it is the one-file experiment — so leaving
it reachable from the LAN is an unauthenticated file write for anyone on the
network. The path guard still confines it to `received/`, but that is the
second line of defence, not the first.

## 6. Tests

```
make test              # 138 tests, in-process, ~2 s
make race              # the same suite under the race detector
make interop-python    # Python suite  -> Go server
make interop-roundtrip # Go pusher     -> Python server, trees compared
make interop-c         # C suite       -> Go server + Go pusher
make check             # vet + all of the above
```

`make test` runs the real `ServeSession` on an **ephemeral port** against a
temp directory, so it needs no server, no `config.json` and no free 8765. It
covers:

- HELLO gating, including a token that is canonically equal but byte-different
- 26 malformed-frame cases, plus invalid UTF-8, the pre-auth frame cap, 200 KB
  of `[`, and 200 random/truncated/lying frames with a fixed seed
- an idle peer being dropped
- a PUT round trip, mtime included, and an aborted PUT's atomicity (old file
  intact, no `.tmp` left behind)
- PUT and DELETE escapes: `..`, absolute paths, an out-of-tree symlink, `.`,
  and out-and-back-in through a link
- symlinked files, symlinked directories, broken links, an unreadable directory
  making the walk partial, and a missing root doing the same
- the diff's 1 ms window, in both directions
- `config.json` typing, every mistyped value by name
- a file truncated *and* a file grown mid-send: the body still matches the
  declared size and the next frame still lines up — and a file that vanished
  before its header went out, which puts no bytes on the wire at all
- the argv guards: `--dryrun` refused, and a flag *after* the positional
  arguments still recognised
- that a socket timeout is per-operation, not per-session (§7)

The `interop` targets start a server on 8765, run the sibling project's runners
against it, and stop it. They refuse to start if 8765 is already taken, so they
can never test the wrong server by accident. The Python run serves
`../lan_folder_sync/sandbox/received` **explicitly**, because that project's
`config.json` points `shared_dir` at a real folder — the config is borrowed for
its token only.

Checked by hand as well, since these don't fit a runner: a 500 KB tree pushed,
re-pushed as a no-op (which is the mtime round trip working), mirrored with
`--delete`, and diffed byte for byte; `--lan` binding the LAN IP and nothing
else; and the four CLI refusals (`--dryrun`, a `~` path, a missing directory,
`--delete` from an empty source) each exiting 1 with its own page.

## 7. Go-specific notes

These are the places where the port needed more than transliteration.

- **`net.Conn` deadlines are absolute; `SO_RCVTIMEO` is per-call.** This is the
  one difference that silently produces a *wrong program* rather than a
  compile error. C, C++, Python and Swift all set a socket option meaning "no
  single read may take longer than this". Go has no such option: `SetDeadline`
  is a point in time. Setting it once per session would give the whole session
  300 seconds instead of giving each read 300 seconds, so a large, perfectly
  healthy transfer would be cut off mid-file. `netx.Conn` therefore holds a
  duration and refreshes the deadline before every operation, and a test pins
  the behaviour by reading six times across three timeouts' worth of elapsed
  time.
- **`defer` replaces the repeated cleanup, which is where the bugs were.** C's
  `recv_file_body` has six early returns, each spelling out
  `close(dst); unlink(tmp); free(tmp); free(safe);` by hand — adding a seventh
  failure path means remembering all four again. Here one `defer` removes the
  temp unless `committed` was set, so "every failure path removes the temp" is
  a property of the function rather than a rule the author has to keep obeying.
  It covers a panic too, which the C++ destructor also does but the C code
  cannot.
- **`encoding/json` needed three things added, not replaced.** Its scanner
  already enforces the JSON number grammar, so `+1`, `01`, `.5` and `nan` are
  refused as Python refuses them — which the C parser got wrong by handing the
  token to `strtod`. What it does *not* do is refuse invalid UTF-8 (it
  substitutes U+FFFD, where Python's `.decode("utf-8")` raises), cap nesting
  anywhere near this protocol's needs (its limit is 10000, and the header is
  parsed before the HELLO), or preserve the difference between a number and a
  bool once decoded into `any` (hence `UseNumber`). `internal/jsonval` is those
  three checks and nothing else.
- **A type switch makes the type confusion impossible to forget.** With
  `UseNumber`, `{"size": true}` misses `Number()` because a `bool` is a
  different dynamic type from a `json.Number`. Python needed an explicit
  `isinstance(bool)` exclusion here — its `bool` is a subclass of `int`, so
  `true` compared equal to 1 — and C's tagged struct needed
  `v->type != JSON_NUMBER` written out at every call site.
- **Go defines what C leaves undefined, and the check is still needed.**
  `static_cast<long long>` of an infinite double is undefined behaviour in C
  and C++; `int64(math.Inf(1))` in Go is merely *implementation-defined
  nonsense*. No panic, no error — just a garbage length used to size a file. So
  every number off the wire is still range-checked before conversion. The
  difference is that the bug would be a wrong file rather than a crash, which
  is worse, not better.
- **A catch-all is not what makes the server safe.** `ServeSession` returns its
  failures as values and handles them, so no frame can end a session by
  accident; `sync_server` adds a `recover()` per session purely as a last line
  of defence against a bug in our own code, the way Python needs a bare
  `except Exception` at its accept loop. Nothing in the protocol path relies on
  it.
- **No SIGPIPE handler and no `setvbuf`.** A write to a vanished peer comes
  back as an `EPIPE` error value instead of killing the process, so the
  `signal(SIGPIPE, SIG_IGN)` that C, C++ and Swift all need has no equivalent
  here. `os.Stdout` is unbuffered, so the line-buffering both Python
  (`sys.stdout.reconfigure`) and C (`setvbuf(..., _IOLBF, 0)`) have to arrange
  — so that a redirected log isn't empty until the process exits — is already
  the default.
- **`filepath.WalkDir` is the right tool here, unlike C++'s
  `recursive_directory_iterator`.** The walk needs a third behaviour that
  neither C++ mode offers: keep going past an unreadable directory *and*
  remember that the picture has a hole in it. WalkDir's error callback does
  exactly that — record the hole, return `nil`, carry on — so this port needs
  no hand-rolled `opendir`/`readdir`. That silence is the hole
  `os.walk(onerror=None)` had, which let `--delete` remove files the walk could
  not see, and a `chmod 000` on a subdirectory was enough to trigger it. One
  case is tightened beyond the other ports' phrasing: an error on the *root*
  itself is never treated as a benign vanished-entry race, because a missing
  `shared_dir` reported as "complete, zero files" is the empty-source wipe
  wearing another hat.
- **`path/filepath` is wrong for every path that came off the wire.**
  `filepath.Join` calls `Clean`, which folds `..` lexically — it would tidy
  `../../etc/passwd` into a legitimate-looking relative path *before* the guard
  got to refuse it — and `Clean` plus `EvalSymlinks` is the same trap C++
  documents for `weakly_canonical`: `EvalSymlinks` refuses a path that doesn't
  exist yet (the normal case for a PUT) and answers "where does this end up"
  rather than "did it leave the tree on the way". So wire paths stay `string`,
  `internal/cli` carries byte-level `PathJoin`/`PathDirname` with `os.path`
  semantics (absolute right side wins, which *preserves* the hostile input for
  the guard), and the confinement walk resolves one component at a time.
  `filepath` is used freely in the tests, where the inputs are ours.
- **`string ==` is byte equality**, which is what the filesystem and every
  other implementation of this protocol mean. Swift's `String ==` is Unicode
  canonical equivalence, and its port had to compare `.utf8` views explicitly;
  here the default is right, and two tests pin it — one on the token, one on
  the path guard — so a later "normalise the input" change can't quietly
  introduce a normalising comparison.
- **Manifests are a `map` with an imposed order.** Go randomises map
  iteration, so `Manifest.Paths()` sorts, and every caller that puts paths on
  the wire or on screen goes through it. Without that the same folder produces
  a different byte string on every run and two implementations stop being
  comparable line by line. (C++ gets this free from `std::map`; Python's dict
  order is insertion order, and nothing in the protocol cares.)
- **The output JSON is written by hand, as in C++.** `size` is an integer and
  `mtime` is `%.6f`; `json.Marshal` would render the float in Go's shortest
  round-trip form (`1.7606e+09`) and escape `<`, `>` and `&` as `<`. Both
  are valid JSON and neither matches what the other four ports emit.
- **`encoding/json` is likewise not used to read `config.json`.**
  Unmarshalling into a struct would report
  `json: cannot unmarshal string into Go struct field ... of type int` for
  `"port": "9999"` — a message about Go's types, naming a Go field, for a
  mistake the user made in their editor — and it zeroes mistyped values and
  ignores unknown ones unless told otherwise, which is the silent-default
  behaviour the named messages exist to replace.
- **The `flag` package is not used for argv**, for three reasons that are all
  about the same bug: it stops parsing flags at the first positional argument,
  so `sync_push 192.168.1.42 /tmp/x --dry-run` would ignore the flag and push
  for real; it treats `-delete` and `--delete` as the same thing, where the
  other ports accept only the second; and it exits with its own usage text on
  an unknown flag, leaving no room for the advice that makes the message
  useful. `cli.ParseArgs` is twenty lines and a test says so.
- **The server still handles one session at a time.** A goroutine per
  connection is nearly free here and is deliberately not taken: the 300 s idle
  drop exists *because* one wedged peer would otherwise lock out everyone
  else, and that reasoning — along with the log being a readable narration of
  one session — is what the other four ports are being compared against. The
  concurrency Go does buy is in the test harness, where the server runs in a
  goroutine on an ephemeral port and the suite drives it in-process.
- **The race detector replaces ASan.** It is a different class of bug from the
  C and C++ builds' sanitizer target — there is no memory to corrupt — but the
  suites drive the server from goroutines and the harness captures its log from
  one, so `make race` is checking something real.

## 8. Layout

```
internal/cli/         die, argv, byte-level path helpers, mtime conversion
internal/jsonval/     the three checks encoding/json needs, plus the escaper and repr
internal/netx/        Conn with per-operation timeouts, dial-with-diagnosis, listener, en0
internal/framing/     frames, PUT bodies, the temp-then-rename receive, handshake
internal/paths/       the two-stage path-confinement guard
internal/manifest/    the walk, the wire form, the diff
internal/config/      config.json, with every mistyped value named
internal/server/      ServeSession — in a package, so tests can run it in-process
internal/testsupport/ the harness: a real server on an ephemeral port, socket pairs
cmd/sync_server/      argv, bind, accept loop
cmd/sync_push/        argv, preflight checks, diff, transfer, diagnostics
cmd/file_sender/      the one-file experiments, as in Python
cmd/file_receiver/
```

Tests live beside the code they cover (`internal/*/..._test.go`), as Go expects,
rather than in a separate `tests/` tree — `go test ./...` finds them all.
