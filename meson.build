project(
    'puci',
    'c',
    version: '0.4.0',
    license: 'MIT',
    meson_version: '>=1.0.0',
    default_options: [
        'c_std=c17',
        'buildtype=release',
        'warning_level=3',
        'werror=true',
    ],
)

# ---- Global compiler flags ------------------------------------------------
cc = meson.get_compiler('c')

c_args = [
    '-Wall',
    '-Wextra',
    '-Wpedantic',
    '-Werror',
    '-Wconversion',
    # libpuci targets POSIX (fork+exec, pipes, clock_gettime, strdup). Expose
    # the POSIX API surface under the strict -std=c17 flag.
    '-D_POSIX_C_SOURCE=200809L',
]

if get_option('native_arch')
    foreach a : cc.get_supported_arguments('-march=native')
        c_args += a
    endforeach
endif

if get_option('lto')
    add_project_arguments('-flto', language: 'c')
    add_project_link_arguments('-flto', language: 'c')
endif

add_project_arguments(c_args, language: 'c')

# ---- Feature flag macros ----------------------------------------------------
# Fold the feature options into C macros so `puci doctor` and embedders can
# report which features a given build was compiled with. 0/1 macros are always
# defined so `#if PUCI_FEATURE_X` is valid even when the feature is off.
foreach feat : ['tui', 'memory', 'swarm', 'browser', 'mcp']
    if get_option(feat).enabled()
        add_project_arguments('-DPUCI_FEATURE_@0@=1'.format(feat.to_upper()), language: 'c')
    else
        add_project_arguments('-DPUCI_FEATURE_@0@=0'.format(feat.to_upper()), language: 'c')
    endif
endforeach
# Boolean options (native_arch, lto) don't expose .enabled(); fold them as 0/1.
foreach feat : ['native_arch', 'lto']
    if get_option(feat)
        add_project_arguments('-DPUCI_FEATURE_@0@=1'.format(feat.to_upper()), language: 'c')
    else
        add_project_arguments('-DPUCI_FEATURE_@0@=0'.format(feat.to_upper()), language: 'c')
    endif
endforeach

# ---- Library ABI version (must match include/puci/version.h) ---------------
PUCI_ABI_VERSION = 2

# ---- Dependencies ---------------------------------------------------------
curl_dep = dependency('libcurl')
        
# ---- Top-level summary ----------------------------------------------------
summary({
    'cli': get_option('cli'),
    'tui': get_option('tui'),
    'memory': get_option('memory'),
    'swarm': get_option('swarm'),
    'browser': get_option('browser'),
    'mcp': get_option('mcp'),
    'native arch': get_option('native_arch'),
    'LTO': get_option('lto'),
}, section: 'Feature flags')

# ---- Subdirectories -------------------------------------------------------
subdir('src/lib')
subdir('src/cli')
subdir('examples')
subdir('tests')
subdir('packaging')
if get_option('bench')
    subdir('bench')
endif
if get_option('fuzz')
    subdir('fuzz')
endif
if get_option('lua')
    subdir('bindings/lua')
endif
