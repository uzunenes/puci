# PUCI TUI — Keybindings (matches `src/cli/tui/tui.c`)

All bindings below are taken from the actual key-dispatch in the event loop
(`puci_tui_run`). The user is never in "two modes" at once; the active context
is one of: normal (conversation view), composer, a picker, or the approval
dialog.

## Normal mode (conversation, idle or streaming)

| Key | Action |
|-----|--------|
| `Enter` | Open the composer |
| `q` | Quit (restores the terminal) |
| `Ctrl+C` | Cancel an in-flight turn; if nothing is streaming, quit |
| `↑` / `k` | Scroll conversation up (numeric step 2 / 1) |
| `↓` / `j` | Scroll conversation down (numeric step 2 / 1) |
| `PageUp` | Scroll up 10 lines |
| `PageDown` | Scroll down 10 lines |
| `Home` / `g` | Scroll to top |
| `End` / `G` | Scroll to bottom (return to follow mode) |
| `s` | Open the session picker (sidebar / overlay) |
| `m` | Open the model picker |
| `Tab` | Cycle focus between conversation panes (single-pane default) |
| `?` | Show a key summary in the conversation |

Scrolling is relative to the bottom. `scroll_off = 0` is the normal
follow-the-stream position; increasing it scrolls up (older content).

## Composer

The composer is opened with `Enter`. It holds a single logical line (which
may contain `\n`), so `g_input_len` counts bytes and `g_input_curs` is a byte
offset. Multiline text and pasted content stay as text.

| Key | Action |
|-----|--------|
| `Enter` | Send the draft |
| `Ctrl+J` | Insert a newline **without sending** |
| `Esc` | Close the composer, keep the draft (unsent) |
| `Ctrl+C` | Clear the pending draft (and any pasted block) and close the composer |
| `Ctrl+U` | Clear the whole draft (and any pasted block) |
| `Backspace` / `Delete` | Delete the whole code-point *before* the cursor (never splits a UTF-8 sequence) |
| `←` / `→` | Move cursor one UTF-8 code point |
| `↑` / `↓` | Previous / next entry in prompt history (`~/.local/share/puci/history`) |
| `Ctrl+Y` | Copy the selected session/model id to the clipboard (pickers) |

Bracketed paste: a bracketed paste is captured byte-for-byte (CRLF collapsed
to LF) into a collapsed block shown as `[pasted N lines · X KiB]` on its own
composer row — it never floods the preview and never truncates the typed
text. On `Enter` the exact draft (typed text + newline + paste bytes) is sent
to the provider. A paste that exceeds the 4 MiB budget is refused and
reported; it is never silently cut.

Composer growth: it expands from 1 up to 8 visible rows and then scrolls its
content; the draft survives opening/closing a picker (it is restored when the
composer reopens).

## Session picker (`s`)

| Key | Action |
|-----|--------|
| `↑` / `↓` | Move selection (viewport-clamped) |
| `Enter` | Load exactly one session |
| `Esc` / `q` (empty filter) | Close without changing the session |
| `Ctrl+Y` | Copy the selected session id |

The `current session is shown; typing filters the list; `(no sessions)` /
`(no match)` is shown when empty.

## Model picker (`m`)

| Key | Action |
|-----|--------|
| `↑` / `↓` | Move selection |
| `Enter` | Apply the selected model to the session |
| `Esc` | Close without changing the model |
| `Ctrl+Y` | Copy the selected model id |

A background fetch runs on open; the result merges into the list when ready
without blocking the TUI, and the selection is clamped to stay onscreen.

## Approval dialog

Shown while a tool call awaits approval. `Deny` is the default policy.

| Key | Action |
|-----|--------|
| `y` | Allow once |
| `A` | Allow always (per-session, exact tool name; not persisted across restart) |
| `n` / `q` | Deny |
| `Esc` | Deny (safe default) |
| `Ctrl+C` | Cancel → deny |
| `d` | Deny with a reason (feedback mode) |
| `j` / `k` | Scroll the diff view within the dialog |

A resolved approval cannot be resolved again, and a stale/second request is
auto-denied.

## Slash commands

Typed in the composer and sent with `Enter`:

`/clear`, `/help` (`/?`), `/model [name]`, `/sessions` (`/s`),
`/new`, `/compact`, `/doctor`, `/plan`

## Exit paths

`q`, `Ctrl+C` (no streaming), EOF, `SIGTERM`, and `SIGHUP` all restore the
alternate screen, cursor visibility, and text modes before exiting.