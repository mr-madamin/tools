SWIFT   ?= swift
CONFIG  ?= release

PROGS   := sync_server sync_push file_sender file_receiver
BIN     := bin
LOGS    := $(BIN)/test-logs

# The sibling ports. Their runners dial 127.0.0.1:8765, so a server started from
# here must be the only thing on that port while they run.
PY_DIR  := ../lan_folder_sync
C_DIR   := ../lan_folder_sync_c
PORT    := 8765

PY_TESTS := hello_test bad_frame_test atomic_test dry_run_test delete_test nasty_test
C_TESTS  := hello_test bad_frame_test dry_run_test delete_test atomic_test \
            partial_test config_test

# SwiftPM tracks its own dependencies, so `swift build` is always safe to run;
# the copy into bin/ is what lets the C runners find ./bin/sync_push.
all:
	$(SWIFT) build -c $(CONFIG)
	@mkdir -p $(BIN)
	@set -e; for p in $(PROGS); do cp .build/$(CONFIG)/$$p $(BIN)/$$p; done

test:
	$(SWIFT) test

check: test interop

# Refuse to start if something already holds the port: the runners would
# silently test THAT server instead of this one.
port-free:
	@if lsof -nP -iTCP:$(PORT) -sTCP:LISTEN >/dev/null 2>&1; then \
	  echo "port $(PORT) is in use -- stop the server holding it first"; exit 1; fi

# The Python suite against the Swift server. The server takes its token from the
# Python config, but serves the Python sandbox EXPLICITLY: that config.json may
# point shared_dir at a real folder, and a test must never plant files there.
interop-python: all port-free
	@set -e; \
	mkdir -p $(PY_DIR)/sandbox/source $(PY_DIR)/sandbox/received $(LOGS); \
	LAN_FOLDER_SYNC_CONFIG=$(PY_DIR)/config.json \
	  ./$(BIN)/sync_server $(abspath $(PY_DIR))/sandbox/received $(PORT) > $(LOGS)/py-server.log 2>&1 & \
	SRV=$$!; trap "kill $$SRV 2>/dev/null || true" EXIT INT TERM; sleep 1; \
	fail=0; \
	for t in $(PY_TESTS); do \
	  printf '  python %-16s ' $$t; \
	  if (cd $(PY_DIR) && python3 -m tests.$$t) > $(LOGS)/py-$$t.log 2>&1; then echo PASS; \
	  else echo FAIL; fail=1; sed 's/^/      /' $(LOGS)/py-$$t.log; fi; \
	done; exit $$fail

# The C suite against the Swift server AND the Swift pusher: the C runners shell
# out to ./bin/sync_push relative to the cwd, so running them from here drives
# this build. nasty/truncate/symlink test C library code, not a peer, so they
# are left out.
interop-c: all port-free
	@$(MAKE) --no-print-directory -C $(C_DIR) tests >/dev/null
	@set -e; \
	mkdir -p sandbox/source sandbox/received $(LOGS); \
	./$(BIN)/sync_server > $(LOGS)/c-server.log 2>&1 & \
	SRV=$$!; trap "kill $$SRV 2>/dev/null || true" EXIT INT TERM; sleep 1; \
	fail=0; \
	for t in $(C_TESTS); do \
	  printf '  c      %-16s ' $$t; \
	  if $(C_DIR)/bin/$$t > $(LOGS)/c-$$t.log 2>&1; then echo PASS; \
	  else echo FAIL; fail=1; sed 's/^/      /' $(LOGS)/c-$$t.log; fi; \
	done; exit $$fail

interop: interop-python interop-c

clean:
	$(SWIFT) package clean
	rm -rf $(BIN)

.PHONY: all test check port-free interop interop-python interop-c interop clean
