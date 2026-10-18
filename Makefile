# SPDX-License-Identifier: GPL-3.0-or-later
#
# ham-cwkeyer: low-latency iambic CW sidetone keyer.

CC       ?= gcc
CFLAGS   ?= -O2
# Set WERROR= to build despite warnings (e.g. on an unknown toolchain).
WERROR   ?= -Werror

# Release tags are named vX.Y.Z; strip the leading v so the reported
# version and the dist tarball read X.Y.Z.  A release tarball has no git
# history, so fall back to the VERSION file that `make dist` writes.
VERSION  ?= $(patsubst v%,%,$(shell git describe --tags --always --dirty 2>/dev/null || cat VERSION 2>/dev/null || echo 0.0.0))
# git archive only contains HEAD, so a -dirty suffix would name a tarball that
# does not actually contain the working-tree changes.  Strip it for dist.
DIST_VERSION ?= $(patsubst %-dirty,%,$(VERSION))
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

# The differential test transcribes third-party keyer source verbatim, so it
# is not ours to restructure.  GCC's level-4 -Wstrict-overflow reports false
# positives on some of it that vary between GCC versions; clang has no such
# warning.  The main code keeps the full warning set.
ifneq ($(findstring clang,$(CC)),)
DIFF_WARNINGS =
else
DIFF_WARNINGS = -Wno-strict-overflow
endif

CPPFLAGS += -DCWKEYER_VERSION='"$(VERSION)"'
LDLIBS    = -lasound -lpthread -lm

CTESTS = test_keyer test_signals test_audio test_evdev

.PHONY: test test-differential analyze coverage test-libcw install uninstall dist clean

cwkeyer: cwkeyer.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o $@ $< $(LDLIBS)

test:
	@tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	for t in $(CTESTS); do \
	    $(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -o "$$tmp/$$t" tests/$$t.c $(LDLIBS) || exit 1; \
	    "$$tmp/$$t" || exit 1; \
	done; \
	bash tests/test_mic_mix.sh

# Differential test against eight third-party iambic keyer variants (seven
# upstream projects; transcribed in the test, so no external dependency).
# Kept out of `make test` because it takes a few seconds.  Pass arguments
# to sweep speed and input timing, e.g. `make test-differential ARGS="20 50 300"`.
ARGS ?=
test-differential:
	@tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(DIFF_WARNINGS) \
	    -o "$$tmp/test_differential_thirdparty" \
	    tests/test_differential_thirdparty.c $(LDLIBS) || exit 1; \
	"$$tmp/test_differential_thirdparty" $(ARGS) || exit 1

# GCC's static analyzer; slower than a normal build, so kept separate.
analyze:
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -fanalyzer -c cwkeyer.c -o /dev/null

# Differential test against UnixCW's iambic keyer. Needs a built unixcw
# checkout (./configure && make); override LIBCW_DIR if it lives elsewhere.
LIBCW_DIR ?= ../unixcw-code
test-libcw:
	@test -f "$(LIBCW_DIR)/src/libcw/.libs/libcw.a" || { \
	    echo "error: build unixcw first, or set LIBCW_DIR (now $(LIBCW_DIR))"; \
	    exit 1; }
	$(CC) $(CFLAGS) -Wall -Wextra -o test_differential_libcw \
	    -I"$(LIBCW_DIR)/src" -I"$(LIBCW_DIR)/src/libcw" \
	    tests/test_differential_libcw.c \
	    "$(LIBCW_DIR)/src/libcw/.libs/libcw.a" \
	    -lasound -lpulse -lpulse-simple -ldl $(LDLIBS)
	./test_differential_libcw
	rm -f test_differential_libcw

# Code coverage (needs gcovr). Writes coverage/index.html and coverage.xml.
coverage:
	@rm -rf coverage
	@mkdir -p coverage
	@for t in $(CTESTS); do \
	    $(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) --coverage -O0 -g -c tests/$$t.c -o coverage/$$t.o || exit 1; \
	    $(CC) --coverage -o coverage/$$t coverage/$$t.o $(LDLIBS) || exit 1; \
	    ./coverage/$$t || exit 1; \
	done
	gcovr --root . --filter 'cwkeyer\.c' --txt --cobertura coverage/coverage.xml \
	    --html-details coverage/index.html
	@echo "coverage report: coverage/index.html"

install: cwkeyer
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(MANDIR)"
	install -m 0755 cwkeyer "$(DESTDIR)$(BINDIR)/cwkeyer"
	install -m 0755 cw-mic-mix.sh "$(DESTDIR)$(BINDIR)/cw-mic-mix"
	install -m 0644 cwkeyer.1 "$(DESTDIR)$(MANDIR)/cwkeyer.1"
	install -m 0644 cw-mic-mix.1 "$(DESTDIR)$(MANDIR)/cw-mic-mix.1"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/cwkeyer" \
	      "$(DESTDIR)$(BINDIR)/cw-mic-mix" \
	      "$(DESTDIR)$(MANDIR)/cwkeyer.1" \
	      "$(DESTDIR)$(MANDIR)/cw-mic-mix.1"

dist:
	@tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	dir=ham-cwkeyer-$(DIST_VERSION); \
	git archive --format=tar --prefix="$$dir/" HEAD | tar -x -C "$$tmp" || exit 1; \
	echo "$(DIST_VERSION)" > "$$tmp/$$dir/VERSION"; \
	tar -czf "$$dir.tar.gz" -C "$$tmp" "$$dir"; \
	echo "wrote $$dir.tar.gz"

clean:
	rm -f cwkeyer test_differential_libcw
	rm -rf coverage
