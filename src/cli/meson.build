# puci-cli - the terminal client.
#
# IMPORTANT: the CLI must ONLY use the public headers in include/puci/. We do
# NOT add src/lib/ to the include path here. If the CLI needs something from
# libpuci internals, the correct fix is to grow the public API, not to include
# a private header.

cli_inc = include_directories('../../include')

# Let `puci doctor` report how the CLI itself links libpuci: shared (the
# default consumer path) or static. This is a build-time fact, not something
# we can discover at runtime, so fold it into a macro scoped to this target.
cli_c_args = []
if get_option('shared_lib')
    cli_c_args += '-DPUCI_CLI_LINK_SHARED=1'
else
    cli_c_args += '-DPUCI_CLI_LINK_STATIC=1'
endif

cli_sources = files(
    'main.c',
    'cmd/run.c',
    'cmd/config.c',
    'cmd/doctor.c',
    'cmd/session_mgr.c',
    'repl/repl.c',
    'repl/lineedit.c',
)

cli_deps = [puci_dep]

# TUI lives exclusively under src/cli/tui and is shielded behind the 'tui'
# feature. libpuci never links it. termbox2 is hybrid cell renderer (header-only).
# Resolved via Meson Wrap (subprojects/termbox2.wrap) with fallback to vendored
# header in src/cli/tui/vendor/termbox2.h — zero .so, zero pkg-config needed.
termbox2_dep = dependency('termbox2', required: false, fallback: ['termbox2', 'termbox2_dep'])
if not termbox2_dep.found()
    termbox2_inc = include_directories('tui/vendor')
    termbox2_dep = declare_dependency(include_directories: termbox2_inc)
endif
curl_dep = dependency('libcurl', required: false)
if curl_dep.found()
    cli_deps += curl_dep
    cli_c_args += '-DHAVE_CURL=1'
endif

tui_sources = files('tui/tui.c', 'tui/ttg.c', 'tui/content.c', 'tui/md.c', 'tui/hl.c')
if get_option('tui').enabled()
    cli_sources += tui_sources
    cli_deps += termbox2_dep
    if curl_dep.found()
        cli_deps += curl_dep
    endif
endif

if get_option('cli')
    executable(
        'puci',
        cli_sources,
        include_directories: cli_inc,
        dependencies: cli_deps,
        c_args: cli_c_args,
        install: true,
    )
endif
