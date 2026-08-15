# bindings/lua/meson.build - optional Lua C-module (puci.so).
#
# Built only when -Dlua=true. Requires the Lua 5.1/5.2/5.3/5.4 C development
# headers (any one works: the C ABI is compatible across LuaJIT and Neovim).

cc = meson.get_compiler('c')

lua_dep = dependency('lua5.2', required: false)
if not lua_dep.found()
    lua_dep = dependency('lua5.1', required: false)
endif
if not lua_dep.found()
    lua_dep = dependency('lua', required: false)
endif

# Fallback: probe for the header + library directly (covers distros that ship
# Lua dev files without a pkg-config module, e.g. some Neovim environments).
if not lua_dep.found()
    lua_lib = cc.find_library('lua5.2', required: false)
    if not lua_lib.found()
        lua_lib = cc.find_library('lua5.1', required: false)
    endif
    if not lua_lib.found()
        lua_lib = cc.find_library('lua', required: false)
    endif
    if lua_lib.found() and cc.has_header('lua.h')
        lua_dep = declare_dependency(dependencies: [lua_lib])
    endif
endif

has_lua_header = cc.has_header('lua.h') or cc.has_header('lua5.2/lua.h')

if lua_dep.found()
    shared_module(
        'puci',
        'puci_lua.c',
        name_prefix: '',
        dependencies: [puci_dep, lua_dep],
        install: true,
        install_dir: get_option('libdir') / 'lua',
    )
    # Also provide a Neovim-friendly build without linking -llua (host provides symbols)
    shared_module(
        'puci_nvim',
        'puci_lua.c',
        name_prefix: '',
        name_suffix: 'so',
        dependencies: [puci_dep],
        c_args: ['-I/usr/include/lua5.2', '-I/usr/include'],
        link_args: ['-Wl,--allow-shlib-undefined'],
        install: false,
    )
elif has_lua_header
    # Headers present but no pkg-config: build Neovim-style module without linking lua
    shared_module(
        'puci',
        'puci_lua.c',
        name_prefix: '',
        dependencies: [puci_dep],
        c_args: ['-I/usr/include/lua5.2', '-I/usr/include'],
        link_args: ['-Wl,--allow-shlib-undefined'],
        install: true,
        install_dir: get_option('libdir') / 'lua',
    )
else
    message('Lua dev headers not found; skipping puci.so Lua module')
endif

# Headless smoke test: nvim if available, else lua
nvim = find_program('nvim', required: false)
lua_prog = find_program('lua', required: false)
if lua_prog.found() and (lua_dep.found() or has_lua_header)
    lua_cpath_add = 'package.cpath=package.cpath..";' + meson.current_build_dir() + '/?.so"'
    test('puci_lua_smoke',
        lua_prog,
        args: ['-e', lua_cpath_add + '; require("puci"); print("puci lua smoke ok")'],
        suite: 'lua')
endif
if nvim.found() and (lua_dep.found() or has_lua_header)
    nvim_cpath = 'lua package.cpath=package.cpath..";' + meson.current_build_dir() + '/?.so"; require("puci")'
    test('puci_nvim_headless',
        nvim,
        args: ['--headless', '-u', 'NONE', '-c', nvim_cpath, '-c', 'q'],
        suite: 'lua')
endif
