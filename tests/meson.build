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

test_approval_event = executable(
    'test_approval_event',
    'integration/test_approval_event.c',
    include_directories: [internal_inc],
    dependencies: puci_static_dep,
)
test('test_approval_event', test_approval_event, timeout: 120)

test_session_api = executable(
    'test_session_api',
    'unit/test_session_api.c',
    include_directories: [internal_inc],
    dependencies: puci_static_dep,
)
test('test_session_api', test_session_api)

test_mcp = executable(
    'test_mcp',
    'unit/test_mcp.c',
    include_directories: [internal_inc],
    dependencies: puci_static_dep,
)
test('test_mcp', test_mcp)

test_compaction = executable(
    'test_context_compaction',
    'unit/test_context_compaction.c',
    include_directories: [internal_inc],
    dependencies: puci_static_dep,
)
test('test_context_compaction', test_compaction)

test_config = executable(
    'test_config',
    ['unit/test_config.c', '../src/cli/cmd/config.c'],
    include_directories: [include_directories('../include'), internal_inc],
    dependencies: puci_static_dep,
)
test('test_config', test_config)

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
test('test_cancel', test_cancel, timeout: 30)

test_cancel_stress = executable(
    'test_cancel_stress',
    'integration/test_cancel.c',
    include_directories: [internal_inc],
    dependencies: [puci_static_dep, threads_dep],
    c_args: ['-DITERS=100'],
)
test('test_cancel_stress', test_cancel_stress, timeout: 300, suite: 'stress')

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

# Async tool lifecycle regression test (Issue #2 / Stage A): drives a
# run_shell("/bin/sleep 1") tool call from a host-owned poll() loop that also
# owns an unrelated timerfd, and asserts the host's own fd keeps firing (and
# no single loop iteration blocks anywhere near the command's runtime) while
# the tool runs, proving process() never blocks the host loop for the tool's
# duration. Public API only; Linux-specific (timerfd, /proc).
test_async_tool = executable(
    'test_async_tool',
    'integration/test_async_tool.c',
    include_directories: [internal_inc],
    dependencies: puci_static_dep,
)
test('test_async_tool', test_async_tool, timeout: 60)

# Two-concurrent-agents test (Stage A requirement 5, the primary gate): two
# sessions, each dispatching an async run_shell tool call against its own mock
# server, driven from ONE poll() loop with no threads and no global state.
# Proves genuine concurrency (wall time close to max, not sum, of the two
# tool durations) and asymmetric cancellation (cancelling one session's
# in-flight tool leaves the other's turn to complete normally).
test_two_agents = executable(
    'test_two_agents',
    'integration/test_two_agents.c',
    include_directories: [internal_inc],
    dependencies: puci_static_dep,
)
test('test_two_agents', test_two_agents, timeout: 60)



# TTG envelope filter is dependency-free (src/cli/tui/ttg.c) so it links
# standalone; include both src/cli (for ttg.h) and the test source itself.
test_ttg = executable(
    'test_ttg',
    ['unit/test_ttg.c', '../src/cli/tui/ttg.c'],
    include_directories: [include_directories('../src/cli/tui')],
)
test('test_ttg', test_ttg)

# Owned content + Markdown parser are dependency-free (no termbox) so they link
# standalone from the TUI source dir, mirroring the test_ttg pattern.
test_content = executable(
    'test_content',
    ['unit/test_content.c',
     '../src/cli/tui/content.c', '../src/cli/tui/md.c'],
    include_directories: [include_directories('../src/cli/tui')],
)
test('test_content', test_content)

# Semantic TUI-polish grid assertions (PTY render integrity). Guarded on
# python3 being present; uses the shell-captured binary (PUCI_BIN) or the
# in-tree build. These are heavier PTY tests, so keep them out of the default
# suite and run under the explicit 'tui' suite.
py3 = find_program('python3', required: false)
if py3.found()
    test(
        'test_tui_polish',
        py3,
        args: [files('test_tui_polish.py')],
        timeout: 180,
        is_parallel: false,
        suite: 'tui',
        env: ['PUCI_BIN=' + meson.project_build_root() + '/src/cli/puci'],
    )
endif
