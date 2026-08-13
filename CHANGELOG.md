# Changelog

All notable changes to this project are documented here.

## [0.1.0] - 2026-08-13

### Added

- C17 embeddable agent core with blocking and caller-owned poll-loop APIs.
- Borrowed payloads for text, reasoning, tool calls, tool results, completion,
  cancellation, and errors.
- GCC and Clang strict builds, sanitizer tests, and libFuzzer harnesses.

### Limitations

- Tool execution is still blocking and the public API is not stable before 1.0.
- Stage A host-fd/concurrent-agent/cancellation example work and Phase 3b are
  not included.
