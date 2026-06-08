# Makefile — Anteater Poker  (Team 23, EECS 22L)
# Top-level Makefile.  Delegates to src/Makefile.
#
# Usage:
#   make            — build server and client
#   make test       — run unit tests
#   make test-gui   — GUI smoke test
#   make test-comm  — communication test
#   make clean      — remove build artifacts
#   make tar        — package source archive
#   make bin-tar    — package binary archive

PROJECT  = Anteater_Poker
VERSION  = V1.0
SRC_TAR  = Poker_V1.0_src.tar.gz
BIN_TAR  = Poker_V1.0.tar.gz

.PHONY: all test test-gui test-comm clean tar bin-tar

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

# Create source tarball (Poker_V1.0_src.tar.gz)
tar: clean
	mkdir -p bin doc
	tar --exclude='.git' \
	    --exclude='*.tar.gz' \
	    --exclude='*.o' \
	    -czf ../$(SRC_TAR) \
	    -C .. Anteater-Poker/
	@echo "Created ../$(SRC_TAR)"

# Create binary tarball (Poker_V1.0.tar.gz) — builds first!
bin-tar: all
	mkdir -p bin doc
	tar -czf ../$(BIN_TAR) \
	    bin/ doc/ README COPYRIGHT INSTALL
	@echo "Created ../$(BIN_TAR)"
