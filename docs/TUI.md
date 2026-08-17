# PUCI TUI — Conversation-First Terminal UI (v0.5)

A single conversation-first screen with a reliable multiline composer,
streaming render, predictable scrolling, event-driven (non-blocking)
approvals, and simple session/model pickers. Built on termbox2
(`vendor/termbox2.h`); implementation in `src/cli/tui/tui.c`.

## Invocation

```
puci              -> TUI if isatty(0) && isatty(1), else REPL
puci --tui        -> force TUI
puci --no-tui     -> force REPL
puci run "prompt" -> headless (pipe-safe)
PUCI_NO_TUI=1 puci -> suppress
```

## Architecture

- `poll()` master loop over `[STDIN, ... session_fds]`. The fd array is sized
  dynamically from `puci_session_get_fds()` so a pane FD is never silently
  dropped. A capped 250 ms idle timeout lets signals and new input arrive.
- **Idle CPU**: rendering is only forced on a cheap 100 ms tick while
  something is actually animating (streaming, an open dialog, or a pending
  model fetch). A fully static idle screen does not repaint on a timer, so
  idle CPU stays near zero.
- **Ring**: `RING_LINES x RING_COLS` logical lines with a per-line role
  (user/assistant/tool/system/error). `ring_append_to_last()` spills a full
  line into new ring rows instead of silently dropping tail bytes.
- **UTF-8**: the composer collects full code points (including sequences split
  across stdin reads) and moves/backspaces by code point, so multi-byte text
  is never cut. Rendering advances columns by display width via
  `utf8_seq_width()` and only emits whole code points, so wrapping never
  splits a character.
- Consecutive blank lines are preserved (a pending-newline counter
  materializes them). `wrap_visual_count("") == 1` matches the render slice,
  so blank lines are never dropped and scroll math stays consistent.

## Views

1. **Conversation (fixed, primary)** - one full-width pane.
2. **Multiline composer** - grows 1 -> 8 rows, then scrolls.
3. **Session picker (`s`)** - list + filter, Enter loads exactly one.
4. **Model picker (`m`)** - static + async-fetched list; the background
   fetch never blocks the loop.
5. **Approval dialog** - event-driven `puci_session_resolve_approval()`
   asynchronous; the main `poll()` keeps serving other FDs, resize, timers
   and other sessions while the dialog is open.

Responsive: optional sidebar on wide terminals; layout clamps at small sizes
with a clear "Window too small" screen below the supported minimum.

## Terminal lifecycle

All exit paths (`q`, `Ctrl+C` with nothing running, EOF, `SIGTERM`, `SIGHUP`,
and startup/fatal errors) run `tui_teardown()` -> `tb_shutdown()`, which
restores the alternate screen, cursor visibility, and input/output modes.
`SIGTERM`/`SIGHUP` are trapped to that clean path so a kill leaves the shell
unbroken.

## Keybindings

See `docs/KEYBINDINGS.md` for the full, implementation-matched reference
(normal mode, composer, both pickers, approval dialog, and slash commands).
