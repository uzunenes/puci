# PUCI - Daily-Driver Gap Assessment

This document assesses what stands between today's PUCI CLI (v0.1.0) and a tool
a developer can comfortably use as their daily coding agent harness instead of
external commercial tools.

---

## 1. Configuration and Credentials

### Current State
- **Source of configuration**: Environment variables only (`PUCI_BASE_URL`,
  `PUCI_API_KEY`, `PUCI_MODEL`, `PUCI_CA_BUNDLE`, `PUCI_EXTRA_BODY`,
  `PUCI_TLS_VERIFY`, `PUCI_STREAM_IDLE_TIMEOUT_SECS`) parsed in
  `src/cli/cmd/config.c:32-78` and `src/cli/cmd/run.c:70-108`.
- **Config file**: None. `src/lib/config/config.c` has internal structs, but
  neither `~/.puci/config.toml` nor `.puci.toml` are parsed by the CLI.
- **Missing / invalid credentials**: When `PUCI_API_KEY` is not set, PUCI
  passes `NULL` or empty key to the OpenAI provider. The upstream endpoint
  returns HTTP 401, which is mapped to `PUCI_ERR_UNAUTHORIZED` (status code 8)
  in `src/lib/net/http.c:480-484`. In the REPL (`src/cli/repl/repl.c:498-506`),
  it prints `puci: unauthorized` with minimal explanation.
- **Repository hygiene**: Secrets and keys are excluded from git and redacted
  in memory via `puci_secret_add()` and `redact_in_place()` in
  `src/lib/core/context.c:19-67`.

### What is Missing
1. Standard config file hierarchy: global (`~/.config/puci/config.toml`) and
   per-project (`.puci/config.toml` or `puci.toml` with `.gitignore` guard).
   *Constraint*: API keys and sensitive endpoint hostnames must never live in a
   project-local file. Credentials must come from `~/.config/puci/` or the
   environment only. If project-local override is supported, credential fields
   must be rejected there with only non-sensitive fields (e.g. model, timeout)
   permitted.
2. Profiles / named configurations (e.g., `openai`, `anthropic`, `local-ollama`).
3. Explicit startup check with actionable error: "No API key found. Set
   PUCI_API_KEY or configure ~/.config/puci/config.toml".

*Size Estimate: Medium*

---

## 2. Editing Capability

### Current State
- **Available tool**: Only `write_file` (`src/lib/tools/write_file.c:23-81`).
  Note: An experimental non-blocking async tool execution engine (`puci_tool_async_vtable`
  and `src/lib/tools/run_shell.c`) landed in 0.1.0 and has not yet been independently
  audited; the next development cycle should audit and extend it.
- **Mechanism**: `fopen(path, "wb")` followed by `fwrite(content, 1, len, f)`.
- **Failure modes**:
  - Direct truncation before write completes: if `fwrite` is interrupted,
    fails, or crashes, the original file is already truncated and lost.
  - No atomic rename: writes directly to the destination path.
  - No diff preview: the approval callback receives a raw character count or
    dump rather than a structured before/after diff.
  - No search-and-replace / hunk editing: modifying a 500-line file requires the
    model to reproduce all 500 lines verbatim. Any hallucination or omission
    silently corrupts the file.

### What is Missing
1. `edit_file` / `replace_block` tool: takes `path`, `old_str`, `new_str` (or
   line range / regex), validates exact occurrence, applies surgical change.
2. Atomic writes: write to temporary sibling file (`.path.tmp.XXXXXX`),
   `fsync()`, and `rename()` over the target.
3. Safe rollback / backups on failed edits.
4. Unified diff integration with the approval callback (`puci_compute_diff()`
   exists in `src/lib/core/diff.c` but is not wired into `write_file`).

*Size Estimate: Large (Core Daily Driver Requirement)*

---

## 3. Code Navigation and Exploration

### Current State
- **Available tools**: `read_file` (`src/lib/tools/read_file.c`) and `run_shell`
  (`src/lib/tools/run_shell.c`).
- **File reads**: `read_file` reads up to 8 MB (`MAX_READ_BYTES`) in one shot
  (`src/lib/tools/read_file.c:21`). It reports `truncated: true` if capped, but
  offers no offset/line range parameters.
- **Search**: No built-in `grep` or symbol search tool. The model is forced to
  invoke `run_shell` with `grep -rn` or `find`, requiring extra tool
  invocations and parsing raw shell outputs.
- **Directory listing**: No `list_dir` or `glob` tool. `repl.c:395-401` runs a
  shallow 24-entry `find` for initial system prompt context, but the agent
  cannot query subdirectories during a turn.
- **Gitignore awareness**: None built into file tools.

### What is Missing
1. `grep` / `search_files` tool: fast regex/substring search across codebase
   with line numbers and file pattern filters.
2. `glob` / `list_files` tool: directory tree exploration with `.gitignore`
   filtering.
3. Range-based reading in `read_file` (e.g. `start_line`, `end_line` or byte
   offsets).

*Size Estimate: Medium*

---

## 4. Session Ergonomics

### Current State
- **Persistence**: None. Every invocation of `puci run` or `puci` REPL starts
  from an empty conversation.
- **History across runs**: None. REPL command history is kept in memory
  (`src/cli/repl/lineedit.c:138-164`) and lost on exit.
- **Resume capability**: None. There is no session serialization to disk or
  `--resume` flag.
- **Approvals**: In `src/cli/repl/repl.c:133-151`, `policy()` prompts
  `Allow? [y]es [n]o:` for every single tool invocation. Running a multi-step
  build or 15 tool calls requires 15 separate interactive confirmations.
- **Plan / Read-only mode**: `/plan` prints `"plan mode is added in Phase 3b."`
  and does nothing (`src/cli/repl/repl.c:306-310`).

### What is Missing
1. Session saving & resumption (`~/.puci/sessions/<id>.json`).
2. REPL history persistence to `~/.puci_history`.
3. Granular approval policies: "always allow for this session", "auto-allow
   read-only tools (`read_file`, `grep`, `glob`)", only prompt on writes/shell.
4. Read-only / plan exploration mode disabling mutation tools.

*Size Estimate: Medium*

---

## 5. Context Window Management

### Current State
- **Budgeting**: Unbounded message accumulation in `sess->messages`
  (`src/lib/core/session.c:54-81`).
- **Overflow behavior**: When turn history exceeds the upstream provider's
  context limit (e.g., 128k tokens), the API returns a 400 Bad Request error.
  PUCI surfaces this as a hard failure (`PUCI_ERR_PROTOCOL` or
  `PUCI_ERR_UNKNOWN`).
- **Token visibility**: `puci_session_get_usage()` tracks prompt and completion
  tokens (`src/lib/core/session.c:260-267`), but in the REPL it is only shown
  when explicitly typing `/cost` (`src/cli/repl/repl.c:286-304`).

### What is Missing
1. Adaptive context truncation: sliding window preserving system prompt,
   project context, initial user goal, and the most recent N turns.
2. Token budget headroom checks before making upstream requests.
3. Warning indicator in CLI when approaching context capacity.

*Size Estimate: Medium*

---

## 6. Output and Interaction Quality

### Current State
- **Streaming response**: Tokens stream to stdout in real time
  (`src/cli/cmd/run.c:22-29`, `src/cli/repl/repl.c:103-110`).
- **Reasoning / Thinking models**: `PUCI_EVENT_REASONING_DELTA` is parsed by
  the engine (`src/lib/providers/openai.c:122-138`) but is unhandled in both
  `run.c` and `repl.c` event callbacks. As a result, reasoning models (DeepSeek
  R1, o1, o3-mini) have their thought streams silently discarded.
- **Progress indication**: No spinner or progress indicator during tool
  execution (`run_shell` or long network calls).
- **Interrupt / Cancellation**: Fully functional async signal cancellation
  (`src/cli/repl/repl.c:43-57`). First Ctrl-C cleanly cancels in-flight turns
  without crashing or leaking; second Ctrl-C within 2s exits.

### What is Missing
1. Reasoning stream rendering: distinct formatting (dimmed or bracketed
   `[thinking] ...`) for reasoning deltas.
2. Interactive progress spinner during long-running tool execution.
3. Enhanced error formatting with troubleshooting hints.

*Size Estimate: Small*

---

## 7. Safety and Recoverability (The Self-Hosting Hazard)

### Current State
- **Self-modification risk**: PUCI running inside its own repository has
  direct write access to `src/`, `build/`, `.git/`, and all source files.
- **Tool safety guards**: `run_shell` (`src/lib/tools/run_shell.c:1-240`) runs
  any command via `fork` + `execvp`. There are no path restrictions, no sandbox,
  and no guards preventing the agent from modifying or deleting `.git` or the
  active build directory.
- **Recovery binary**: No `make install-stable` or isolated fallback binary
  maintained outside the development tree. If the agent makes a broken edit to
  `src/lib/core/session.c` and runs a rebuild, the `puci` binary in `build/`
  can become corrupted, leaving the developer without a working tool.

### Recommended Minimum Viable Safety Strategy
1. **Isolated Bootstrap Binary**: Maintain a stable bootstrap binary in
   `~/.local/bin/puci-stable` with a one-command fallback (`make bootstrap-save`
   and `make bootstrap-restore`).
2. **Protected Paths Guard**: Refuse writes via `write_file` / `edit_file` to
   `.git/*` and the current build output directory (`build/*`).
3. **Dirty-tree checkpointing**: Ensure git working tree diffs can be inspected
   or rolled back easily.

*Size Estimate: Small-to-Medium*

---

## 8. Prioritized Roadmap to Daily Driver

### Priority 1: Blocking (Cannot work a full day without these)
1. **`edit_file` Tool with Atomic Writes and Diff Preview** *(Large)*
   - Surgical block replacement (`old_str` -> `new_str`).
   - Atomic rename via temporary file.
   - Prevents file corruption and massive token consumption from whole-file rewrites.
2. **Code Search (`grep` / `glob`)** *(Medium)*
   - Fast codebase search and file discovery without resorting to manual shell commands.
3. **Configuration File (`~/.config/puci/config.toml`)** *(Medium)*
   - Persistent endpoints, models, and keys across terminal sessions.
4. **Self-Hosting Safe Binary & Path Guards** *(Small)*
   - Prevent agent from overwriting `.git/` or active build binary; maintain stable backup.

### Priority 2: Painful (Usable but causes high friction)
5. **Ergonomic Approval Policies** *(Small-to-Medium)*
   - Session-wide auto-allow, auto-allow for read-only tools (`read_file`, `grep`).
6. **Plan / Read-Only Mode (`/plan`)** *(Medium)*
   - Disables mutation tools during codebase exploration to guarantee no unintended edits.
7. **Session Persistence & Resume** *(Medium)*
   - Save session state to disk; allow `--resume` on previous conversation.
8. **Adaptive Context Truncation** *(Medium)*
   - Prevent hard 400 crashes when conversations grow beyond token window limits.
9. **Render Reasoning Streams** *(Small)*
   - Display reasoning deltas in dimmed style for reasoning models.

### Priority 3: Nice to Have (Polish and quality of life)
10. **REPL History File (`~/.puci_history`)** *(Small)*
11. **Interactive Spinners for Long Tool Invocations** *(Small)*
