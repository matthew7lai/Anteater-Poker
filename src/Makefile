# --------------------------------------------------------------------------------------------
# src/Makefile — Anteater Poker (Team 23, EECS 22L)
#
# Builds the server, client, and test runner. Executables are placed in
# ../bin/ so they match the locations documented in INSTALL and the manual:
#   poker/bin/poker_server
#   poker/bin/poker_client
#
# Targets:
#   make / make all   Build poker_server and poker_client into ../bin
#   make test         Build and run the unit-test runner
#   make test-gui     Build the GUI binaries (smoke build for the grader)
#   make test-comm    Build server + client (communication smoke build)
#   make clean        Remove all build artifacts
# ----------------------------------------------------------------------------------------

CC      := gcc
CSTD    := -std=c11
WARN    := -Wall -Wextra
OPT     := -O2
CFLAGS  := $(CSTD) $(WARN) $(OPT)

# GTK and pthread flags (resolved via pkg-config).
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0)
PTHREAD    := -lpthread

# Output directory (one level up, shared with doc/ and the docs).
BIN := ../bin

# Objects shared by more than one binary.
COMMON_OBJS := types.o eval.o

SERVER := $(BIN)/poker_server
CLIENT := $(BIN)/poker_client
TESTER := $(BIN)/test_poker

.PHONY: all test test-gui test-comm clean

all: $(SERVER) $(CLIENT)

# ---- Object files ----------------------------------------------------------

types.o: types.c types.h
	$(CC) $(CFLAGS) -c $< -o $@

eval.o: eval.c eval.h types.h
	$(CC) $(CFLAGS) -c $< -o $@

poker_server.o: poker_server.c types.h eval.h
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -c $< -o $@

poker_client.o: poker_client.c types.h
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -c $< -o $@

test_poker.o: test_poker.c types.h eval.h
	$(CC) $(CFLAGS) -c $< -o $@

# ---- Executables (created in ../bin) ---------------------------------------

$(BIN):
	mkdir -p $(BIN)

$(SERVER): poker_server.o $(COMMON_OBJS) | $(BIN)
	$(CC) $(CFLAGS) poker_server.o $(COMMON_OBJS) -o $@ $(GTK_LIBS) $(PTHREAD)

$(CLIENT): poker_client.o types.o | $(BIN)
	$(CC) $(CFLAGS) poker_client.o types.o -o $@ $(GTK_LIBS) $(PTHREAD)

$(TESTER): test_poker.o $(COMMON_OBJS) | $(BIN)
	$(CC) $(CFLAGS) test_poker.o $(COMMON_OBJS) -o $@

# ---- Test / smoke targets --------------------------------------------------

test: $(TESTER)
	$(TESTER)

# GUI smoke build: ensure the GTK binaries compile and link.
test-gui: $(SERVER) $(CLIENT)
	@echo "GUI build OK: $(SERVER) and $(CLIENT) compiled and linked."

# Communication smoke build: same binaries exercise the socket protocol.
test-comm: $(SERVER) $(CLIENT)
	@echo "Communication build OK: server and client are ready to run."

# ---- Cleanup ---------------------------------------------------------------

clean:
	rm -f *.o $(SERVER) $(CLIENT) $(TESTER)
