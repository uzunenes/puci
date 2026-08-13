# libFuzzer harnesses (dev-only). Requires clang for -fsanitize=fuzzer.
# Enable with: meson setup build-fuzz -Dfuzz=true -Dcli=false
# These fuzz the internal JSON parser and SSE parser against arbitrary bytes.
# CI does not build or run fuzzing.

if cc.get_id() != 'clang'
    message('Fuzzing requires clang; skipping fuzz/ targets.')
else
    internal_inc = include_directories('../src/lib')
    fuzz_san = ['-fsanitize=fuzzer,address,undefined']

    executable(
        'fuzz_json',
        'fuzz_json.c',
        include_directories: [internal_inc],
        dependencies: puci_static_dep,
        c_args: fuzz_san,
        link_args: fuzz_san,
    )
    executable(
        'fuzz_sse',
        'fuzz_sse.c',
        include_directories: [internal_inc],
        dependencies: puci_static_dep,
        c_args: fuzz_san,
        link_args: fuzz_san,
    )
endif
