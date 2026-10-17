# SPDX-License-Identifier: GPL-3.0-or-later
#
# ham-cwkeyer: low-latency iambic CW sidetone keyer.

CC       ?= gcc
CFLAGS   ?= -O2
# Set WERROR= to build despite warnings (e.g. on an unknown toolchain).
WERROR   ?= -Werror

VERSION  ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo 1.0.0)
PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
MANDIR   ?= $(PREFIX)/share/man/man1
DESTDIR  ?=

# A deliberately aggressive warning set; the tree is clean under all of it.
# GCC-only flags clang rejects are kept separate.
COMMON_WARNINGS = \
	-Wall -Wextra -Wpedantic $(WERROR) \
	-Wshadow -Wformat=2 -Wundef -Wcast-qual -Wwrite-strings \
	-Wmissing-prototypes -Wstrict-prototypes -Wold-style-definition \
	-Wredundant-decls -Wfloat-equal -Wswitch-enum -Wswitch-default \
	-Wvla -Wimplicit-fallthrough -Wcast-align -Wconversion \
	-Wsign-conversion -Wunused-macros -Wnull-dereference

GCC_WARNINGS = $(COMMON_WARNINGS) \
	-Wdouble-promotion -Walloc-zero -Wlogical-op -Wduplicated-cond \
	-Wduplicated-branches -Wrestrict -Wstringop-overflow=4 \
	-Warray-bounds=2 -Wstrict-overflow=4 -Wshift-overflow=2 -Wtrampolines

ifneq ($(findstring clang,$(CC)),)
WARNINGS = $(COMMON_WARNINGS)
else
WARNINGS = $(GCC_WARNINGS)
endif

CPPFLAGS += -DCWKEYER_VERSION='"$(VERSION)"'
LDLIBS    = -lasound -lpthread -lm

.PHONY: test analyze coverage install uninstall dist clean

cwkeyer: cwkeyer.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ $< $(LDLIBS)

test:
	@tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o "$$tmp/test_keyer" tests/test_keyer.c $(LDLIBS) && \
	"$$tmp/test_keyer" && \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o "$$tmp/test_signals" tests/test_signals.c $(LDLIBS) && \
	"$$tmp/test_signals" && \
	bash tests/test_mic_mix.sh

# GCC's static analyzer; slower than a normal build, so kept separate.
analyze:
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -fanalyzer -c cwkeyer.c -o /dev/null

# Code coverage (needs gcovr). Writes coverage/index.html and coverage.xml.
coverage:
	@rm -rf coverage
	@mkdir -p coverage
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) --coverage -O0 -g -c tests/test_keyer.c -o coverage/test_keyer.o
	$(CC) --coverage -o coverage/test_keyer coverage/test_keyer.o $(LDLIBS)
	./coverage/test_keyer
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) --coverage -O0 -g -c tests/test_signals.c -o coverage/test_signals.o
	$(CC) --coverage -o coverage/test_signals coverage/test_signals.o $(LDLIBS)
	./coverage/test_signals
	gcovr --root . --filter 'cwkeyer\.c' --txt --cobertura coverage/coverage.xml \
	    --html-details coverage/index.html
	@echo "coverage report: coverage/index.html"

install: cwkeyer
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(MANDIR)"
	install -m 0755 cwkeyer "$(DESTDIR)$(BINDIR)/cwkeyer"
	install -m 0755 cw-mic-mix.sh "$(DESTDIR)$(BINDIR)/cw-mic-mix"
	install -m 0644 cwkeyer.1 "$(DESTDIR)$(MANDIR)/cwkeyer.1"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/cwkeyer" \
	      "$(DESTDIR)$(BINDIR)/cw-mic-mix" \
	      "$(DESTDIR)$(MANDIR)/cwkeyer.1"

dist:
	git archive --prefix=ham-cwkeyer-$(VERSION)/ \
	    -o ham-cwkeyer-$(VERSION).tar.gz HEAD
	@echo "wrote ham-cwkeyer-$(VERSION).tar.gz"

clean:
	rm -f cwkeyer
	rm -rf coverage
