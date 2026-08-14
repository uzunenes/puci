# Self-Hosting Recovery Guide

When developing PUCI with PUCI, an incorrect code modification in `src/lib/` or `src/cli/` may break the build, preventing the development binary in `build/src/cli/puci` from compiling or functioning.

## 1. Preserving a Known-Good Binary

To save the current working binary outside the build tree:

```sh
make install-good
```

This copies the verified binary to `~/.local/bin/puci-good`.

## 2. One-Line Restore

If the build is broken and you need to restore or run PUCI:

```sh
~/.local/bin/puci-good run "revert the previous edit in src/lib/..."
```

Or to restore the binary into the build tree:

```sh
make restore-good
```

## 3. Path Protection Guards

PUCI file tools (`write_file`, `edit_file`) automatically refuse modifications to:
- `.git/` repository metadata
- `build*/` build directories
- `/proc/self/exe` (the currently running binary)
- `~/.local/bin/puci-good` (the installed recovery binary)
