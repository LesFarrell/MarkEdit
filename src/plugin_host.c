/* Lua extension host: owns the sandboxed state, registrations, extension
   discovery and folder-local module loading. Application services arrive
   through callbacks; editor operations are delegated to lua_editor_api.c. */
#include "plugin_host.h"

#include <limits.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "file_io.h"
#include "lua_editor_api.h"
#include "path_utils.h"
#include "text_codec.h"
#include "../third_party/scintilla/include/Scintilla.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define PLUGIN_MAX_SCRIPT_BYTES (1024 * 1024)
#define PLUGIN_MAX_CHOICES 10000
#define PLUGIN_MAX_BINARY_BYTES (64 * 1024 * 1024)

typedef struct {
    char *label;
    int ref;          /* the command's function, in the Lua registry */
    int placed;       /* by the menu definition, rather than by its label */
    char *overrides;  /* the built-in command it runs for (commands.register), or NULL */
    int builtIn;      /* from a script built into MarkEdit: no time limit */
    int separator;    /* menu.separator: label is a menu path, ref is LUA_NOREF */
    char *owner;       /* script name, used to namespace config values */
    char *configKey;   /* menu.checkbox's persisted key, or NULL */
    int checkDefault;
    char *shortcut;
    BYTE shortcutFlags;
    WORD shortcutKey;
} PluginCommand;

typedef struct {
    char *name;
    char *scope;
    int enabled;
} PluginExtension;

typedef struct {
    const char *event; /* one of EVENT_NAMES */
    char *label;       /* "script.lua: event", for dialog titles */
    int ref;
    char *owner;
} PluginHandler;

static const char *const EVENT_NAMES[] = { "save", "open", "tab_changed" };

static lua_State *s_lua = NULL;
static PluginCommand s_commands[PLUGIN_MAX_COMMANDS];
static int s_commandCount = 0;
static PluginHandler s_handlers[PLUGIN_MAX_HANDLERS];
static int s_handlerCount = 0;
static PluginExtension s_extensions[256];
static int s_extensionCount = 0;
static WCHAR *s_firstErrorPath = NULL;
static int s_firstErrorLine = 0;
/* The script whose top-level code is running, while it loads, and whether
   it is one built into MarkEdit. */
static const char *s_loadingName = NULL;
static int s_loadingBuiltIn = 0;
static const WCHAR *s_loadingModuleRoot = NULL;
/* Set while any plugin code runs, so nothing re-enters the Lua state from
   a message loop (a plugin dialog, say) mid-call. */
static int s_busy = 0;
static const char *s_runningLabel = NULL;
static const char *s_runningOwner = NULL;
/* Built-in commands are trusted not to hang, and may legitimately take
   longer than the limit on a large document. */
static int s_timeLimited = 1;
static PluginHostCallbacks s_callbacks;
static ULONGLONG s_callStarted = 0;

void plugin_host_set_callbacks(const PluginHostCallbacks *callbacks) {
    if (callbacks) s_callbacks = *callbacks;
    else memset(&s_callbacks, 0, sizeof(s_callbacks));
    lua_editor_api_set_callbacks(callbacks);
}

static char *dup_string(const char *value) {
    if (!value) value = "unknown error";
    size_t bytes = strlen(value) + 1;
    char *copy = (char *)malloc(bytes);
    if (copy) memcpy(copy, value, bytes);
    return copy;
}

/* Lua strings are length-counted, while labels, callback arguments and
   command/event names cross C APIs as NUL-terminated UTF-8. Reject an
   embedded NUL instead of silently treating a different prefix as the
   value (notably, "file.save\0..." must not override file.save). */
static const char *checked_cstring(lua_State *L, int index, const char *what) {
    size_t length = 0;
    const char *value = luaL_checklstring(L, index, &length);
    if (memchr(value, 0, length))
        luaL_error(L, "%s must not contain NUL bytes", what);
    if (length > INT_MAX || (length && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value, (int)length, NULL, 0) <= 0))
        luaL_error(L, "%s must be valid UTF-8", what);
    return value;
}

static const char *optional_cstring(lua_State *L, int index, const char *fallback,
                                    const char *what) {
    return lua_isnoneornil(L, index) ? fallback : checked_cstring(L, index, what);
}

/* Appends line (taking ownership of it) to a newline-separated report. */
static char *append_line(char *report, char *line) {
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
static char *protected_call(int nargs) {
    int base = lua_gettop(s_lua) - nargs;
    lua_pushcfunction(s_lua, traceback_handler);
    lua_insert(s_lua, base);
    s_callStarted = GetTickCount64();
    int status = lua_pcall(s_lua, nargs, 0, base);
    char *error = status == LUA_OK ? NULL : dup_string(lua_tostring(s_lua, -1));
    lua_settop(s_lua, base - 1);
    return error;
}

static void discard_commands_from(int first) {
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

static void discard_handlers_from(int first) {
    for (int i = first; i < s_handlerCount; i++) {
        if (s_lua) luaL_unref(s_lua, LUA_REGISTRYINDEX, s_handlers[i].ref);
        free(s_handlers[i].label);
        free(s_handlers[i].owner);
        memset(&s_handlers[i], 0, sizeof(s_handlers[i]));
    }
    if (first < s_handlerCount) s_handlerCount = first;
}

/* ---- the API plugins see: editor.*, ui.*, menu.* ----
   Positions are 1-based byte offsets into the document's UTF-8 text, and a
   range is start..finish inclusive, as with string.sub: for a range s, f,
   editor.get_text():sub(s, f) is its text, and s, s - 1 is the empty range
   just before s. */


static const char *current_title(void) {
    return s_runningLabel ? s_runningLabel : "MarkEdit Plugin";
}

/* Time spent waiting on the user doesn't count against the plugin. */
static void restart_time_limit(void) {
    s_callStarted = GetTickCount64();
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

/* Adds an entry the application places by its label: a menu.add command
   (fn at stack index 2), or a menu.separator. */
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
    if (!event)
        return luaL_error(L, "unknown event '%s' (expected save, open or tab_changed)", name);
    if (s_handlerCount >= PLUGIN_MAX_HANDLERS)
        return luaL_error(L, "too many event handlers (the limit is %d)", PLUGIN_MAX_HANDLERS);
    const char *owner = s_loadingName ? s_loadingName : current_title();
    char *label = (char *)malloc(strlen(owner) + strlen(event) + 3);
    if (!label) return luaL_error(L, "out of memory");
    sprintf(label, "%s: %s", owner, event);
    lua_pushvalue(L, 2);
    s_handlers[s_handlerCount].event = event;
    s_handlers[s_handlerCount].label = label;
    s_handlers[s_handlerCount].ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s_handlers[s_handlerCount].owner = dup_string(s_loadingName ? s_loadingName : "plugin");
    if (!s_handlers[s_handlerCount].owner) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_handlers[s_handlerCount].ref);
        free(s_handlers[s_handlerCount].label);
        return luaL_error(L, "out of memory");
    }
    s_handlerCount++;
    return 0;
}

static const char *current_owner(lua_State *L) {
    const char *owner = s_loadingName ? s_loadingName : s_runningOwner;
    if (!owner) luaL_error(L, "config is only available while loading or running a plugin");
    return owner;
}

static int config_get(lua_State *L) {
    const char *key = checked_config_key(L, 1);
    char *value = s_callbacks.config_get ? s_callbacks.config_get(current_owner(L), key) : NULL;
    if (!value) {
        if (lua_gettop(L) >= 2) lua_pushvalue(L, 2);
        else lua_pushnil(L);
        return 1;
    }
    if (lua_type(L, 2) == LUA_TBOOLEAN) {
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
    switch (lua_type(L, 2)) {
        case LUA_TNIL: break;
        case LUA_TBOOLEAN: value = lua_toboolean(L, 2) ? "1" : "0"; break;
        case LUA_TNUMBER:
            snprintf(number, sizeof(number), "%.17g", (double)lua_tonumber(L, 2));
            value = number;
            break;
        case LUA_TSTRING: value = checked_cstring(L, 2, "config value"); break;
        default: return luaL_argerror(L, 2, "expected string, number, boolean or nil");
    }
    if (!s_callbacks.config_set || !s_callbacks.config_set(current_owner(L), key, value))
        return luaL_error(L, "could not write the plugin setting");
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

static int valid_module_name(const char *module) {
    size_t length = strlen(module);
    if (!length || length > 128 || module[0] == '.' || module[length - 1] == '.') return 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)module[i];
        if (!(isalnum(c) || c == '_' || c == '.') ||
            (c == '.' && i && module[i - 1] == '.')) return 0;
    }
    return 1;
}

/* Leaves the module cache table on the stack, and a cached value above it
   when one exists. */
static int push_module_cache(lua_State *L, const char *cacheKey) {
    lua_getfield(L, LUA_REGISTRYINDEX, "markedit.modules");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "markedit.modules");
    }
    lua_getfield(L, -1, cacheKey);
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    return 0;
}

static WCHAR *module_source_path(const char *module, char relative[260]) {
    snprintf(relative, 260, "%s.lua", module);
    for (char *p = relative; *p; p++) if (*p == '.') *p = '/';
    /* Restore the extension dot replaced by the loop above. */
    memcpy(relative + strlen(relative) - 4, ".lua", 5);
    WCHAR *wide = to_wide(relative);
    WCHAR *path = wide ? path_join(s_loadingModuleRoot, wide) : NULL;
    free(wide);
    return path;
}

/* A small replacement for Lua's package loader. It reads source modules only
   below the folder extension whose init.lua is currently loading. */
static int plugin_require(lua_State *L) {
    const char *module = checked_cstring(L, 1, "module name");
    if (!s_loadingModuleRoot)
        return luaL_error(L, "require is available only from a folder extension's init.lua");
    if (!valid_module_name(module))
        return luaL_error(L, "module names may contain letters, digits, '_' and single dots");
    char cacheKey[520];
    snprintf(cacheKey, sizeof(cacheKey), "%s\n%s", s_loadingName ? s_loadingName : "plugin", module);
    if (push_module_cache(L, cacheKey)) return 1;

    char relative[260];
    WCHAR *path = module_source_path(module, relative);
    size_t codeLength = 0;
    char *code = path ? read_utf8_text_file_length(path, PLUGIN_MAX_SCRIPT_BYTES, &codeLength) : NULL;
    free(path);
    if (!code) return luaL_error(L, "module '%s' was not found or is not valid UTF-8", module);
    char chunkName[300];
    snprintf(chunkName, sizeof(chunkName), "=%s/%s", s_loadingName ? s_loadingName : "plugin", relative);
    int status = luaL_loadbufferx(L, code, codeLength, chunkName, "t");
    free(code);
    if (status != LUA_OK) return lua_error(L);
    lua_call(L, 0, 1);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushboolean(L, 1); }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, cacheKey);
    return 1;
}

static const luaL_Reg ui_functions[] = {
    {"message", ui_message},
    {"prompt", ui_prompt},
    {"choose", ui_choose},
    {"save_binary", ui_save_binary},
    {NULL, NULL}
};
static const luaL_Reg menu_functions[] = {
    {"add", menu_add}, {"checkbox", menu_checkbox}, {"separator", menu_separator}, {NULL, NULL}
};
static const luaL_Reg events_functions[] = { {"on", events_on}, {NULL, NULL} };
static const luaL_Reg commands_functions[] = { {"register", commands_register}, {NULL, NULL} };
static const luaL_Reg config_functions[] = { {"get", config_get}, {"set", config_set}, {NULL, NULL} };
static const luaL_Reg app_functions[] = {
    {"date", app_date}, {"time", app_time}, {"date_time", app_date_time}, {NULL, NULL}
};

/* ---- menu definitions ---- */

#define PLUGIN_MENU_MAX_DEPTH 8
#define PLUGIN_MENU_MAX_ITEMS 4000

static PluginMenuItem *s_menu = NULL;
static int s_menuCount = 0;
/* While a definition converts: the labels leading to the current item, for
   error messages and function commands' labels, and how many items so far. */
static const char *s_menuPath[PLUGIN_MENU_MAX_DEPTH];
static int s_menuItemsSeen = 0;

static void free_menu_items(PluginMenuItem *items, int count) {
    for (int i = 0; i < count; i++) {
        free(items[i].label);
        free(items[i].name);
        free_menu_items(items[i].items, items[i].itemCount);
    }
    free(items);
}

static void free_menu(void) {
    free_menu_items(s_menu, s_menuCount);
    s_menu = NULL;
    s_menuCount = 0;
}

/* Raises "File > Recent Files: problem" for an item at the given depth. */
static int menu_error(lua_State *L, int depth, const char *problem) {
    luaL_Buffer buffer;
    luaL_buffinit(L, &buffer);
    for (int i = 0; i < depth; i++) {
        if (i) luaL_addstring(&buffer, " > ");
        luaL_addstring(&buffer, s_menuPath[i]);
    }
    luaL_addstring(&buffer, depth ? ": " : "the menu bar: ");
    luaL_addstring(&buffer, problem);
    luaL_pushresult(&buffer);
    return lua_error(L);
}

static char *checked_copy(lua_State *L, const char *value) {
    char *copy = dup_string(value);
    if (!copy) luaL_error(L, "out of memory");
    return copy;
}

/* Registers the function on the stack top as a command placed at the
   current path plus label, e.g. "/File/Insert Date". */
static int register_menu_function(lua_State *L, int depth, const char *label) {
    if (s_commandCount >= PLUGIN_MAX_COMMANDS)
        return menu_error(L, depth, "too many commands");
    luaL_Buffer buffer;
    luaL_buffinit(L, &buffer);
    for (int i = 0; i < depth; i++) {
        luaL_addchar(&buffer, '/');
        luaL_addstring(&buffer, s_menuPath[i]);
    }
    luaL_addchar(&buffer, '/');
    luaL_addstring(&buffer, label);
    luaL_pushresult(&buffer);
    char *path = checked_copy(L, lua_tostring(L, -1));
    char *owner = dup_string(s_loadingName ? s_loadingName : "menus.lua");
    if (!owner) {
        free(path);
        luaL_error(L, "out of memory");
    }
    lua_pop(L, 1);
    lua_pushvalue(L, -1);
    s_commands[s_commandCount].label = path;
    s_commands[s_commandCount].ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s_commands[s_commandCount].placed = 1;
    s_commands[s_commandCount].overrides = NULL;
    s_commands[s_commandCount].builtIn = s_loadingBuiltIn;
    s_commands[s_commandCount].separator = 0;
    s_commands[s_commandCount].owner = owner;
    s_commands[s_commandCount].configKey = NULL;
    s_commands[s_commandCount].checkDefault = 0;
    return s_commandCount++;
}

/* Converts the item array at the stack top. *outItems is set before it is
   filled, so a conversion stopped by an error leaves a tree free_menu()
   can release. Raw access throughout, so no metamethod runs. */
static void convert_menu_items(lua_State *L, PluginMenuItem **outItems, int *outCount, int depth) {
    if (depth >= PLUGIN_MENU_MAX_DEPTH) menu_error(L, depth, "menus are nested too deeply");
    lua_Integer count = (lua_Integer)lua_rawlen(L, -1);
    if (count == 0) return;
    if (count > PLUGIN_MENU_MAX_ITEMS - s_menuItemsSeen) menu_error(L, depth, "too many menu items");
    s_menuItemsSeen += (int)count;
    PluginMenuItem *items = (PluginMenuItem *)calloc((size_t)count, sizeof(*items));
    if (!items) luaL_error(L, "out of memory");
    *outItems = items;
    *outCount = (int)count;
    for (lua_Integer i = 0; i < count; i++) {
        PluginMenuItem *item = &items[i];
        int type = lua_rawgeti(L, -1, i + 1);
        if (type == LUA_TSTRING) {
            const char *separator = checked_cstring(L, -1, "menu item");
            if (strcmp(separator, "-") != 0 || depth == 0)
                menu_error(L, depth, depth ? "a text item must be \"-\" (a separator)"
                                           : "each item must be a menu: { label, { items } }");
            item->kind = PLUGIN_MENU_SEPARATOR;
            lua_pop(L, 1);
            continue;
        }
        if (type != LUA_TTABLE) menu_error(L, depth, "each item must be a table or \"-\"");
        lua_pushliteral(L, "list");
        if (lua_rawget(L, -2) == LUA_TSTRING) {
            if (depth == 0) menu_error(L, depth, "a list must be inside a menu");
            item->kind = PLUGIN_MENU_LIST;
            item->name = checked_copy(L, checked_cstring(L, -1, "menu list name"));
            lua_pop(L, 2);
            continue;
        }
        lua_pop(L, 1);
        if (lua_rawgeti(L, -1, 1) != LUA_TSTRING)
            menu_error(L, depth, "an item needs a text label first: { label, action }");
        const char *label = checked_cstring(L, -1, "menu label"); /* kept alive by the item table */
        item->label = checked_copy(L, label);
        lua_pop(L, 1);
        int actionType = lua_rawgeti(L, -1, 2);
        if (depth == 0 && actionType != LUA_TTABLE) {
            s_menuPath[0] = label;
            menu_error(L, 1, "a top-level item must be a menu: { label, { items } }");
        }
        if (actionType == LUA_TSTRING) {
            item->kind = PLUGIN_MENU_COMMAND;
            item->name = checked_copy(L, checked_cstring(L, -1, "menu command name"));
        } else if (actionType == LUA_TFUNCTION) {
            item->kind = PLUGIN_MENU_FUNCTION;
            item->command = register_menu_function(L, depth, label);
        } else if (actionType == LUA_TTABLE) {
            item->kind = PLUGIN_MENU_SUBMENU;
            s_menuPath[depth] = label;
            convert_menu_items(L, &item->items, &item->itemCount, depth + 1);
        } else {
            s_menuPath[depth] = label;
            menu_error(L, depth + 1, "the action must be a command name, a function or a table of items");
        }
        lua_pop(L, 2);
    }
}

/* Called with the compiled definition: runs it, then converts its result. */
static int run_menu_definition(lua_State *L) {
    lua_call(L, 0, 1);
    if (!lua_istable(L, -1)) return luaL_error(L, "a menu definition must return a table of menus");
    s_menuItemsSeen = 0;
    convert_menu_items(L, &s_menu, &s_menuCount, 0);
    return 0;
}

/* Only libraries without file system or process access are opened. Run in
   protected mode, since any of this can fail for lack of memory. */
static int open_environment(lua_State *L) {
    static const luaL_Reg libraries[] = {
        {LUA_GNAME, luaopen_base},
        {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    for (size_t i = 0; i < sizeof(libraries) / sizeof(libraries[0]); i++) {
        luaL_requiref(L, libraries[i].name, libraries[i].func, 1);
        lua_pop(L, 1);
    }
    /* dofile/loadfile read files; load is limited to source text, since
       malformed precompiled chunks can corrupt memory. env is passed on as
       varargs: load() tells an absent env (use the globals) from a nil one
       (no globals at all), so it must stay absent when the caller omits it. */
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    static const char textOnlyLoad[] =
        "local load = load\n"
        "_G.load = function(chunk, name, _, ...) return load(chunk, name, 't', ...) end\n";
    if (luaL_loadbufferx(L, textOnlyLoad, sizeof(textOnlyLoad) - 1, "=markedit", "t") != LUA_OK)
        return lua_error(L);
    lua_call(L, 0, 0);

    lua_editor_api_push(L);
    lua_setglobal(L, "editor");
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
    lua_pushcfunction(L, plugin_require);
    lua_setglobal(L, "require");
    lua_sethook(L, time_limit_hook, LUA_MASKCOUNT, 10000);
    return 0;
}

static char *ensure_state(void) {
    if (s_lua) return NULL;
    s_lua = luaL_newstate();
    if (!s_lua) return dup_string("Lua could not start: out of memory");
    lua_pushcfunction(s_lua, open_environment);
    char *error = protected_call(0);
    if (error) {
        lua_close(s_lua);
        s_lua = NULL;
    }
    return error;
}

/* Runs a script's top-level code, or with runner given, calls runner with
   the compiled script instead. */
static char *run_script(const char *name, const char *code, size_t length,
                        lua_CFunction runner, const WCHAR *moduleRoot) {
    if (s_busy) return dup_string("Plugins can't be loaded while a plugin is running.");
    char *error = ensure_state();
    if (error) return error;
    int firstCommand = s_commandCount, firstHandler = s_handlerCount;
    char chunkName[260];
    snprintf(chunkName, sizeof(chunkName), "=%s", name);
    if (runner) lua_pushcfunction(s_lua, runner);
    if (luaL_loadbufferx(s_lua, code, length, chunkName, "t") != LUA_OK) {
        error = dup_string(lua_tostring(s_lua, -1));
        lua_pop(s_lua, runner ? 2 : 1);
    } else {
        s_busy = 1;
        s_loadingName = name;
        s_loadingModuleRoot = moduleRoot;
        error = protected_call(runner ? 1 : 0);
        s_loadingModuleRoot = NULL;
        s_loadingName = NULL;
        s_busy = 0;
    }
    /* A script that fails part-way contributes none of its commands or handlers. */
    if (error) {
        discard_commands_from(firstCommand);
        discard_handlers_from(firstHandler);
    }
    return error;
}

char *plugin_host_load_chunk(const char *name, const char *code, size_t length) {
    return run_script(name, code, length, NULL, NULL);
}

char *plugin_host_load_builtin(const char *name, const char *code, size_t length) {
    s_loadingBuiltIn = 1;
    char *error = run_script(name, code, length, NULL, NULL);
    s_loadingBuiltIn = 0;
    return error;
}

char *plugin_host_load_menu(const char *name, const char *code, size_t length) {
    /* run_script() would refuse too, but only after the current definition
       had already been thrown away. */
    if (s_busy) return dup_string("Plugins can't be loaded while a plugin is running.");
    free_menu();
    char *error = run_script(name, code, length, run_menu_definition, NULL);
    if (error) free_menu();
    return error;
}

const PluginMenuItem *plugin_host_menu(int *count) {
    *count = s_menuCount;
    return s_menu;
}

/* ---- generated pages ---- */

typedef struct {
    const char *name;
    const char *code;
    size_t length;
    const PluginShortcut *shortcuts;
    int shortcutCount;
} PageJob;

/* Pushes menu definition items as a Lua list of { kind = ..., ... } tables
   (the shape src/help/keyboard-shortcuts.lua describes). */
static void push_menu_items(lua_State *L, const PluginMenuItem *items, int count) {
    static const char *const KINDS[] = { "separator", "command", "function", "submenu", "list" };
    luaL_checkstack(L, 4, "menu definition nested too deeply");
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; i++) {
        const PluginMenuItem *item = &items[i];
        lua_createtable(L, 0, 3);
        lua_pushstring(L, KINDS[item->kind]);
        lua_setfield(L, -2, "kind");
        if (item->label) {
            lua_pushstring(L, item->label);
            lua_setfield(L, -2, "label");
        }
        if (item->name) {
            lua_pushstring(L, item->name);
            lua_setfield(L, -2, item->kind == PLUGIN_MENU_LIST ? "list" : "command");
        }
        if (item->kind == PLUGIN_MENU_SUBMENU) {
            push_menu_items(L, item->items, item->itemCount);
            lua_setfield(L, -2, "items");
        }
        lua_rawseti(L, -2, i + 1);
    }
}

/* Run in protected mode, since any of it can fail for lack of memory:
   opens the libraries, compiles the script and calls it with its data. */
static int render_page(lua_State *L) {
    const PageJob *job = (const PageJob *)lua_touserdata(L, 1);
    static const luaL_Reg libraries[] = {
        {LUA_GNAME, luaopen_base},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    for (size_t i = 0; i < sizeof(libraries) / sizeof(libraries[0]); i++) {
        luaL_requiref(L, libraries[i].name, libraries[i].func, 1);
        lua_pop(L, 1);
    }
    char chunkName[260];
    snprintf(chunkName, sizeof(chunkName), "=%s", job->name);
    if (luaL_loadbufferx(L, job->code, job->length, chunkName, "t") != LUA_OK) return lua_error(L);

    lua_createtable(L, 0, 2);
    push_menu_items(L, s_menu, s_menuCount);
    lua_setfield(L, -2, "menus");
    lua_createtable(L, 0, job->shortcutCount);
    for (int i = 0; i < job->shortcutCount; i++) {
        lua_pushstring(L, job->shortcuts[i].shortcut);
        lua_setfield(L, -2, job->shortcuts[i].name);
    }
    lua_setfield(L, -2, "shortcuts");

    lua_call(L, 1, 1);
    if (lua_type(L, -1) != LUA_TSTRING) return luaL_error(L, "%s must return the page as a string", job->name);
    return 1;
}

char *plugin_host_render_page(const char *name, const char *code, size_t length,
                              const PluginShortcut *shortcuts, int shortcutCount, char **page) {
    *page = NULL;
    lua_State *L = luaL_newstate();
    if (!L) return dup_string("Lua could not start: out of memory");
    PageJob job = { name, code, length, shortcuts, shortcutCount };
    lua_pushcfunction(L, render_page);
    lua_pushlightuserdata(L, &job);
    char *error = NULL;
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        error = dup_string(message ? message : "the page script failed");
    } else {
        size_t size;
        const char *text = lua_tolstring(L, -1, &size);
        *page = (char *)malloc(size + 1);
        if (*page) {
            memcpy(*page, text, size);
            (*page)[size] = 0;
        } else {
            error = dup_string("out of memory");
        }
    }
    lua_close(L);
    return error;
}

typedef struct {
    WCHAR *name;
    int folder;
} ExtensionCandidate;

static int compare_candidates(const void *a, const void *b) {
    return _wcsicmp(((const ExtensionCandidate *)a)->name,
                    ((const ExtensionCandidate *)b)->name);
}

/* FindFirstFileW's "*.lua" also matches longer extensions such as ".luac"
   through 8.3 short names, so the extension is checked exactly. */
static int is_lua_script(const WIN32_FIND_DATAW *found) {
    if (found->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    const WCHAR *dot = wcsrchr(found->cFileName, L'.');
    return dot && _wcsicmp(dot, L".lua") == 0;
}

static int is_folder_extension(const WCHAR *directory, const WCHAR *name) {
    WCHAR *folder = path_join(directory, name);
    WCHAR *entry = folder ? path_join(folder, L"init.lua") : NULL;
    DWORD attributes = entry ? GetFileAttributesW(entry) : INVALID_FILE_ATTRIBUTES;
    free(entry);
    free(folder);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void remember_load_error(const WCHAR *path, const WCHAR *moduleRoot,
                                const char *error, const char *shownName) {
    if (s_firstErrorPath || !path || !error) return;
    const char *at = shownName ? strstr(error, shownName) : NULL;
    if (at) {
        at += strlen(shownName);
        if (*at == '/' && moduleRoot) {
            const char *colon = strchr(at + 1, ':');
            if (colon && colon > at + 1 && (size_t)(colon - at) < 260) {
                char relative[260];
                memcpy(relative, at + 1, (size_t)(colon - at - 1));
                relative[colon - at - 1] = 0;
                WCHAR *wide = to_wide(relative);
                if (wide) {
                    for (WCHAR *p = wide; *p; p++) if (*p == L'/') *p = L'\\';
                    s_firstErrorPath = path_join(moduleRoot, wide);
                }
                free(wide);
                at = colon;
            }
        }
        if (*at == ':') {
            char *end = NULL;
            long line = strtol(at + 1, &end, 10);
            if (end != at + 1 && line > 0 && line <= INT_MAX) s_firstErrorLine = (int)line;
        }
    }
    if (!s_firstErrorPath) s_firstErrorPath = wcsdup_heap(path);
}

static char *scoped_owner(const char *scope, const char *name) {
    if (!scope || !*scope || _stricmp(scope, "bundled") == 0) return dup_string(name);
    size_t size = strlen(scope) + strlen(name) + 2;
    char *owner = (char *)malloc(size);
    if (owner) snprintf(owner, size, "%s/%s", scope, name);
    return owner;
}

static char *load_script_file(const WCHAR *directory, const WCHAR *fileName,
                              const char *scope, int folder) {
    char *plainName = to_utf8(fileName);
    char *name = plainName ? scoped_owner(scope, plainName) : NULL;
    WCHAR *root = folder ? path_join(directory, fileName) : NULL;
    WCHAR *path = folder ? path_join(root, L"init.lua") : path_join(directory, fileName);
    size_t codeLength = 0;
    char *code = path ? read_utf8_text_file_length(path, PLUGIN_MAX_SCRIPT_BYTES, &codeLength) : NULL;
    char *error;
    if (code) {
        error = run_script(name ? name : "plugin", code, codeLength, NULL, root);
    } else {
        const char *shown = name ? name : "A plugin";
        error = (char *)malloc(strlen(shown) + 64);
        if (error) sprintf(error, "%s: could not be read as UTF-8 text (1 MiB at most)", shown);
    }
    remember_load_error(path, root, error, name);
    free(code);
    free(path);
    free(root);
    free(name);
    free(plainName);
    return error;
}

static int remember_extension(const char *scope, const char *name, int enabled) {
    if (s_extensionCount >= (int)(sizeof(s_extensions) / sizeof(s_extensions[0]))) return 0;
    PluginExtension *extension = &s_extensions[s_extensionCount];
    extension->name = dup_string(name);
    extension->scope = dup_string(scope ? scope : "");
    if (!extension->name || !extension->scope) {
        free(extension->name);
        free(extension->scope);
        memset(extension, 0, sizeof(*extension));
        return 0;
    }
    extension->enabled = enabled;
    s_extensionCount++;
    return 1;
}

char *plugin_host_load_directory(const WCHAR *directory, const char *scope) {
    WCHAR *pattern = path_join(directory, L"*");
    if (!pattern) return dup_string("out of memory");
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW(pattern, &found);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) return NULL;

    ExtensionCandidate *candidates = NULL;
    size_t count = 0, capacity = 0;
    char *report = NULL;
    do {
        int folder = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (folder && (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0)) continue;
        if (folder ? !is_folder_extension(directory, found.cFileName) : !is_lua_script(&found)) continue;
        if (count == capacity) {
            size_t grown = capacity ? capacity * 2 : 16;
            ExtensionCandidate *larger = (ExtensionCandidate *)realloc(
                candidates, grown * sizeof(*candidates));
            if (!larger) { report = dup_string("out of memory listing plugins"); break; }
            candidates = larger;
            capacity = grown;
        }
        WCHAR *name = wcsdup_heap(found.cFileName);
        if (name) candidates[count++] = (ExtensionCandidate){ name, folder };
    } while (FindNextFileW(find, &found));
    FindClose(find);

    /* Name order, so scripts load (and their menu items appear) predictably. */
    if (count) qsort(candidates, count, sizeof(*candidates), compare_candidates);
    for (size_t i = 0; i < count; i++) {
        char *name = to_utf8(candidates[i].name);
        int enabled = !s_callbacks.plugin_enabled ||
            s_callbacks.plugin_enabled(scope ? scope : "", name ? name : "plugin");
        remember_extension(scope, name ? name : "plugin", enabled);
        if (enabled) report = append_line(report,
            load_script_file(directory, candidates[i].name, scope, candidates[i].folder));
        free(name);
        free(candidates[i].name);
    }
    free(candidates);
    return report;
}

void plugin_host_shutdown(void) {
    if (s_busy) return;
    free_menu();
    discard_commands_from(0);
    discard_handlers_from(0);
    for (int i = 0; i < s_extensionCount; i++) {
        free(s_extensions[i].name);
        free(s_extensions[i].scope);
    }
    s_extensionCount = 0;
    free(s_firstErrorPath);
    s_firstErrorPath = NULL;
    s_firstErrorLine = 0;
    if (s_lua) {
        lua_close(s_lua);
        s_lua = NULL;
    }
}

int plugin_host_is_busy(void) {
    return s_busy;
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

int plugin_host_extension_count(void) { return s_extensionCount; }
const char *plugin_host_extension_name(int index) {
    return index >= 0 && index < s_extensionCount ? s_extensions[index].name : NULL;
}
const char *plugin_host_extension_scope(int index) {
    return index >= 0 && index < s_extensionCount ? s_extensions[index].scope : NULL;
}
int plugin_host_extension_is_enabled(int index) {
    return index >= 0 && index < s_extensionCount && s_extensions[index].enabled;
}
const WCHAR *plugin_host_error_path(void) { return s_firstErrorPath; }
int plugin_host_error_line(void) { return s_firstErrorLine; }

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
    s_busy = 1;
    lua_editor_api_set_editor(hEdit);
    s_runningLabel = label;
    s_runningOwner = owner;
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
    SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
    if (error && (intptr_t)SendMessageA(hEdit, SCI_GETUNDOCURRENT, 0, 0) != undoBefore)
        SendMessageA(hEdit, SCI_UNDO, 0, 0);
    lua_editor_api_set_editor(NULL);
    s_runningLabel = NULL;
    s_runningOwner = NULL;
    s_timeLimited = 1;
    s_busy = 0;
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
    /* Handlers registered while these run wait for the next event. */
    int count = s_handlerCount;
    for (int i = 0; i < count; i++) {
        if (strcmp(s_handlers[i].event, event) != 0) continue;
        report = append_line(report, call_registered(s_handlers[i].ref, s_handlers[i].label,
                                                     s_handlers[i].owner, argument, -1, hEdit, 1));
    }
    return report;
}
