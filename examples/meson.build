# Example programs. Built only when -Dexamples=true.
#
# Minimal_agent is the canonical "smallest consumer" and must compile against
# the installed public headers. The examples meson intentionally does NOT pull
# in src/lib/ directly; it consumes puci_dep exactly like an external program.

if get_option('examples')
    executable(
        'minimal_agent',
        'minimal_agent.c',
        dependencies: puci_dep,
        install: false,
    )
    executable(
        'custom_tool',
        'custom_tool.c',
        dependencies: puci_dep,
        install: false,
    )
    executable(
        'embedded_headless',
        'embedded_headless.c',
        dependencies: puci_dep,
        install: false,
    )
    executable(
        'poll_loop',
        'poll_loop.c',
        dependencies: puci_dep,
        install: false,
    )
    executable(
        'embedded_editor_agent',
        'embedded_editor_agent.c',
        dependencies: puci_dep,
        install: false,
    )
    executable(
        'benchmark_footprint',
        'benchmark_footprint.c',
        dependencies: puci_dep,
        install: false,
    )
endif
