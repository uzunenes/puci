# DOGFOOD

Friction points, missing features and annoyances hit while using puci with
puci. This file is the honest record of what does not yet work cleanly; it
feeds the v0.2.0 roadmap. GitHub issues are the actionable tracker; entries
here link to them.

## Embeddable-core / poll-loop acceptance test (examples/poll_loop.c)

Trying to write the acceptance test for the embeddable thesis surfaced that
the public API cannot yet be driven from a host-owned event loop without a
dedicated thread. Two concrete blockers:

- **The pollable agent path is unimplemented.** `puci_session_poll()` /
  `puci_session_poll_fd()` are stubs (`PUCI_ERR_OUT_OF_STATE` / `-1`), and the
  agent loop is one synchronous `puci_agent_run_turn()` call with no resumable
  state machine. No `get_fds` / `next_timeout_ms` / `process` / `next_event`
  surface to poll from. → issue #1
- **The generic tool interface is synchronous.** `puci_tool_invoke_cb` blocks;
  `run_shell`'s child pipe fds cannot be surfaced to a host loop. Poll-loop
  req #7 (non-blocking tool execution) therefore fails until an async tool
  life-cycle is added. → issue #2

What was rescued from the same work and landed:

- Non-blocking HTTP request handles (`puci_http_begin/get_fds/next_timeout_ms/
  step/end`) over the session's persistent curl multi handle, verified under
  ASan/UBSan (stream + cancel). This is the transport the state machine will
  drive, and the reason issue #1 is "finish the state machine" rather than
  "start from zero".

Driver-harness note (bites every event-loop integrator): for the curl multi
interface, `curl_multi_timeout()==0` is an **upper bound**, not a force to
poll immediately. A host that passes 0 straight to `poll()` busy-spins when a
streaming server dribbles chunks; floor the poll timeout (~1 s, as curl's own
example does).