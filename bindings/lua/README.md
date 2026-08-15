# puci Lua bindings

Minimal C-ABI wrapper exposing `libpuci` to Lua 5.1 / LuaJIT / Neovim. It lets a
Lua host embed an agent and drive it from its own event loop, or register Lua
functions as agent tools.

## Build

Any Lua C ABI works (5.1, LuaJIT, 5.2, 5.3, 5.4, Neovim):

```sh
cc -fPIC -shared puci_lua.c -o puci.so \
   $(pkg-config --cflags --libs puci) -llua5.2
```

For Neovim (which already links the Lua symbols), omit `-llua`:

```sh
cc -fPIC -shared puci_lua.c -o puci.so $(pkg-config --cflags --libs puci)
```

Or via Meson (builds when a Lua dev package is present):

```sh
meson setup build -Dlua=true && ninja -C build
```

## Usage

```lua
local puci = require("puci")

-- Create a context and a session (base_url, model, api_key are optional).
local ctx = puci.context()
local sess = ctx:session("https://api.openai.com/v1", "gpt-4o-mini", "")

-- Register a Lua function as an agent tool. The function receives the tool's
-- JSON argument string and must return a JSON result string.
sess:register_tool("my_lua_tool", "A custom Lua tool", function(args_json)
    return '{"ok":true,"text":"hello from lua"}'
end)

-- Blocking turn:
local status = sess:send("run my_lua_tool")

-- Non-blocking turn (host owns the poll loop):
sess:begin_turn("run my_lua_tool")
while true do
    local st = sess:process()
    local ev = sess:next_event()
    while ev do
        -- ev.kind and ev.text are populated
        ev = sess:next_event()
    end
    if st ~= 0 then break end
end
```

## Notes

- The tool bridge passes the tool's JSON arguments as a single string to the
  Lua function and treats the function's string return value as the tool result.
  A production wrapper would parse/construct JSON objects; this binding stays
  dependency-free and string-oriented by design.
- `session:process()` performs one bounded, non-blocking step (100 ms max poll),
  so a Lua host can interleave it with its own UI/event-loop work.
