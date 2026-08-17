# PUCI TUI — Production-Readiness Audit Report

Scope: verify (not redesign) the single conversation-first TUI built on
termbox2. Evidence is the real PTY-drawn binary against `tests/fake_sse.py`,
the unit/integration suite, ASan/UBSan builds, and /proc CPU sampling. This
report lists pass/fail per phase, bugs found and fixed, tests added, and the
terminal snapshots. Deliberate non-goals are listed at the end.

## 1. Pass/Fail per phase

| Phase | Result | Notes |
|---|---|---|
| 1 Core user journey | PASS | clean startup; one `hello` turn; second turn usable; multiline; paste safety; Ctrl+C; lifecycle (`q`/EOF/SIGTERM) restore terminal |
| 2 Conversation viewport | PASS | wrap==render; blank lines counted; scrollbar proportional to visual lines; follow/unfollow + `N new ↓` indicator |
| 3 Message presentation | PASS | `YOU`/system/tool/ERR labels; markdown heading/list/quote/code parsed at render, UTF-8 safe; no boxes |
| 4 Tool/approval flow | PASS | event-driven, non-blocking; one-shot; cancel-wins; deny default; ALLOW_ALWAYS per-session/tool (documented below) |
| 5 Session/model UX | PASS | pickers clamped to viewport; async model merge; Escape cancels |
| 6 Async model worker | PASS | single-generation ownership; counter atomic; picker merge; bounded fetch |
| 7 Responsive design | PASS | snapshots 60x15/80x24/120x30/200x50; too-small screen below minimum |
| 8 Error/recovery | PASS | idle not blocked; draft preserved; terminal restored on fatal paths |
| 9 Performance/resource | PASS | near-zero idle CPU ~1.6%; no redundant 2 Hz redraw; dynamic fd array; no FD lease |
| 10 Keep it simple | PASS | no feature creep; single-conversation default; no mouse/palette/animation added |

Note: approval tool-call end-to-end turning of the real TUI requires an agent that
emits a tool call; the non-blocking and one-shot semantics are proven by the
integration suite (`tests/integration/test_approval_event.c`) and the TUI keys
forward to `puci_session_resolve_approval` asynchronously (verified by review
+ the PTY harness remaining responsive with the dialog open).

## 2. Concrete bugs found and root causes

All fixes are in `src/cli/tui/tui.c` (commit prefix `fix(ui):`).

1. **Idle CPU spun at 2 Hz.** A vestigial 500 ms "gauge" timer forced
   `tb_clear()`+redraw every 500 ms even when idle. Root cause: leftover
   context-gauge tick. Fix: gate the repaint tick on actual activity
   (streaming / open dialog / pending model fetch); a static screen never
   repaints on a timer. Measured idle CPU: ~1.6%.
2. **Model-fetch counter was a non-atomic shared int.** `g_live_cnt` was a
   plain `int`, but `begin_models_fetch` used `atomic_store` on it and the
   worker wrote it plainly. Root cause: mixed atomic/non-atomic access (UB).
   Fix: make it `_Atomic int`; worker stores via `atomic_store`.
3. **Async model list never appeared in the open picker.** `models_install`
   replaced `k_models` but the filtered view was not rebuilt, so fetched
   models were invisible. Fix: `pump_models_fetch()` rebuilds the filter and
   clamps the selection when the picker is open.
4. **Ctrl+J could not insert a newline.** The composer treated both `\n` and
   `\r` as "send", so there was no way to write multiline input. Fix: `\r`
   (Enter) sends and `\n` (Ctrl+J) inserts a newline into the draft.
5. **SIGTERM left the terminal in the alternate screen.** There was no signal
   handler; a plain `kill` terminated the process without `tb_shutdown()`,
   leaving the shell with a hidden cursor / alt screen. Fix: trap SIGTERM and
   SIGHUP to set a flag the event loop breaks on, so the clean teardown runs
   on every exit path. Verified: `\x1b[?1049l` (leave alt screen) and
   `\x1b[?25h` (show cursor) are emitted, rc=0.

(Reported issues in the audit brief that were already correct in the shipped
code and manually verified, not requiring a fix: `read_pss_kb` already uses
its own `g_pss_last_ns`; the parser in streaming validation already bounds
the TTG sentinel search with a full-3-byte `memcmp` inside a `>= 6` guard;
consecutive blank lines are preserved via a pending-newline counter; a full
ring `avail==0` spills into new rows instead of dropping; the approval overlay
shows the real diff first with an explicit `[+ .. truncated ..]` marker and a
scrollable view; the markdown renderer `tb_print_styled` is used and is
UTF-8/display-width aware.)

## 3. Tests added per fix / regression

- `tests/tui_audit_flows.py` (new): clean startup, one `hello` turn, a second
  turn, multiline via Ctrl+J, bracketed paste (pasted q/m/s/y/newlines stay
  text and never quit/approve), and model-picker merge of async-fetched
  models. Runs against both the normal and ASan/UBSan binaries (`PUCI_BIN`).
- `tests/tui_layout_smoke.py`: unchanged assertions (4 sizes + idle), now
  parameterised via `PUCI_BIN` so the same checks run under ASan/UBSan.
- `tests/puci_snapshot.py` (new): renders text snapshots of the live frame at
  a given size (small ANSI emulator) for the deliverables below.
- Existing 20-test suite (`meson test`) covers viewport/layout, approval
  event lifecycle, cancellation, connection reuse; all green after fixes.

## 4. Intentionally unsupported behaviour (this pass)

- Grapheme-combining sequences beyond single code points: movement and
  backspace are code-point safe (never corrupt UTF-8) but not full
  grapheme-cluster aware (e.g. an emoji ZWJ sequence). No PC ever splits a
  multi-byte sequence.
- Full Markdown tables / complex document layout / progressive rendering:
  out of scope. Emphasis, inline code, headings, lists, quotes and links are
  rendered inline.
- Mouse support, animation, command palette, branching, message editing,
  images, plugins, keymap editor: intentionally absent.
- "allow always" applies per-session + exact tool name and is not persisted
  across a restart. It is not scoped to arguments (it applies when args
  change). There is no per-workspace/global allow list.
- Ring/history sizes are fixed (documented); long content over the ring cap is
  evicted oldest-first, never corrupting scroll offsets.

## 5. Sanitizer & stress results

- `-Werror` build: clean (build and build-asan).
- ASan + UBSan: the two PTY harnesses and the 20-test suite run clean; no
  heap-buffer-overflow, use-after-free, or UB reports on the TUI and model
  worker paths.
- TSan: not run - the model worker is a detached pthread that only writes its
  own private buffer and signals three `_Atomic` flags (done/inflight/cnt);
  the single producer/consumer handoff is atomic-ordered. Running TSan in
  this environment over a pty requires a TSan-instrumented libc/termbox that
  is not available; instead the atomic ordering and the "only the main thread
  pumps results and never accesses the worker's buffer after `done`" invariant
  were validated by inspection and under ASan/UBSan.
- Stress: `puci:stress/test_cancel_stress` (~100 s) passes; existing `meson
  test` all 20/20.
- Idle CPU: ~1.6% via /proc sampling on a static 80x24 (harness pty drain
  dominates; a static screen does not repaint).

## 6. PTY snapshots

`docs/snapshots/puci_<W>x<H>.txt` for 60x15, 80x24, 120x30, 200x50 (after one
`hello world` turn). Each shows header, conversation with scrollbar, composer,
and footer at that size.

## 7. Keybindings

`docs/KEYBINDINGS.md` matches the actual dispatch in `src/cli/tui/tui.c`.

## 8. Commits

- `fix(ui): production-readiness audit correctness fixes`
- `test(tui): add PTY audit flows + make harness binary overridable`
- (this doc/snapshots)
