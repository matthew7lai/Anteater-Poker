# Makefile — Anteater Poker  (Team 23, EECS 22L)
# Top-level Makefile.  Delegates to src/Makefile.
#
# Usage:
#   make            — build server and client
#   make test       — run unit tests
#   make test-gui   — GUI smoke test
#   make clean      — remove build artifacts
#   make tar        — package source archive

PROJECT  = Anteater_Poker
VERSION  = Alpha
SRC_TAR  = Poker_Alpha_src.tar.gz
BIN_TAR  = Poker_Alpha.tar.gz

.PHONY: all test test-gui clean tar

all:
	$(MAKE) -C src all

test:
	$(MAKE) -C src test

test-gui:
	$(MAKE) -C src test-gui

clean:
	$(MAKE) -C src clean

# Create the source tarball (Poker_Alpha_src.tar.gz)
tar: clean
	mkdir -p bin doc
	tar --exclude='.git' \
	    --exclude='*.tar.gz' \
	    -czf ../$(SRC_TAR) \
	    -C .. poker/
	@echo "Created ../$(SRC_TAR)"
