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

# End-to-end agent loop test with an embedded mock SSE server.
test_agent = executable(
    'test_agent_loop',
    'integration/test_agent_loop.c',
    dependencies: puci_static_dep,
)
test('test_agent_loop', test_agent)