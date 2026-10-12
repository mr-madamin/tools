# LAN Folder Sync — C++

A C++ port of [`lan_folder_sync/`](../lan_folder_sync), written to be compared
against it and against the [C port](../lan_folder_sync_c) and the
[Swift port](../lan_folder_sync_swift). Same one-way folder mirror over raw TCP,
same wire protocol, same CLI, same safety guards.

- **Receiver** runs the server and holds the copy.
- **Source** runs the push and holds the truth.

> As in the other three, the **server is the passive side**: it receives and
> obeys. The **push client is the brain**: it reads its folder, diffs against
> the peer, and decides every PUT and DELETE.

**All four implementations interoperate**, and this is checked, not assumed: the
Python suite and the C suite both pass against this server, the C suite drives
this pusher, and this pusher syncs into the C and Python servers (§6).

## 1. Build

```
make          # bin/sync_server, bin/sync_push, bin/file_sender, bin/file_receiver
make test     # the in-process suite — needs no server, no free port
make asan     # the same suite under AddressSanitizer + UBSan
make clean
```

No dependencies: C++23 and libc. It needs a recent toolchain (`std::format`,
`std::expected`, `std::jthread` — Apple clang 16 or later). Everything runs from
this directory (`lan_folder_sync_cpp/`).

## 2. One-time setup (do this on *both* machines)

```
cp config.example.json config.json
```

It's the same file, with the same rules, as every other port:

- **`token`**: the **same random string on both machines**.
- **`shared_dir`**: an **absolute path**. `~` is not expanded.
- **`peer`**: `{ "host": "<receiver's LAN IP>", "port": 8765 }`, needed on the source.

`config.json` is gitignored because it carries the token.

The config is looked for in the same order as the C port: `$LAN_FOLDER_SYNC_CONFIG`,
then `./config.json`, then next to the executable, then its parent. A value of
the wrong type is reported by name — `"port": "9999"` says
`peer.port must be a number, not in quotes` rather than silently falling back to
8765.

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
- **Error messages:** the server's `ERROR` texts match the other three word for
  word (`bad token`, `unknown op: 'MANFEST'`,
  `PUT header has an out-of-range 'size'`, and so on).
- **Every guard:**
  - path confinement for PUT *and* DELETE, including symlink escapes and the
    shared root itself
  - atomic temp-then-rename receive
  - `size`/`mtime` bounds checked before any cast
  - the declared-size contract when a file changes mid-send
  - no `--delete` from an empty or partial source listing
  - connect / protocol / transfer timeouts, and a 300 s idle-session drop
  - a frame refused on its length prefix alone, before the HELLO
  - JSON nesting capped at 64

## 5. Where it follows the C port rather than `framing.py`

The C README lists these as fixes "worth porting back" to Python. Here they are
simply in place:

| | Python | C, Swift and C++ |
|---|---|---|
| mtime window in the diff | 2 s (misses same-size edits) | 1 ms |
| server idle session | waits forever | dropped after 300 s |
| unknown server flag (`--lann`) | ignored, serves loopback | refused |
| skipped symlinked dirs | silent | listed by the push, counted by the server |
| mistyped `config.json` value | falls back to a default | named |

Four places where this port is deliberately more specific than the C one:

- A PUT number that is present but unusable (`"size": true`, `"size": "5"`) says
  `out-of-range 'size'`. C's `json_get_num` returns the same "not found" for a
  wrong type as for an absent key, so it reports `missing field: 'size'` for a
  field that is right there.
- `unknown op:` prints Python's `repr` for a non-string op (`5`, `True`) where C
  prints `<non-string>`.
- The oversized-frame log line carries the length the peer claimed, which is the
  one number worth having when you're looking at it.
- A file that disappears between the walk and the send is reported and skipped.
  Nothing has reached the socket at that point, so the stream is still in sync
  and the remaining files still go. C treats the failed `open` as fatal and
  abandons the rest of the push; `sync_push.py` is worse still -- it catches
  only `TimeoutError` and the two connection errors, so a `FileNotFoundError`
  escapes as an uncaught traceback. Both are worth porting back.

## 6. Tests

```
make test             # 38 tests, in-process, ~3 s
make interop-python   # Python suite  -> C++ server
make interop-c        # C suite       -> C++ server + C++ pusher
make check            # all of the above
make asan             # the in-process suite under ASan + UBSan
```

`make test` runs the real `serve_session` on an **ephemeral port** against a temp
directory, so it needs no server, no `config.json` and no free 8765. It covers:

- HELLO gating, including a token that is canonically equal but byte-different
- 12 malformed-frame cases, plus invalid UTF-8, the pre-auth frame cap, 200 KB of
  `[`, and 200 random/truncated/lying frames with a fixed seed
- an idle peer being dropped
- a PUT round trip, mtime included, and an aborted PUT's atomicity (old file
  intact, no `.tmp` left behind)
- PUT and DELETE escapes: `..`, absolute paths, an out-of-tree symlink, `.`
- symlinked files, symlinked directories, broken links, and an unreadable
  directory making the walk partial
- the diff's 1 ms window, in both directions
- `config.json` typing
- a file truncated *and* a file grown mid-send: the body still matches the
  declared size, and the next frame still lines up -- and a file that vanished
  before its header went out, which puts no bytes on the wire at all

The `interop` targets start `./bin/sync_server` on 8765, run the sibling
project's runners against it, and stop it. They refuse to start if 8765 is
already taken, so they can never test the wrong server by accident. The Python
run serves `../lan_folder_sync/sandbox/received` **explicitly**, because that
project's `config.json` points `shared_dir` at a real folder.

Checked by hand as well, since these don't fit a runner: pushing from here into
the C server and into the Python server (both produce byte-identical trees),
`--lan` binding the LAN IP and nothing else, and a peer that accepts and stays
silent (15.0 s, then the right page).

## 7. C++-specific notes

These are the places where the port needed more than transliteration.

- **RAII replaces the repeated cleanup, which is where the bugs were.** C's
  `recv_file_body` has six early returns, each spelling out
  `close(dst); unlink(tmp); free(tmp); free(safe);` by hand — adding a seventh
  failure path means remembering all four again. Here `Fd` closes itself and
  `TempFile`'s destructor unlinks unless `commit()` renamed it, so "every
  failure path removes the temp" is a property of the type rather than a rule
  the author has to keep obeying.
- **`std::variant` makes a type confusion impossible to forget.** `{"size": true}`
  misses `as_number()` because a bool is a different alternative, not a number
  with a flag. Python needed an explicit `isinstance(bool)` exclusion here — its
  `bool` is a subclass of `int`, so `true` compared equal to 1 — and C's tagged
  struct needed `v->type != JSON_NUMBER` written out at every call site.
- **Exceptions for failures, values for distinctions.** I/O errors throw
  (`SocketError`, `SocketTimeout`), because there is nothing useful to do with
  them locally. But anything the caller has to *explain* to the user stays a
  value: `ConnectFailure::Kind`, `SendResult::Changed`, `HeaderError` through
  `std::expected`, `PutError::Kind`. The pusher's four connect diagnoses are a
  `switch` over the kind, so a new kind fails to compile rather than silently
  printing the wrong advice.
- **A catch-all is not what makes the server safe.** It has one, at the session
  boundary, so no frame can take the process down. But `static_cast<long long>`
  of an out-of-range double is *undefined behaviour*, not an exception — no
  `catch` would ever see it — so every number off the wire is range-checked
  before it is converted. UBSan confirms it: `make asan` is clean, and it was
  `size: 1e999` that this catches.
- **`std::filesystem` is wrong for three specific jobs here**, and is used in
  the tests instead:
  - `recursive_directory_iterator` either throws mid-scan or, with
    `skip_permission_denied`, walks past an unreadable directory in silence.
    That silence is the exact hole `os.walk(onerror=None)` had, which let
    `--delete` remove files the walk couldn't see. The walk needs a third
    behaviour — keep going *and* report the hole — so it's `opendir`/`readdir`.
  - `weakly_canonical()` folds `..` lexically before resolving, so
    `link/../x` answers a question about a directory the kernel would never
    visit; `canonical()` refuses a path that doesn't exist yet, which is the
    normal case for a PUT. The guard resolves component by component instead.
  - `path::operator/` discards the left side when the right is absolute — which
    is precisely the traversal being guarded against. Paths that came off the
    wire stay `std::string`.
- **`std::string ==` is byte equality**, which is what the filesystem and every
  other implementation of this protocol mean. Swift's `String ==` is Unicode
  canonical equivalence, and its port had to compare `.utf8` views explicitly;
  here the default is right, and a test pins it so a later "cleanup" can't
  introduce a normalising comparison.
- **`std::format` keeps wire data out of format strings.** `die()` takes one
  already-formatted string rather than a format plus varargs, so a peer-supplied
  path can never *be* the format string — the discipline C maintains by hand
  (`die("%s", msg)`) is structural here, and argument/format mismatches are
  compile errors.
- **The JSON number grammar is checked before `strtod`**, so `+1`, `01`, `.5`
  and `nan` are refused as Python refuses them, where C's parser handed the
  token to `strtod` and accepted whatever it consumed. `1e999` is still parsed —
  to infinity, deliberately — so the refusal comes from the PUT range check and
  names the field, instead of a parse error that names nothing.
- **Manifests are `std::map`**, so the wire form and the diff come out in a
  stable byte order and two runs (or two implementations) can be compared line
  by line. Python's dict order is insertion order; nothing in the protocol cares.
- **The tests run the server in a `std::jthread`** whose accept loop `poll()`s
  with a 50 ms timeout rather than blocking in `accept()`, so the harness can
  shut a test server down without a peer ever arriving. `socketpair()` covers
  the two send-side contracts with no listener at all.

## 8. Layout

```
src/util.*        die, argv, byte-level path helpers, UTF-8 validation
src/json.*        the parser (depth-capped) and the escaper
src/socket.*      Fd/Socket/Listener, connect-with-timeout, en0 lookup
src/framing.*     frames, PUT bodies, the temp-then-rename receive
src/paths.*       the two-stage path-confinement guard
src/manifest.*    the walk, the wire form, the diff
src/config.*      config.json, with every mistyped value named
src/server.*      serve_session — in the library, so tests can run it in-process
src/sync_server.cpp  argv, bind, accept loop
src/sync_push.cpp    argv, preflight checks, diff, transfer, diagnostics
src/file_sender.cpp  the one-file experiments, as in Python
src/file_receiver.cpp
tests/            the runner (test.*), the harness (support.*), four suites
```
