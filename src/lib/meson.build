# libpuci - the reusable core library.
#
# Sources are collected across the lib/ subdirectories. libpuci must never link
# a terminal library and must never pull in anything from src/cli.

lib_inc = include_directories('../../include')

lib_sources = files(
    'core/context.c',
    'core/session.c',
    'core/agent.c',
    'core/diff.c',
    'core/error.c',
    'net/http.c',
    'net/sse.c',
    'json/json.c',
    'providers/openai.c',
    'providers/registry.c',
    'tools/path_guard.c',
    'tools/read_file.c',
    'tools/write_file.c',
    'tools/edit_file.c',
    'tools/grep.c',
    'tools/glob.c',
    'tools/run_shell.c',
    'config/config.c',
    'util/arena.c',
    'util/alloc.c',
    'util/strbuf.c',
    'util/log.c',
    'version.c',
)

if get_option('memory').enabled()
    lib_sources += files('tools/memory.c')
endif

libpuci_static = []
libpuci_shared = []

# Shared library, soname derived from the ABI version.
if get_option('shared_lib')
    libpuci_shared = shared_library(
        'puci',
        lib_sources,
        include_directories: lib_inc,
        dependencies: [curl_dep],
        version: meson.project_version(),
        soversion: '@0@'.format(PUCI_ABI_VERSION),
        install: true,
    )
endif

# Static library.
if get_option('static_lib')
    libpuci_static = static_library(
        'puci',
        lib_sources,
        include_directories: lib_inc,
        dependencies: [curl_dep],
        install: true,
    )
endif

libpuci_targets = []
if get_option('static_lib')
    libpuci_targets += libpuci_static
endif
if get_option('shared_lib')
    libpuci_targets += libpuci_shared
endif

# Install the public headers.
install_headers(
    files(
        '../../include/puci/puci.h',
        '../../include/puci/version.h',
        '../../include/puci/types.h',
        '../../include/puci/session.h',
        '../../include/puci/provider.h',
        '../../include/puci/tool.h',
        '../../include/puci/event.h',
        '../../include/puci/error.h',
        '../../include/puci/approval.h',
        '../../include/puci/diff.h',
    ),
    subdir: 'puci',
)

# pkg-config file so downstream consumers link with `pkg-config --cflags --libs puci`.
pkg_mod = import('pkgconfig')
pkg_mod.generate(
    libraries: libpuci_targets,
    name: 'puci',
    description: 'Minimal C17 AI coding agent harness core library',
    version: meson.project_version(),
)

# Consumer-facing dependency. Link the shared library when available (the
# normal way downstream embeds libpuci), falling back to the static library.
if get_option('shared_lib')
    consumer_target = libpuci_shared
else
    consumer_target = libpuci_static
endif
puci_dep = declare_dependency(
    link_with: consumer_target,
    include_directories: lib_inc,
    dependencies: [curl_dep],
)

# Static-only dependency for the test suite so internal (non-public) symbols
# can be exercised from unit tests.
puci_static_dep = declare_dependency(
    link_with: libpuci_static,
    include_directories: lib_inc,
    dependencies: [curl_dep],
)
