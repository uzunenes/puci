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
# feature. libpuci never links it.
tui_sources = files('tui/tui.c')
if get_option('tui').enabled()
    tui_dep = dependency('ncurses', required: false)
    cli_sources += tui_sources
    cli_deps += tui_dep
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
