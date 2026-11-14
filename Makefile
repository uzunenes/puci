# Convenience wrapper around Meson so the usual workflow just works:
#
#   make                         # check deps, configure, build
#   make test                    # run the test suite
#   sudo make install            # install to /usr/local
#   make install PREFIX=~/.local # per-user install, no root
#   sudo make uninstall
#
# Meson remains the real build system; use it directly for anything advanced.

PREFIX   ?= /usr/local
BUILDDIR ?= build
MESON    ?= meson
NINJA    ?= $(shell command -v ninja || command -v ninja-build || echo ninja)
MESONFLAGS ?=

.PHONY: all deps configure build test install uninstall clean distclean help \
        install-good restore-good

all: build

help:
	@sed -n '1,9p' Makefile | sed 's/^# \{0,1\}//'

deps:
	@sh scripts/check-deps.sh

$(BUILDDIR)/build.ninja:
	@sh scripts/check-deps.sh
	$(MESON) setup $(BUILDDIR) --prefix=$(abspath $(PREFIX)) $(MESONFLAGS)

# Prefix the build dir was configured with (empty if not configured).
CONFIGURED_PREFIX = $(MESON) introspect $(BUILDDIR) --buildoptions 2>/dev/null | \
    python3 -c 'import json,sys;print(next(o["value"] for o in json.load(sys.stdin) if o["name"]=="prefix"))' 2>/dev/null

configure: $(BUILDDIR)/build.ninja
	@cur=$$($(CONFIGURED_PREFIX)); \
	if [ -n "$$cur" ] && [ "$$cur" != "$(abspath $(PREFIX))" ]; then \
	    echo "puci: prefix changed ($$cur -> $(abspath $(PREFIX))), reconfiguring"; \
	    $(MESON) configure $(BUILDDIR) --prefix=$(abspath $(PREFIX)); \
	fi

build: configure
	$(MESON) compile -C $(BUILDDIR)

test: build
	$(MESON) test -C $(BUILDDIR) --print-errorlogs --no-suite stress

# Install never compiles as root, so `sudo make install` can't leave
# root-owned files in the build tree. Run `make` (as yourself) first.
install:
	@if [ ! -f $(BUILDDIR)/build.ninja ]; then \
	    echo "puci: not built yet. Run 'make' first (without sudo)." >&2; exit 1; fi
	@cur=$$($(CONFIGURED_PREFIX)); want="$(abspath $(PREFIX))"; \
	if [ "$(origin PREFIX)" = "command line" ] && [ "$$cur" != "$$want" ]; then \
	    if [ "$$(id -u)" = 0 ]; then \
	        echo "puci: built for prefix $$cur. Run 'make PREFIX=$$want' first (without sudo)." >&2; exit 1; \
	    fi; \
	    $(MAKE) --no-print-directory build PREFIX="$$want" || exit 1; \
	fi
	$(MESON) install -C $(BUILDDIR) --no-rebuild $(if $(DESTDIR),--destdir $(DESTDIR))
	@if [ "$$(uname -s)" = Linux ] && [ "$$(id -u)" = 0 ] && [ -z "$(DESTDIR)" ]; then \
	    ldconfig 2>/dev/null || true; fi
	@p=$$($(CONFIGURED_PREFIX)); echo; echo "puci installed to $(DESTDIR)$$p/bin/puci"; \
	case ":$$PATH:" in *":$$p/bin:"*) ;; *) echo "note: $$p/bin is not on your PATH";; esac

uninstall:
	$(NINJA) -C $(BUILDDIR) uninstall

clean:
	@if [ -d $(BUILDDIR) ]; then $(MESON) compile -C $(BUILDDIR) --clean; fi

distclean:
	rm -rf $(BUILDDIR)

# ---- maintainer helpers (keep a known-good binary while hacking on puci) ----
install-good: build
	mkdir -p $(HOME)/.local/bin
	cp -f $(BUILDDIR)/src/cli/puci $(HOME)/.local/bin/puci-good
	chmod +x $(HOME)/.local/bin/puci-good
	@echo "Installed known-good binary to $(HOME)/.local/bin/puci-good"

restore-good:
	mkdir -p $(BUILDDIR)/src/cli && cp -f $(HOME)/.local/bin/puci-good $(BUILDDIR)/src/cli/puci
	@echo "Restored binary from $(HOME)/.local/bin/puci-good"
