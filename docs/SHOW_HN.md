# Show HN: PUCI — The SQLite of AI Agent Harnesses (C17, 1000 agents in 106 MB)

We built PUCI because every agent harness we tried needed Python or Node just to call an API and run a shell command. That doesn't embed into Neovim, Unreal, or a 1,000-way evaluation swarm.

**PUCI is a minimal C17 core library (libpuci) + pollable event loop:**

- **176 kB session delta, sub-ms cold start, zero runtime bloat.** Pure C17, libcurl only.
- **Caller-owned `poll()`/`epoll` loop:** `begin_turn / get_fds / next_timeout_ms / process / next_event` — the host aggregates fds across N sessions, no threads, no global state.
- **Non-blocking tools:** `run_shell` is async (`fork+execvp`, `setpgid`, `SIGTERM→SIGKILL`, non-blocking pipe drain) so a 10s command never stalls the other 999 agents.
- **1000 agents, one thread, 106 MB RSS** on a single loop against a mock SSE server (see `bench/bench_swarm`, `docs/BENCHMARKS.md`). ~103 kB PSS per session. Python would need ~45 GB.
- **Neovim-native:** `puci.nvim` (`lua/puci/init.lua`, `plugin/puci.lua`) — `require('puci').create_session()` with `vim.diff` preview and `vim.ui.select` approval, `puci.so` without `-llua` (Neovim provides symbols). Open `:Puci` and drive the same poll loop from `vim.loop`.

```lua
local puci = require("puci")
local s = puci.create_session({model="gpt-4o-mini"})
s:send("summarize the buffer", {
  on_delta = function(t) io.write(t) end,
  on_done = function() print("\n-- done") end,
})
-- or poll manually with session:poll_step() / session:cancel()
```

**Quickstart:**

```sh
meson setup build && ninja -C build && meson test -C build
PUCI_API_KEY=sk-... ./build/examples/poll_loop
# swarm density
meson setup build-bench -Dbench=true && ./build-bench/bench/bench_swarm 1000
```

**Status:** v0.3.0, MIT, ABI 2 stable, dual-compiler `-Wconversion -Werror`, ASan/UBSan + Valgrind clean (`--leak-check=full --show-leak-kinds=definite,indirect --error-exitcode=99`), MCP stdio + Anthropic + OpenAI-compatible (Ollama/vLLM/OpenRouter).

Repo: `github.com/uzunenes/puci` — `docs/ARCHITECTURE.md`, `docs/EMBEDDING.md`, `bench/bench_swarm.c`.

We'd love your hardest embedding use-case — game engine, editor, or swarm — and your `bench_swarm` numbers on your hardware.
