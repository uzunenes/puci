.PHONY: install-good restore-good clean

install-good:
	mkdir -p $(HOME)/.local/bin
	@if [ -d build ]; then \
		ninja -C build src/cli/puci && cp -f build/src/cli/puci $(HOME)/.local/bin/puci-good; \
	elif [ -d build-gcc ]; then \
		ninja -C build-gcc src/cli/puci && cp -f build-gcc/src/cli/puci $(HOME)/.local/bin/puci-good; \
	else \
		echo "No build or build-gcc directory found. Run meson setup first." >&2; exit 1; \
	fi
	chmod +x $(HOME)/.local/bin/puci-good
	@echo "Installed known-good binary to $(HOME)/.local/bin/puci-good"

restore-good:
	@if [ -d build ]; then \
		mkdir -p build/src/cli && cp -f $(HOME)/.local/bin/puci-good build/src/cli/puci; \
	fi
	@if [ -d build-gcc ]; then \
		mkdir -p build-gcc/src/cli && cp -f $(HOME)/.local/bin/puci-good build-gcc/src/cli/puci; \
	fi
	@echo "Restored binary from $(HOME)/.local/bin/puci-good"
