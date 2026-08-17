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
- Idle CPU: measured **0.00 %**/0 repaints/0 wakeups on the release build (defect 10, §6b). An earlier ~1.6 % figure was dominated by the harness pty drain; a static screen does not repaint.

## 6. PTY snapshots

`docs/snapshots/puci_<W>x<H>.txt` for 60x15, 80x24, 120x30, 200x50 (after one
`hello world` turn). Each shows header, conversation with scrollbar, composer,
and footer at that size.

## 6b. Final visual-correctness polish pass (defect-by-defect root cause)

Fix commits: `de53e36`, `c06e9e2`, `28b83d5`, `ddee86b`.

| # | Defect | Root cause | Where (TUI/harness) | Resolution |
|---|--------|-----------|---------------------|------------|
| 1 | TTG sentinels leak into assistant content | instrumentation envelope (U+2063 + timestamp + U+2063) reached the ring | TUI | `ttg.c` strips the envelope at the stream boundary (commit `1be687f`); `test_ttg` covers every split position, multiple/malformed/incomplete envelopes, ordinary U+2063 |
| 2 | Scrollbar corrupts message text | content wrapped at full width while the thumb column overlapped the last cell | TUI | gutter column reserved only when a pane is scrollable; content wraps strictly inside it; width cache invalidated on visibility change |
| 3 | Composer draws onto the border row | message content emitted onto the bottom-border row | TUI | distinct top/content/bottom rows; 1–8 content rows when active; compact borderless mode below 5 rows |
| 4 | "N new ↓" badge while already at bottom | follow state not tied to visual bottom | TUI | follow enables at turn start at bottom; scrolling up disables; returning re-enables + clears unread; resize/turn-completion never fabricate unread |
| 5 | Raw [think] reasoning shown | REASONING alts rendered verbatim | TUI | raw reasoning hidden by default; compact "▸ Thought for Ns" notice instead; deltas no longer consume ring lines |
| 6 | Role semantics incomplete | assistant heading repeated per token, prompt glyph duplicated | TUI | "PUCI · model" rendered once per subtitle block; YOU heading shown; prompt glyph dropped when YOU is present |
| 7 | Transcript clutter | startup tutorial + "— done —" in history | TUI | removed from conversation; ready state is footer/status; model changes are one transient notice |
| 8 | Footer truncated | single action string byte-truncated | TUI | width-adaptive whole-action lists (wide/medium/narrow/composer/approval) |
| 9 | Missing/overwritten right border in snapshots | termbox repaints a full frame after `\x1b[2J` but re-emits only changed cells; the emulator blanked the grid on `\x1b[2J`, erasing unchanged border cells | **harness/snapshot emulator** | emulator now keeps unchanged cells across `\x1b[2J` (persistent screen); real grid was already correct |
| 10 | Idle poll/wakeups | gauge timer forced periodic repaints | TUI | `poll(-1)` (indefinite block) when no deadline; active deadlines (streaming/session/model-fetch/test-hook) still wake on a bounded tick |
| 11 | Model-fetch memory ordering | informal atomic note only | TUI | documented happens-before contract with explicit `memory_order_release`/`acquire` (commit `ddee86b`) |

### Release-build idle measurement (defect 10)

Release build, no sanitizer (`buildtype=release`, `b_sanitize=none`), 80x24, 30 s:

| Metric | Value |
|--------|-------|
| process CPU (ps cputime delta) | 0.00 % |
| full clear repaints (`\x1b[2J`) | 0 |
| idle-window bytes emitted (after initial paint drained) | 0 |
| select wakeups (events) | 0 |

A static, freshly-painted screen repaints nothing and blocks on `poll(-1)`.
The only periodic wakeups are intentional and bounded: the streaming/context-gauge
`poll_to` while a turn is active, the far-base model-fetch tick, session deadline
ticks, and the `PUCI_TUI_EXIT_AFTER_MS` **test hook** (250 ms, used only by tests).
Production with no active deadline runs `poll(-1)` => 0 wakeups.

### Memory-ordering deliverable (defect 11)

TSan is not available (needs a TSan-instrumented libc/termbox over a pty), so the
contract is documented and machine-checkable via explicit memory-order ops in
`src/cli/tui/tui.c`:
- Producer fully populates `g_live[]`, then publishes `g_live_cnt` and
  `g_live_done` with `memory_order_release`.
- Consumer `pump_models_fetch()` accepts `g_live_done` with
  `memory_order_acquire`, establishing synchronizes-with, before reading the
  buffer and count.
- A new generation re-arms `done==0` only after the previous result was pumped,
  so an older fetch never overwrites a newer one and the buffer is never
  mutated while the UI reads it.
- The buffer is static and never freed, so teardown cannot free storage while a
  detached worker may still access it.
- Running under ASan/UBSan: the whole suite (22 tests incl. the ~100 s stress and
  `test_tui_polish`) is clean (0 sanitizer reports).

## 7. Keybindings

`docs/KEYBINDINGS.md` matches the actual dispatch in `src/cli/tui/tui.c`.

## 8. Commits (production-readiness pass)

- `fix(ui): production-readiness audit correctness fixes`
- `test(tui): add PTY audit flows + make harness binary overridable`
- `fix(tui): strip TTG instrumentation envelopes at the stream boundary`
- (this doc/snapshots)

## 8b. Commits (final visual-correctness & polish pass, in order)

- `refactor(tui): explicit model-fetch memory-ordering contract` (`ddee86b`)
- `fix(ui): strip TTG instrumentation envelopes ...` (`1be687f`, earlier)
- `fix(ui): conversation renderer correctness and polish` (`de53e36`)
- `test(ui): semantic grid assertions + persistent emulator + readable fake flow` (`c06e9e2`)
- `docs(tui): regenerate snapshot acceptance artifacts` (`28b83d5`)
- `docs(tui): audit memory ordering + idle metrics` (this commit)

## 8c. Commits (owned-content + bounded Markdown/highlight + safe clipboard + paste)

- `feat(tui): owned content model + bounded Markdown parser` (`adff700`)
- `feat(tui): deterministic semantic syntax highlighter` (`3c48be5`)
- `feat(tui): safe clipboard with refusable OSC 52 size policy` (`49a9596`)
- `feat(tui): capture bracketed paste into its own collapsed block` (`c128200`)
- `feat(tui): syntax-highlight code fences and Markdown in transcripts` (`0c8ef53`)

### Paste + composer ownership

A bracketed paste is captured byte-for-byte into an owned buffer (CRLF->LF,
others preserved), presented in the composer as a collapsed
`[pasted N lines · X KiB]` chip, and on Enter the exact draft (typed text +
newline + paste bytes) is sent to `puci_session_begin_turn`. Pastes past the
4 MiB budget are refused, never truncated. The module-level guarantee is
asserted in `tests/unit/test_content.c` (`t_compose`): the full paste body
appears in `ctx_composer_serialize`'s output with a matching hash. In-unit
clipboard assertions cover exact capture incl. UTF-8 + embedded NUL + CRLF,
oversized-refusal, and empty copy rejection.

### Syntax highlighting

`tb_print_hl` runs the semantic lexer over recognized Markdown code-fence rows
and maps token classes to theme colors; `md_fence_lang` normalizes the fence
tag across the contiguous assistant block. Bounded to HL_MAX_SPANS per row.
Non-fence content uses the existing inline renderer (headings/bold/inline
code/lists/quotes/links).

## Retained boundaries (deliberate, not silent bugs)

- **Raw Markdown is not re-owned per assistant message in the transcript.**
  The Ring remains a wrapped display grid; `content.c`'s raw-ownership /
  exact-serialization guarantee is exercised at the composer/send/copy layer
  (the byte-exact provider payload) and documented in the module, but the
  transcript does not store per-message `raw_source` objects. Copying the
  *raw* assistant markdown (vs. the sent draft) and versioned per-message
  persistence would require reworking the Ring into a sampled message model,
  which is out of scope for this pass.
- **Session save/load of user/paste content** continues to use the library
  `puci_cli_save_session` serializer (provider messages), not the new content
  model. Backward-compatible; semantic content-version tags are not yet used.
- **Tool/file output**: streamed tool output renders as RL_TOOL records (as
  before); dedicated file-PATH tap/copy affordances are not added.

These boundaries keep the current fixed-size ring and the existing 23-test
suite (incl. semantic grid assertions) stable and sanitizer-clean.
