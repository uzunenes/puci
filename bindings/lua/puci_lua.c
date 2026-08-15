/*
 * bindings/lua/puci_lua.c - minimal C-ABI wrapper exposing libpuci to Lua.
 *
 * Targets Lua 5.1 / LuaJIT / Neovim's embedded Lua. A Lua host can:
 *   - create a context and a session (puci.context():session())
 *   - register a Lua function as an agent tool (session:register_tool)
 *   - drive the poll loop manually (session:begin_turn / process / next_event)
 *     or use the blocking send (session:send)
 *   - cancel an in-flight turn (session:cancel)
 *
 * Build (Lua 5.1 / LuaJIT / Neovim all expose the same C ABI):
 *   cc -fPIC -shared puci_lua.c -o puci.so \
 *      $(pkg-config --cflags --libs puci) -llua5.2
 * or without liblua (Neovim links the symbols itself):
 *   cc -fPIC -shared puci_lua.c -o puci.so $(pkg-config --cflags --libs puci)
 */

#include <puci/puci.h>

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>

#if !defined(LUA_VERSION_NUM) || LUA_VERSION_NUM < 501
#error "puci_lua requires Lua 5.1 or newer (LuaJIT and Neovim are fine)"
#endif

typedef struct puci_lua_ctx {
    puci_context *ctx;
} puci_lua_ctx;

typedef struct puci_lua_sess {
    puci_session *sess;
    puci_context *ctx; /* borrowed; owned by the puci_lua_ctx userdata */
    int tool_registry; /* registry ref to a table mapping name -> tool ud */
} puci_lua_sess;

typedef struct lua_tool_userdata {
    lua_State *L;
    int fnref; /* registry ref to the user's Lua function */
} lua_tool_userdata;

#define PUCILUA_CTX "puci.ctx"
#define PUCILUA_SESS "puci.sess"

static puci_lua_sess *check_sess(lua_State *L, int idx) {
    return (puci_lua_sess *)luaL_checkudata(L, idx, PUCILUA_SESS);
}

/* ---- context ---------------------------------------------------------- */

static int l_puci_context(lua_State *L) {
    puci_lua_ctx *c = (puci_lua_ctx *)lua_newuserdata(L, sizeof(*c));
    c->ctx = NULL;
    luaL_getmetatable(L, PUCILUA_CTX);
    lua_setmetatable(L, -2);
    if (puci_context_create(NULL, &c->ctx) != PUCI_OK) {
        return luaL_error(L, "puci: context create failed");
    }
    return 1;
}

static int l_ctx_gc(lua_State *L) {
    puci_lua_ctx *c = (puci_lua_ctx *)luaL_checkudata(L, 1, PUCILUA_CTX);
    if (c && c->ctx) {
        puci_context_destroy(c->ctx);
        c->ctx = NULL;
    }
    return 0;
}

static int l_ctx_session(lua_State *L) {
    puci_lua_ctx *c = (puci_lua_ctx *)luaL_checkudata(L, 1, PUCILUA_CTX);
    const char *base = luaL_optstring(L, 2, "https://api.openai.com/v1");
    const char *model = luaL_optstring(L, 3, "gpt-4o-mini");
    const char *key = luaL_optstring(L, 4, "");

    puci_provider_config pcfg = {
        .name = "openai",
        .base_url = base,
        .api_key = key[0] ? key : NULL,
        .model = model,
        .timeout_ms = 60000,
    };
    puci_context_set_provider(c->ctx, &pcfg);

    puci_session_options sopts = {0};
    sopts.provider = pcfg;
    puci_session *sess = NULL;
    if (puci_session_create(c->ctx, &sopts, &sess) != PUCI_OK) {
        return luaL_error(L, "puci: session create failed");
    }

    puci_lua_sess *s = (puci_lua_sess *)lua_newuserdata(L, sizeof(*s));
    s->sess = sess;
    s->ctx = c->ctx;
    s->tool_registry = LUA_NOREF;
    luaL_getmetatable(L, PUCILUA_SESS);
    lua_setmetatable(L, -2);
    return 1;
}

/* ---- tool bridge ------------------------------------------------------ */

static int l_tool_dispatch(void *ud, const puci_cancel_token *cancel,
                           const char *args_json, char **out_result) {
    (void)cancel;
    lua_tool_userdata *tu = (lua_tool_userdata *)ud;
    if (!tu || !tu->L || tu->fnref == LUA_NOREF) {
        *out_result = strdup("{\"ok\":false,\"error\":\"lua tool unbound\"}");
        return 1;
    }
    lua_State *L = tu->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, tu->fnref);
    lua_pushstring(L, args_json ? args_json : "{}");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        size_t cap = 64 + (err ? strlen(err) : 0);
        *out_result = (char *)malloc(cap);
        if (*out_result) {
            snprintf(*out_result, cap, "{\"ok\":false,\"error\":\"%s\"}",
                     err ? err : "lua error");
        }
        lua_pop(L, 1);
        return 1;
    }
    const char *ret = lua_tostring(L, -1);
    if (!ret) {
        ret = "{}";
    }
    size_t rlen = strlen(ret);
    *out_result = (char *)malloc(rlen + 1);
    if (*out_result) {
        memcpy(*out_result, ret, rlen + 1);
    }
    lua_pop(L, 1);
    return 0;
}

static int l_sess_register_tool(lua_State *L) {
    puci_lua_sess *s = check_sess(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const char *desc = luaL_optstring(L, 3, "");
    luaL_checktype(L, 4, LUA_TFUNCTION);

    lua_pushvalue(L, 4);
    int fnref = luaL_ref(L, LUA_REGISTRYINDEX);

    lua_tool_userdata *tu = (lua_tool_userdata *)malloc(sizeof(*tu));
    if (!tu) {
        luaL_unref(L, LUA_REGISTRYINDEX, fnref);
        return luaL_error(L, "puci: out of memory");
    }
    tu->L = L;
    tu->fnref = fnref;

    puci_tool_spec spec = {
        .name = name,
        .description = desc,
        .input_schema = "{\"type\":\"object\"}",
    };

    puci_status st = puci_tool_register(s->ctx, &spec, l_tool_dispatch, tu);
    if (st != PUCI_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, fnref);
        free(tu);
        return luaL_error(L, "puci: tool register failed (%d)", (int)st);
    }

    /* Keep the tool userdata reachable from the session's registry table. */
    if (s->tool_registry == LUA_NOREF) {
        lua_newtable(L);
        s->tool_registry = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, s->tool_registry);
    lua_pushstring(L, name);
    lua_pushlightuserdata(L, tu);
    lua_settable(L, -3);
    lua_pop(L, 1);

    return 0;
}

/* ---- session driving -------------------------------------------------- */

static int l_sess_send(lua_State *L) {
    puci_lua_sess *s = check_sess(L, 1);
    const char *msg = luaL_checkstring(L, 2);
    lua_pushinteger(L, (lua_Integer)puci_session_send(s->sess, msg));
    return 1;
}

static int l_sess_begin_turn(lua_State *L) {
    puci_lua_sess *s = check_sess(L, 1);
    const char *msg = luaL_checkstring(L, 2);
    lua_pushinteger(L, (lua_Integer)puci_session_begin_turn(s->sess, msg));
    return 1;
}

static int l_sess_process(lua_State *L) {
    puci_lua_sess *s = check_sess(L, 1);
    struct pollfd fds[32];
    size_t n = 0;
    puci_session_get_fds(s->sess, fds, 32, &n);
    int ms = -1;
    puci_session_next_timeout_ms(s->sess, &ms);
    if (ms < 0 || ms > 100) {
        ms = 100;
    }
    (void)poll(n ? fds : NULL, (nfds_t)n, ms);
    lua_pushinteger(L, (lua_Integer)puci_session_process(s->sess, fds, n));
    return 1;
}

static int l_sess_next_event(lua_State *L) {
    puci_lua_sess *s = check_sess(L, 1);
    puci_event ev;
    if (!puci_session_next_event(s->sess, &ev)) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)ev.kind);
    lua_setfield(L, -2, "kind");
    if (ev.text) {
        lua_pushlstring(L, ev.text, ev.text_len);
        lua_setfield(L, -2, "text");
    }
    return 1;
}

static int l_sess_cancel(lua_State *L) {
    puci_lua_sess *s = check_sess(L, 1);
    lua_pushinteger(L, (lua_Integer)puci_session_cancel(s->sess));
    return 1;
}

static int l_sess_last_status(lua_State *L) {
    puci_lua_sess *s = check_sess(L, 1);
    lua_pushinteger(L, (lua_Integer)puci_session_last_status(s->sess));
    return 1;
}

static int l_sess_gc(lua_State *L) {
    puci_lua_sess *s = (puci_lua_sess *)luaL_checkudata(L, 1, PUCILUA_SESS);
    if (s && s->sess) {
        puci_session_destroy(s->sess);
        s->sess = NULL;
    }
    if (s && s->tool_registry != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s->tool_registry);
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            lua_tool_userdata *tu = (lua_tool_userdata *)lua_touserdata(L, -1);
            if (tu) {
                luaL_unref(L, LUA_REGISTRYINDEX, tu->fnref);
                free(tu);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        luaL_unref(L, LUA_REGISTRYINDEX, s->tool_registry);
        s->tool_registry = LUA_NOREF;
    }
    return 0;
}

/* ---- module registration --------------------------------------------- */

static const luaL_Reg ctx_methods[] = {
    {"session", l_ctx_session},
    {NULL, NULL},
};

static const luaL_Reg sess_methods[] = {
    {"register_tool", l_sess_register_tool},
    {"send", l_sess_send},
    {"begin_turn", l_sess_begin_turn},
    {"process", l_sess_process},
    {"next_event", l_sess_next_event},
    {"cancel", l_sess_cancel},
    {"last_status", l_sess_last_status},
    {NULL, NULL},
};

static void register_metatable(lua_State *L, const char *name,
                               const luaL_Reg *methods) {
    luaL_newmetatable(L, name);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
#if LUA_VERSION_NUM >= 502
    luaL_setfuncs(L, methods, 0);
#else
    luaL_register(L, NULL, methods);
#endif
    lua_pushcfunction(L, strcmp(name, PUCILUA_CTX) == 0 ? l_ctx_gc : l_sess_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
}

int luaopen_puci(lua_State *L) {
    register_metatable(L, PUCILUA_CTX, ctx_methods);
    register_metatable(L, PUCILUA_SESS, sess_methods);

    lua_newtable(L);
    lua_pushcfunction(L, l_puci_context);
    lua_setfield(L, -2, "context");
    return 1;
}
