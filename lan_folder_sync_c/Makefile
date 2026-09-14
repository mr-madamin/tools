CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -D_DARWIN_C_SOURCE
LDFLAGS ?=

BIN     := bin
LIB_SRC := util.c json.c config.c framing.c
LIB_OBJ := $(LIB_SRC:%.c=$(BIN)/%.o)

PROGS   := sync_server sync_push file_sender file_receiver
TESTS   := hello_test bad_frame_test dry_run_test delete_test nasty_test \
           truncate_test atomic_test

all: $(PROGS:%=$(BIN)/%)

tests: $(TESTS:%=$(BIN)/%)

$(BIN):
	@mkdir -p $(BIN)

$(BIN)/%.o: %.c | $(BIN)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN)/%.o: tests/%.c | $(BIN)
	$(CC) $(CFLAGS) -I. -c $< -o $@

$(BIN)/sync_server: $(BIN)/sync_server.o $(LIB_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN)/sync_push: $(BIN)/sync_push.o $(LIB_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN)/file_sender: $(BIN)/file_sender.o $(LIB_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN)/file_receiver: $(BIN)/file_receiver.o $(LIB_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN)/%_test: $(BIN)/%_test.o $(BIN)/report.o $(LIB_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lpthread

# Start a loopback server, run every runner against it, tear it down whatever
# happens. The sockets tests need a peer; nasty_test and truncate_test don't,
# but running them here keeps "make check" the one command that proves the tree.
LOGS := $(BIN)/test-logs

check: all tests
	@set -e; \
	mkdir -p sandbox/source sandbox/received $(LOGS); \
	./$(BIN)/sync_server > $(LOGS)/server.log 2>&1 & \
	SRV=$$!; \
	trap "kill $$SRV 2>/dev/null || true" EXIT INT TERM; \
	sleep 1; \
	fail=0; \
	for t in $(TESTS); do \
	  printf '  %-16s ' $$t; \
	  if ./$(BIN)/$$t > $(LOGS)/$$t.log 2>&1; then \
	    echo "PASS"; \
	  else \
	    echo "FAIL"; fail=1; sed 's/^/      /' $(LOGS)/$$t.log; \
	  fi; \
	done; \
	kill $$SRV 2>/dev/null || true; \
	if [ $$fail -eq 0 ]; then echo "  all $(words $(TESTS)) runners passed"; \
	else echo "  FAILURES - full logs in $(LOGS)/"; fi; \
	exit $$fail

# Same tree under AddressSanitizer + UBSan, built into bin-asan/ so it never
# mixes objects with the normal build. Note: LeakSanitizer is unavailable on
# macOS, so this catches memory *errors* and UB, not leaks.
ASAN_FLAGS := -std=c11 -O1 -g -fsanitize=address,undefined \
              -fno-omit-frame-pointer -fno-sanitize-recover=undefined \
              -Wall -Wextra -Wno-unused-parameter -D_DARWIN_C_SOURCE

asan:
	@$(MAKE) --no-print-directory BIN=bin-asan \
	  CFLAGS="$(ASAN_FLAGS)" LDFLAGS="-fsanitize=address,undefined" check

clean:
	rm -rf $(BIN) bin-asan

.PHONY: all tests check asan clean
