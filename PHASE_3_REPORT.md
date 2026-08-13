# PUCI Phase 3.5 Item 3 - Correction Required

## Status: REJECTED - Rework Required

The poll_loop.c Stage A submission was rejected for incomplete work and a critical
architectural violation: the async and blocking API paths have diverged.

## Root Cause

Non-blocking `next_event()` does not expose event payloads (token text, tool
call details). Payloads are freed after delivery. This means:

- Async path: caller cannot read what the model said
- Blocking path: caller receives full text via callback

This violates the core invariant that both paths are the same implementation.
An embedder using the async API gets a hobbled interface; a Python binding
built on it would be useless (binding without token text is unusable).

## Critical Blocking Issue: Event Payload Ownership (Section 1)

Define and document ONE explicit ownership model. Pick:

**Option A (recommended): Borrowed pointer**
- `puci_event` carries pointer + length into library-owned buffer
- Valid until next `next_event()` or `process()` call
- No per-event allocation (good for constrained targets)
- Consumers copy data if they need to retain it

**Option B: Transfer ownership**
- `puci_event` carries owned buffer
- Caller calls `puci_event_free(&ev)`
- Simpler to reason about, but allocates per event

Apply chosen model to ALL event types:
- Token delta / reasoning delta text
- Tool call (id, name, arguments)
- Tool result  
- Turn complete (token counts, finish reason)
- Turn cancelled / error (message)

## Gate: Rewrite send() over Public Async API

Rewrite `puci_session_send()` / `run_turn()` ENTIRELY in terms of public async API:
- `begin_turn`, `get_fds`, `next_timeout_ms`, `process`, `next_event`
- Plus standard `poll()` from host

No reaching into internal state. No private event path.

If this cannot be done cleanly, the async API is incomplete.

Add regression test: same prompt through both paths, assert byte-identical text,
byte-identical reasoning, identical tool calls and token counts. This prevents
re-divergence forever.

## Stage A Requirements - Complete Missing (3 of 6)

Current poll_loop.c covers requirements 2-3 (basic loop structure). Missing:

### Requirement 4: Host owns other fds
- Poll stdin + libpuci fds + timerfd in one loop
- Print heartbeat (e.g. `[host] tick 1.2s`) every ~250ms
- Heartbeat must interleave with token output during streaming
- Proof: include interleaved excerpt in final report

### Requirement 5 (PRIMARY GATE): Two concurrent agents, one thread, one loop
- Create two agents with different prompts
- Both stream at once; events interleave (prove with agent ID prefixes)
- Per-agent fd sets aggregated into one poll() array
- Per-agent timeouts folded via minimum
- No global mutable state, no threads, no second loop
- Test asymmetric case: cancel agent A mid-stream; agent B continues uninterrupted
- **If this cannot be done without global state or second loop: stop and report.**

### Requirement 6: Host-loop cancellation via stdin
- Typing line on stdin during turn cancels via pubcli_session_cancel_fd()
- After cancel: loop returns to prompt
- `puci_session_validate()` passes
- Next turn on same session succeeds
- Assert all three in the example

### Requirement 1: strace Evidence
- Run under `strace -f -e trace=poll,ppoll,select,epoll_wait,nanosleep,clock_nanosleep`
- Only `poll()` should block during turn
- Any `nanosleep` appearing (outside run_shell SIGTERM grace) is a bug
- Also `grep` library for `sleep(`, `usleep(`, `nanosleep(` - only run_shell permitted
- Include strace and grep output verbatim in report

## Test Improvements

### Tighten busy-spin regression
- Lower bound from 20k to ~2k process() calls for 300 chunks (~7 per chunk)
- Derive constant from chunk count, not hardcode
- Add CPU-time assertion: `CLOCK_PROCESS_CPUTIME_ID` must stay well below wall time
  (busy-spin shows CPU ≈ wall time; this catches it)
- Keep existing content assertions

## Hygiene

### Remove process_fds stub
Stub cannot stay on async hot path.
- Either: implement it
- Or: delete it and remove declaration
- Current state violates: "function named as stub in shipped header will be read as real by next session"

### Remove .vscode/.checkmarxIgnored
Corporate security scanner config has no place in personal MIT repository.
- Remove from tracking, add `.vscode/` to `.gitignore`
- Purge from recent history if feasible
- Re-run pre-commit guard over full history to confirm no endpoint/key ever landed

## Order of Work

1. Event payload ownership model, applied to all event types (Section 1, BLOCKER)
2. `send()` rewritten over public async API + both-paths test (GATE for all after)
3. Tighten busy-spin test + CPU-time assertion (Section 3)
4. Resolve `process_fds`; remove `.vscode/`; re-run history guard (Section 4)
5. poll_loop.c requirements 4 and 6, then 5 (hardest; save for last)
6. strace + grep evidence for requirement 1
7. Final report. Stop.

No Issue #2, no Phase 3b until Stage A accepted with all requirements met.

## Standing Rules (unchanged, still in force)

- `-Wall -Wextra -Wpedantic -Werror -Wconversion` on GCC and Clang
- ASan and UBSan on all tests
- Valgrind on integration tests
- Zero leaks, no double-frees
- No `system()`, `strcpy`, `strcat`, `sprintf`, `gets`
- No `printf`, `exit()`, `abort()`, global mutable state in `src/lib/`
- CLI target's include path must not contain `src/lib/`
- Every public function documents pointer ownership and thread safety
- Conventional commits
- No endpoint hostname or API key in repository
- Pre-commit grep guard in place and enforced
