# libpuci - the reusable core library.
#
# Sources are collected across the lib/ subdirectories. libpuci must never link
# a terminal library and must never pull in anything from src/cli.

lib_inc = include_directories('../../include')

lib_sources = files(
    'core/context.c',
    'core/session.c',
    'core/agent.c',
    'core/error.c',
    'net/http.c',
    'net/sse.c',
    'json/json.c',
    'providers/openai.c',
    'providers/registry.c',
    'tools/read_file.c',
    'tools/write_file.c',
    'tools/run_shell.c',
    'config/config.c',
    'util/arena.c',
    'util/strbuf.c',
    'util/log.c',
    'version.c',
)

if get_option('memory').enabled()
    lib_sources += files('tools/memory.c')
endif

libgen = []

# Shared library, soname derived from ABI version.
if get_option('shared_lib')
    libgen += shared_library(
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
    libgen += static_library(
        'puci',
        lib_sources,
        include_directories: lib_inc,
        dependencies: [curl_dep],
        install: true,
    )
endif

libpuci = libgen

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
    ),
    subdir: 'puci',
)

# pkg-config file.
pkgconf = configuration_data()
pkgconf.set('version', meson.project_version())
pkgconf.set('abi_version', PUCI_ABI_VERSION)
configure_file(
    input: 'puci.pc.in',
    output: 'puci.pc',
    configuration: pkgconf,
    install_dir: get_option('libdir') / 'pkgconfig',
)

# Abstract the curl dependency so consumer targets link cleanly.
puci_dep = declare_dependency(
    link_with: libgen,
    include_directories: lib_inc,
    dependencies: [curl_dep],
)