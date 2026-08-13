# Test suite. Unit tests exercise internal modules (JSON parser, strbuf) and
# must therefore include the private src/lib include path in addition to the
# public include path. CI runs these under ASan/UBSan and Valgrind.

# Internal include dir so tests can reach json/json.h, util/*.h, etc.
internal_inc = include_directories('../src/lib')

test_exe = executable(
    'test_json',
    'unit/test_json.c',
    include_directories: [internal_inc],
    dependencies: puci_static_dep,
)
test('test_json', test_exe)

test_strbuf = executable(
    'test_strbuf',
    'unit/test_strbuf.c',
    include_directories: [internal_inc],
    dependencies: puci_static_dep,
)
test('test_strbuf', test_strbuf)

test_session = executable(
    'test_session',
    'unit/test_session.c',
    include_directories: [internal_inc],
    dependencies: puci_static_dep,
)
test('test_session', test_session)

test_tools = executable(
    'test_tools',
    'unit/test_tools.c',
    include_directories: [internal_inc],
    dependencies: puci_static_dep,
)
test('test_tools', test_tools)

test_serialize = executable(
    'test_serialize',
    'unit/test_serialize.c',
    include_directories: [internal_inc],
    dependencies: puci_static_dep,
)
test('test_serialize', test_serialize)

# Regression test for the per-provider SSE transcript fixtures: feeds each
# transcript through the real OpenAI provider parser and checks reasoning stays
# separate from the answer. Reads the fixtures from the source tree.
test_pfx = executable(
    'test_provider_fixtures',
    'unit/test_provider_fixtures.c',
    include_directories: [internal_inc],
    dependencies: puci_static_dep,
    c_args: '-DPUCI_FIXTURES_DIR="' +
           meson.current_source_dir() + '/fixtures/transcripts"',
)
test('test_provider_fixtures', test_pfx)

test_approval = executable(
    'test_approval',
    'unit/test_approval.c',
    include_directories: [internal_inc],
    dependencies: puci_static_dep,
)
test('test_approval', test_approval)

test_session_api = executable(
    'test_session_api',
    'unit/test_session_api.c',
    include_directories: [internal_inc],
    dependencies: puci_static_dep,
)
test('test_session_api', test_session_api)

# End-to-end agent loop test with an embedded mock SSE server.
test_agent = executable(
    'test_agent_loop',
    'integration/test_agent_loop.c',
    dependencies: puci_static_dep,
)
test('test_agent_loop', test_agent)

# Interrupt / cancellation test: drives the agent loop against a forked mock SSE
# server and cancels in-flight turns from a separate thread at reproducible,
# seeded points (mid-stream x100, reasoning-silence gap, during tool
# execution, and while the approval policy is waiting for input). The
# 100-iteration loop forks, streams, cancels, and recovers every iteration, so
# it gets a generous timeout.
threads_dep = dependency('threads')
test_cancel = executable(
    'test_cancel',
    'integration/test_cancel.c',
    include_directories: [internal_inc],
    dependencies: [puci_static_dep, threads_dep],
)
test('test_cancel', test_cancel, timeout: 300)

# Connection / keep-alive regression test: verifies the Task-0 scope-down of
# CURLOPT_FRESH_CONNECT (normal turns reuse one connection; the turn after a
# cancellation opens a fresh one). Uses a keep-alive and a streaming mock
# server, plus a caretaker thread for the cancellation.
test_conn = executable(
    'test_conn_reuse',
    'integration/test_conn_reuse.c',
    dependencies: [puci_static_dep, threads_dep],
)
test('test_conn_reuse', test_conn, timeout: 120)

# Non-blocking HTTP primitives integration test: drives puci_http_begin /
# get_fds / next_timeout_ms / step / end from the caller's own poll() loop
# against a streaming mock server, verifying full-body streaming and
# mid-stream cancellation (with the connection marked dirty). Requires the
# internal include path to reach net/http.h.
test_http_async = executable(
    'test_http_async',
    'integration/test_http_async.c',
    include_directories: [internal_inc],
    dependencies: [puci_static_dep],
)
test('test_http_async', test_http_async, timeout: 120)

# Async-loop busy-spin regression test: drives one full turn through the public
# non-blocking API (begin_turn / get_fds / next_timeout_ms / process / next_event)
# from a caller-owned poll() loop against a dribbling SSE mock server, counting
# process() calls. Asserts the count stays bounded (no busy-spin when curl_multi
# reports a 0 timeout) and that the full streamed text arrives and the turn
# completes. Public API only.
test_async_loop = executable(
    'test_async_loop',
    'integration/test_async_loop.c',
    include_directories: [internal_inc],
    dependencies: puci_static_dep,
)
test('test_async_loop', test_async_loop, timeout: 120)