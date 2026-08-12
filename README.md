# puci

A minimal, embedded-friendly AI coding agent harness written in pure C17.

This README is a placeholder. It will be finalized in Phase 6 with install
instructions, quick start, an embedding example, and measured performance
numbers.

## Repository layout

- `libpuci` — the reusable core (agent loop, providers, tools, streaming HTTP)
- `puci-cli` — the terminal client, a thin consumer of libpuci
- See `docs/ARCHITECTURE.md` for the system design.

## Building

```sh
meson setup build
ninja -C build
```