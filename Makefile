# puci - plain Makefile. Needs only a C17 compiler, make and libcurl.
#
#   make                          build libpuci (.a + .so) and the puci CLI
#   make test                     run the test suite
#   make install                  install to /usr/local (may need sudo)
#   make install PREFIX=~/.local  per-user install, no root
#   make uninstall                remove what install put there
#
# GNU make (gmake on the BSDs). Honours the usual variables: PREFIX DESTDIR BINDIR LIBDIR INCLUDEDIR MANDIR
# PKGCONFIGDIR CC CFLAGS CPPFLAGS LDFLAGS AR. libcurl is found with
# pkg-config, else curl-config; override with CURL_CFLAGS / CURL_LIBS.
# Feature switches: TUI=0 (line-mode CLI only), SHARED=0 (no .so).
#
# Developers: Meson (meson.build) is the other build, used by CI for the
# full matrix; both build the same sources.

.SUFFIXES:

PREFIX       ?= /usr/local
BINDIR       ?= $(PREFIX)/bin
LIBDIR       ?= $(PREFIX)/lib
INCLUDEDIR   ?= $(PREFIX)/include
MANDIR       ?= $(PREFIX)/share/man
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2 -g
TUI     ?= 1
SHARED  ?= 1
O       ?= build/make

VERSION := $(shell awk '/^\#define PUCI_VERSION_(MAJOR|MINOR|PATCH) /{v = v (v == "" ? "" : ".") $$3} END{print v}' include/puci/version.h)
ABI     := 2

PKG_CONFIG ?= pkg-config
CURL_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libcurl 2>/dev/null || curl-config --cflags 2>/dev/null)
CURL_LIBS   ?= $(shell $(PKG_CONFIG) --libs libcurl 2>/dev/null || curl-config --libs 2>/dev/null || echo -lcurl)

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  SO_REAL  := libpuci.$(VERSION).dylib
  SO_NAME  := libpuci.$(ABI).dylib
  SO_LINK  := libpuci.dylib
  SO_FLAGS := -dynamiclib -install_name $(LIBDIR)/$(SO_NAME)
else
  SO_REAL  := libpuci.so.$(VERSION)
  SO_NAME  := libpuci.so.$(ABI)
  SO_LINK  := libpuci.so
  SO_FLAGS := -shared -Wl,-soname,$(SO_NAME)
endif

# Flags the code needs; user CFLAGS/CPPFLAGS add to them, never replace them.
WARN     := -Wall -Wextra -Wno-format-truncation
BASE_CPPFLAGS := -D_POSIX_C_SOURCE=200809L -Iinclude -Isrc/lib \
                 -DPUCI_FEATURE_TUI=$(TUI) -DPUCI_FEATURE_MEMORY=0 \
                 -DPUCI_FEATURE_SWARM=0 -DPUCI_FEATURE_BROWSER=0 \
                 -DPUCI_FEATURE_MCP=0 -DPUCI_FEATURE_NATIVE_ARCH=0 \
                 -DPUCI_FEATURE_LTO=0
ALL_CFLAGS := -std=c17 $(WARN) $(BASE_CPPFLAGS) $(CURL_CFLAGS) $(CPPFLAGS) $(CFLAGS)

LIB_SRC := \
	src/lib/core/context.c src/lib/core/session.c src/lib/core/agent.c \
	src/lib/core/diff.c src/lib/core/error.c \
	src/lib/net/http.c src/lib/net/sse.c src/lib/json/json.c \
	src/lib/providers/openai.c src/lib/providers/anthropic.c \
	src/lib/providers/registry.c src/lib/mcp/mcp_client.c \
	src/lib/tools/path_guard.c src/lib/tools/read_file.c \
	src/lib/tools/write_file.c src/lib/tools/edit_file.c \
	src/lib/tools/grep.c src/lib/tools/glob.c src/lib/tools/run_shell.c \
	src/lib/config/config.c src/lib/util/arena.c src/lib/util/alloc.c \
	src/lib/util/strbuf.c src/lib/util/log.c src/lib/version.c

CLI_SRC := \
	src/cli/main.c src/cli/cmd/run.c src/cli/cmd/config.c \
	src/cli/cmd/doctor.c src/cli/cmd/session_mgr.c \
	src/cli/repl/repl.c src/cli/repl/lineedit.c
TUI_SRC := \
	src/cli/tui/tui.c src/cli/tui/theme.c src/cli/tui/ttg.c \
	src/cli/tui/content.c src/cli/tui/md.c src/cli/tui/hl.c \
	src/cli/tui/clip.c src/cli/tui/conv.c
ifeq ($(TUI),1)
  CLI_SRC += $(TUI_SRC)
endif
CLI_CPPFLAGS := -Isrc/cli/tui/vendor -DPUCI_CLI_LINK_STATIC=1 -DHAVE_CURL=1

HEADERS := $(wildcard include/puci/*.h)

LIB_PIC := $(LIB_SRC:%.c=$(O)/pic/%.o)
LIB_OBJ := $(LIB_SRC:%.c=$(O)/obj/%.o)
CLI_OBJ := $(CLI_SRC:%.c=$(O)/cli/%.o)

LIBS_ALL := $(O)/libpuci.a
ifeq ($(SHARED),1)
  LIBS_ALL += $(O)/$(SO_REAL)
endif

all: $(LIBS_ALL) $(O)/puci

# ---- objects -------------------------------------------------------------
# -MMD -MP: the compiler records every header an object includes, so a
# change to any header (public or internal) rebuilds what uses it.
DEPFLAGS := -MMD -MP

$(O)/obj/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(O)/pic/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) $(DEPFLAGS) -fPIC -c $< -o $@

$(O)/cli/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) $(CLI_CPPFLAGS) $(DEPFLAGS) -c $< -o $@

-include $(LIB_OBJ:.o=.d) $(LIB_PIC:.o=.d) $(CLI_OBJ:.o=.d)

# ---- libraries and CLI --------------------------------------------------
$(O)/libpuci.a: $(LIB_OBJ)
	rm -f $@
	$(AR) rcs $@ $(LIB_OBJ)

$(O)/$(SO_REAL): $(LIB_PIC)
	$(CC) $(SO_FLAGS) $(LDFLAGS) -o $@ $(LIB_PIC) $(CURL_LIBS)
	ln -sf $(SO_REAL) $(O)/$(SO_NAME)
	ln -sf $(SO_REAL) $(O)/$(SO_LINK)

# The CLI links libpuci statically: it then runs from any PREFIX without
# ldconfig or rpath. libpuci.so is installed for programs that embed it.
$(O)/puci: $(CLI_OBJ) $(O)/libpuci.a
	$(CC) $(LDFLAGS) -o $@ $(CLI_OBJ) $(O)/libpuci.a $(CURL_LIBS) -pthread

# puci.pc is written at install time so it always matches the PREFIX /
# LIBDIR / INCLUDEDIR given to `make install`.
PC_SED := sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@LIBDIR@|$(LIBDIR)|' \
              -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|' -e 's|@VERSION@|$(VERSION)|'

# ---- tests -----------------------------------------------------------------
T := $(O)/tests
TEST_LIB := json strbuf session tools serialize provider_fixtures approval \
            session_api mcp context_compaction
TEST_INT := approval_event agent_loop cancel conn_reuse http_async async_loop \
            async_tool two_agents
TEST_BINS := $(TEST_LIB:%=$(T)/test_%) $(TEST_INT:%=$(T)/test_%) \
             $(T)/test_config $(T)/test_ttg $(T)/test_content $(T)/test_conv
TEST_LINK := $(O)/libpuci.a $(CURL_LIBS) -pthread

$(T)/test_provider_fixtures: tests/unit/test_provider_fixtures.c $(O)/libpuci.a
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) '-DPUCI_FIXTURES_DIR="$(CURDIR)/tests/fixtures/transcripts"' -o $@ $< $(TEST_LINK)
$(T)/test_%: tests/unit/test_%.c $(O)/libpuci.a
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) -o $@ $< $(TEST_LINK)
$(T)/test_%: tests/integration/test_%.c $(O)/libpuci.a
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) -o $@ $< $(TEST_LINK)
$(T)/test_config: tests/unit/test_config.c src/cli/cmd/config.c $(O)/libpuci.a
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) -o $@ tests/unit/test_config.c src/cli/cmd/config.c $(TEST_LINK)
$(T)/test_ttg: tests/unit/test_ttg.c src/cli/tui/ttg.c
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) -Isrc/cli/tui -o $@ $^
$(T)/test_content: tests/unit/test_content.c src/cli/tui/content.c src/cli/tui/md.c src/cli/tui/hl.c src/cli/tui/clip.c
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) -Isrc/cli/tui -o $@ $^
$(T)/test_conv: tests/unit/test_conv.c src/cli/tui/conv.c src/cli/tui/md.c
	@mkdir -p $(@D)
	$(CC) $(ALL_CFLAGS) -Isrc/cli/tui -o $@ $^

test check: $(TEST_BINS) $(O)/puci
	@fail=0; for t in $(TEST_BINS); do \
	    if $$t >$$t.log 2>&1; then echo "ok   $${t##*/}"; \
	    else echo "FAIL $${t##*/} (see $$t.log)"; fail=1; fi; \
	done; \
	if sh scripts/check_tui_tokens.sh >/dev/null; then echo "ok   tui_tokens"; \
	else echo "FAIL tui_tokens"; fail=1; fi; \
	if [ "$(TUI)" = 1 ] && command -v python3 >/dev/null 2>&1; then \
	    if PUCI_BIN=$(CURDIR)/$(O)/puci python3 tests/test_tui_polish.py >$(T)/tui.log 2>&1; \
	    then echo "ok   test_tui_polish"; else echo "FAIL test_tui_polish (see $(T)/tui.log)"; fail=1; fi; \
	fi; \
	exit $$fail

# ---- install / uninstall ---------------------------------------------------
install: all
	mkdir -p $(DESTDIR)$(BINDIR) $(DESTDIR)$(LIBDIR) $(DESTDIR)$(PKGCONFIGDIR) \
	         $(DESTDIR)$(INCLUDEDIR)/puci $(DESTDIR)$(MANDIR)/man1 $(DESTDIR)$(MANDIR)/man3
	cp -f $(O)/puci $(DESTDIR)$(BINDIR)/puci
	chmod 755 $(DESTDIR)$(BINDIR)/puci
	cp -f $(O)/libpuci.a $(DESTDIR)$(LIBDIR)/libpuci.a
	chmod 644 $(DESTDIR)$(LIBDIR)/libpuci.a
	if [ "$(SHARED)" = 1 ]; then \
	    cp -f $(O)/$(SO_REAL) $(DESTDIR)$(LIBDIR)/$(SO_REAL) && \
	    chmod 755 $(DESTDIR)$(LIBDIR)/$(SO_REAL) && \
	    ln -sf $(SO_REAL) $(DESTDIR)$(LIBDIR)/$(SO_NAME) && \
	    ln -sf $(SO_REAL) $(DESTDIR)$(LIBDIR)/$(SO_LINK); \
	fi
	cp -f $(HEADERS) $(DESTDIR)$(INCLUDEDIR)/puci/
	chmod 644 $(DESTDIR)$(INCLUDEDIR)/puci/*.h
	$(PC_SED) puci.pc.in > $(DESTDIR)$(PKGCONFIGDIR)/puci.pc
	cp -f man/puci.1 $(DESTDIR)$(MANDIR)/man1/puci.1
	cp -f man/libpuci.3 $(DESTDIR)$(MANDIR)/man3/libpuci.3
	chmod 644 $(DESTDIR)$(PKGCONFIGDIR)/puci.pc $(DESTDIR)$(MANDIR)/man1/puci.1 $(DESTDIR)$(MANDIR)/man3/libpuci.3
	@if [ -z "$(DESTDIR)" ] && [ "$(UNAME_S)" = Linux ] && [ "$$(id -u)" = 0 ]; then ldconfig 2>/dev/null || true; fi
	@echo "puci $(VERSION) installed to $(DESTDIR)$(PREFIX)"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/puci
	rm -f $(DESTDIR)$(LIBDIR)/libpuci.a $(DESTDIR)$(LIBDIR)/$(SO_REAL) \
	      $(DESTDIR)$(LIBDIR)/$(SO_NAME) $(DESTDIR)$(LIBDIR)/$(SO_LINK)
	rm -f $(DESTDIR)$(PKGCONFIGDIR)/puci.pc
	rm -f $(DESTDIR)$(MANDIR)/man1/puci.1 $(DESTDIR)$(MANDIR)/man3/libpuci.3
	rm -f $(HEADERS:include/puci/%=$(DESTDIR)$(INCLUDEDIR)/puci/%)
	-rmdir $(DESTDIR)$(INCLUDEDIR)/puci 2>/dev/null

# ---- release tarball ---------------------------------------------------------
dist:
	@mkdir -p $(O)
	git archive --format=tar.gz --prefix=puci-$(VERSION)/ -o $(O)/puci-$(VERSION).tar.gz HEAD
	@echo $(O)/puci-$(VERSION).tar.gz

clean:
	rm -rf $(O)

deps:
	@sh scripts/check-deps.sh

# ---- maintainer helpers (keep a known-good binary while hacking on puci) ----
install-good: $(O)/puci
	mkdir -p $(HOME)/.local/bin
	cp -f $(O)/puci $(HOME)/.local/bin/puci-good
	@echo "Installed known-good binary to $(HOME)/.local/bin/puci-good"

restore-good:
	mkdir -p $(O) && cp -f $(HOME)/.local/bin/puci-good $(O)/puci
	@echo "Restored $(O)/puci from $(HOME)/.local/bin/puci-good"

.PHONY: all test check install uninstall dist clean deps install-good restore-good
