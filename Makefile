# SPDX-License-Identifier: GPL-3.0-or-later
CC       ?= gcc
CFLAGS   ?= -O2
# Set WERROR= to build despite warnings (e.g. on an unknown toolchain).
WERROR   ?= -Werror

# A deliberately aggressive warning set; the tree is clean under all of it.
WARNINGS  = -Wall -Wextra -Wpedantic $(WERROR) \
	    -Wshadow -Wformat=2 -Wundef -Wcast-qual -Wwrite-strings \
	    -Wmissing-prototypes -Wstrict-prototypes -Wold-style-definition \
	    -Wredundant-decls -Wdouble-promotion -Wfloat-equal \
	    -Wswitch-enum -Wswitch-default -Wnull-dereference -Walloc-zero \
	    -Wvla -Wimplicit-fallthrough=5 -Wcast-align=strict -Wlogical-op \
	    -Wduplicated-cond -Wduplicated-branches -Wrestrict \
	    -Wstringop-overflow=4 -Warray-bounds=2 -Wunused-macros \
	    -Wstrict-overflow=4 -Wshift-overflow=2 -Wtrampolines \
	    -Wconversion -Wsign-conversion

LDLIBS    = -lasound -lpthread -lm

cwkeyer: cwkeyer.c
	$(CC) $(CFLAGS) $(WARNINGS) -o $@ $< $(LDLIBS)

.PHONY: test analyze clean
test:
	@tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	$(CC) $(CFLAGS) $(WARNINGS) -o "$$tmp/test_keyer" tests/test_keyer.c $(LDLIBS) && \
	"$$tmp/test_keyer" && \
	$(CC) $(CFLAGS) $(WARNINGS) -o "$$tmp/test_signals" tests/test_signals.c $(LDLIBS) && \
	"$$tmp/test_signals" && \
	bash tests/test_mic_mix.sh

# GCC's static analyzer; slower than a normal build, so kept separate.
analyze:
	$(CC) $(CFLAGS) $(WARNINGS) -fanalyzer -c cwkeyer.c -o /dev/null

clean:
	rm -f cwkeyer
