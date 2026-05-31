# Makefile — Anteater Poker  (Team 23, EECS 22L)
# Top-level Makefile.  Delegates to src/Makefile.
#
# Usage:
#   make            — build server and client
#   make test       — run unit tests
#   make test-gui   — GUI smoke test
#   make test-comm  — communication test
#   make clean      — remove build artifacts
#   make tar        — package source and binary archives

PROJECT  = Anteater_Poker
VERSION  = Beta
SRC_TAR  = Poker_Beta_src.tar.gz
BIN_TAR  = Poker_Beta.tar.gz

.PHONY: all test test-gui test-comm clean tar

all:
	$(MAKE) -C src all

test:
	$(MAKE) -C src test

test-gui:
	$(MAKE) -C src test-gui

test-comm:
	$(MAKE) -C src test-comm

clean:
	$(MAKE) -C src clean

# Create source tarball (Poker_Beta_src.tar.gz)
tar: clean
	mkdir -p bin doc
	tar --exclude='.git' \
	    --exclude='*.tar.gz' \
	    -czf ../$(SRC_TAR) \
	    -C .. poker/
	@echo "Created ../$(SRC_TAR)"

# Create binary tarball (Poker_Beta.tar.gz)
bin-tar:
	mkdir -p bin doc
	tar -czf ../$(BIN_TAR) \
	    bin/ doc/ README COPYRIGHT INSTALL
	@echo "Created ../$(BIN_TAR)"
