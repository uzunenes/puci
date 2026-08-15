# PUCI TUI — Featherweight Terminal UI (v0.5.0)

Single dual-mode `puci` binary, pure POSIX ANSI (no ncurses), <100 KB TUI heap.

## Invocation

```
puci              → TUI if isatty(0)&&isatty(1), else REPL
puci --tui        → force TUI
puci --no-tui     → force REPL
puci run "prompt" → headless (pipe-safe)
PUCI_NO_TUI=1 puci → suppress
```

## Architecture

- `src/cli/tui/tui.c` (~400 LOC) + `tui.h` — alt-screen `ESC[?1049h/l`, raw termios, 60 Hz poll ring.
- Ring: `STDIN_FILENO` folded with `puci_session_get_fds()` / `next_timeout_ms()` → `poll()` timeout `min(16ms, session_timeouts)`.
- Views: header (version/swarm/RSS/PSS/context), 2×2 grid, gauge bars, diff overlay, sidebar, footer.
- Input: non-blocking `read(0)`, ESC parsing, `q/Ctrl-C` quit, `TAB` focus, `s` sidebar, `Enter` prompt.

## Views

1. **Swarm Grid (2×2)** — each pane = one `puci_session` on same host `poll()`. N=1 single pane, N>4 paginated. Proves `bench_swarm` density visually.
2. **Gauges** — `Context cur/budget` (via `puci_session_context_bytes()`), `RSS` from `/proc/self/status` (fallback PSS).
3. **Diff & Approval** — `puci_approval_cb` → overlay with `j/k` scroll, `y`/`n`, colors `+` green/`-` red via SGR.
4. **Sidebar (`s`)** — session list (`--resume`), instant switch.

## Poll Ring

```c
for(;;){
  fds[0]=STDIN; for each sess: get_fds(fds+n); timeout=min(next_timeout_ms,16);
  poll(fds,n,timeout);
  if(stdin POLLIN) handle_key();
  for each sess: process(fds+1); while(next_event) ring_push();
  if(dirty||approval) render_frame();
}
```

## Keys

`q` quit, `TAB` next pane, `s` sidebar, `Enter` prompt, `y/n` approval, `j/k` hunk.

## Embed Guarantee

`src/lib/` unchanged, no TUI dep. CLI only. Tested `isatty` fallback to REPL in CI.
