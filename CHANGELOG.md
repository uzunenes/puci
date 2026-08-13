# Changelog

All notable changes to this project are documented here.

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
