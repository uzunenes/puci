# Contributing

Thanks for considering contributing to puci.

## Ground rules

- **No `system()`.** It is a shell injection vector. Use the internal
  process-spawn layer instead.
- **No `strcpy` / `strcat` / `sprintf` / `gets`.** Use bounded equivalents or
  the internal string builder.
- **Prefix everything public** with `puci_` / `PUCI_`. libpuci is FFI-facing.
- **No global mutable state** in libpuci; everything hangs off a context.
- **No `printf` in libpuci.** All output goes through the logging callback.
- Everything compiles clean under `-Wall -Wextra -Wpedantic -Werror`.
- Tests must pass under ASan and UBSan.

## Development loop

```sh
meson setup build
ninja -C build
meson test -C build
```

## Commit style

Use conventional commits: `feat:`, `fix:`, `perf:`, `docs:`, `build:`,
`test:`, `refactor:`.

## Code of conduct

See `CODE_OF_CONDUCT.md`. Be kind.