# PUCI Benchmarks — 1000-Agent Swarm Density (v0.3.0)

## Summary

PUCI achieves **<300 kB per-session PSS delta** and **<110 MB total RSS for 1000 concurrent sessions** on a single `poll()` loop, proving the "SQLite of AI Agent Harnesses" density thesis. The benchmark drives N sessions against a mock OpenAI-compatible SSE server on one thread, one loop.

## Reproduction

```sh
meson setup build-bench -Dbench=true
ninja -C build-bench bench/bench_swarm
./build-bench/bench/bench_swarm 100   # quick
./build-bench/bench/bench_swarm 1000  # full density run (~1.5s)
./build-bench/bench/bench_swarm 500   # intermediate
```

Also via examples:

```sh
meson setup build-examples -Dexamples=true
ninja -C build-examples examples/bench_swarm
```

## Measured Results (Linux x86_64, c5.2xlarge-class, 2026-08-15)

| N | Baseline PSS (kB) | Creation PSS delta (kB) | Peak PSS (kB) | Peak RSS (kB) | Wall (ms) | Events | Turns/s | Verdict |
|---|-------------------|--------------------------|---------------|---------------|-----------|--------|---------|---------|
| 100 | 4709 | ~265 total / 2.6 per-sess | 18010 | 19856 | 39 | 300 | 2564 | PASS |
| 1000 | 4713 | 265 total / 0.27 per-sess | 104250 | 106096 | 1460 | 3000 | 685 | PASS |

*Target: <250 MB RSS for 1000 sessions (<300 kB/session). Achieved: 106 MB RSS (103 kB/session peak PSS).*

## Method

- One `puci_context` + N `puci_session` (`puci_session_create`) with identical provider pointing at a forked mock HTTP server (`AF_INET` loopback, `Content-Length` SSE, `Connection: close`).
- Mock streams 3 SSE deltas (`hello i`, `world`, `finish_reason:stop` + `usage`) plus `[DONE]` per accepted connection, sequentially serving N connections (128 backlog).
- After `begin_turn` on all N, aggregate `get_fds` + folded `next_timeout_ms` into a single `poll()` array (up to 8192 fds), drive `process() + next_event()` until all sessions idle (`get_fds==0`).
- Memory via `/proc/self/smaps_rollup` PSS (or `VmRSS` fallback) sampled at baseline, post-creation, and post-poll peak. Wall time via `CLOCK_MONOTONIC`.

## Comparison (illustrative)

| Harness | Per-session RSS | 1000-session RSS | Startup | Runtime |
|---------|-----------------|------------------|---------|---------|
| PUCI (C17, libcurl) | ~103 kB PSS | ~106 MB | <1 ms | none (embedder owns loop) |
| Python (LangChain/Autogen) | ~45 MB | ~45 GB | ~300 ms | Python + venv |
| Node (AI SDK) | ~30 MB | ~30 GB | ~150 ms | Node + npm |

*Python/Node estimates are order-of-magnitude from public profiling; PUCI numbers are measured above.*

## Hardening Notes (v0.3.0)

- SSE parser caps at 8 MB (`PUCI_ERR_LIMIT_EXCEEDED`).
- MCP stdio uses 4 KB buffered reader (vs byte-at-a-time).
- `run_shell` abort: 2s `SIGTERM→WNOHANG/50ms→SIGKILL→blocking reap`.
- `test_async_loop` asserts `cpu_ms*4 < wall_ms` (no busy-spin).
