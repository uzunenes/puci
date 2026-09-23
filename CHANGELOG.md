# Changelog

All notable changes to this project are documented here.

## [Unreleased]

### Added

- GitHub Actions CI (`.github/workflows/ci.yml`): dual-compiler (gcc + clang) build & test matrix, ASan + UBSan sanitizer job, and swarm density benchmark job.
- README badges (CI status, MIT license, version, C17) and a "SQLite of AI agent harnesses" tagline with a one-line value proposition.

### Fixed

- Clipboard: backend commands (`wl-copy`, `xclip`, `xsel`, `pbcopy`) now run with stdout/stderr redirected to `/dev/null`. `xclip` and `wl-copy` fork a daemon to serve the selection, which kept the host's stdout/stderr pipe open indefinitely (an embedding host or test runner reading puci's stdout would hang).
- `test_content` no longer depends on the host: the OSC 52 size-gate check now skips OS clipboard backends via the new `clip_set_test_no_backend()` hook. Previously it timed out on any machine with `xclip`/`wl-copy` installed, and wrote a 64 KiB payload to the real clipboard.
- `bench_swarm`: the per-session PSS gate now uses peak PSS per session instead of the post-creation delta, which is within shared-page PSS noise (sessions are idle then) and was often negative, so the gate could never fail. JSON output gains `per_session_peak_pss_kb`; `docs/benchmarks.json` regenerated.
- Clang build: added missing trailing newlines in `src/cli/tui/content.h`, `src/cli/tui/ttg.h`, `src/cli/tui/ttg.c`, and `tests/integration/test_approval_event.c` (`-Wnewline-eof` under `-Werror`).
- Clang build: `do_one_turn` in `test_approval_event.c` now takes a `puci_approval_decision` instead of `int` (`-Wsign-conversion`).
- `subprojects/termbox2.wrap` now declares `patch_directory = termbox2` so the bundled `meson.build` is applied automatically on a fresh checkout (previously required a manual copy, leaving the TUI silently disabled).

### Changed

- LICENSE copyright holder corrected from the project name to the author (`Enes Uzun`).
- Documentation version references synchronized to v0.5.0: `docs/SHOW_HN.md`, `docs/BENCHMARKS.md`, `docs/COMPARISON.md`, `docs/EMBEDDING.md`. Corrected the per-session figure in `SHOW_HN.md` to ~103 kB PSS (matching `BENCHMARKS.md`/`COMPARISON.md`) and the test count in `COMPARISON.md` (18 → 24).

## [0.5.0] - 2026-08-15

### Added

- Featherweight TUI: single dual-mode `puci` binary (`isatty` dispatch, `--tui`/`--no-tui`), pure POSIX ANSI state machine in `src/cli/tui/` (zero deps, <100 KB heap, 60 Hz poll ring folding `STDIN` + session fds).
- 2×2 Swarm Monitor (4 panes, paginated), live RSS/PSS + context budget gauges, interactive diff approval (`y/n`, `j/k`, colors), session sidebar (`s`), SIGWINCH resize.
- `docs/TUI.md` and `isatty` headless fallback to REPL.

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
