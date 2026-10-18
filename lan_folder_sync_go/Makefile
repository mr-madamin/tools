# The Go toolchain comes from asdf (see .tool-versions), so a bare `go` may not
# be on PATH in a fresh shell. Prefer the shim, fall back to whatever is there.
GO    ?= $(shell command -v go 2>/dev/null || echo $(HOME)/.asdf/shims/go)
GOFMT ?= $(shell command -v gofmt 2>/dev/null || echo $(HOME)/.asdf/shims/gofmt)

BIN   := bin
PROGS := sync_server sync_push file_sender file_receiver

# One command builds all four. `go build` does its own change detection and
# caching, so there are no object files or header dependencies to track here --
# the whole of the C port's -MMD/-MP machinery is the toolchain's problem.
all:
	@mkdir -p $(BIN)
	$(GO) build -o $(BIN)/ ./cmd/...

test:
	$(GO) test ./...

# Verbose, with the test names, when you want to read what is covered.
test-v:
	$(GO) test -v ./...

# The race detector is this port's equivalent of the C and C++ builds' ASan
# target. It is a different class of bug -- the server runs one session at a
# time, but the suites drive it from goroutines, and the harness captures its
# log from one.
race:
	$(GO) test -race -count=2 ./...

vet:
	$(GO) vet ./...
	@unformatted=$$($(GOFMT) -l ./cmd ./internal); \
	if [ -n "$$unformatted" ]; then \
	  echo "gofmt would change files:"; echo "$$unformatted"; exit 1; \
	fi

# ---- interop ---------------------------------------------------------------
#
# The sibling ports. Their runners dial 127.0.0.1:8765, so a server started
# from here must be the only thing on that port while they run.
PY_DIR  := ../lan_folder_sync
C_DIR   := ../lan_folder_sync_c
CPP_DIR := ../lan_folder_sync_cpp
PORT    := 8765
LOGS    := $(BIN)/test-logs

PY_TESTS := hello_test bad_frame_test atomic_test dry_run_test delete_test nasty_test

# Refuse to start if something already holds the port: the runners would
# silently test THAT server instead of this one.
port-free:
	@if lsof -nP -iTCP:$(PORT) -sTCP:LISTEN >/dev/null 2>&1; then \
	  echo "port $(PORT) is in use -- stop the server holding it first"; exit 1; fi

# The Python suite against the Go server. The server takes its token from the
# Python config, but serves the Python sandbox EXPLICITLY: that config.json may
# point shared_dir at a real folder, and a test must never plant files there.
interop-python: all port-free
	@set -e; \
	mkdir -p $(PY_DIR)/sandbox/source $(PY_DIR)/sandbox/received $(LOGS); \
	LAN_FOLDER_SYNC_CONFIG=$(abspath $(PY_DIR))/config.json \
	  ./$(BIN)/sync_server $(abspath $(PY_DIR))/sandbox/received $(PORT) \
	  > $(LOGS)/py-server.log 2>&1 & \
	SRV=$$!; trap "kill $$SRV 2>/dev/null || true" EXIT INT TERM; sleep 1; \
	fail=0; \
	for t in $(PY_TESTS); do \
	  printf '  python %-16s ' $$t; \
	  if (cd $(PY_DIR) && python3 -m tests.$$t) > $(LOGS)/py-$$t.log 2>&1; then echo PASS; \
	  else echo FAIL; fail=1; sed 's/^/      /' $(LOGS)/py-$$t.log; fi; \
	done; exit $$fail

# This pusher into the Python server: both directions produce the same tree,
# which is the actual interop claim.
#
# The push printing "Done" only means the bytes reached the kernel, not that the
# peer has written them, so the recipe waits for the server to log the BYE
# before killing it -- otherwise the diff races a file still in flight.
interop-roundtrip: all port-free
	@set -e; \
	mkdir -p $(LOGS); \
	rm -rf sandbox/interop; mkdir -p sandbox/interop/source sandbox/interop/received; \
	printf 'one\n' > sandbox/interop/source/a.txt; \
	mkdir -p sandbox/interop/source/sub; \
	printf 'two\n' > sandbox/interop/source/sub/b.txt; \
	dd if=/dev/urandom of=sandbox/interop/source/blob.dat bs=1024 count=300 2>/dev/null; \
	( cd $(PY_DIR) && exec python3 sync_server.py $(abspath sandbox/interop/received) $(PORT) ) \
	  > $(LOGS)/rt-py-server.log 2>&1 & \
	SRV=$$!; trap "kill $$SRV 2>/dev/null || true" EXIT INT TERM; sleep 1; \
	LAN_FOLDER_SYNC_CONFIG=$(abspath $(PY_DIR))/config.json \
	  ./$(BIN)/sync_push 127.0.0.1 $(abspath sandbox/interop/source) --delete \
	  > $(LOGS)/rt-push.log 2>&1 || { cat $(LOGS)/rt-push.log; exit 1; }; \
	for i in $$(seq 1 100); do \
	  grep -q "Peer said BYE" $(LOGS)/rt-py-server.log && break; sleep 0.1; \
	done; \
	kill $$SRV 2>/dev/null || true; sleep 0.3; \
	if diff -r sandbox/interop/source sandbox/interop/received > $(LOGS)/rt-diff.log 2>&1; then \
	  echo "  go push -> python server: identical trees"; \
	else echo "  go push -> python server: TREES DIFFER"; cat $(LOGS)/rt-diff.log; exit 1; fi

# The C suite against the Go server AND the Go pusher: the C runners shell out
# to ./bin/sync_push relative to the cwd, so running them from here drives this
# build. nasty/truncate/symlink/config test C library code, not a peer, so they
# are left out.
#
# LAN_FOLDER_SYNC_CONFIG is exported rather than prefixed onto the server: the C
# runners spawn ./bin/sync_push themselves, and it has to find the same token
# the C side is using. The shared_dir is still passed explicitly, so a config
# pointing at a real folder can never be the thing a test plants files in.
C_TESTS := hello_test bad_frame_test dry_run_test delete_test atomic_test partial_test

interop-c: all port-free
	@$(MAKE) --no-print-directory -C $(C_DIR) tests >/dev/null
	@set -e; \
	mkdir -p sandbox/source sandbox/received $(LOGS); \
	export LAN_FOLDER_SYNC_CONFIG=$(abspath $(C_DIR))/config.json; \
	./$(BIN)/sync_server sandbox/received $(PORT) > $(LOGS)/c-server.log 2>&1 & \
	SRV=$$!; trap "kill $$SRV 2>/dev/null || true" EXIT INT TERM; sleep 1; \
	fail=0; \
	for t in $(C_TESTS); do \
	  printf '  c      %-16s ' $$t; \
	  if $(C_DIR)/bin/$$t > $(LOGS)/c-$$t.log 2>&1; then echo PASS; \
	  else echo FAIL; fail=1; sed 's/^/      /' $(LOGS)/c-$$t.log; fi; \
	done; exit $$fail

interop: interop-python interop-roundtrip interop-c

check: vet test interop

clean:
	rm -rf $(BIN)
	$(GO) clean -testcache

.PHONY: all test test-v race vet port-free interop interop-python interop-roundtrip interop-c check clean
