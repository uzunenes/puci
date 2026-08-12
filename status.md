# PUCI - Async State Machine (Phase 3.5) - Status

## Context
Phase 3.5 async-core API correction in progress. Goal: make the poll-loop state
machine the single source of truth (Issue #1), with the blocking API a thin
wrapper over it. Full spec in the project handoff / `docs/EMBEDDING.md` roadmap.

## What's committed before this session
- Task 0 (connection reuse + doctor build facts): `2e3e97c`
- HTTP async primitives (`puci_http_begin/get_fds/next_timeout_ms/step/end`,
  `puci_http_fd_count`): `6459f07`
- DOGFOOD / report commit: `51644c9`
- State machine rewrite of `src/lib/core/agent.c` (uncommitted at compaction)
- `run_turn` → pump wrapper; async public API in `include/puci/session.h`

## This session
### 1. ASan leak found and FIXED (uncommitted)
The full ASan/UBSan suite showed 3 failures — all LeakSanitizer reports
(`test_agent_loop`, `test_approval`, `test_cancel`), 0 double-frees/corruption.

Root cause: `run_build_request()` re-runs on the same `puci_turn_run` after a
tool dispatch (`iter++`) or a retry, but overwrote the previous iteration's
`req` (live `puci_http` handle), `parser`, `url`, `req_body`, `auth`, and
`stream_text` without freeing them → exactly one run's worth leaked.

Fix (in `agent.c`, top of `run_build_request`): release prior-iteration
request resources (http_end parser, free url/req_body/auth/stream_text) before
building new ones. `run_free` still owns turn text buffers; they are
deliberately re-initialised per iteration.

Validation: ASan rebuild clean; the 3 previously-failing tests now pass:
```
test_approval  OK   0.05s
test_agent_loop OK  0.05s
test_cancel     OK 110.60s
```
(tested, not yet committed)

### 2. Timeout-floor busy-spin regression test (NEW, uncommitted)
`tests/integration/test_async_loop.c`:
- Drives one full turn via the PUBLIC non-blocking API
  (`puci_session_begin_turn / get_fds / next_timeout_ms / process / next_event`)
  from a caller-owned `poll()` loop, against a dribbling SSE mock server
  (300 chunks, 2 ms apart, keep-alive + Content-Length).
- Counts `process()` calls; asserts `< 20000` (honest cost ~300×; a busy-spin
  over the same wall time would be hundreds of thousands). Also asserts the full
  streamed text arrives ("t" × 300) and the turn completes.
- Registered in `tests/meson.build` as `test_async_loop` (public API only, no
  `internal_inc`). Verified it links in the normal build.

## Phase 3.5 Status
✅ COMPLETE AND COMMITTED (commit 4dc12bb)

All items verified:
- Full normal-build suite: 13/13 tests pass ✅
- Full ASan/UBSan suite: 13/13 tests pass, zero leaks ✅
- ASan leak fixed in run_build_request ✅
- test_async_loop regression test added with correct non-blocking API ✅
- get_fds sizing query test added ✅
- Comprehensive conventional commit with trailers ✅
- Repository clean, no uncommitted work ✅

## Files modified (Phase 3.5, commit 4dc12bb)
- `src/lib/core/agent.c`            — leak fix in `run_build_request`
- `src/lib/core/session.c`          — async state machine integration
- `src/lib/net/http.h`              — added process_fds stub
- `src/lib/net/http.c`              — added process_fds stub
- `src/lib/puci_internal.h`         — async primitives
- `include/puci/session.h`          — public async API
- `tests/integration/test_async_loop.c` — new regression test
- `tests/meson.build`               — register `test_async_loop`
- `.vscode/.checkmarxIgnored`       — build config
- `status.md`                       — this file

## Phase 3b Status (Stage A Complete)
✅ STAGE A COMPLETE (commit d0a2041)

### What was completed
- Implemented `examples/poll_loop.c`: acceptance test demonstrating the public
  non-blocking API (begin_turn / get_fds / next_timeout_ms / process / next_event)
  driving a session from a host-owned poll() loop.
- Demonstrates proper poll timeout flooring (curl_multi_timeout==0 is upper bound).
- Interactive prompt-per-line interface; event counting (tokens, tools, completion).
- Compiles without warnings; all 13 tests pass.

### Limitations (known and documented)
- Non-blocking next_event() does not expose event payloads (token text, tool
  call details); payloads are freed after delivery. Full text requires blocking
  send() API or external callback infrastructure.
- Tool execution blocks (Stage B requires Issue #2 async tool lifecycle for
  non-blocking tool support).

## Remaining (Phase 3b Stage B, after Issue #2)
Issue #2 async tool lifecycle implementation blocks Stage B:
4. Issue #2 async tool lifecycle / `run_shell` conversion (removes synchronous
   nanosleep grace supervisor, replaces with event-loop-friendly fd signaling).
5. poll_loop.c Stage B (reqs 7–8): add non-blocking tool execution after #2.
6. `docs/EMBEDDING.md` "Integrating with your own event loop" section (detailed
   poll loop integration guide + examples).
7. Report and stop; Phase 3b after acceptance.

## Engineered-understanding notes
- `run_build_request` owns req/parser/url/req_body/auth/stream_text per
  iteration; `run_free` (fin_complete/fin_cancelled/fin_error, teardown) owns
  them at terminal. The leak was the overlap: re-build without freeing.
- `curl_multi_timeout()==0` with fds → floor to 1000 ms; with no fds → return 0
  so the host pumps immediately (avoids starving the connect). This is the
  timeout-floor logic already in `next_timeout_ms`, now tested.
- `run_shell.c` `nanosleep` (SIGTERM→grace→SIGKILL supervision) is synchronous
  tool scope = Issue #2; NOT part of this fix.