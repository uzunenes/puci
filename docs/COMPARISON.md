# PUCI vs Competitors — Technical Comparison (v0.4.0)

> **Methodology & Honesty:** PUCI numbers are measured on this repo (`bench/bench_swarm`, `bench/bench_session_growth`, `/proc/smaps_rollup` PSS). Competitor numbers are **order-of-magnitude estimates** from public profiling, docs, and isolated local runs — not controlled lab benchmarks. They are marked `~` (estimated) or `measured` where we ran them. Reproduce PUCI numbers via commands below; competitor numbers vary by version/model/plugins.

| Dimension | **PUCI (C17)** | **Aider (Python)** | **Claude Code (Node/TS)** | **Cline (TS/VSCode)** | **agent.cpp (C++17)** |
|-----------|---------------|--------------------|--------------------------|-----------------------|----------------------|
| **Runtime** | Static/shared lib, `libcurl` only. No Python/Node. `c_std=c17`. | `python3 + pip + venv` | `node + npm` | VSCode extension host (Electron/Node) | C++17, often `libcurl` + `nlohmann/json` |
| **Memory (PSS/session)** | **~103 kB** measured (1000 sess: 106 MB RSS, `bench_swarm`) | ~30–80 MB | ~40–100 MB | ~80–150 MB (VSCode overhead) | ~200–500 kB (est.) |
| **Concurrency (8 GB host)** | **~1000 active agents** (single `poll()` loop, no threads) | ~20–40 | ~20–40 | ~10–20 | ~500–800 (est.) |
| **Cold start** | **<2 ms** (no runtime init) | 250–800 ms | 150–400 ms | 300–600 ms | 2–5 ms |
| **Event loop** | **Caller-owned `poll()`/`epoll`** (`begin_turn/get_fds/next_timeout_ms/process/next_event`), level-triggered, timeout floor 1000 ms | Forced `asyncio` | Forced `libuv` | VSCode event loop | Often caller-owned or `asio` |
| **Embeddability** | **C ABI** (`#include <puci/session.h>`), Lua (`puci.nvim` without `-llua`), game engines (Unreal/Godot), `poll()` drones | IPC/subprocess | IPC/subprocess | VSCode API only | C++ ABI (harder for Lua) |
| **Process supervision** | `fork+execvp` (no `system()`), `setpgid`/`killpg`, non-blocking pipe drain, `SIGTERM→SIGKILL` 2s grace, `waitpid` reaping, bounded `CAPTURE_CAP` 1 MB | `subprocess` (Python) | `child_process` (Node) | VSCode `Terminal` | Varies |
| **Safety** | Path guards (`.git/build*/puci-good` via `realpath`), atomic `tmp→fsync→rename`, credential redaction, `plan_mode` | Minimal | Minimal | VSCode sandbox | Varies |
| **Ecosystem** | **MCP stdio JSON-RPC 2.0** (4 KB buffered), OpenAI-compatible + Anthropic (`anthropic-version: 2023-06-01`), Ollama/vLLM/OpenRouter | MCP (py) | MCP (js) | MCP (js) | Often custom |
| **Context management** | **3-tier budget** (head+tail preserve, verbose TOOL truncation, dedup, pre-flight auto-compact, `PUCI_CONTEXT_BUDGET`) — v0.4.0 | Sliding window + repo map | Repo map + summarization | VSCode context | Usually none |
| **ABI stability** | **ABI v2** stable, `pkg-config`, dual-compiler `-Wconversion -Werror`, ASan/UBSan + Valgrind `definite,indirect` 0 leaks | Semver (py) | Semver (npm) | VSCode API | Varies |

## Reproduction (PUCI)

```sh
meson setup build && ninja -C build && meson test -C build          # 18 tests, 0 leaks
meson setup build-bench -Dbench=true && ./build-bench/bench/bench_swarm 1000  # 685 turns/s, 106 MB
./build-bench/bench/bench_session_growth 16384                        # per-msg PSS delta
```

## Why This Matters

- **Density:** 10×–40× more agents per host → swarm benchmarks and CI fleets at 1/10th cost.
- **Embeddability:** Drop into a host that already owns an event loop (Neovim `vim.loop`, Unreal tick, `epoll` daemon) without spawning a language runtime per agent.
- **Determinism:** C17 + bounded budgets = predictable RSS/latency under load, vs GC pauses.

## Limitations vs Competitors (honest)

- **Repo intelligence:** No tree-sitter AST / vector index yet (Aider/Claude have mature repo-map). Planned v0.4.1.
- **MCP:** stdio only; no SSE/HTTP transport yet.
- **Windows:** Linux primary; macOS kqueue path partial; Windows IOCP not shipped.

*Last updated: 2026-08-15, PUCI v0.4.0, ABI 2.*
