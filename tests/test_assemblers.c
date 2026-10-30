#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* read_file(path): the file's contents, so the tests can assemble the
   example sources. Tinker Editor's Lua has no io library to do it with. */
static int read_file(lua_State *L) {
    FILE *file = fopen(luaL_checkstring(L, 1), "rb");
    if (!file) return luaL_error(L, "cannot open %s", lua_tostring(L, 1));
    luaL_Buffer buffer;
    luaL_buffinit(L, &buffer);
    char chunk[4096];
    size_t read;
    while ((read = fread(chunk, 1, sizeof(chunk), file)) > 0) luaL_addlstring(&buffer, chunk, read);
    fclose(file);
    luaL_pushresult(&buffer);
    return 1;
}

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
    lua_register(state, "read_file", read_file);
    int result = luaL_dofile(state, "tests/test_assemblers.lua");
    if (result != LUA_OK) fprintf(stderr, "%s\n", lua_tostring(state, -1));
    lua_close(state);
    return result == LUA_OK ? 0 : 1;
}
