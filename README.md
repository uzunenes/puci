# puci

PUCI is a minimal AI coding-agent harness written in portable C17. It is for
embedders that want a small provider/tool core and a caller-owned poll loop.

## What works

- OpenAI-compatible streaming and provider transcript fixtures
- Blocking `puci_session_send()` and non-blocking, pollable session driving
- Non-blocking (async) tool execution lifecycle (new in 0.1.0, lightly tested)
- Text, reasoning, tool-call, tool-result, completion, cancellation, and error
	event payloads
- Custom tools, approval callbacks, cancellation, ASan/UBSan, and Clang builds

## Current limitations

The v0.1.0 release includes the non-blocking async tool lifecycle and
multi-agent poll-loop support in `libpuci`. However, async tool execution is
newly introduced and lightly tested; Stage A CLI examples and embedding guides
do not yet demonstrate advanced features such as interactive stdin-driven
cancellation or full terminal-based multiplexing. Python bindings and Phase 3b
feature sets are not part of this source release.

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
continuing tool dispatch indefinitely and returns `PUCI_ERR_LIMIT_EXCEEDED`.

Install the library and compile an embedder with `pkg-config`:

```sh
meson setup build -Dprefix=/usr/local
meson compile -C build
meson install -C build
cc examples/embedded_headless.c $(pkg-config --cflags --libs puci) -o embedded_headless
```

## License

PUCI is released under the MIT License. See [LICENSE](LICENSE).
