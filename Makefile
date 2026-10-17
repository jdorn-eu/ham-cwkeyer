# SPDX-License-Identifier: GPL-3.0-or-later
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS   = -lasound -lpthread -lm

cwkeyer: cwkeyer.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

.PHONY: test clean
test:
	@tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	$(CC) $(CFLAGS) -o "$$tmp/test_keyer" tests/test_keyer.c $(LDLIBS) && \
	"$$tmp/test_keyer" && \
	$(CC) $(CFLAGS) -o "$$tmp/test_signals" tests/test_signals.c $(LDLIBS) && \
	"$$tmp/test_signals" && \
	bash tests/test_mic_mix.sh

clean:
	rm -f cwkeyer
