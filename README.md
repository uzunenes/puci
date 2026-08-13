# puci

PUCI is a minimal AI coding-agent harness written in portable C17. It is for
embedders that want a small provider/tool core and a caller-owned poll loop.

## What works

- OpenAI-compatible streaming and provider transcript fixtures
- Blocking `puci_session_send()` and non-blocking, pollable session driving
- Text, reasoning, tool-call, tool-result, completion, cancellation, and error
	event payloads
- Custom tools, approval callbacks, cancellation, ASan/UBSan, and Clang builds

## Current limitations

The v0.1.0 async API does not yet provide non-blocking tool execution. Tool
execution can block the host loop. The Stage A example does not yet demonstrate
unrelated host fds, two concurrent agents, stdin-driven cancellation, or a
non-blocking tool lifecycle. Python bindings, Phase 3b, and Issue #2 are not
part of this source release.

The public API is an early 0.x contract and may change before 1.0.

## Repository layout

- `libpuci` — the reusable core (agent loop, providers, tools, streaming HTTP)
- `puci-cli` — the terminal client, a thin consumer of libpuci
- See `docs/ARCHITECTURE.md` for the system design.

## Building

```sh
meson setup build
ninja -C build
```

For both supported compilers:

```sh
CC=gcc meson setup build-gcc && meson compile -C build-gcc && meson test -C build-gcc
CC=clang meson setup build-clang && meson compile -C build-clang && meson test -C build-clang
```

For bounded one-shot agent runs, set `PUCI_MAX_TOOL_ITERATIONS` to a positive
value up to 10000. When the limit is reached, PUCI ends the turn instead of
continuing tool dispatch indefinitely and returns `PUCI_ERR_TIMEOUT`.

Install the library and compile an embedder with `pkg-config`:

```sh
meson setup build -Dprefix=/usr/local
meson compile -C build
meson install -C build
cc examples/embedded_headless.c $(pkg-config --cflags --libs puci) -o embedded_headless
```

## License

PUCI is released under the MIT License. See [LICENSE](LICENSE).
