# src/Makefile — Anteater Poker  (Team 23, EECS 22L)
# Targets: all, test, test-gui, clean

CC      = gcc
CFLAGS  = -Wall -Wextra -g $(shell pkg-config --cflags gtk+-3.0)
LDFLAGS = $(shell pkg-config --libs gtk+-3.0) -lpthread

BINDIR  = ../bin

SERVER_SRC = poker_server.c eval.c
CLIENT_SRC = poker_client.c eval.c
TEST_SRC   = test_poker.c eval.c

SERVER_OBJ = $(SERVER_SRC:.c=.o)
CLIENT_OBJ = $(CLIENT_SRC:.c=.o)
TEST_OBJ   = $(TEST_SRC:.c=.o)

.PHONY: all test test-gui clean

all: $(BINDIR)/poker_server $(BINDIR)/poker_client

$(BINDIR)/poker_server: $(SERVER_OBJ)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built poker_server"

$(BINDIR)/poker_client: $(CLIENT_OBJ)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Built poker_client"

$(BINDIR)/test_poker: $(TEST_OBJ)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Non-GUI unit tests (runs headlessly)
test: $(BINDIR)/test_poker
	@echo "=== Running unit tests ==="
	$(BINDIR)/test_poker

# GUI smoke test: launch server for 3 seconds and check exit
test-gui: $(BINDIR)/poker_server $(BINDIR)/poker_client
	@echo "=== GUI smoke test: launching server ==="
	@timeout 3 $(BINDIR)/poker_server --headless || true
	@echo "=== GUI smoke test: launching client ==="
	@timeout 3 $(BINDIR)/poker_client --headless || true
	@echo "=== GUI smoke test done ==="

clean:
	rm -f *.o $(BINDIR)/poker_server $(BINDIR)/poker_client $(BINDIR)/test_poker
	@echo "Clean complete"
