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

## Comparison (vs Aider / Claude Code / Cline / agent.cpp)

|  | **PUCI (C17)** | Python/Node competitors |
|--|---------------|------------------------|
| **PSS / session** | **~103 kB** measured | ~30–150 MB |
| **8 GB host** | **~1000 agents** (one `poll()` loop) | ~20–40 |
| **Cold start** | **<2 ms** | 150–800 ms |
| **Embed** | C ABI + `puci.nvim` (no `-llua`) | IPC only |

Full grounded matrix + methodology: [`docs/COMPARISON.md`](docs/COMPARISON.md) · Repro: `bench/bench_swarm 1000`

## What works

- OpenAI-compatible streaming (OpenAI, OpenRouter, Ollama, vLLM, LM Studio)
- Anthropic Messages API (`anthropic-version: 2023-06-01`) with `tool_use` round-trip
- Blocking `puci_session_send()` and non-blocking, pollable session driving
- Async tool execution: long-running tools yield and register fds so the host loop never stalls
- Model Context Protocol (MCP) stdio client: discover and dispatch external tools over JSON-RPC 2.0
- Text, reasoning, tool-call, tool-result, completion, cancellation, and error payloads
- Custom tools, approval callbacks, cancellation, ASan/UBSan, and Clang builds
- Concurrent agents: multiple sessions on a single host `poll()` loop

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
meson test -C build
```

Both compilers are supported and CI-tested:

```sh
CC=gcc   meson setup build-gcc   && meson compile -C build-gcc   && meson test -C build-gcc
CC=clang meson setup build-clang && meson compile -C build-clang && meson test -C build-clang
```

Install the library and compile an embedder with `pkg-config`:

```sh
meson setup build -Dprefix=/usr/local && meson compile -C build && meson install -C build
cc examples/embedded_headless.c $(pkg-config --cflags --libs puci) -o embedded_headless
```

Optional build switches: `-Dexamples=true`, `-Dlua=true` (Neovim bindings), `-Dbench=true` (swarm benchmark). See `docs/EMBEDDING.md` for the full embedding guide.

Notable env vars: `PUCI_MAX_TOOL_ITERATIONS` (cap tool rounds), `PUCI_CONTEXT_BUDGET` (3-tier compaction budget).

## Configuration

PUCI auto-detects the provider: a `base_url` or model containing `anthropic`/`claude` selects the Anthropic Messages API; everything else uses the OpenAI-compatible provider (covers Ollama, vLLM, LM Studio, OpenRouter).

Set credentials via environment variables (never committed to repos):

```bash
export PUCI_API_KEY="sk-..."
export PUCI_BASE_URL="https://openrouter.ai/api/v1"
export PUCI_MODEL="anthropic/claude-3.5-sonnet"
```

`OPENROUTER_API_KEY` is a first-class fallback for `PUCI_API_KEY`. Config resolution precedence: CLI flags > env vars > project-local config (`.puci.toml`, credentials forbidden) > global config (`~/.config/puci/config.toml`) > defaults. API keys in project-local config are rejected with a fatal error to prevent secret leakage.

Global config template (`~/.config/puci/config.toml`):

```toml
base_url = "https://openrouter.ai/api/v1"
model = "anthropic/claude-3.5-sonnet"
context_budget_bytes = 131072
tool_timeout_ms = 60000
```

## MCP servers

Connect external Model Context Protocol servers via stdio in `~/.config/puci/config.toml`:

```toml
[mcp_servers.git]
command = "mcp-server-git"
args = ["--repository", "."]
```

PUCI launches each server, performs the MCP handshake, discovers tools, and dispatches `tools/call` over JSON-RPC 2.0.

## Self-Hosting and Recovery

When using PUCI to develop PUCI, `make install-good` preserves a verified binary at `~/.local/bin/puci-good`. See [docs/RECOVERY.md](docs/RECOVERY.md) for the recovery guide.

## License

PUCI is released under the MIT License. See [LICENSE](LICENSE).
