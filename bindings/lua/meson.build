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

if lua_dep.found()
    shared_module(
        'puci',
        'puci_lua.c',
        name_prefix: '',
        dependencies: [puci_dep, lua_dep],
        install: true,
        install_dir: get_option('libdir') / 'lua',
    )
else
    message('Lua dev headers not found; skipping puci.so Lua module')
endif
