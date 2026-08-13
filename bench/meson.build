# Developer memory/unix benchmarks. Opt-in via -Dbench=true. These are manual
# measuring tools (linked against internal headers), not part of the unit test
# suite, so they are never run in CI.

internal_inc = include_directories('../src/lib')

executable(
    'bench_session_growth',
    'bench_session_growth.c',
    include_directories: [internal_inc],
    dependencies: puci_static_dep,
)
