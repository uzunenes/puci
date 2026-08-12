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

# End-to-end agent loop test with an embedded mock SSE server.
test_agent = executable(
    'test_agent_loop',
    'integration/test_agent_loop.c',
    dependencies: puci_static_dep,
)
test('test_agent_loop', test_agent)