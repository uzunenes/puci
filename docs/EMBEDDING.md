# Embedding libpuci

PUCI exposes a caller-owned, non-blocking session driver. The host calls
`puci_session_begin_turn()`, obtains the current fds and timeout, waits in its
own `poll()` loop, calls `puci_session_process()`, and drains events with
`puci_session_next_event()`.

```c
puci_session_begin_turn(session, prompt);
for (;;) {
	int timeout_ms = -1;
	struct pollfd fds[64];
	size_t count = 0;
	puci_session_next_timeout_ms(session, &timeout_ms);
	puci_session_get_fds(session, fds, 64, &count);
	poll(fds, (nfds_t)count, timeout_ms < 0 ? 1000 : timeout_ms);
	puci_session_process(session, fds, count);

	puci_event event;
	while (puci_session_next_event(session, &event)) {
		/* Copy borrowed payloads before the next process/next_event call. */
	}
}
```

## FD and timeout contract

The returned descriptor set is level-triggered. The set may change after every
`puci_session_process()` call, so call `puci_session_get_fds()` again before
each wait. A capacity query uses `puci_session_get_fds(session, NULL, 0,
&count)`. The cancellation fd is included while a turn is active.

`puci_session_next_timeout_ms()` is an upper bound for the host wait. When
`curl_multi_timeout()` reports zero with pollable fds, PUCI returns an
approximately 1000 ms floor to avoid a busy-spin. A zero timeout with no fds
means the state machine has immediate work and the host should call
`puci_session_process()` promptly.

## Thread safety and ownership

Everything is single-thread-per-session and must not overlap with another
operation on that session. Context configuration must finish before sessions
are created. Returned handles and descriptors remain library-owned. Strings
returned in errors, tools, and `puci_event` are borrowed and must be copied if
retained. Strings returned by `puci_compute_diff()` are heap-owned and must be
released with `puci_diff_free()`.

`puci_session_cancel()` is the exception: it is safe from another thread and
from an asynchronous signal handler. It performs only cancellation signalling;
the caller must not destroy the session from the signal handler.

Event payload pointers are session-owned and valid until the next
`puci_session_next_event()` or `puci_session_process()` call on that session.

Set `puci_session_options.max_tool_iterations` to bound tool dispatch per turn;
zero selects the default of 32. A bounded value is recommended for automated
audit or batch agents.

## Current limitations

- Unrelated host fds, two concurrent agents, stdin cancellation, and a
	non-blocking tool lifecycle are not demonstrated by the current Stage A
	example.
- Tool execution can block the host loop; Issue #2 is not implemented.
- Phase 3b, Python bindings, and PyPI publication are out of scope for v0.1.0.
