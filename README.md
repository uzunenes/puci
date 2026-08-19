# puci

[![CI](https://github.com/uzunenes/puci/actions/workflows/ci.yml/badge.svg)](https://github.com/uzunenes/puci/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-0.5.0-2.0)](CHANGELOG.md)
[![C17](https://img.shields.io/badge/C-C17-blue.svg)](https://en.cppreference.com/w/c/17)

> **The SQLite of AI agent harnesses** — a minimal C17 core you embed into
> Neovim, game engines, or a 1,000-way evaluation swarm. No Python, no Node,
> no runtime bloat: ~103 kB per session, sub-ms cold start, 1,000 agents on
> one `poll()` loop.

PUCI is a minimal AI coding-agent harness written in portable C17. It is for
embedders that want a small provider/tool core and a caller-owned poll loop.

**v0.5.0 — Featherweight TUI & Swarm Dashboard (no ncurses, 60 Hz poll ring).** `docs/TUI.md` · `docs/BENCHMARKS.md` · `docs/COMPARISON.md`

## Comparison (vs Aider / Claude Code / Cline / agent.cpp)

|  | **PUCI (C17)** | Python/Node competitors |
|--|---------------|------------------------|
| **PSS / session** | **~103 kB** measured | ~30–150 MB |
| **8 GB host** | **~1000 agents** (one `poll()` loop) | ~20–40 |
| **Cold start** | **<2 ms** | 150–800 ms |
| **Embed** | C ABI + `puci.nvim` (no `-llua`) | IPC only |

Full grounded matrix + methodology: [`docs/COMPARISON.md`](docs/COMPARISON.md) · Repro: `bench/bench_swarm 1000`

## What works

- OpenAI-compatible streaming and provider transcript fixtures
- Anthropic Messages API (`anthropic-version: 2023-06-01`) with `tool_use` round-trip
- Local / OpenAI-compatible endpoints (Ollama, vLLM, LM Studio, OpenRouter)
- Blocking `puci_session_send()` and non-blocking, pollable session driving
- Non-blocking (async) tool execution lifecycle: long-running tools yield their
  execution and register file descriptors so the host loop never stalls
- Model Context Protocol (MCP) stdio client: connect external MCP servers,
  discover their tools dynamically, and dispatch `tools/call` over JSON-RPC 2.0
- Text, reasoning, tool-call, tool-result, completion, cancellation, and error
	event payloads
- Custom tools, approval callbacks, cancellation, ASan/UBSan, and Clang builds
- Concurrent agents: multiple sessions driven from a single host `poll()` loop

## Current limitations

The public API is an early 0.x contract and may change before 1.0. The MCP
client and Anthropic provider are new in the v0.2.0 track and are exercised by
offline mock fixtures; live MCP servers and live Anthropic endpoints should be
validated against your own deployments.

## Repository layout

- `libpuci` — the reusable core (agent loop, providers, tools, streaming HTTP,
  MCP client)
- `puci-cli` — the terminal client, a thin consumer of libpuci
- `bindings/lua` — minimal C-ABI Lua module (`puci.so`) for Neovim / LuaJIT
- `lua/puci` + `plugin/puci.lua` — zero-dependency `puci.nvim` plugin (`:Puci`, `require('puci').create_session()`)
- `examples/` — embedder examples including `embedded_editor_agent`, `poll_loop`, and `bench_swarm`
- `bench/bench_swarm` — 1000-agent density benchmark (`docs/BENCHMARKS.md`)
- See `docs/ARCHITECTURE.md` for the system design.

## Building

```sh
meson setup build
ninja -C build
```

For both supported compilers:

```sh
CC=gcc meson setup build-gcc && meson compile -C build-gcc && meson test -C build-gcc
CC=clang meson setup build-clang && meson compile -C build-clang && meson test -C build-clang
```

For bounded one-shot agent runs, set `PUCI_MAX_TOOL_ITERATIONS` to a positive
value up to 10000. When the limit is reached, PUCI ends the turn instead of
continuing tool dispatch indefinitely and returns `PUCI_ERR_LIMIT_EXCEEDED`.

Context budget (3-tier compaction: head+tail preserve, verbose TOOL truncation, dedup):

```sh
PUCI_CONTEXT_BUDGET=32768 puci run "long session"
# or puci_session_set_context_budget(sess, 131072); puci_session_compact_context(sess, &pruned);
```

Install the library and compile an embedder with `pkg-config`:

```sh
meson setup build -Dprefix=/usr/local
meson compile -C build
meson install -C build
cc examples/embedded_headless.c $(pkg-config --cflags --libs puci) -o embedded_headless
```

Build the examples and Lua bindings:

```sh
meson setup build-examples -Dexamples=true -Dlua=true
ninja -C build-examples
```

Neovim (`puci.nvim`, no `-llua` needed — Neovim provides Lua symbols):

```sh
meson setup build -Dlua=true && ninja -C build
# manual: cc -fPIC -shared bindings/lua/puci_lua.c -o puci.so $(pkg-config --cflags --libs puci)
# headless smoke: nvim --headless -u NONE -c "lua package.cpath=package.cpath..';build/bindings/lua/?.so'; require('puci')" -c "q"
```

Swarm density benchmark:

```sh
meson setup build-bench -Dbench=true && ninja -C build-bench bench/bench_swarm
./build-bench/bench/bench_swarm 1000  # ~106 MB RSS for 1000 sessions, see docs/BENCHMARKS.md
```

## Multi-provider configuration

PUCI auto-detects the provider from the endpoint URL or the model name:

- `base_url` containing `anthropic` or a model beginning with `claude` selects
  the Anthropic Messages API provider (`anthropic-version: 2023-06-01`).
- Everything else uses the OpenAI-compatible provider, which also covers local
  endpoints: Ollama (`http://127.0.0.1:11434/v1`), vLLM, LM Studio, and
  OpenRouter (set `PUCI_BASE_URL` and `PUCI_API_KEY` accordingly).

**OpenRouter BYOK** — `OPENROUTER_API_KEY` is a first-class fallback for `PUCI_API_KEY`; when present alone, PUCI defaults `base_url` to `https://openrouter.ai/api/v1` and `model` to `anthropic/claude-3.5-sonnet`.

Recommended `~/.bashrc`:

```bash
# GitHub Copilot CLI / OpenRouter BYOK for PUCI
export OPENROUTER_API_KEY="sk-or-v1-..."
export PUCI_API_KEY="$OPENROUTER_API_KEY"
export PUCI_BASE_URL="https://openrouter.ai/api/v1"
export PUCI_MODEL="anthropic/claude-3.5-sonnet"
```

Global config template (`~/.config/puci/config.toml`):

```toml
base_url = "https://openrouter.ai/api/v1"
model = "anthropic/claude-3.5-sonnet"
context_budget_bytes = 131072
tool_timeout_ms = 60000
```

## MCP servers

Connect external Model Context Protocol (MCP) servers via stdio by adding them
to `~/.config/puci/config.toml` or `.puci.toml`:

```toml
[mcp_servers.git]
command = "mcp-server-git"
args = ["--repository", "."]
```

PUCI launches each server as a subprocess, performs the MCP
`initialize`/`notifications/initialized` handshake, fetches `tools/list`, and
registers the discovered tools dynamically onto the context. Invoking an MCP
tool dispatches `tools/call` over JSON-RPC 2.0. MCP binaries are subject to the
same path guards as built-in tools (they cannot live inside `.git/` or `build*/`).

## Configuration and Credentials

Configuration resolution follows strict precedence (highest first):

1. Command-line flags (e.g. `--model`)
2. Environment variables (`PUCI_API_KEY`, `PUCI_BASE_URL`, `PUCI_MODEL`, etc.)
3. Project-local config (`.puci.toml` or `.puci/config.toml`) — **credentials forbidden**
4. User global config (`~/.config/puci/config.toml`)
5. Built-in defaults

**Credentials Rule**: API keys (`api_key`) and base URLs (`base_url`) are only accepted from environment variables or the user's global config file (`~/.config/puci/config.toml`). If credential fields appear in project-local configuration files, they are rejected with a fatal error to prevent accidental secret leakage into repositories.

## Self-Hosting and Recovery

When using PUCI to develop PUCI, preserve a verified known-good binary with `make install-good` to `~/.local/bin/puci-good`. See [docs/RECOVERY.md](docs/RECOVERY.md) for the one-line recovery guide and path protection details.

## License

PUCI is released under the MIT License. See [LICENSE](LICENSE).
