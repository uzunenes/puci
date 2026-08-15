# Memory strategy

This document describes how libpuci manages memory in the hot paths: session
message growth, per-turn request building, JSON parsing, and SSE streaming. It
is the companion to `PROTOCOL.md` (wire format) and `ARCHITECTURE.md`.

## Two-tier allocation model

libpuci uses two distinct allocation facilities depending on lifetime:

1.  **Heap allocations through `puci_allocator`** (`util/alloc.[ch]`) for data
    whose lifetime spans turns: session messages, tool schemas, provider
    config, the system prompt, and owned error strings. These live until the
    owning session/context is destroyed.

2.  **Arena allocations through `puci_arena`** (`util/arena.[ch]`) for strictly
    scoped scratch data: every JSON parse, every SSE event's `pj_val` tree, and
    the per-request serialization buffers. An arena grabs large blocks from the
    allocator and hands out bump allocations; one `puci_arena_reset` (or
    `puci_arena_destroy`) frees everything since the last reset.

The rule is: **anything that must outlive the current request/turn is heap,
anything that can be dropped when the function returns is arena.**

## Session message growth (heap)

The session holds an array `puci_msg *messages` with a geometric growth policy:

```
ncap = messages_cap ? messages_cap * 2 : 8;   // 8, 16, 32, 64, ...
```

Doubling keeps amortized append cost O(1) and bounds wasted capacity to at most
2x the live message count. Each `puci_msg` owns one heap string (`content`) and,
for assistant tool-call messages, an array of `puci_internal_tool_call`, each of
which owns `name`, `args`, and `id` heap strings. On `puci_session_destroy` every
message, tool call, and string in the array is freed individually before the
array itself.

Because most conversations are short (tens of messages), the practical footprint
is dominated by the message content itself, not the array slack. The PSS
benchmarks in `bench/` quantify this for large synthetic transcripts.

## Per-turn request building (heap strbuf)

For each agent-loop iteration the engine builds the JSON request body into a
`puci_strbuf` (`util/strbuf.[ch]`) using the current heap. The request is the
concatenation of:

- the serialized message array (`session_serialize_messages`),
- the serialized tool schemas (`session_serialize_tools`),
- the provider's endpoint body (`openai.c`).

Both serializers append into the same heap buffer. Message/tool arrays are
guaranteed to be valid JSON arrays (regression-tested in `tests/unit/test_serialize.c`).
The request buffer is destroyed at the end of the iteration.

`extra_body` (caller-supplied JSON object, `PUCI_EXTRA_BODY`) is merged at the
DOM level: the request is parsed into an arena `pj_val *`, the caller's keys are
overlaid (caller keys win), and the merged tree is re-serialized into a fresh
heap buffer via `pj_serialize`. Invalid JSON is logged and ignored; the original
(request) buffer is never corrupted on failure.

## JSON parsing (arena scratch)

`pj_parse` (`src/lib/json/json.c`) parses into a caller-supplied arena. Every
`pj_val` node, string, key, and array/object bucket is bumped from that arena, so
the whole parse is torn down with a single `puci_arena_destroy`.

The agent loop keeps a long-lived `sess->scratch` arena and calls
`puci_arena_reset` at the top of each SSE event callback before submitting the
next chunk. This guarantees peak parse memory is bounded by the largest single
event, not by the total conversation:

```
on_sse_event:
    puci_arena_reset(&sess->scratch);   // drop the previous chunk's DOM
    vt->parse_stream(..., &sess->scratch, ...);
```

The SSE parser itself (`net/sse.c`) is a small steady-state state machine that
accumulates a bounded line buffer and does not allocate per event beyond the
current line.

### Growth and fragmentation

Arena blocks default to 64 KiB (`puci_arena_init(..., 0)`). Bump allocation has
zero per-node overhead and no fragmentation within a block; a block that is
partially used is abandoned on reset until the arena is reused. Reusing the same
arena instance across a session avoids repeated malloc churn in steady state.

## Redaction and owned strings

Credentials (the provider API key) are copied into the context and recorded in a
redaction list. `puci_logf` formats into a fixed stack buffer and rewrites any
secret substring to asterisks before the callback sees it, so no owned string is
required for logging. Provider config strings (model, base URL, key, CA bundle,
extra body) are deep-copied into the session and freed on destroy.

## Worst-case live-turn footprint

For one streaming turn of `N` SSE events and `M` messages:

- `sess->scratch` peak ≈ size of the largest single SSE event's DOM.
- serialized request heap peak ≈ size of `M` serialized messages + tools.
- the session's `messages` array grows geometrically to fit `M`.

This keeps a long mult-turn session linear in the *provided transcript* while
holding per-chunk parse memory constant.
