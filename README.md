# puci

PUCI is a minimal AI coding-agent harness written in portable C17. It is for
embedders that want a small provider/tool core and a caller-owned poll loop.

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
- `examples/` — embedder examples including `embedded_editor_agent` and
  `benchmark_footprint`
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

## Multi-provider configuration

PUCI auto-detects the provider from the endpoint URL or the model name:

- `base_url` containing `anthropic` or a model beginning with `claude` selects
  the Anthropic Messages API provider (`anthropic-version: 2023-06-01`).
- Everything else uses the OpenAI-compatible provider, which also covers local
  endpoints: Ollama (`http://127.0.0.1:11434/v1`), vLLM, LM Studio, and
  OpenRouter (set `PUCI_BASE_URL` and `PUCI_API_KEY` accordingly).

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
