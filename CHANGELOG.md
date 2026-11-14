# Changelog

All notable changes to this project are documented here.

## [Unreleased]

### Fixed

- TUI: type to compose. While idle, any printable key now opens the composer and is typed as the first character, instead of being read as a one-letter shortcut. Forgetting to press Enter first made the opening letters of a message fire shortcuts (`s` sessions, `m` model, `q` quit). Sessions/model are reached with `/sessions` and `/model`; quit is `Ctrl+C`. Arrows/PgUp/PgDn/Home/End still scroll and `Tab` still switches panes. Covered by `test_tui_polish`.
- TUI did not follow window resizes: puci reads stdin itself and owns `SIGWINCH`, so termbox2's own resize path never ran and its size and cell buffers stayed at the start-up size (a window enlarged after launch kept the small frame). The `SIGWINCH` path now updates termbox's size and buffers and repaints. `test_tui_polish` grows 60x15 -> 120x30 and shrinks back.
- `make test` with an absolute `O=` looked for the CLI at the wrong path.

## [0.6.0] - 2026-11-14

**ABI 3** (`libpuci.so.3`): `puci_session_options` gained `workspace_root`; rebuild programs that embed libpuci.

### Added

- GitHub Actions CI (`.github/workflows/ci.yml`): dual-compiler (gcc + clang) build & test matrix, ASan + UBSan sanitizer job, and swarm density benchmark job.
- README badges (CI status, MIT license, version, C17) and a "SQLite of AI agent harnesses" tagline with a one-line value proposition.

### Added

- Plain `Makefile`: builds libpuci (`.a` + versioned `.so` with soname, `.dylib` on macOS) and the CLI with only a C compiler, make and libcurl, no Meson. `make test`, `make install` (bin, lib, `include/puci`, `puci.pc`, man pages), `make uninstall`, `make dist`; honours `PREFIX DESTDIR BINDIR LIBDIR INCLUDEDIR MANDIR PKGCONFIGDIR CC CFLAGS CPPFLAGS LDFLAGS`, `TUI=0`, `SHARED=0`. Warnings never fail a user build. Header dependencies tracked with `-MMD`. The CLI links libpuci statically so it runs from any prefix. `make deps` (`scripts/check-deps.sh`) prints the dependency command for the OS and never installs anything. Meson stays as the developer/CI build.
- Man pages: `puci(1)` and `libpuci(3)`.
- CI: a plain-make job on Linux and macOS (build with `-Werror`, `make test`, `TUI=0 SHARED=0`, install + embed via pkg-config shared and static, uninstall leaves nothing, `DESTDIR` staging). Releases are built with `make` and ship a source tarball.
- Release workflow (`.github/workflows/release.yml`): pushing a `v*` tag builds Linux x86_64/arm64 binaries (static libpuci, dynamic libcurl), runs the tests and attaches `.tar.gz` + `.sha256` to the GitHub Release.
- OpenRouter: mid-stream `{"error":{...}}` payloads (e.g. "Insufficient credits") and `delta.reasoning` are handled; HTTP 4xx/5xx errors now show the provider's message ("... is not a valid model ID") instead of a bare status code.

### Changed

- termbox2 is always taken from the vendored, pinned header. The Meson wrap that `git clone`d termbox2's moving `head` at configure time was removed, so builds are offline and reproducible.
- Installed `puci` carries an RPATH to its `libdir`, so per-user installs (`PREFIX=~/.local`) work without `ldconfig`/`LD_LIBRARY_PATH`. `puci.pc` lists `-lpuci` once and declares `Requires.private: libcurl`.
- Default OpenRouter model is now `z-ai/glm-5.3` (pinned, tool-capable; the old `anthropic/claude-3.5-sonnet` no longer exists on OpenRouter), and it is also applied when `PUCI_BASE_URL` points at OpenRouter with no model set. The TUI's offline model list and the Anthropic provider default were refreshed.

### Fixed (build)

- `-Dtui=disabled` / `TUI=0` builds failed to link: `main.c` called the TUI unconditionally. `tui.h` now provides stubs when the TUI is compiled out.

### Fixed (TUI readability)

- Hints, the status line and other secondary text were drawn bold-black (`ESC[1;30m`), i.e. black on dark terminals that don't brighten bold (GNOME Terminal/VTE, kitty, Alacritty defaults), so they were invisible; body text and borders were hard-coded white, invisible on light themes. The theme now uses the terminal's own foreground for text and a dimmed default foreground for secondary text, so it is readable on any palette. `test_tui_polish` guards against both.

### Added (TUI design rules)

- Light-terminal palette: the TUI detects the background (`PUCI_THEME=light|dark`, `COLORFGBG`, an OSC 11 query bounded at 200 ms and cut short by the DA1 reply, then the computer's light/dark setting via the freedesktop portal, GNOME `gsettings` or macOS `defaults`) and swaps yellow/cyan accents, which wash out on white, for blue/magenta.
- `NO_COLOR` support (no-color.org): no colour escapes at all; bold, dim, underline and reverse carry meaning.
- All colours moved into `src/cli/tui/theme.c` as semantic roles; `scripts/check_tui_tokens.sh` (`meson test tui_tokens`) rejects raw `TB_<colour>` names elsewhere and bold black anywhere. Rules documented in `docs/TUI.md`.

### Added (workspace)

- Workspace confinement: `puci_session_options.workspace_root` (library, opt-in) and always on in the CLI/TUI (the start directory, or `puci -C <dir>`). Reads inside it run without approval, reads outside always ask (refused when nobody can be asked), `write_file`/`edit_file` and `run_shell`'s `cwd` outside it are refused, even with `--yes`. Symlinks and `..` are resolved before the check. The model is told its workspace in the system prompt.
- `puci -C <dir>` / `--cwd <dir>`; the TUI shows the workspace in its empty state, and starting in `~` or `/` warns.
- `puci_session_workspace()` and `puci_session_message_count()`.

### Changed (files on disk)

- No `/tmp` fallbacks: without `HOME`, history and transcripts are simply not kept (previously `/tmp/history` and `/tmp/puci-sessions`, shared and world-writable).
- Sessions with no messages are no longer saved.
- System prompts no longer say "C coding agent".

### Security

- **Project MCP servers need trust.** An MCP server in a project's `.puci.toml` is a command puci starts at launch; a cloned repository could run code just because someone typed `puci` in it. Project-declared servers are now skipped with a notice unless `--trust-project-mcp` / `PUCI_TRUST_PROJECT_MCP=1`; servers in the user's own config are unaffected. Covered by `test_config`.
- The TUI now loads MCP servers (it previously ignored them; only `puci run` and the REPL did).

- Session transcripts and prompt history are now owner-only: `~/.local/share/puci` and `sessions/` are created (and tightened, if created by older versions) as `0700`, transcript and history files as `0600`. They were `0755`/`0664`, readable by other local users, although they contain tool output and everything typed.
- TUI test harnesses run with a temporary `HOME`/`XDG_*` and no provider env vars, so they no longer read the developer's `~/.config/puci` (API key) or write mock sessions and history into `~/.local/share/puci`.
- **Approval is now fail-closed.** Previously a session with no `approve_cb` (e.g. `puci run`, or any embedder that forgot to set one) executed `run_shell`, `write_file`, `edit_file`, MCP and custom tools with no approval at all, and the REPL auto-allowed every tool when stdin was not a TTY. Now, without a policy, only `read_file`/`grep`/`glob` run; everything else is denied. Unattended use is an explicit opt-in: `PUCI_APPROVAL_MODE_AUTO` (library) or `--yes` / `PUCI_AUTO_APPROVE=1` (CLI). `puci run` prompts on a TTY and denies when none is available.
- `puci_atomic_write_file` creates its temp file with `O_EXCL|O_NOFOLLOW` (no write-through of planted symlinks) and uses an atomic counter.
- Path guard matched any path segment *starting with* `build` (e.g. `builder.c`, `build_info.c`); it now matches `build` and `build-*` only.

### Fixed

- Debug/sanitizer builds with recent GCC no longer fail under `-Werror` on intentional `snprintf` truncation (`-Wno-format-truncation`).

- Clipboard: backend commands (`wl-copy`, `xclip`, `xsel`, `pbcopy`) now run with stdout/stderr redirected to `/dev/null`. `xclip` and `wl-copy` fork a daemon to serve the selection, which kept the host's stdout/stderr pipe open indefinitely (an embedding host or test runner reading puci's stdout would hang).
- `test_content` no longer depends on the host: the OSC 52 size-gate check now skips OS clipboard backends via the new `clip_set_test_no_backend()` hook. Previously it timed out on any machine with `xclip`/`wl-copy` installed, and wrote a 64 KiB payload to the real clipboard.
- `bench_swarm`: the per-session PSS gate now uses peak PSS per session instead of the post-creation delta, which is within shared-page PSS noise (sessions are idle then) and was often negative, so the gate could never fail. JSON output gains `per_session_peak_pss_kb`; `docs/benchmarks.json` regenerated.
- Clang build: added missing trailing newlines in `src/cli/tui/content.h`, `src/cli/tui/ttg.h`, `src/cli/tui/ttg.c`, and `tests/integration/test_approval_event.c` (`-Wnewline-eof` under `-Werror`).
- Clang build: `do_one_turn` in `test_approval_event.c` now takes a `puci_approval_decision` instead of `int` (`-Wsign-conversion`).
- `subprojects/termbox2.wrap` now declares `patch_directory = termbox2` so the bundled `meson.build` is applied automatically on a fresh checkout (previously required a manual copy, leaving the TUI silently disabled).

### Changed

- LICENSE copyright holder corrected from the project name to the author (`Enes Uzun`).
- Documentation version references synchronized to v0.5.0: `docs/SHOW_HN.md`, `docs/BENCHMARKS.md`, `docs/COMPARISON.md`, `docs/EMBEDDING.md`. Corrected the per-session figure in `SHOW_HN.md` to ~103 kB PSS (matching `BENCHMARKS.md`/`COMPARISON.md`) and the test count in `COMPARISON.md` (18 → 24).

## [0.5.0] - 2026-08-15

### Added

- Featherweight TUI: single dual-mode `puci` binary (`isatty` dispatch, `--tui`/`--no-tui`), pure POSIX ANSI state machine in `src/cli/tui/` (zero deps, <100 KB heap, 60 Hz poll ring folding `STDIN` + session fds).
- 2×2 Swarm Monitor (4 panes, paginated), live RSS/PSS + context budget gauges, interactive diff approval (`y/n`, `j/k`, colors), session sidebar (`s`), SIGWINCH resize.
- `docs/TUI.md` and `isatty` headless fallback to REPL.

## [0.4.0] - 2026-08-15

### Added

- Three-tier context budget compaction: head+tail preserve, verbose TOOL result truncation (`[Prior tool output truncated: N bytes...]`), duplicate dedup, env `PUCI_CONTEXT_BUDGET`, auto pre-flight before dispatch, API `puci_session_{set,get,context_bytes,compact}_context*` (ABI append, v2 stable).
- Empirical competitor matrix `docs/COMPARISON.md` (PUCI ~103 kB vs 30-150 MB, 1000 vs 20-40 agents, <2 ms vs 150-800 ms, caller-owned poll loop) with methodology/disclaimer and README summary table.
- `test_context_compaction`: 25-turn 5 KB dumps, 32 KB budget, continuity assertions.

### Changed

- Documentation pruned to 5 authoritative docs + `docs/archive/`; `README` v0.4.0 banner with comparison table.

## [0.3.0] - 2026-08-15

### Added

- Model Context Protocol (MCP) stdio JSON-RPC 2.0 client: launches external
  MCP servers, performs the initialize handshake, discovers tools via
  `tools/list`, and dispatches `tools/call`. Configuration via
  `[mcp_servers.<name>]` in `~/.config/puci/config.toml` / `.puci.toml`.
- Anthropic Messages API provider (`anthropic-version: 2023-06-01`) with
  `tool_use` round-trip translation and automatic provider detection.
- Local / OpenAI-compatible provider tuning for Ollama, vLLM, LM Studio, and
  OpenRouter endpoints.
- Embedder showcase: `examples/embedded_editor_agent.c`, a Lua C-module
  (`bindings/lua/puci.so`) for LuaJIT / Neovim, and
  `examples/benchmark_footprint.c` (< 5 MB RSS baseline).

### Changed

- Hardened the non-blocking async tool lifecycle and concurrent multi-agent
  poll-loop driving (tested via `test_async_tool_nonblocking`,
  `test_concurrent_two_agents`, `test_async_tool_cancel`).

## [0.1.0] - 2026-08-13

### Added

- C17 embeddable agent core with blocking (`puci_session_send`) and caller-owned poll-loop APIs.
- Non-blocking (async) tool execution lifecycle (`puci_tool_async_vtable` in `tool.h` and async `run_shell`) - new in 0.1.0 and lightly tested.
- Borrowed payloads for text, reasoning, tool calls, tool results, completion,
  cancellation, and errors.
- GCC and Clang strict builds, sanitizer tests, and libFuzzer harnesses.

### Limitations

- Async tool execution is newly introduced and not yet fully hardened across all platforms.
- Stage A host-fd/concurrent-agent/cancellation example work and Phase 3b are
  not included.
