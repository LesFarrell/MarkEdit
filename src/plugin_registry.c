/* Lua extension host: the API plugins see (ui.*, menu.*, events.*,
   commands.*, config.*, app.*, documents.*; editor.* lives in
   lua_editor_api.c), the command and handler registries, and running a
   registered command or event. */
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "plugin_host_internal.h"

#include "lua_editor_api.h"
#include "text_codec.h"
#include "../third_party/scintilla/include/Scintilla.h"

#include "lauxlib.h"
#include "lualib.h"

static const char *const EVENT_NAMES[] = {
    "before_save", "save", "after_save", "open", "close", "tab_changed",
    "text_changed", "selection_changed", "workspace_changed", "theme_changed", "shutdown"
};

PluginCommand s_commands[PLUGIN_MAX_COMMANDS];
int s_commandCount = 0;
PluginHandler s_handlers[PLUGIN_MAX_HANDLERS];
int s_handlerCount = 0;
int s_nextHandlerId = 1;
PluginDeferred s_deferred[PLUGIN_MAX_DEFERRED];
int s_nextDeferredToken = 1;

static const char *current_owner(lua_State *L);

void discard_commands_from(int first) {
    for (int i = first; i < s_commandCount; i++) {
        if (s_lua) luaL_unref(s_lua, LUA_REGISTRYINDEX, s_commands[i].ref);
        free(s_commands[i].label);
        free(s_commands[i].overrides);
        free(s_commands[i].owner);
        free(s_commands[i].configKey);
        free(s_commands[i].shortcut);
        memset(&s_commands[i], 0, sizeof(s_commands[i]));
    }
    if (first < s_commandCount) s_commandCount = first;
}

void discard_handlers_from(int first) {
    for (int i = first; i < s_handlerCount; i++) {
        if (s_lua) luaL_unref(s_lua, LUA_REGISTRYINDEX, s_handlers[i].ref);
        free(s_handlers[i].label);
        free(s_handlers[i].owner);
        memset(&s_handlers[i], 0, sizeof(s_handlers[i]));
    }
    if (first < s_handlerCount) s_handlerCount = first;
}

static const char *current_title(void) {
    return s_runningLabel ? s_runningLabel : "MarkEdit Plugin";
}

static int ui_message(lua_State *L) {
    const char *text = checked_cstring(L, 1, "message text");
    const char *title = optional_cstring(L, 2, current_title(), "message title");
    static const char *const kinds[] = { "info", "warning", "error", NULL };
    const char *kind = kinds[luaL_checkoption(L, 3, "info", kinds)];
    if (!s_callbacks.message) return luaL_error(L, "ui.message is not available here");
    s_callbacks.message(title, text, kind);
    restart_time_limit();
    return 0;
}

static int ui_prompt(lua_State *L) {
    lua_editor_api_require_editor(L);
    const char *label = checked_cstring(L, 1, "prompt label");
    const char *initial = optional_cstring(L, 2, "", "prompt default");
    const char *title = optional_cstring(L, 3, current_title(), "prompt title");
    if (!s_callbacks.prompt) return luaL_error(L, "ui.prompt is not available here");
    char *answer = s_callbacks.prompt(title, label, initial);
    restart_time_limit();
    if (answer) lua_pushstring(L, answer);
    else lua_pushnil(L);
    free(answer);
    return 1;
}

static int ui_choose(lua_State *L) {
    lua_editor_api_require_editor(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    const char *label = optional_cstring(L, 2, "Choose an item:", "choice label");
    const char *title = optional_cstring(L, 3, current_title(), "choice title");
    lua_Integer count = luaL_len(L, 1);
    if (count < 1 || count > PLUGIN_MAX_CHOICES)
        return luaL_error(L, "ui.choose needs between 1 and %d items", PLUGIN_MAX_CHOICES);
    if (!s_callbacks.choose) return luaL_error(L, "ui.choose is not available here");
    /* Userdata, so the array is freed even if a bad item raises an error.
       The pointers stay valid because the table keeps the strings alive. */
    const char **items = (const char **)lua_newuserdatauv(L, (size_t)count * sizeof(*items), 0);
    for (lua_Integer i = 0; i < count; i++) {
        if (lua_geti(L, 1, i + 1) != LUA_TSTRING)
            return luaL_error(L, "ui.choose item %d is not a string", (int)(i + 1));
        items[i] = checked_cstring(L, -1, "ui.choose item");
        lua_pop(L, 1);
    }
    int chosen = s_callbacks.choose(title, label, items, (int)count);
    restart_time_limit();
    if (chosen < 0 || chosen >= count) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, chosen + 1);
    lua_geti(L, 1, chosen + 1);
    return 2;
}

static int ui_save_binary(lua_State *L) {
    lua_editor_api_require_editor(L);
    const char *defaultName = checked_cstring(L, 1, "default file name");
    size_t length = 0;
    const char *data = luaL_checklstring(L, 2, &length);
    const char *title = optional_cstring(L, 3, current_title(), "dialog title");
    if (!defaultName[0]) return luaL_argerror(L, 1, "default file name must not be empty");
    if (length > PLUGIN_MAX_BINARY_BYTES)
        return luaL_error(L, "binary output exceeds the %d MiB limit",
                          PLUGIN_MAX_BINARY_BYTES / (1024 * 1024));
    if (!s_callbacks.save_binary)
        return luaL_error(L, "ui.save_binary is not available here");
    int result = s_callbacks.save_binary(title, defaultName, data, length);
    restart_time_limit();
    if (result < 0) return luaL_error(L, "the binary file could not be written");
    lua_pushboolean(L, result > 0);
    return 1;
}

static int ui_confirm(lua_State *L) {
    lua_editor_api_require_editor(L);
    const char *text = checked_cstring(L, 1, "confirmation text");
    const char *title = optional_cstring(L, 2, current_title(), "confirmation title");
    if (!s_callbacks.confirm) return luaL_error(L, "ui.confirm is not available here");
    int answer = s_callbacks.confirm(title, text);
    restart_time_limit();
    lua_pushboolean(L, answer);
    return 1;
}

static int ui_status(lua_State *L) {
    const char *value = checked_cstring(L, 1, "status text");
    if (!s_callbacks.status) return luaL_error(L, "ui.status is not available here");
    s_callbacks.status(value);
    return 0;
}

static int ui_log(lua_State *L) {
    const char *value = checked_cstring(L, 1, "log text");
    if (s_callbacks.log) s_callbacks.log(current_owner(L), value);
    else OutputDebugStringA(value);
    return 0;
}

static int ui_pick_path(lua_State *L, int folder) {
    lua_editor_api_require_editor(L);
    const char *title = optional_cstring(L, 1,
        folder ? "Choose Folder" : "Open File", "dialog title");
    if (!s_callbacks.pick_path) return luaL_error(L, "path picker is not available here");
    char *path = s_callbacks.pick_path(title, folder);
    restart_time_limit();
    if (path) lua_pushstring(L, path); else lua_pushnil(L);
    free(path);
    return 1;
}

static int ui_pick_file(lua_State *L) { return ui_pick_path(L, 0); }
static int ui_pick_folder(lua_State *L) { return ui_pick_path(L, 1); }

static int parse_shortcut(const char *value, BYTE *flags, WORD *key) {
    char copy[64];
    strcpy(copy, value);
    *flags = FVIRTKEY;
    *key = 0;
    int ctrlOrAlt = 0;
    char *context = NULL;
    for (char *part = strtok_s(copy, "+", &context); part;
         part = strtok_s(NULL, "+", &context)) {
        if (_stricmp(part, "Ctrl") == 0) { *flags |= FCONTROL; ctrlOrAlt = 1; }
        else if (_stricmp(part, "Alt") == 0) { *flags |= FALT; ctrlOrAlt = 1; }
        else if (_stricmp(part, "Shift") == 0) *flags |= FSHIFT;
        else if (!*key && strlen(part) == 1 && isalnum((unsigned char)part[0]))
            *key = (WORD)toupper((unsigned char)part[0]);
        else if (!*key && (part[0] == 'F' || part[0] == 'f')) {
            char *end = NULL;
            long number = strtol(part + 1, &end, 10);
            if (!end || *end || number < 1 || number > 24) return 0;
            *key = (WORD)(VK_F1 + number - 1);
        } else if (!*key && _stricmp(part, "Tab") == 0) *key = VK_TAB;
        else if (!*key && _stricmp(part, "Space") == 0) *key = VK_SPACE;
        else if (!*key && _stricmp(part, "Insert") == 0) *key = VK_INSERT;
        else if (!*key && _stricmp(part, "Delete") == 0) *key = VK_DELETE;
        else return 0;
    }
    return ctrlOrAlt && *key;
}

static char *checked_shortcut(lua_State *L, int index, BYTE *flags, WORD *key) {
    if (lua_isnoneornil(L, index)) return NULL;
    const char *value = checked_cstring(L, index, "shortcut");
    size_t length = strlen(value);
    if (!length || length > 48) luaL_argerror(L, index, "shortcut must be 1 to 48 characters");
    if (!parse_shortcut(value, flags, key))
        luaL_argerror(L, index, "shortcut needs Ctrl or Alt, optional Shift, and one supported key");
    char *copy = dup_string(value);
    if (!copy) luaL_error(L, "out of memory");
    return copy;
}

static int add_labelled_command(lua_State *L, const char *label, int functionIndex,
                                int separator, int shortcutIndex) {
    if (s_commandCount >= PLUGIN_MAX_COMMANDS)
        return luaL_error(L, "too many plugin commands (the limit is %d)", PLUGIN_MAX_COMMANDS);
    BYTE shortcutFlags = 0;
    WORD shortcutKey = 0;
    char *shortcut = shortcutIndex
        ? checked_shortcut(L, shortcutIndex, &shortcutFlags, &shortcutKey) : NULL;
    int ref = LUA_NOREF;
    if (!separator) {
        lua_pushvalue(L, functionIndex);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    char *copy = dup_string(label);
    char *owner = dup_string(s_loadingName ? s_loadingName : "plugin");
    if (!copy || !owner) {
        free(copy);
        free(owner);
        free(shortcut);
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return luaL_error(L, "out of memory");
    }
    s_commands[s_commandCount].label = copy;
    s_commands[s_commandCount].ref = ref;
    s_commands[s_commandCount].placed = 0;
    s_commands[s_commandCount].overrides = NULL;
    s_commands[s_commandCount].builtIn = s_loadingBuiltIn;
    s_commands[s_commandCount].separator = separator;
    s_commands[s_commandCount].owner = owner;
    s_commands[s_commandCount].configKey = NULL;
    s_commands[s_commandCount].checkDefault = 0;
    s_commands[s_commandCount].shortcut = shortcut;
    s_commands[s_commandCount].shortcutFlags = shortcutFlags;
    s_commands[s_commandCount].shortcutKey = shortcutKey;
    s_commandCount++;
    return 0;
}

static int menu_add(lua_State *L) {
    const char *label = checked_cstring(L, 1, "menu label");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    return add_labelled_command(L, label, 2, 0, 3);
}

static const char *checked_config_key(lua_State *L, int index) {
    size_t length = 0;
    const char *key = luaL_checklstring(L, index, &length);
    luaL_argcheck(L, length > 0 && length <= 64, index, "config key must be 1 to 64 characters");
    for (size_t i = 0; i < length; i++)
        luaL_argcheck(L, isalnum((unsigned char)key[i]) || key[i] == '_' ||
                      key[i] == '-' || key[i] == '.', index,
                      "config key may contain only letters, digits, '.', '_' and '-'");
    return key;
}

/* menu.checkbox(label, key, default, fn): a checkable command whose state is
   stored automatically. fn receives the new boolean state after each click. */
static int menu_checkbox(lua_State *L) {
    const char *label = checked_cstring(L, 1, "menu label");
    const char *key = checked_config_key(L, 2);
    luaL_checktype(L, 3, LUA_TBOOLEAN);
    luaL_checktype(L, 4, LUA_TFUNCTION);
    int index = s_commandCount;
    add_labelled_command(L, label, 4, 0, 5);
    char *copy = dup_string(key);
    if (!copy) {
        discard_commands_from(index);
        return luaL_error(L, "out of memory");
    }
    s_commands[index].configKey = copy;
    s_commands[index].checkDefault = lua_toboolean(L, 3);
    return 0;
}

/* menu.separator([path]): a separator after whatever this plugin and those
   before it have put in the menu path names so far - the Plugins menu when
   path is omitted, "Lines" its Lines submenu, "/Snippets/Markdown" a
   submenu of a menu-bar menu. */
static int menu_separator(lua_State *L) {
    const char *path = optional_cstring(L, 1, "", "menu path");
    return add_labelled_command(L, path, 0, 1, 0);
}

/* commands.register(name, fn [, title]): fn runs in place of the built-in
   command called name, wherever it is invoked from (menu, toolbar
   or shortcut). The last registration for a name wins. title names it in
   dialogs. Whether name is a real command is up to the application. */
static int commands_register(lua_State *L) {
    const char *name = checked_cstring(L, 1, "command name");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const char *title = optional_cstring(L, 3, "MarkEdit", "command title");
    if (s_commandCount >= PLUGIN_MAX_COMMANDS)
        return luaL_error(L, "too many plugin commands (the limit is %d)", PLUGIN_MAX_COMMANDS);
    char *label = dup_string(title);
    char *overrides = dup_string(name);
    if (!label || !overrides) {
        free(label);
        free(overrides);
        return luaL_error(L, "out of memory");
    }
    lua_pushvalue(L, 2);
    PluginCommand *command = &s_commands[s_commandCount++];
    command->label = label;
    command->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    command->placed = 1;
    command->overrides = overrides;
    command->builtIn = s_loadingBuiltIn;
    command->separator = 0;
    command->owner = dup_string(s_loadingName ? s_loadingName : "plugin");
    command->configKey = NULL;
    command->checkDefault = 0;
    command->shortcut = NULL;
    command->shortcutFlags = 0;
    command->shortcutKey = 0;
    if (!command->owner) {
        discard_commands_from(s_commandCount - 1);
        return luaL_error(L, "out of memory");
    }
    return 0;
}

/* events.on(name, fn): fn(path) runs on each such event. */
static int events_on(lua_State *L) {
    const char *name = checked_cstring(L, 1, "event name");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const char *event = NULL;
    for (size_t i = 0; i < sizeof(EVENT_NAMES) / sizeof(EVENT_NAMES[0]); i++)
        if (strcmp(name, EVENT_NAMES[i]) == 0) event = EVENT_NAMES[i];
    if (!event) return luaL_error(L, "unknown event '%s'", name);
    if (s_handlerCount >= PLUGIN_MAX_HANDLERS)
        return luaL_error(L, "too many event handlers (the limit is %d)", PLUGIN_MAX_HANDLERS);
    const char *owner = s_loadingName ? s_loadingName
                                      : s_runningOwner ? s_runningOwner : current_title();
    char *label = (char *)malloc(strlen(owner) + strlen(event) + 3);
    if (!label) return luaL_error(L, "out of memory");
    sprintf(label, "%s: %s", owner, event);
    lua_pushvalue(L, 2);
    s_handlers[s_handlerCount].event = event;
    s_handlers[s_handlerCount].label = label;
    s_handlers[s_handlerCount].ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s_handlers[s_handlerCount].owner = dup_string(owner);
    if (!s_handlers[s_handlerCount].owner) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_handlers[s_handlerCount].ref);
        free(s_handlers[s_handlerCount].label);
        return luaL_error(L, "out of memory");
    }
    s_handlers[s_handlerCount].id = s_nextHandlerId;
    s_nextHandlerId = s_nextHandlerId == INT_MAX ? 1 : s_nextHandlerId + 1;
    lua_pushinteger(L, s_handlers[s_handlerCount].id);
    s_handlerCount++;
    return 1;
}

static int events_off(lua_State *L) {
    int id = (int)luaL_checkinteger(L, 1);
    const char *owner = current_owner(L);
    for (int i = 0; i < s_handlerCount; i++) {
        if (s_handlers[i].id != id || strcmp(s_handlers[i].owner, owner) != 0) continue;
        luaL_unref(L, LUA_REGISTRYINDEX, s_handlers[i].ref);
        free(s_handlers[i].label);
        free(s_handlers[i].owner);
        memmove(&s_handlers[i], &s_handlers[i + 1],
                (size_t)(s_handlerCount - i - 1) * sizeof(s_handlers[0]));
        s_handlerCount--;
        memset(&s_handlers[s_handlerCount], 0, sizeof(s_handlers[0]));
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}

static const char *current_owner(lua_State *L) {
    const char *owner = s_loadingName ? s_loadingName : s_runningOwner;
    if (!owner) luaL_error(L, "config is only available while loading or running a plugin");
    return owner;
}

#define CONFIG_TABLE_PREFIX "@ME1:"
#define CONFIG_MAX_DEPTH 32

static void encode_config_value(lua_State *L, int index, luaL_Buffer *buffer, int depth) {
    if (depth > CONFIG_MAX_DEPTH) luaL_error(L, "config table is nested too deeply");
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
        case LUA_TNIL: luaL_addchar(buffer, 'N'); return;
        case LUA_TBOOLEAN:
            luaL_addstring(buffer, lua_toboolean(L, index) ? "B1" : "B0"); return;
        case LUA_TNUMBER: {
            char number[64];
            if (lua_isinteger(L, index))
                snprintf(number, sizeof(number), "I%lld;", (long long)lua_tointeger(L, index));
            else {
                double value = (double)lua_tonumber(L, index);
                if (!isfinite(value)) luaL_error(L, "config numbers must be finite");
                snprintf(number, sizeof(number), "D%.17g;", value);
            }
            luaL_addstring(buffer, number); return;
        }
        case LUA_TSTRING: {
            static const char HEX[] = "0123456789abcdef";
            size_t length;
            const unsigned char *value = (const unsigned char *)lua_tolstring(L, index, &length);
            char size[32]; snprintf(size, sizeof(size), "S%zu:", length);
            luaL_addstring(buffer, size);
            for (size_t i = 0; i < length; i++) {
                luaL_addchar(buffer, HEX[value[i] >> 4]);
                luaL_addchar(buffer, HEX[value[i] & 15]);
            }
            return;
        }
        case LUA_TTABLE: {
            size_t arrayLength = lua_rawlen(L, index), count = 0;
            int array = 1;
            lua_pushnil(L);
            while (lua_next(L, index)) {
                count++;
                if (!lua_isinteger(L, -2)) array = 0;
                else {
                    lua_Integer key = lua_tointeger(L, -2);
                    if (key < 1 || (size_t)key > arrayLength) array = 0;
                }
                lua_pop(L, 1);
                if (count > 10000) luaL_error(L, "config table has too many entries");
            }
            if (array && count == arrayLength) {
                char header[32]; snprintf(header, sizeof(header), "A%zu;", arrayLength);
                luaL_addstring(buffer, header);
                for (size_t i = 1; i <= arrayLength; i++) {
                    lua_rawgeti(L, index, (lua_Integer)i);
                    encode_config_value(L, -1, buffer, depth + 1);
                    lua_pop(L, 1);
                }
            } else {
                char header[32]; snprintf(header, sizeof(header), "O%zu;", count);
                luaL_addstring(buffer, header);
                lua_pushnil(L);
                while (lua_next(L, index)) {
                    if (lua_type(L, -2) != LUA_TSTRING)
                        luaL_error(L, "config object keys must be strings");
                    encode_config_value(L, -2, buffer, depth + 1);
                    encode_config_value(L, -1, buffer, depth + 1);
                    lua_pop(L, 1);
                }
            }
            return;
        }
        default: luaL_error(L, "config values cannot contain %s", luaL_typename(L, index));
    }
}

static int decode_size(const char **cursor, const char *end, char delimiter, size_t *value) {
    size_t result = 0;
    const char *p = *cursor;
    if (p >= end || !isdigit((unsigned char)*p)) return 0;
    while (p < end && isdigit((unsigned char)*p)) {
        size_t digit = (size_t)(*p++ - '0');
        if (result > (SIZE_MAX - digit) / 10) return 0;
        result = result * 10 + digit;
    }
    if (p >= end || *p++ != delimiter) return 0;
    *cursor = p; *value = result; return 1;
}

static int decode_config_value(lua_State *L, const char **cursor, const char *end, int depth) {
    if (depth > CONFIG_MAX_DEPTH || *cursor >= end) return 0;
    char type = *(*cursor)++;
    if (type == 'N') { lua_pushnil(L); return 1; }
    if (type == 'B' && *cursor < end && (**cursor == '0' || **cursor == '1')) {
        lua_pushboolean(L, *(*cursor)++ == '1'); return 1;
    }
    if (type == 'I' || type == 'D') {
        const char *finish = memchr(*cursor, ';', (size_t)(end - *cursor));
        if (!finish || finish == *cursor || (size_t)(finish - *cursor) >= 64) return 0;
        char number[64]; memcpy(number, *cursor, (size_t)(finish - *cursor));
        number[finish - *cursor] = 0;
        char *parsedEnd = NULL;
        if (type == 'I') {
            long long value = strtoll(number, &parsedEnd, 10);
            if (!parsedEnd || *parsedEnd) return 0;
            lua_pushinteger(L, (lua_Integer)value);
        } else {
            double value = strtod(number, &parsedEnd);
            if (!parsedEnd || *parsedEnd || !isfinite(value)) return 0;
            lua_pushnumber(L, (lua_Number)value);
        }
        *cursor = finish + 1; return 1;
    }
    if (type == 'S') {
        size_t length;
        if (!decode_size(cursor, end, ':', &length) || length > (size_t)(end - *cursor) / 2) return 0;
        luaL_Buffer buffer;
        char *target = luaL_buffinitsize(L, &buffer, length);
        for (size_t i = 0; i < length; i++) {
            int high = hex_digit_value((WCHAR)(*cursor)[i * 2]);
            int low = hex_digit_value((WCHAR)(*cursor)[i * 2 + 1]);
            if (high < 0 || low < 0) return 0;
            target[i] = (char)((high << 4) | low);
        }
        *cursor += length * 2;
        luaL_pushresultsize(&buffer, length);
        return 1;
    }
    if (type == 'A' || type == 'O') {
        size_t count;
        if (!decode_size(cursor, end, ';', &count) || count > 10000) return 0;
        lua_createtable(L, type == 'A' ? (int)count : 0, type == 'O' ? (int)count : 0);
        for (size_t i = 0; i < count; i++) {
            if (type == 'A') {
                if (!decode_config_value(L, cursor, end, depth + 1)) return 0;
                lua_rawseti(L, -2, (lua_Integer)i + 1);
            } else {
                if (!decode_config_value(L, cursor, end, depth + 1) ||
                    lua_type(L, -1) != LUA_TSTRING ||
                    !decode_config_value(L, cursor, end, depth + 1)) return 0;
                lua_rawset(L, -3);
            }
        }
        return 1;
    }
    return 0;
}

static int config_get(lua_State *L) {
    const char *key = checked_config_key(L, 1);
    char *value = s_callbacks.config_get ? s_callbacks.config_get(current_owner(L), key) : NULL;
    if (!value) {
        if (lua_gettop(L) >= 2) lua_pushvalue(L, 2);
        else lua_pushnil(L);
        return 1;
    }
    if (lua_istable(L, 2)) {
        size_t prefix = sizeof(CONFIG_TABLE_PREFIX) - 1, length = strlen(value);
        const char *cursor = value + prefix, *end = value + length;
        if (length < prefix || memcmp(value, CONFIG_TABLE_PREFIX, prefix) != 0 ||
            !decode_config_value(L, &cursor, end, 0) || cursor != end) {
            lua_settop(L, 2);
            lua_pushvalue(L, 2);
        }
    } else if (lua_type(L, 2) == LUA_TBOOLEAN) {
        lua_pushboolean(L, strcmp(value, "1") == 0 || _stricmp(value, "true") == 0 ||
                           _stricmp(value, "yes") == 0 || _stricmp(value, "on") == 0);
    } else if (lua_type(L, 2) == LUA_TNUMBER) {
        char *end = NULL;
        double number = strtod(value, &end);
        if (end && *end == 0) lua_pushnumber(L, number);
        else lua_pushvalue(L, 2);
    } else {
        lua_pushstring(L, value);
    }
    free(value);
    return 1;
}

static int config_set(lua_State *L) {
    const char *key = checked_config_key(L, 1);
    const char *value = NULL;
    char number[64];
    char *structured = NULL;
    switch (lua_type(L, 2)) {
        case LUA_TNIL: break;
        case LUA_TBOOLEAN: value = lua_toboolean(L, 2) ? "1" : "0"; break;
        case LUA_TNUMBER:
            snprintf(number, sizeof(number), "%.17g", (double)lua_tonumber(L, 2));
            value = number;
            break;
        case LUA_TSTRING: value = checked_cstring(L, 2, "config value"); break;
        case LUA_TTABLE: {
            luaL_Buffer buffer; luaL_buffinit(L, &buffer);
            luaL_addstring(&buffer, CONFIG_TABLE_PREFIX);
            encode_config_value(L, 2, &buffer, 0);
            luaL_pushresult(&buffer);
            structured = dup_string(lua_tostring(L, -1));
            lua_pop(L, 1);
            if (!structured) return luaL_error(L, "out of memory");
            value = structured;
            break;
        }
        default: return luaL_argerror(L, 2, "expected string, number, boolean or nil");
    }
    if (!s_callbacks.config_set || !s_callbacks.config_set(current_owner(L), key, value)) {
        free(structured);
        return luaL_error(L, "could not write the plugin setting");
    }
    free(structured);
    return 0;
}

/* Safe clock access for plugins. The full Lua os library remains closed
   because it also exposes files, processes, environment variables and locale
   mutation. These fixed formats are suitable for inserting into documents. */
static int app_date(lua_State *L) {
    SYSTEMTIME now;
    char text[11];
    GetLocalTime(&now);
    snprintf(text, sizeof(text), "%04u-%02u-%02u",
             (unsigned)now.wYear, (unsigned)now.wMonth, (unsigned)now.wDay);
    lua_pushstring(L, text);
    return 1;
}

static int app_time(lua_State *L) {
    SYSTEMTIME now;
    char text[9];
    GetLocalTime(&now);
    snprintf(text, sizeof(text), "%02u:%02u:%02u",
             (unsigned)now.wHour, (unsigned)now.wMinute, (unsigned)now.wSecond);
    lua_pushstring(L, text);
    return 1;
}

static int app_date_time(lua_State *L) {
    SYSTEMTIME now;
    char text[20];
    GetLocalTime(&now);
    snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u",
             (unsigned)now.wYear, (unsigned)now.wMonth, (unsigned)now.wDay,
             (unsigned)now.wHour, (unsigned)now.wMinute, (unsigned)now.wSecond);
    lua_pushstring(L, text);
    return 1;
}

static int app_version(lua_State *L) { lua_pushliteral(L, "1.0.0"); return 1; }
static int app_api_version(lua_State *L) {
    lua_pushinteger(L, MARKEDIT_PLUGIN_API_VERSION);
    return 1;
}
static int app_platform(lua_State *L) { lua_pushliteral(L, "windows"); return 1; }
static int app_memory_used(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)s_memory.used);
    return 1;
}
static int app_workspace_path(lua_State *L) {
    char *path = s_callbacks.workspace_path ? s_callbacks.workspace_path() : NULL;
    if (path) lua_pushstring(L, path); else lua_pushnil(L);
    free(path);
    return 1;
}
static int app_capabilities(lua_State *L) {
    static const char *const names[] = {
        "plugin_environments", "memory_limit", "lazy_require", "event_unsubscribe",
        "documents", "command_execution", "path_picker", "status", "logging", "defer",
        "eol_conversion", "save_encoding", "tab_width"
    };
    lua_createtable(L, 0, (int)(sizeof(names) / sizeof(names[0])));
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, names[i]);
    }
    return 1;
}

static int app_defer(lua_State *L) {
    lua_Integer delay = luaL_checkinteger(L, 1);
    luaL_argcheck(L, delay >= 0 && delay <= 60000, 1, "delay must be from 0 to 60000 ms");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!s_callbacks.schedule_deferred)
        return luaL_error(L, "app.defer is not available here");
    int slot = -1;
    for (int i = 0; i < PLUGIN_MAX_DEFERRED; i++) if (!s_deferred[i].token) { slot = i; break; }
    if (slot < 0) return luaL_error(L, "too many deferred plugin callbacks");
    int token = s_nextDeferredToken;
    s_nextDeferredToken = s_nextDeferredToken == INT_MAX ? 1 : s_nextDeferredToken + 1;
    const char *owner = current_owner(L);
    lua_pushvalue(L, 2);
    s_deferred[slot].ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s_deferred[slot].token = token;
    s_deferred[slot].owner = dup_string(owner);
    s_deferred[slot].label = dup_string(current_title());
    if (!s_deferred[slot].owner || !s_deferred[slot].label ||
        !s_callbacks.schedule_deferred(token, (unsigned)delay, 0)) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_deferred[slot].ref);
        free(s_deferred[slot].owner); free(s_deferred[slot].label);
        memset(&s_deferred[slot], 0, sizeof(s_deferred[slot]));
        return luaL_error(L, "the deferred callback could not be scheduled");
    }
    lua_pushinteger(L, token);
    return 1;
}

static int app_cancel(lua_State *L) {
    int token = (int)luaL_checkinteger(L, 1);
    for (int i = 0; i < PLUGIN_MAX_DEFERRED; i++) {
        if (s_deferred[i].token != token) continue;
        if (strcmp(s_deferred[i].owner, current_owner(L)) != 0) break;
        if (s_callbacks.schedule_deferred) s_callbacks.schedule_deferred(token, 0, 1);
        luaL_unref(L, LUA_REGISTRYINDEX, s_deferred[i].ref);
        free(s_deferred[i].owner); free(s_deferred[i].label);
        memset(&s_deferred[i], 0, sizeof(s_deferred[i]));
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}

static int commands_execute(lua_State *L) {
    const char *name = checked_cstring(L, 1, "command name");
    if (!s_callbacks.invoke_command)
        return luaL_error(L, "commands.execute is not available here");
    lua_pushboolean(L, s_callbacks.invoke_command(name));
    return 1;
}

static int documents_count(lua_State *L) {
    lua_pushinteger(L, s_callbacks.document_count ? s_callbacks.document_count() : 1);
    return 1;
}
static int documents_active(lua_State *L) {
    lua_pushinteger(L, (s_callbacks.document_active ? s_callbacks.document_active() : 0) + 1);
    return 1;
}
static int checked_document_index(lua_State *L, int argument) {
    int count = s_callbacks.document_count ? s_callbacks.document_count() : 1;
    lua_Integer index = luaL_checkinteger(L, argument);
    luaL_argcheck(L, index >= 1 && index <= count, argument, "document index out of range");
    return (int)index - 1;
}
static int documents_activate(lua_State *L) {
    int index = checked_document_index(L, 1);
    if (!s_callbacks.document_activate)
        return luaL_error(L, "documents.activate is not available here");
    lua_pushboolean(L, s_callbacks.document_activate(index));
    return 1;
}
static int documents_new(lua_State *L) {
    if (!s_callbacks.document_new) return luaL_error(L, "documents.new is not available here");
    lua_pushboolean(L, s_callbacks.document_new());
    return 1;
}
static int documents_open(lua_State *L) {
    const char *path = checked_cstring(L, 1, "file path");
    if (!s_callbacks.document_open) return luaL_error(L, "documents.open is not available here");
    lua_pushboolean(L, s_callbacks.document_open(path));
    return 1;
}
static int documents_save(lua_State *L) {
    if (!s_callbacks.document_save) return luaL_error(L, "documents.save is not available here");
    lua_pushboolean(L, s_callbacks.document_save(lua_toboolean(L, 1)));
    return 1;
}
static int documents_close(lua_State *L) {
    int index = lua_isnoneornil(L, 1)
        ? (s_callbacks.document_active ? s_callbacks.document_active() : 0)
        : checked_document_index(L, 1);
    if (!s_callbacks.document_close) return luaL_error(L, "documents.close is not available here");
    lua_pushboolean(L, s_callbacks.document_close(index));
    return 1;
}
static int documents_path(lua_State *L) {
    int index = lua_isnoneornil(L, 1)
        ? (s_callbacks.document_active ? s_callbacks.document_active() : 0)
        : checked_document_index(L, 1);
    char *path = s_callbacks.document_path ? s_callbacks.document_path(index) : NULL;
    if (path) lua_pushstring(L, path); else lua_pushnil(L);
    free(path);
    return 1;
}
static int documents_modified(lua_State *L) {
    int index = lua_isnoneornil(L, 1)
        ? (s_callbacks.document_active ? s_callbacks.document_active() : 0)
        : checked_document_index(L, 1);
    lua_pushboolean(L, s_callbacks.document_modified && s_callbacks.document_modified(index));
    return 1;
}
static int documents_encoding(lua_State *L) {
    if (!lua_isnoneornil(L, 1)) {
        lua_editor_api_require_editor(L);
        const char *encoding = checked_cstring(L, 1, "encoding");
        int valid = strcmp(encoding, "utf-8") == 0 || strcmp(encoding, "utf-8-bom") == 0 ||
                    strcmp(encoding, "utf-16le") == 0 || strcmp(encoding, "utf-16be") == 0 ||
                    strcmp(encoding, "ansi") == 0;
        luaL_argcheck(L, valid, 1,
                      "expected utf-8, utf-8-bom, utf-16le, utf-16be or ansi");
        if (!s_callbacks.document_set_encoding)
            return luaL_error(L, "documents.encoding cannot be changed here");
        lua_pushboolean(L, s_callbacks.document_set_encoding(encoding));
        return 1;
    }
    const char *encoding = s_callbacks.document_encoding ? s_callbacks.document_encoding() : "utf-8";
    lua_pushstring(L, encoding ? encoding : "utf-8");
    return 1;
}

static const luaL_Reg ui_functions[] = {
    {"message", ui_message},
    {"prompt", ui_prompt},
    {"choose", ui_choose},
    {"save_binary", ui_save_binary},
    {"confirm", ui_confirm}, {"status", ui_status}, {"log", ui_log},
    {"pick_file", ui_pick_file}, {"pick_folder", ui_pick_folder},
    {NULL, NULL}
};
static const luaL_Reg menu_functions[] = {
    {"add", menu_add}, {"checkbox", menu_checkbox}, {"separator", menu_separator}, {NULL, NULL}
};
static const luaL_Reg events_functions[] = {
    {"on", events_on}, {"off", events_off}, {NULL, NULL}
};
static const luaL_Reg commands_functions[] = {
    {"register", commands_register}, {"execute", commands_execute}, {NULL, NULL}
};
static const luaL_Reg config_functions[] = { {"get", config_get}, {"set", config_set}, {NULL, NULL} };
static const luaL_Reg app_functions[] = {
    {"date", app_date}, {"time", app_time}, {"date_time", app_date_time},
    {"version", app_version}, {"api_version", app_api_version},
    {"platform", app_platform}, {"memory_used", app_memory_used},
    {"workspace_path", app_workspace_path}, {"capabilities", app_capabilities},
    {"defer", app_defer}, {"cancel", app_cancel},
    {NULL, NULL}
};
static const luaL_Reg documents_functions[] = {
    {"count", documents_count}, {"active", documents_active},
    {"activate", documents_activate}, {"new", documents_new}, {"open", documents_open},
    {"save", documents_save}, {"close", documents_close}, {"path", documents_path},
    {"modified", documents_modified}, {"encoding", documents_encoding}, {NULL, NULL}
};

void plugin_registry_open(lua_State *L) {
    luaL_newlib(L, ui_functions);
    lua_setglobal(L, "ui");
    luaL_newlib(L, menu_functions);
    lua_setglobal(L, "menu");
    luaL_newlib(L, events_functions);
    lua_setglobal(L, "events");
    luaL_newlib(L, commands_functions);
    lua_setglobal(L, "commands");
    luaL_newlib(L, config_functions);
    lua_setglobal(L, "config");
    luaL_newlib(L, app_functions);
    lua_setglobal(L, "app");
    luaL_newlib(L, documents_functions);
    lua_setglobal(L, "documents");
}

int plugin_host_command_count(void) {
    return s_commandCount;
}

const char *plugin_host_command_label(int index) {
    return index >= 0 && index < s_commandCount ? s_commands[index].label : NULL;
}

const char *plugin_host_command_shortcut(int index) {
    return index >= 0 && index < s_commandCount ? s_commands[index].shortcut : NULL;
}

int plugin_host_command_accelerator(int index, BYTE *flags, WORD *key) {
    if (index < 0 || index >= s_commandCount || !s_commands[index].shortcut) return 0;
    if (flags) *flags = s_commands[index].shortcutFlags;
    if (key) *key = s_commands[index].shortcutKey;
    return 1;
}

int plugin_host_command_is_placed(int index) {
    return index >= 0 && index < s_commandCount && s_commands[index].placed;
}

int plugin_host_command_is_separator(int index) {
    return index >= 0 && index < s_commandCount && s_commands[index].separator;
}

int plugin_host_command_is_checkable(int index) {
    return index >= 0 && index < s_commandCount && s_commands[index].configKey != NULL;
}

static int stored_checkbox_state(const PluginCommand *command) {
    char *value = s_callbacks.config_get
        ? s_callbacks.config_get(command->owner, command->configKey) : NULL;
    int checked = value ? (strcmp(value, "1") == 0 || _stricmp(value, "true") == 0 ||
                           _stricmp(value, "yes") == 0 || _stricmp(value, "on") == 0)
                        : command->checkDefault;
    free(value);
    return checked;
}

int plugin_host_command_is_checked(int index) {
    return plugin_host_command_is_checkable(index) ? stored_checkbox_state(&s_commands[index]) : 0;
}

const char *plugin_host_command_overrides(int index) {
    return index >= 0 && index < s_commandCount ? s_commands[index].overrides : NULL;
}

int plugin_host_find_override(const char *name) {
    for (int i = s_commandCount - 1; i >= 0; i--)
        if (s_commands[i].overrides && strcmp(s_commands[i].overrides, name) == 0) return i;
    return -1;
}

/* Calls a registered function against hEdit as a single undo step, undoing
   its edits if it fails. argument is its one parameter (nil when NULL). */
static char *call_registered(int ref, const char *label, const char *owner,
                             const char *argument, int booleanArgument,
                             HWND hEdit, int timeLimited) {
    char *stableLabel = dup_string(label);
    char *stableOwner = dup_string(owner);
    if (!stableLabel || !stableOwner) {
        free(stableLabel); free(stableOwner);
        return dup_string("out of memory starting plugin callback");
    }
    s_busy = 1;
    lua_editor_api_set_editor(hEdit);
    s_runningLabel = stableLabel;
    s_runningOwner = stableOwner;
    s_timeLimited = timeLimited;
    /* An empty undo group isn't recorded, so the undo position only moves
       if the function really changed the document. */
    intptr_t undoBefore = (intptr_t)SendMessageA(hEdit, SCI_GETUNDOCURRENT, 0, 0);
    SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
    lua_rawgeti(s_lua, LUA_REGISTRYINDEX, ref);
    if (booleanArgument >= 0) lua_pushboolean(s_lua, booleanArgument);
    else if (argument) lua_pushstring(s_lua, argument);
    else lua_pushnil(s_lua);
    char *error = protected_call(1);
    lua_editor_api_finish_undo();
    SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
    if (error && (intptr_t)SendMessageA(hEdit, SCI_GETUNDOCURRENT, 0, 0) != undoBefore)
        SendMessageA(hEdit, SCI_UNDO, 0, 0);
    lua_editor_api_set_editor(NULL);
    s_runningLabel = NULL;
    s_runningOwner = NULL;
    s_timeLimited = 1;
    s_busy = 0;
    free(stableLabel);
    free(stableOwner);
    return error;
}

char *plugin_host_run(int index, HWND hEdit) {
    if (s_busy) return dup_string("Another plugin is still running.");
    if (!s_lua || index < 0 || index >= s_commandCount)
        return dup_string("That plugin command is no longer loaded.");
    const PluginCommand *command = &s_commands[index];
    if (command->separator) return NULL; /* nothing to run */
    if (command->configKey) {
        int oldState = stored_checkbox_state(command), newState = !oldState;
        if (!s_callbacks.config_set ||
            !s_callbacks.config_set(command->owner, command->configKey, newState ? "1" : "0"))
            return dup_string("Could not save the plugin checkbox state.");
        char *error = call_registered(command->ref, command->label, command->owner,
                                      NULL, newState, hEdit, !command->builtIn);
        if (error) s_callbacks.config_set(command->owner, command->configKey, oldState ? "1" : "0");
        return error;
    }
    return call_registered(command->ref, command->label, command->owner,
                           NULL, -1, hEdit, !command->builtIn);
}

char *plugin_host_fire(const char *event, const char *argument, HWND hEdit) {
    if (s_busy || !s_lua) return NULL;
    char *report = NULL;
    /* Snapshot stable IDs. Handlers may remove themselves or each other,
       which compacts s_handlers; newly registered handlers wait until the
       next event. */
    int ids[PLUGIN_MAX_HANDLERS], count = 0;
    for (int i = 0; i < s_handlerCount; i++)
        if (strcmp(s_handlers[i].event, event) == 0) ids[count++] = s_handlers[i].id;
    for (int n = 0; n < count; n++) {
        int index = -1;
        for (int i = 0; i < s_handlerCount; i++)
            if (s_handlers[i].id == ids[n]) { index = i; break; }
        if (index < 0) continue;
        report = append_line(report, call_registered(s_handlers[index].ref,
            s_handlers[index].label, s_handlers[index].owner, argument, -1, hEdit, 1));
    }
    return report;
}

char *plugin_host_run_deferred(int token, HWND hEdit) {
    if (s_busy || !s_lua) return NULL;
    for (int i = 0; i < PLUGIN_MAX_DEFERRED; i++) {
        PluginDeferred *deferred = &s_deferred[i];
        if (deferred->token != token) continue;
        int ref = deferred->ref;
        char *owner = deferred->owner;
        char *label = deferred->label;
        memset(deferred, 0, sizeof(*deferred));
        char *error = call_registered(ref, label, owner, NULL, -1, hEdit, 1);
        luaL_unref(s_lua, LUA_REGISTRYINDEX, ref);
        free(owner); free(label);
        return error;
    }
    return NULL;
}