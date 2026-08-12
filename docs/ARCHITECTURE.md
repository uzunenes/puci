# puci architecture

> Placeholder: filled in during Phase 6 and refined throughout development.

## Design goals

- Minimal RAM per session, near-instant cold start, no runtime bloat.
- Pure C17. No Node, no Python, no GC, no embedded scripting engine.
- Library plus thin CLI split that is non-negotiable from the first commit.

## The split

- `libpuci` — the reusable core. Agent loop, provider abstraction, tool
  execution, session state, streaming HTTP. Ships as static and shared
  libraries with a stable, versioned C ABI.
- `puci-cli` — a thin consumer of libpuci. May only call the public headers in
  `include/puci/`.

## Dependencies

Every dependency must be justified here. Current list:

- **libcurl** (HTTP + TLS), abstracted behind `src/lib/net/internal`. Replacable
  later with raw sockets plus mbedTLS for ultra-minimal embedded builds.
- **JSON**: a minimal streaming parser lives in `src/lib/json/`; not a public
  dependency.
- **TUI**: ncurses-or-notcurses, and it lives exclusively in `src/cli/tui/`.
  libpuci never links a terminal library.

## Compile-time stripping

Memory, swarm, browser, and MCP subsystems are each behind a Meson feature flag
and must be fully strippable.
