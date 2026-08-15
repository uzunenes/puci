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

- `src/cli/tui/tui.c` (~700 LOC) + `tui.h` + `vendor/termbox2.h` — `tb_init/tb_present` double-buffer, `poll([STDIN,…session_fds])` master (250 ms idle).
- Ring: logical lines (`RING 800×4096`) + `ring_append_to_last` stream coalescing → word-wrap at render (`wrap_visual_count`).
- Dirty: key, `TOKEN_DELTA` append, `TURN_COMPLETE`, `SIGWINCH`, 2 Hz gauge (500 ms). No 60 Hz flicker (`\x1b[H` + `K`).

## Views

1. **Chat Flow (fixed)** — assistant stream coalesced into single logical line, word-wrapped to pane width, `↑/↓` scroll, `PgUp/Dn`, `g` top / `G` bottom, `%` indicator.
2. **Swarm Grid (2×2)** — each pane = one `puci_session` on same host `poll()`. N=1 single pane, N>4 paginated.
3. **Gauges** — `Context cur/budget` (`puci_session_context_bytes()`), `RSS/PSS` live 2 Hz.
4. **Diff & Approval** — `puci_approval_cb` → `y`/`n` overlay, `j/k` hunk, `+` green/`-` red.
5. **Session Picker (`s`)** — `opendir(~/.local/share/puci/sessions)` real list, `↑/↓` + `Enter` → `puci_cli_load_session`, title update, autosave on `TURN_COMPLETE`.
6. **Model Picker (`m`)** — `↑/↓` + `Enter` over 12-model curated list (`deepseek-chat` cheap, `claude-3.5-sonnet`, `gpt-4o-mini`…), sets `PUCI_MODEL`.

## Poll Ring

```c
for(;;){
  fds[0]=STDIN; for each sess: get_fds(fds+n); timeout=min(session_to, gauge_due, 250);
  poll(fds,n,timeout);
  if(stdin POLLIN) handle_key(); // word-wise, cursor, ESC arrows
  for each sess: process(); while(next_event){
    TOKEN_DELTA → pane_stream_append (coalesce, not ring_push)
    TOOL_* / TURN_COMPLETE → ring_push_typed + scroll_off=0
  }
  if(dirty) tui_render_frame();
}
```

## Keys

`Enter` prompt/send, `Esc` cancel, `↑/↓` or `j/k` scroll (or picker), `PgUp/Dn`, `g`/`G` top/bottom, `←/→` cursor, `Ctrl-U` clear line,
`TAB` next pane, `s` sessions, `m` model, `q` quit, `y/n` approval, `?` help.

## Embed Guarantee

`src/lib/` unchanged, no TUI dep. CLI only. Tested `isatty` fallback to REPL in CI.
