/* Lua plugin host: loads scripts that register menu commands, and runs those
   commands against a supplied Scintilla view. Holds no application state
   beyond the Lua state and the command list. */
#include "plugin_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "file_io.h"
#include "path_utils.h"
#include "text_codec.h"
#include "../third_party/scintilla/include/Scintilla.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define PLUGIN_MAX_SCRIPT_BYTES (1024 * 1024)
#define PLUGIN_MAX_CHOICES 10000

typedef struct {
    char *label;
    int ref;          /* the command's function, in the Lua registry */
    int placed;       /* by the menu definition, rather than by its label */
    char *overrides;  /* the built-in command it runs for (commands.register), or NULL */
    int builtIn;      /* from a script built into MarkEdit: no time limit */
    int separator;    /* menu.separator: label is a menu path, ref is LUA_NOREF */
} PluginCommand;

typedef struct {
    const char *event; /* one of EVENT_NAMES */
    char *label;       /* "script.lua: event", for dialog titles */
    int ref;
} PluginHandler;

static const char *const EVENT_NAMES[] = { "save", "open", "tab_changed" };

static lua_State *s_lua = NULL;
static PluginCommand s_commands[PLUGIN_MAX_COMMANDS];
static int s_commandCount = 0;
static PluginHandler s_handlers[PLUGIN_MAX_HANDLERS];
static int s_handlerCount = 0;
/* The script whose top-level code is running, while it loads, and whether
   it is one built into MarkEdit. */
static const char *s_loadingName = NULL;
static int s_loadingBuiltIn = 0;
/* Set while any plugin code runs, so nothing re-enters the Lua state from
   a message loop (a plugin dialog, say) mid-call. */
static int s_busy = 0;
/* Set only while a command runs; the editor API refuses to work without it. */
static HWND s_edit = NULL;
static const char *s_runningLabel = NULL;
/* Built-in commands are trusted not to hang, and may legitimately take
   longer than the limit on a large document. */
static int s_timeLimited = 1;
static PluginHostCallbacks s_callbacks;
static ULONGLONG s_callStarted = 0;

void plugin_host_set_callbacks(const PluginHostCallbacks *callbacks) {
    if (callbacks) s_callbacks = *callbacks;
    else memset(&s_callbacks, 0, sizeof(s_callbacks));
}

static char *dup_string(const char *value) {
    if (!value) value = "unknown error";
    size_t bytes = strlen(value) + 1;
    char *copy = (char *)malloc(bytes);
    if (copy) memcpy(copy, value, bytes);
    return copy;
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
    }
    if (first < s_commandCount) s_commandCount = first;
}

static void discard_handlers_from(int first) {
    for (int i = first; i < s_handlerCount; i++) {
        if (s_lua) luaL_unref(s_lua, LUA_REGISTRYINDEX, s_handlers[i].ref);
        free(s_handlers[i].label);
    }
    if (first < s_handlerCount) s_handlerCount = first;
}

/* ---- the API plugins see: editor.*, ui.*, menu.* ----
   Positions are 1-based byte offsets into the document's UTF-8 text, and a
   range is start..finish inclusive, as with string.sub: for a range s, f,
   editor.get_text():sub(s, f) is its text, and s, s - 1 is the empty range
   just before s. */

static HWND checked_editor(lua_State *L) {
    if (!s_edit) luaL_error(L, "the editor is only available while a plugin command runs");
    return s_edit;
}

static const char *current_title(void) {
    return s_runningLabel ? s_runningLabel : "MarkEdit Plugin";
}

/* Time spent waiting on the user doesn't count against the plugin. */
static void restart_time_limit(void) {
    s_callStarted = GetTickCount64();
}

static Sci_Position document_length(HWND hEdit) {
    return (Sci_Position)SendMessageA(hEdit, SCI_GETLENGTH, 0, 0);
}

static Sci_Position checked_position(lua_State *L, HWND hEdit, int arg) {
    lua_Integer position = luaL_checkinteger(L, arg);
    luaL_argcheck(L, position >= 1 && position <= (lua_Integer)document_length(hEdit) + 1, arg,
                  "position is outside the document");
    return (Sci_Position)(position - 1);
}

/* Reads the range in arguments arg and arg + 1 as Scintilla's [from, to). */
static void checked_range(lua_State *L, HWND hEdit, int arg, Sci_Position *from, Sci_Position *to) {
    lua_Integer length = (lua_Integer)document_length(hEdit);
    lua_Integer start = luaL_checkinteger(L, arg);
    lua_Integer finish = luaL_checkinteger(L, arg + 1);
    luaL_argcheck(L, start >= 1 && start <= length + 1, arg, "start is outside the document");
    luaL_argcheck(L, finish >= start - 1 && finish <= length, arg + 1,
                  "finish is outside the document or before start");
    *from = (Sci_Position)(start - 1);
    *to = (Sci_Position)finish;
}

static Sci_Position checked_line(lua_State *L, HWND hEdit, int arg) {
    lua_Integer line = luaL_checkinteger(L, arg);
    luaL_argcheck(L, line >= 1 && line <= (lua_Integer)SendMessageA(hEdit, SCI_GETLINECOUNT, 0, 0),
                  arg, "no such line");
    return (Sci_Position)(line - 1);
}

static void push_range(lua_State *L, Sci_Position from, Sci_Position to) {
    lua_pushinteger(L, (lua_Integer)from + 1);
    lua_pushinteger(L, (lua_Integer)to);
}

static void push_text_range(lua_State *L, HWND hEdit, Sci_Position from, Sci_Position to) {
    size_t length = (size_t)(to - from);
    luaL_Buffer buffer;
    char *text = luaL_buffinitsize(L, &buffer, length + 1);
    struct Sci_TextRangeFull range = { { from, to }, text };
    SendMessageA(hEdit, SCI_GETTEXTRANGEFULL, 0, (LPARAM)&range);
    luaL_pushresultsize(&buffer, length);
}

/* Length-counted, so text containing NUL bytes is written whole. */
static void replace_text_range(HWND hEdit, Sci_Position from, Sci_Position to,
                               const char *text, size_t length) {
    SendMessageA(hEdit, SCI_SETTARGETRANGE, (WPARAM)from, (LPARAM)to);
    SendMessageA(hEdit, SCI_REPLACETARGET, (WPARAM)length, (LPARAM)text);
}

static int editor_get_text(lua_State *L) {
    HWND hEdit = checked_editor(L);
    push_text_range(L, hEdit, 0, document_length(hEdit));
    return 1;
}

static int editor_set_text(lua_State *L) {
    HWND hEdit = checked_editor(L);
    size_t length;
    const char *text = luaL_checklstring(L, 1, &length);
    replace_text_range(hEdit, 0, document_length(hEdit), text, length);
    return 0;
}

/* The selection functions work on the main selection only, so what
   get_selection returns is exactly what replace_selection replaces. */
static int editor_get_selection(lua_State *L) {
    HWND hEdit = checked_editor(L);
    push_text_range(L, hEdit, (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0),
                    (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0));
    return 1;
}

static int editor_replace_selection(lua_State *L) {
    HWND hEdit = checked_editor(L);
    size_t length;
    const char *text = luaL_checklstring(L, 1, &length);
    Sci_Position from = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0);
    replace_text_range(hEdit, from, (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0),
                       text, length);
    /* Caret after the new text, as typing over a selection leaves it. */
    Sci_Position caret = from + (Sci_Position)length;
    SendMessageA(hEdit, SCI_SETSEL, (WPARAM)caret, (LPARAM)caret);
    SendMessageA(hEdit, SCI_SCROLLCARET, 0, 0);
    return 0;
}

static int editor_get_selection_range(lua_State *L) {
    HWND hEdit = checked_editor(L);
    push_range(L, (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0),
               (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0));
    return 2;
}

static int editor_set_selection(lua_State *L) {
    HWND hEdit = checked_editor(L);
    Sci_Position from, to;
    checked_range(L, hEdit, 1, &from, &to);
    SendMessageA(hEdit, SCI_SETSEL, (WPARAM)from, (LPARAM)to);
    SendMessageA(hEdit, SCI_SCROLLCARET, 0, 0);
    return 0;
}

static int editor_get_caret(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)SendMessageA(checked_editor(L), SCI_GETCURRENTPOS, 0, 0) + 1);
    return 1;
}

static int editor_set_caret(lua_State *L) {
    HWND hEdit = checked_editor(L);
    SendMessageA(hEdit, SCI_GOTOPOS, (WPARAM)checked_position(L, hEdit, 1), 0);
    return 0;
}

static int editor_replace_range(lua_State *L) {
    HWND hEdit = checked_editor(L);
    Sci_Position from, to;
    checked_range(L, hEdit, 1, &from, &to);
    size_t length;
    const char *text = luaL_checklstring(L, 3, &length);
    replace_text_range(hEdit, from, to, text, length);
    return 0;
}

static int editor_insert(lua_State *L) {
    HWND hEdit = checked_editor(L);
    size_t length;
    const char *text = luaL_checklstring(L, 1, &length);
    Sci_Position position = lua_isnoneornil(L, 2)
        ? (Sci_Position)SendMessageA(hEdit, SCI_GETCURRENTPOS, 0, 0)
        : checked_position(L, hEdit, 2);
    replace_text_range(hEdit, position, position, text, length);
    return 0;
}

static int editor_line_count(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)SendMessageA(checked_editor(L), SCI_GETLINECOUNT, 0, 0));
    return 1;
}

static int editor_current_line(lua_State *L) {
    HWND hEdit = checked_editor(L);
    Sci_Position caret = (Sci_Position)SendMessageA(hEdit, SCI_GETCURRENTPOS, 0, 0);
    lua_pushinteger(L, (lua_Integer)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)caret, 0) + 1);
    return 1;
}

/* A line's range excludes its line ending. */
static void line_bounds(HWND hEdit, Sci_Position line, Sci_Position *from, Sci_Position *to) {
    *from = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
    *to = (Sci_Position)SendMessageA(hEdit, SCI_GETLINEENDPOSITION, (WPARAM)line, 0);
}

static int editor_get_line(lua_State *L) {
    HWND hEdit = checked_editor(L);
    Sci_Position from, to;
    line_bounds(hEdit, checked_line(L, hEdit, 1), &from, &to);
    push_text_range(L, hEdit, from, to);
    return 1;
}

static int editor_line_range(lua_State *L) {
    HWND hEdit = checked_editor(L);
    Sci_Position from, to;
    line_bounds(hEdit, checked_line(L, hEdit, 1), &from, &to);
    push_range(L, from, to);
    return 2;
}

static int editor_length(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)document_length(checked_editor(L)));
    return 1;
}

static int editor_get_range(lua_State *L) {
    HWND hEdit = checked_editor(L);
    Sci_Position from, to;
    checked_range(L, hEdit, 1, &from, &to);
    push_text_range(L, hEdit, from, to);
    return 1;
}

/* The number of the line holding position (the last line for the position
   just past the end). */
static int editor_line_at(lua_State *L) {
    HWND hEdit = checked_editor(L);
    Sci_Position position = checked_position(L, hEdit, 1);
    lua_pushinteger(L, (lua_Integer)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)position, 0) + 1);
    return 1;
}

static int editor_eol(lua_State *L) {
    switch ((int)SendMessageA(checked_editor(L), SCI_GETEOLMODE, 0, 0)) {
        case SC_EOL_CR: lua_pushliteral(L, "\r"); break;
        case SC_EOL_LF: lua_pushliteral(L, "\n"); break;
        default: lua_pushliteral(L, "\r\n"); break;
    }
    return 1;
}

static int editor_file_path(lua_State *L) {
    checked_editor(L);
    if (!s_callbacks.file_path) return luaL_error(L, "editor.file_path is not available here");
    char *path = s_callbacks.file_path();
    if (path) lua_pushstring(L, path);
    else lua_pushnil(L);
    free(path);
    return 1;
}

static int editor_language(lua_State *L) {
    checked_editor(L);
    if (!s_callbacks.language) return luaL_error(L, "editor.language is not available here");
    lua_pushstring(L, s_callbacks.language());
    return 1;
}

/* ui.message(text [, title [, kind]]); kind is "info", "warning" or "error". */
static int ui_message(lua_State *L) {
    const char *text = luaL_checkstring(L, 1);
    const char *title = luaL_optstring(L, 2, current_title());
    static const char *const kinds[] = { "info", "warning", "error", NULL };
    const char *kind = kinds[luaL_checkoption(L, 3, "info", kinds)];
    if (!s_callbacks.message) return luaL_error(L, "ui.message is not available here");
    s_callbacks.message(title, text, kind);
    restart_time_limit();
    return 0;
}

static int ui_prompt(lua_State *L) {
    const char *label = luaL_checkstring(L, 1);
    const char *initial = luaL_optstring(L, 2, "");
    const char *title = luaL_optstring(L, 3, current_title());
    if (!s_callbacks.prompt) return luaL_error(L, "ui.prompt is not available here");
    char *answer = s_callbacks.prompt(title, label, initial);
    restart_time_limit();
    if (answer) lua_pushstring(L, answer);
    else lua_pushnil(L);
    free(answer);
    return 1;
}

static int ui_choose(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char *label = luaL_optstring(L, 2, "Choose an item:");
    const char *title = luaL_optstring(L, 3, current_title());
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
        items[i] = lua_tostring(L, -1);
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

/* Adds an entry the application places by its label: a menu.add command
   (fn at stack index 2), or a menu.separator. */
static int add_labelled_command(lua_State *L, const char *label, int separator) {
    if (s_commandCount >= PLUGIN_MAX_COMMANDS)
        return luaL_error(L, "too many plugin commands (the limit is %d)", PLUGIN_MAX_COMMANDS);
    int ref = LUA_NOREF;
    if (!separator) {
        lua_pushvalue(L, 2);
        ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    char *copy = dup_string(label);
    if (!copy) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return luaL_error(L, "out of memory");
    }
    s_commands[s_commandCount].label = copy;
    s_commands[s_commandCount].ref = ref;
    s_commands[s_commandCount].placed = 0;
    s_commands[s_commandCount].overrides = NULL;
    s_commands[s_commandCount].builtIn = s_loadingBuiltIn;
    s_commands[s_commandCount].separator = separator;
    s_commandCount++;
    return 0;
}

static int menu_add(lua_State *L) {
    const char *label = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    return add_labelled_command(L, label, 0);
}

/* menu.separator([path]): a separator after whatever this plugin and those
   before it have put in the menu path names so far - the Plugins menu when
   path is omitted, "Lines" its Lines submenu, "/Snippets/Markdown" a
   submenu of a menu-bar menu. */
static int menu_separator(lua_State *L) {
    const char *path = luaL_optstring(L, 1, "");
    return add_labelled_command(L, path, 1);
}

/* commands.register(name, fn [, title]): fn runs in place of the built-in
   command called name, wherever it is invoked from (menu, toolbar, palette
   or shortcut). The last registration for a name wins. title names it in
   dialogs. Whether name is a real command is up to the application. */
static int commands_register(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const char *title = luaL_optstring(L, 3, "MarkEdit");
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
    return 0;
}

/* events.on(name, fn): fn(path) runs on each such event. */
static int events_on(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
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
    s_handlerCount++;
    return 0;
}

static const luaL_Reg editor_functions[] = {
    {"get_text", editor_get_text},
    {"set_text", editor_set_text},
    {"get_selection", editor_get_selection},
    {"replace_selection", editor_replace_selection},
    {"get_selection_range", editor_get_selection_range},
    {"set_selection", editor_set_selection},
    {"get_caret", editor_get_caret},
    {"set_caret", editor_set_caret},
    {"replace_range", editor_replace_range},
    {"insert", editor_insert},
    {"line_count", editor_line_count},
    {"current_line", editor_current_line},
    {"get_line", editor_get_line},
    {"line_range", editor_line_range},
    {"line_at", editor_line_at},
    {"length", editor_length},
    {"get_range", editor_get_range},
    {"eol", editor_eol},
    {"file_path", editor_file_path},
    {"language", editor_language},
    {NULL, NULL}
};
static const luaL_Reg ui_functions[] = {
    {"message", ui_message},
    {"prompt", ui_prompt},
    {"choose", ui_choose},
    {NULL, NULL}
};
static const luaL_Reg menu_functions[] = { {"add", menu_add}, {"separator", menu_separator}, {NULL, NULL} };
static const luaL_Reg events_functions[] = { {"on", events_on}, {NULL, NULL} };
static const luaL_Reg commands_functions[] = { {"register", commands_register}, {NULL, NULL} };

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
    lua_pop(L, 1);
    lua_pushvalue(L, -1);
    s_commands[s_commandCount].label = path;
    s_commands[s_commandCount].ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s_commands[s_commandCount].placed = 1;
    s_commands[s_commandCount].overrides = NULL;
    s_commands[s_commandCount].builtIn = s_loadingBuiltIn;
    s_commands[s_commandCount].separator = 0;
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
            if (strcmp(lua_tostring(L, -1), "-") != 0 || depth == 0)
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
            item->name = checked_copy(L, lua_tostring(L, -1));
            lua_pop(L, 2);
            continue;
        }
        lua_pop(L, 1);
        if (lua_rawgeti(L, -1, 1) != LUA_TSTRING)
            menu_error(L, depth, "an item needs a text label first: { label, action }");
        const char *label = lua_tostring(L, -1); /* kept alive by the item table */
        item->label = checked_copy(L, label);
        lua_pop(L, 1);
        int actionType = lua_rawgeti(L, -1, 2);
        if (depth == 0 && actionType != LUA_TTABLE) {
            s_menuPath[0] = label;
            menu_error(L, 1, "a top-level item must be a menu: { label, { items } }");
        }
        if (actionType == LUA_TSTRING) {
            item->kind = PLUGIN_MENU_COMMAND;
            item->name = checked_copy(L, lua_tostring(L, -1));
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

    luaL_newlib(L, editor_functions);
    lua_setglobal(L, "editor");
    luaL_newlib(L, ui_functions);
    lua_setglobal(L, "ui");
    luaL_newlib(L, menu_functions);
    lua_setglobal(L, "menu");
    luaL_newlib(L, events_functions);
    lua_setglobal(L, "events");
    luaL_newlib(L, commands_functions);
    lua_setglobal(L, "commands");
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
static char *run_script(const char *name, const char *code, size_t length, lua_CFunction runner) {
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
        error = protected_call(runner ? 1 : 0);
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
    return run_script(name, code, length, NULL);
}

char *plugin_host_load_builtin(const char *name, const char *code, size_t length) {
    s_loadingBuiltIn = 1;
    char *error = run_script(name, code, length, NULL);
    s_loadingBuiltIn = 0;
    return error;
}

char *plugin_host_load_menu(const char *name, const char *code, size_t length) {
    /* run_script() would refuse too, but only after the current definition
       had already been thrown away. */
    if (s_busy) return dup_string("Plugins can't be loaded while a plugin is running.");
    free_menu();
    char *error = run_script(name, code, length, run_menu_definition);
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

static int compare_names(const void *a, const void *b) {
    return _wcsicmp(*(const WCHAR *const *)a, *(const WCHAR *const *)b);
}

/* FindFirstFileW's "*.lua" also matches longer extensions such as ".luac"
   through 8.3 short names, so the extension is checked exactly. */
static int is_lua_script(const WIN32_FIND_DATAW *found) {
    if (found->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;
    const WCHAR *dot = wcsrchr(found->cFileName, L'.');
    return dot && _wcsicmp(dot, L".lua") == 0;
}

static char *load_script_file(const WCHAR *directory, const WCHAR *fileName) {
    char *name = to_utf8(fileName);
    WCHAR *path = path_join(directory, fileName);
    char *code = path ? read_utf8_text_file(path, PLUGIN_MAX_SCRIPT_BYTES) : NULL;
    char *error;
    if (code) {
        error = plugin_host_load_chunk(name ? name : "plugin", code, strlen(code));
    } else {
        const char *shown = name ? name : "A plugin";
        error = (char *)malloc(strlen(shown) + 64);
        if (error) sprintf(error, "%s: could not be read as UTF-8 text (1 MiB at most)", shown);
    }
    free(code);
    free(path);
    free(name);
    return error;
}

char *plugin_host_load_directory(const WCHAR *directory) {
    WCHAR *pattern = path_join(directory, L"*.lua");
    if (!pattern) return dup_string("out of memory");
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW(pattern, &found);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) return NULL;

    WCHAR **names = NULL;
    size_t count = 0, capacity = 0;
    char *report = NULL;
    do {
        if (!is_lua_script(&found)) continue;
        if (count == capacity) {
            size_t grown = capacity ? capacity * 2 : 16;
            WCHAR **larger = (WCHAR **)realloc(names, grown * sizeof(*names));
            if (!larger) { report = dup_string("out of memory listing plugins"); break; }
            names = larger;
            capacity = grown;
        }
        if ((names[count] = wcsdup_heap(found.cFileName)) != NULL) count++;
    } while (FindNextFileW(find, &found));
    FindClose(find);

    /* Name order, so scripts load (and their menu items appear) predictably. */
    if (count) qsort(names, count, sizeof(*names), compare_names);
    for (size_t i = 0; i < count; i++) {
        report = append_line(report, load_script_file(directory, names[i]));
        free(names[i]);
    }
    free(names);
    return report;
}

void plugin_host_shutdown(void) {
    if (s_busy) return;
    free_menu();
    discard_commands_from(0);
    discard_handlers_from(0);
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

int plugin_host_command_is_placed(int index) {
    return index >= 0 && index < s_commandCount && s_commands[index].placed;
}

int plugin_host_command_is_separator(int index) {
    return index >= 0 && index < s_commandCount && s_commands[index].separator;
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
static char *call_registered(int ref, const char *label, const char *argument, HWND hEdit, int timeLimited) {
    s_busy = 1;
    s_edit = hEdit;
    s_runningLabel = label;
    s_timeLimited = timeLimited;
    /* An empty undo group isn't recorded, so the undo position only moves
       if the function really changed the document. */
    intptr_t undoBefore = (intptr_t)SendMessageA(hEdit, SCI_GETUNDOCURRENT, 0, 0);
    SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
    lua_rawgeti(s_lua, LUA_REGISTRYINDEX, ref);
    if (argument) lua_pushstring(s_lua, argument);
    else lua_pushnil(s_lua);
    char *error = protected_call(1);
    SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
    if (error && (intptr_t)SendMessageA(hEdit, SCI_GETUNDOCURRENT, 0, 0) != undoBefore)
        SendMessageA(hEdit, SCI_UNDO, 0, 0);
    s_edit = NULL;
    s_runningLabel = NULL;
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
    return call_registered(command->ref, command->label, NULL, hEdit, !command->builtIn);
}

char *plugin_host_fire(const char *event, const char *argument, HWND hEdit) {
    if (s_busy || !s_lua) return NULL;
    char *report = NULL;
    /* Handlers registered while these run wait for the next event. */
    int count = s_handlerCount;
    for (int i = 0; i < count; i++) {
        if (strcmp(s_handlers[i].event, event) != 0) continue;
        report = append_line(report, call_registered(s_handlers[i].ref, s_handlers[i].label,
                                                     argument, hEdit, 1));
    }
    return report;
}
