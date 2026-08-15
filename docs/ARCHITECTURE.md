# puci architecture

## Design goals

- Minimal RAM per session, near-instant cold start, no runtime bloat.
- Pure C17. No Node, no Python, no GC, no embedded scripting engine.
- Library plus thin CLI split that is non-negotiable from the first commit.

## The split

- `libpuci` — the reusable core. Agent loop, provider abstraction, tool
  execution, session state, streaming HTTP, MCP stdio client. Ships as static
  and shared libraries with a stable, versioned C ABI.
- `puci-cli` — a thin consumer of libpuci. May only call the public headers in
  `include/puci/`.
- `bindings/lua` — minimal C-ABI Lua module (`puci.so`) for LuaJIT / Neovim.

## Async tool lifecycle

libpuci's core promise is a caller-owned, pollable event pump that never blocks.
Tools implement one of two contracts:

- **Blocking** (`puci_tool_invoke_cb`): fast, bounded work that runs to
  completion on the session thread.
- **Non-blocking** (`puci_tool_async_vtable`): `begin` starts the work and
  returns immediately; `get_fds` reports the file descriptors the host must
  poll; `next_timeout_ms` reports how long the host may block; `step` performs
  one bounded unit of work; `abort` tears down an in-flight invocation. The
  built-in `run_shell` tool uses this contract to poll child stdout/stderr pipes
  alongside the HTTP provider sockets, so a long-running command never stalls
  the host loop.

Multiple `puci_session` instances can be driven concurrently within one host
`poll()`/`epoll` loop: each session exposes its own fd set and timeout, and the
host folds them together. Cancelling one session mid-tool terminates the child,
reaps it, and returns `PUCI_ERR_CANCELLED` without waiting for completion.

## Providers

Providers are wire-format backends behind a vtable (`build_request`,
`parse_stream`, plus auth/header policy). Built-ins:

- **openai** — the OpenAI-compatible `chat.completions` format. Also covers
  local endpoints: Ollama `/v1`, vLLM, LM Studio, OpenRouter.
- **anthropic** — the Anthropic Messages API (`/v1/messages`,
  `anthropic-version: 2023-06-01`). Translates OpenAI-style tool schemas into
  Anthropic `tools` and parses `content_block_start` / `content_block_delta`
  (`text_delta`, `input_json_delta`) / `tool_use` into the universal
  `puci_event` structures.

Provider selection is automatic: the agent resolves the vtable from the
session's configured provider name, falling back to endpoint URL / model-name
heuristics (`anthropic` in the URL or a `claude*` model selects Anthropic).

## MCP stdio client

The MCP client (`src/lib/mcp/`) launches external MCP servers as subprocesses
(stdin/stdout JSON-RPC 2.0, own process group), performs the
`initialize` → `notifications/initialized` → `tools/list` handshake, and maps
each discovered tool into a `puci_tool` with its JSON Schema. Invoking an MCP
tool dispatches `tools/call` and decodes the `content`/`error` response. Server
shutdown is clean: the context destructor SIGTERMs (then SIGKILLs after a grace
period) and reaps every child. Path guards prevent MCP binaries from living
inside `.git/` or `build*/`.

## Dependencies

Every dependency must be justified here. Current list:

- **libcurl** (HTTP + TLS), abstracted behind `src/lib/net/internal`. Replacable
  later with raw sockets plus mbedTLS for ultra-minimal embedded builds.
- **JSON**: a minimal streaming parser lives in `src/lib/json/`; not a public
  dependency.
- **TUI**: ncurses-or-notcurses, and it lives exclusively in `src/cli/tui/`.
  libpuci never links a terminal library.
- **Lua** (optional, `bindings/lua`): the Lua C ABI only, no Lua runtime
  dependency beyond what the host (LuaJIT / Neovim) already provides.

## Context compaction

Three-tier budget (`PUCI_CONTEXT_BUDGET`, default 128 kB): head (system + first turn) + tail (last 8 msgs) preserved atomically; middle verbose `TOOL` results truncated to 64 B + `[Prior tool output truncated: N bytes pruned for context budget]`; duplicate tool outputs deduplicated; pre-flight auto-compact before `build_request` (never emits upstream 400).

## Compile-time stripping

Memory, swarm, browser, and MCP subsystems are each behind a Meson feature flag
and must be fully strippable. The Lua bindings are built only with `-Dlua=true`.
