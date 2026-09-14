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

clean:
	rm -rf $(BIN)

.PHONY: all tests clean
