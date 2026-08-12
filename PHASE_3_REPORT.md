# PUCI Phase 3.5 and 3b Stage A - Final Report

## Executive Summary
Phase 3.5 (Issue #1: Async State Machine) and Phase 3b Stage A (poll-loop acceptance test) are complete and fully tested. All deliverables have been implemented, verified with regression tests, and integrated into the codebase.

## Phase 3.5 Deliverables (COMPLETE)
### 1. Fixed Critical ASan Memory Leak
**File**: `src/lib/core/agent.c` (run_build_request)
**Issue**: Prior-iteration request resources were overwritten without freeing on retry/dispatch, causing exactly-one-iteration leaks.
**Solution**: Added resource cleanup at function start (http_end, parser free, url/req_body/auth/stream_text free).
**Verification**: ASan/UBSan build clean; 3 previously-failing tests now pass.
**Impact**: Prevents resource exhaustion in long-running agent sessions with tool interactions.

### 2. Added test_async_loop Regression Test
**File**: `tests/integration/test_async_loop.c`
**Purpose**: Verify non-blocking poll-loop efficiency (no busy-spinning).
**Test Design**:
- Drives one full turn via non-blocking API (begin_turn / get_fds / next_timeout_ms / process / next_event)
- Streams 300 chunks over 2ms intervals from mock server
- Counts process() calls: asserts < 20,000 (honest cost ~300; busy-spin would be hundreds of thousands)
- Verifies 628 total checks pass (deltas received + turn completion)
**Fix Applied**: Corrected API usage to drain events via next_event() queue (not callbacks - those are blocking-only)
**Result**: Regression test now passes; documents the proper non-blocking integration pattern.

### 3. Added get_fds Sizing Query Test
**File**: `tests/integration/test_async_loop.c` (added in main())
**Requirement**: Verify cap==0 sizing query returns required fd count.
**Test Coverage**:
- Before turn: get_fds returns count=0 (no fds needed)
- After begin_turn: get_fds(NULL, 0) returns required count
- Actual get_fds with buffer fills exactly that many fds
- Sizing query works correctly for buffer allocation
**Result**: All assertions pass; hosts can reliably size their pollfd arrays.

### 4. Comprehensive Phase 3.5 Commit
**Commit**: 4dc12bb "phase3.5(issue#1): Fix async loop leak, add non-blocking API regression test with sizing verification"
**Format**: Conventional commit with detailed changelog and trailers.
**Impact**: Non-blocking async state machine (poll-loop) verified as single source of truth; blocking API confirmed as thin wrapper.

## Phase 3b Stage A Deliverables (COMPLETE)
### 5. Implemented poll_loop.c Acceptance Test
**File**: `examples/poll_loop.c`
**Purpose**: Demonstrate public non-blocking API driving a session from host-owned poll() loop.
**Features Implemented**:
- Interactive prompt-per-line interface via stdin
- Proper poll timeout flooring (curl_multi_timeout==0 is upper bound, not force to poll immediately)
- Correct event loop integration pattern: get_fds → next_timeout_ms → poll() → process() → next_event()
- Bounded non-blocking work (process() never blocks or loops until done)
- Event counting and acknowledgment (token deltas, tool starts, completion)
- Error handling and resource cleanup

**Known Limitations (documented in code)**:
- Non-blocking next_event() does not expose event payloads (token text, tool details) - they are freed after delivery
- Full text access requires blocking send() API or external callback infrastructure
- Tool execution blocks (Stage B requires Issue #2 async tool lifecycle for non-blocking tools)

**Compilation**: Builds without warnings; passes all integration checks.

### 6. Integration Verification
- All 13 tests pass in normal build
- All 13 tests pass in ASan/UBSan sanitizer build (zero leaks)
- poll_loop.c builds cleanly as part of examples build
- Repository clean with no uncommitted changes

## Test Results Summary
```
Normal Build:        13/13 tests PASS ✓
ASan/UBSan Build:    13/13 tests PASS ✓ (zero memory leaks)
Compilation:         All targets build without warnings
Repository State:    Clean (no uncommitted work)
```

## Commits in This Session
1. **4dc12bb**: phase3.5(issue#1) - Main Phase 3.5 implementation
2. **85c7f7f**: docs(status) - Phase 3.5 completion documentation
3. **d0a2041**: feat(example) - poll_loop.c Stage A implementation
4. **aca7008**: docs(status) - Phase 3b Stage A completion roadmap

## Phase 3b Stage B & Beyond - Blocked by Issue #2
The following items cannot proceed without Issue #2 (async tool lifecycle):

### Issue #2: Async Tool Lifecycle / run_shell Conversion
**Scope**: Make tool execution non-blocking so tool fds can be surfaced to host loop.
**Current State**: Synchronous (blocks); nanosleep grace supervisor prevents event loop integration.
**Requirement**: Replace with event-loop-friendly fd signaling for tool process supervision.
**Impact**: Enables Stage B and full embeddable agent functionality.

### Phase 3b Stage B: poll_loop.c Non-blocking Tools
**Dependency**: Issue #2 implementation
**Requirements**: Non-blocking tool execution integrated into poll loop.
**Status**: Pending Issue #2.

### docs/EMBEDDING.md Integration Guide
**Dependency**: Phase 3b Stage B completion
**Content**: Detailed poll loop integration guide with examples and best practices.
**Status**: Pending completion of prior stages.

## Key Architectural Insights
1. **Single Source of Truth**: Non-blocking poll-loop API is the core. Blocking send() API is a wrapper over it.
2. **Event Delivery Contract**:
   - Non-blocking API: Events queued; drained via next_event() (no payloads exposed)
   - Blocking API: Events delivered via callback (full payloads accessible)
3. **Poll Timeout Safety**: curl_multi_timeout()==0 is an upper bound. Hosts must floor to ~1000ms to prevent busy-spin.
4. **Tool Execution Gap**: Current tool system is synchronous (blocks agent loop). Issue #2 required for non-blocking tools.

## Code Quality Metrics
- **Memory Safety**: ASan/UBSan clean; no leaks or undefined behavior
- **Compilation**: All warnings enabled (-Wall -Wextra -Werror); no warnings in new code
- **Test Coverage**: Regression test covers timeout-floor behavior, event queuing, and get_fds sizing
- **Documentation**: Comprehensive comments in examples and code explaining constraints

## What's Ready for Acceptance
✅ Phase 3.5: Non-blocking async state machine with proven efficiency and leak fixes
✅ Phase 3b Stage A: poll_loop.c example demonstrating proper API usage and event loop integration
✅ All tests passing
✅ Repository in clean, shippable state

## What Remains (Not Started)
⏳ Issue #2: Async tool lifecycle implementation (separate, substantial task)
⏳ Phase 3b Stage B: Non-blocking tool execution (blocked on Issue #2)
⏳ docs/EMBEDDING.md: Integration guide (blocked on Stage B)

## Recommendations
1. **Accept Phase 3.5 & 3b Stage A**: All deliverables complete, tested, and ready.
2. **Plan Issue #2**: Define requirements for async tool lifecycle (out of scope for this session).
3. **Schedule Phase 3b Stage B**: After Issue #2 acceptance, implement non-blocking tools.
4. **Document Phase 6**: EMBEDDING.md, cross-compile guide, measured performance.

---
**Status**: Ready for Phase 3b acceptance review. Issue #2 planning and implementation should proceed in parallel.
**Date**: Phase 3.5 and 3b Stage A complete.
