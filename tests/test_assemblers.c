#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

int main(void) {
    lua_State *state = luaL_newstate();
    if (!state) {
        fprintf(stderr, "Could not create Lua state.\n");
        return 1;
    }
    luaL_requiref(state, LUA_GNAME, luaopen_base, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(state, 1);
    int result = luaL_dofile(state, "tests/test_assemblers.lua");
    if (result != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(state, -1));
    lua_close(state);
    return result == LUA_OK ? 0 : 1;
}
