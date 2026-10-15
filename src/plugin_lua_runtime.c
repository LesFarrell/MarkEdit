/* Lua extension host: the sandboxed Lua state, memory limit, time limit
   and protected calls. */
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "plugin_host_internal.h"

#include "lauxlib.h"
#include "lualib.h"

lua_State *s_lua = NULL;
/* Built-in commands are trusted not to hang, and may legitimately take
   longer than the limit on a large document. */
int s_timeLimited = 1;
LuaMemory s_memory = { 0, PLUGIN_MEMORY_LIMIT_BYTES };
static ULONGLONG s_callStarted = 0;

static void *limited_alloc(void *ud, void *pointer, size_t oldSize, size_t newSize) {
    LuaMemory *memory = (LuaMemory *)ud;
    /* Lua passes an object-type tag rather than a byte count in oldSize for
       a fresh allocation. */
    if (!pointer) oldSize = 0;
    if (!newSize) {
        free(pointer);
        memory->used = oldSize <= memory->used ? memory->used - oldSize : 0;
        return NULL;
    }
    if (newSize > oldSize && newSize - oldSize > memory->limit - memory->used) return NULL;
    void *resized = realloc(pointer, newSize);
    if (!resized) return NULL;
    if (newSize >= oldSize) memory->used += newSize - oldSize;
    else memory->used -= oldSize - newSize;
    return resized;
}

char *dup_string(const char *value) {
    if (!value) value = "unknown error";
    size_t bytes = strlen(value) + 1;
    char *copy = (char *)malloc(bytes);
    if (copy) memcpy(copy, value, bytes);
    return copy;
}

static void push_shallow_table_copy(lua_State *L, int sourceIndex) {
    sourceIndex = lua_absindex(L, sourceIndex);
    lua_newtable(L);
    int copyIndex = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, sourceIndex)) {
        lua_pushvalue(L, -2);
        lua_pushvalue(L, -2);
        lua_rawset(L, copyIndex);
        lua_pop(L, 1);
    }
}

/* Pushes a private global environment for owner. Standard-library and API
   tables are shallow-copied so a plugin cannot modify another plugin's
   string/table/editor/etc tables. Functions themselves remain shared. The
   string library behind string methods is guarded separately, by
   plugin_lua_protect_string_metatable(). */
static void push_plugin_environment(lua_State *L, const char *owner) {
    lua_getfield(L, LUA_REGISTRYINDEX, "markedit.environments");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "markedit.environments");
    }
    lua_getfield(L, -1, owner);
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2);
        return;
    }
    lua_pop(L, 1);
    lua_newtable(L);                       /* environments, env */
    int environmentIndex = lua_gettop(L);
    lua_pushglobaltable(L);                /* environments, env, globals */
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        lua_pushvalue(L, -2);              /* copied key */
        if (lua_istable(L, -2)) push_shallow_table_copy(L, -2);
        else lua_pushvalue(L, -2);
        lua_rawset(L, environmentIndex);   /* env[copied key] = copied value */
        lua_pop(L, 1);
    }
    lua_pop(L, 1);                         /* environments, env */
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "_G");
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, owner);
    lua_remove(L, -2);
}

void set_chunk_environment(lua_State *L, int functionIndex, const char *owner) {
    functionIndex = lua_absindex(L, functionIndex);
    push_plugin_environment(L, owner);
    if (!lua_setupvalue(L, functionIndex, 1)) lua_pop(L, 1);
}

const char *checked_cstring(lua_State *L, int index, const char *what) {
    size_t length = 0;
    const char *value = luaL_checklstring(L, index, &length);
    if (memchr(value, 0, length))
        luaL_error(L, "%s must not contain NUL bytes", what);
    if (length > INT_MAX || (length && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value, (int)length, NULL, 0) <= 0))
        luaL_error(L, "%s must be valid UTF-8", what);
    return value;
}

const char *optional_cstring(lua_State *L, int index, const char *fallback,
                                    const char *what) {
    return lua_isnoneornil(L, index) ? fallback : checked_cstring(L, index, what);
}

char *append_line(char *report, char *line) {
    if (!line) return report;
    if (!report) return line;
    size_t reportLength = strlen(report), lineLength = strlen(line);
    char *joined = (char *)realloc(report, reportLength + 1 + lineLength + 1);
    if (!joined) { free(line); return report; }
    joined[reportLength] = '\n';
    memcpy(joined + reportLength + 1, line, lineLength + 1);
    free(line);
    return joined;
}

static void time_limit_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    if (s_timeLimited && GetTickCount64() - s_callStarted > PLUGIN_TIME_LIMIT_MS)
        luaL_error(L, "stopped after %d seconds without finishing", PLUGIN_TIME_LIMIT_MS / 1000);
}

static int traceback_handler(lua_State *L) {
    const char *message = lua_tostring(L, 1);
    luaL_traceback(L, L, message ? message : "(error object is not a string)", 1);
    return 1;
}

/* Calls the function under its nargs arguments on the stack top, with the
   time limit armed, and leaves the stack as it was below the function. */
char *protected_call(int nargs) {
    int base = lua_gettop(s_lua) - nargs;
    lua_pushcfunction(s_lua, traceback_handler);
    lua_insert(s_lua, base);
    s_callStarted = GetTickCount64();
    int status = lua_pcall(s_lua, nargs, 0, base);
    char *error = status == LUA_OK ? NULL : dup_string(lua_tostring(s_lua, -1));
    lua_settop(s_lua, base - 1);
    return error;
}

/* Time spent waiting on the user doesn't count against the plugin. */
void restart_time_limit(void) {
    s_callStarted = GetTickCount64();
}

/* A Lua state whose allocations are counted in *memory and refused beyond
   PLUGIN_MEMORY_LIMIT_BYTES, or NULL when memory ran out. *memory must
   outlive the state. */
lua_State *plugin_lua_new_state(LuaMemory *memory) {
    memory->used = 0;
    memory->limit = PLUGIN_MEMORY_LIMIT_BYTES;
    return lua_newstate(limited_alloc, memory);
}

/* Starts the sandboxed state, or returns NULL when memory ran out. */
lua_State *plugin_lua_start(void) {
    s_lua = plugin_lua_new_state(&s_memory);
    return s_lua;
}

/* Every string shares one metatable, whose __index is the original string
   library rather than any plugin's copy of it, so a plugin that reached it
   through getmetatable("") could change ("x"):upper() for every plugin.
   A __metatable field makes getmetatable("") return false instead; method
   calls still find the original functions. */
void plugin_lua_protect_string_metatable(lua_State *L) {
    lua_pushliteral(L, "");
    if (lua_getmetatable(L, -1)) {
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "__metatable");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

/* Arms the time limit, so a plugin that does not finish is stopped. */
void plugin_lua_install_hook(lua_State *L) {
    lua_sethook(L, time_limit_hook, LUA_MASKCOUNT, 10000);
}