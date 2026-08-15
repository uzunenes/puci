# Changelog

All notable changes to this project are documented here.

## [0.4.0] - 2026-08-15

### Added

- Three-tier context budget compaction: head+tail preserve, verbose TOOL result truncation (`[Prior tool output truncated: N bytes...]`), duplicate dedup, env `PUCI_CONTEXT_BUDGET`, auto pre-flight before dispatch, API `puci_session_{set,get,context_bytes,compact}_context*` (ABI append, v2 stable).
- Empirical competitor matrix `docs/COMPARISON.md` (PUCI ~103 kB vs 30-150 MB, 1000 vs 20-40 agents, <2 ms vs 150-800 ms, caller-owned poll loop) with methodology/disclaimer and README summary table.
- `test_context_compaction`: 25-turn 5 KB dumps, 32 KB budget, continuity assertions.

### Changed

- Documentation pruned to 5 authoritative docs + `docs/archive/`; `README` v0.4.0 banner with comparison table.

## [0.3.0] - 2026-08-15

### Added

- Model Context Protocol (MCP) stdio JSON-RPC 2.0 client: launches external
  MCP servers, performs the initialize handshake, discovers tools via
  `tools/list`, and dispatches `tools/call`. Configuration via
  `[mcp_servers.<name>]` in `~/.config/puci/config.toml` / `.puci.toml`.
- Anthropic Messages API provider (`anthropic-version: 2023-06-01`) with
  `tool_use` round-trip translation and automatic provider detection.
- Local / OpenAI-compatible provider tuning for Ollama, vLLM, LM Studio, and
  OpenRouter endpoints.
- Embedder showcase: `examples/embedded_editor_agent.c`, a Lua C-module
  (`bindings/lua/puci.so`) for LuaJIT / Neovim, and
  `examples/benchmark_footprint.c` (< 5 MB RSS baseline).

### Changed

- Hardened the non-blocking async tool lifecycle and concurrent multi-agent
  poll-loop driving (tested via `test_async_tool_nonblocking`,
  `test_concurrent_two_agents`, `test_async_tool_cancel`).

## [0.1.0] - 2026-08-13

### Added

- C17 embeddable agent core with blocking (`puci_session_send`) and caller-owned poll-loop APIs.
- Non-blocking (async) tool execution lifecycle (`puci_tool_async_vtable` in `tool.h` and async `run_shell`) - new in 0.1.0 and lightly tested.
- Borrowed payloads for text, reasoning, tool calls, tool results, completion,
  cancellation, and errors.
- GCC and Clang strict builds, sanitizer tests, and libFuzzer harnesses.

### Limitations

- Async tool execution is newly introduced and not yet fully hardened across all platforms.
- Stage A host-fd/concurrent-agent/cancellation example work and Phase 3b are
  not included.
