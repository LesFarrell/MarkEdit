#ifndef MARKEDIT_LUA_EDITOR_API_H
#define MARKEDIT_LUA_EDITOR_API_H

#include <windows.h>
#include "lua.h"
#include "plugin_host.h"

void lua_editor_api_set_callbacks(const PluginHostCallbacks *callbacks);
void lua_editor_api_set_editor(HWND editor);
void lua_editor_api_require_editor(lua_State *L);
void lua_editor_api_push(lua_State *L);

#endif
