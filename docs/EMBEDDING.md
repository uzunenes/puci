# Embedding libpuci

PUCI exposes a caller-owned, non-blocking session driver. The host calls
`puci_session_begin_turn()`, obtains the current fds and timeout, waits in its
own `poll()` loop, calls `puci_session_process()`, and drains events with
`puci_session_next_event()`.

```c
puci_session_begin_turn(session, prompt);
for (;;) {
	int timeout_ms = -1;
	struct pollfd fds[64];
	size_t count = 0;
	puci_session_next_timeout_ms(session, &timeout_ms);
	puci_session_get_fds(session, fds, 64, &count);
	poll(fds, (nfds_t)count, timeout_ms < 0 ? 1000 : timeout_ms);
	puci_session_process(session, fds, count);

	puci_event event;
	while (puci_session_next_event(session, &event)) {
		/* Copy borrowed payloads before the next process/next_event call. */
	}
}
```

## FD and timeout contract

The returned descriptor set is level-triggered. The set may change after every
`puci_session_process()` call, so call `puci_session_get_fds()` again before
each wait. A capacity query uses `puci_session_get_fds(session, NULL, 0,
&count)`. The cancellation fd is included while a turn is active.

Array sizing: `puci_session_get_fds(session, fds, cap, &count)` writes up to
`cap` entries and sets `count` to the actual count written (or the required
count if `cap == 0`). When capacity is insufficient (`cap < required`), the
cancellation descriptor is always prioritized and written to `fds[0]` (when
`cap >= 1`), remaining slots are filled with sub-operation descriptors up to
`cap`, and the function returns `PUCI_ERR_BUFFER_TOO_SMALL`. Embedders should
allocate an array sized to their expected host and session fds (e.g. 64 or 128)
or query capacity dynamically with `cap == 0`.

`puci_session_next_timeout_ms()` is an upper bound for the host wait. When
`curl_multi_timeout()` reports zero with pollable fds, PUCI returns an
approximately 1000 ms floor to avoid a busy-spin. A zero timeout with no fds
means the state machine has immediate work and the host should call
`puci_session_process()` promptly.

## Thread safety and ownership

Everything is single-thread-per-session and must not overlap with another
operation on that session. Context configuration must finish before sessions
are created. Returned handles and descriptors remain library-owned. Strings
returned in errors, tools, and `puci_event` are borrowed and must be copied if
retained. Strings returned by `puci_compute_diff()` are heap-owned and must be
released with `puci_diff_free()`.

`puci_session_cancel()` is the exception: it is safe from another thread and
from an asynchronous signal handler. It performs only cancellation signalling;
the caller must not destroy the session from the signal handler.

Event payload pointers are session-owned and valid until the next
`puci_session_next_event()` or `puci_session_process()` call on that session.

Set `puci_session_options.max_tool_iterations` to bound tool dispatch per turn;
zero selects the default of 32. A bounded value is recommended for automated
audit or batch agents.

## Neovim (puci.nvim)

Zero-dependency plugin: `lua/puci/init.lua` + `plugin/puci.lua` + `bindings/lua/puci.so`.

```lua
-- init.lua (lazy.nvim)
{ "uzunenes/puci", config = function() require("puci").open() end }
-- :Puci [model]  or  :PuciOpen
local s = require("puci").create_session({model="gpt-4o-mini"})
s:send("fix the buffer", {on_delta=function(t) io.write(t) end})
s:register_tool("my_tool","desc", function(args_json) return '{"ok":true}' end)
s:write_buffer(0, new_text, true) -- vim.diff preview, vim.ui.select approval
```

Build `puci.so` without linking `-llua` (Neovim provides symbols):

```sh
cc -fPIC -shared bindings/lua/puci_lua.c -o puci.so $(pkg-config --cflags --libs puci)
nvim --headless -u NONE -c "lua package.cpath=package.cpath..';./?.so'; require('puci')" -c "q"
```

## OpenRouter BYOK & .bashrc (GitHub Copilot CLI pattern)

Recommended `~/.bashrc` for OpenRouter Bring-Your-Own-Key:

```bash
# GitHub Copilot CLI / OpenRouter BYOK for PUCI
export OPENROUTER_API_KEY="sk-or-v1-..."
export PUCI_API_KEY="$OPENROUTER_API_KEY"
export PUCI_BASE_URL="https://openrouter.ai/api/v1"
export PUCI_MODEL="anthropic/claude-3.5-sonnet"
```

PUCI resolves `OPENROUTER_API_KEY` as a first-class fallback when `PUCI_API_KEY` is unset, defaulting `base_url` to `https://openrouter.ai/api/v1` and `model` to `anthropic/claude-3.5-sonnet`. Template global config at `~/.config/puci/config.toml`:

```toml
base_url = "https://openrouter.ai/api/v1"
model = "anthropic/claude-3.5-sonnet"
context_budget_bytes = 131072
tool_timeout_ms = 60000
```

## 1000-agent density

`bench/bench_swarm` drives N sessions on one `poll()` loop against a mock SSE server.
`docs/BENCHMARKS.md` reports 1000 sessions: 106 MB RSS, 104 MB PSS (~103 kB/session), 685 turns/s.

## Current limitations

- Python bindings and PyPI publication are out of scope for the 0.x track.
- Phase 3b features (tree-sitter search, MCP SSE transport) are staged post-1.0.
