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

## The TUI

`puci-cli` ships a termbox2 TUI for interactive use — multi-session, scrollable
transcript, model switcher, approval prompts. A real 80×24 session:

<pre lang="text">
 PUCI · Agent A · mock-model                            ● Ready · Context 0%
┌─ Agent A ────────────────────────────────────────────────────────────────────┐
│YOU  ❯ hello
│PUCI · mock-model  Hi there! How can I help?                                  │
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘
┌─ Composer ───────────────────────────────────────────────────────────────────┐
│Message … (Enter to compose)                                                  │
└──────────────────────────────────────────────────────────────────────────────┘
Enter prompt   ·   ↑/↓ scroll   ·   s sessions   ·   m model
</pre>

More sizes in [`docs/snapshots/`](docs/snapshots/). Keybindings: [`docs/KEYBINDINGS.md`](docs/KEYBINDINGS.md).

## Comparison (vs Aider / Claude Code / Cline / agent.cpp)

|  | **PUCI (C17)** | Python/Node competitors |
|--|---------------|------------------------|
| **PSS / session** | **~103 kB** measured | ~30–150 MB |
| **8 GB host** | **~1000 agents** (one `poll()` loop) | ~20–40 |
| **Cold start** | **<2 ms** | 150–800 ms |
| **Embed** | C ABI + `puci.nvim` (no `-llua`) | IPC only |

Full grounded matrix + methodology: [`docs/COMPARISON.md`](docs/COMPARISON.md) · Repro: `bench/bench_swarm 1000`

## What works

- **Providers:** OpenAI-compatible streaming (OpenAI, OpenRouter, Ollama, vLLM, LM Studio) + Anthropic Messages API with `tool_use`
- **Sessions:** blocking `puci_session_send()` and non-blocking pollable driving; concurrent agents on one `poll()` loop
- **Tools:** custom tools, async execution (long-running tools yield fds), MCP stdio client (JSON-RPC 2.0)
- **Events:** text, reasoning, tool-call/result, completion, cancellation, error
- **Quality:** ASan/UBSan-clean, Clang + GCC, 24 tests

## Current limitations

0.x API — may change before 1.0. MCP client and Anthropic provider are new; validated against offline mock fixtures, not live endpoints.

## Repository layout

- `libpuci` — reusable core (agent loop, providers, tools, streaming HTTP, MCP)
- `puci-cli` — terminal client (thin libpuci consumer)
- `bindings/lua` + `lua/puci` + `plugin/puci.lua` — `puci.nvim` plugin (zero `-llua`)
- `examples/` — `embedded_editor_agent`, `poll_loop`, `bench_swarm`, and more
- `bench/bench_swarm` — 1000-agent density benchmark ([`docs/BENCHMARKS.md`](docs/BENCHMARKS.md))
- Design: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)

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

Optional switches (`-Dexamples=true`, `-Dlua=true`, `-Dbench=true`) and the full embedding guide: [`docs/EMBEDDING.md`](docs/EMBEDDING.md).

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
