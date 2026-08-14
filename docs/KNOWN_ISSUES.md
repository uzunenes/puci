# Known Issues and Deferred Findings

The following items are known limitations and deferred findings tracked for future milestones:

- `max_tool_iterations = 1` yields `tool_starts == 0`; possible off-by-one, and the intended semantics are not documented in `session.h`
- the error-equivalence test checks each path's status separately but never compares blocking against async directly, unlike the cancellation case
- `get_fds()` clamps a third-party vtable's write count after the fact; the clamp corrects the counter but cannot prevent an overrun, so the contract lives on the vtable side and must say so in `tool.h`
- on `PUCI_ERR_BUFFER_TOO_SMALL`, `*out_count` reports what was written, not what was required, forcing a second `cap == 0` call
- `test_cancel` takes ~103 s and is ~97% of suite runtime; iteration count should drop and the long run should move to a separate suite
- unanswered: whether the memory-safety fixes can be split out of the release commit, and why `heartbeats >= 3` was retained alongside the new structural assertions
