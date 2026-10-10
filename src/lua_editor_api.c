#include "lua_editor_api.h"

#include <stdlib.h>
#include <string.h>

#include "plugin_host.h"
#include "../third_party/scintilla/include/Scintilla.h"
#include "lauxlib.h"

static HWND s_edit;
static PluginHostCallbacks s_callbacks;

void lua_editor_api_set_callbacks(const PluginHostCallbacks *callbacks) {
    if (callbacks) s_callbacks = *callbacks;
    else memset(&s_callbacks, 0, sizeof(s_callbacks));
}

void lua_editor_api_set_editor(HWND editor) {
    s_edit = editor;
}

static HWND checked_editor(lua_State *L) {
    if (!s_edit) luaL_error(L, "the editor is only available while a plugin command or event handler runs");
    return s_edit;
}

void lua_editor_api_require_editor(lua_State *L) {
    (void)checked_editor(L);
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

/* Selection numbers exposed to Lua are 1-based. Omitting one keeps the
   original API behaviour: read the main selection, or replace all selections
   with a single one when changing it. */
static int checked_selection(lua_State *L, HWND hEdit, int arg) {
    if (lua_isnoneornil(L, arg))
        return (int)SendMessageA(hEdit, SCI_GETMAINSELECTION, 0, 0);
    lua_Integer selection = luaL_checkinteger(L, arg);
    lua_Integer count = (lua_Integer)SendMessageA(hEdit, SCI_GETSELECTIONS, 0, 0);
    luaL_argcheck(L, selection >= 1 && selection <= count, arg, "no such selection");
    return (int)(selection - 1);
}

static Sci_Position selection_start(HWND hEdit, int selection) {
    return (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONNSTART, (WPARAM)selection, 0);
}

static Sci_Position selection_end(HWND hEdit, int selection) {
    return (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONNEND, (WPARAM)selection, 0);
}

static int editor_get_selection(lua_State *L) {
    HWND hEdit = checked_editor(L);
    int selection = checked_selection(L, hEdit, 1);
    push_text_range(L, hEdit, selection_start(hEdit, selection), selection_end(hEdit, selection));
    return 1;
}

static int editor_replace_selection(lua_State *L) {
    HWND hEdit = checked_editor(L);
    size_t length;
    const char *text = luaL_checklstring(L, 1, &length);
    int selection = checked_selection(L, hEdit, 2);
    Sci_Position from = selection_start(hEdit, selection);
    replace_text_range(hEdit, from, selection_end(hEdit, selection), text, length);
    /* Caret after the new text, as typing over a selection leaves it. */
    Sci_Position caret = from + (Sci_Position)length;
    if (lua_isnoneornil(L, 2)) {
        SendMessageA(hEdit, SCI_SETSEL, (WPARAM)caret, (LPARAM)caret);
    } else {
        SendMessageA(hEdit, SCI_SETSELECTIONNANCHOR, (WPARAM)selection, (LPARAM)caret);
        SendMessageA(hEdit, SCI_SETSELECTIONNCARET, (WPARAM)selection, (LPARAM)caret);
    }
    SendMessageA(hEdit, SCI_SCROLLCARET, 0, 0);
    return 0;
}

static int editor_get_selection_range(lua_State *L) {
    HWND hEdit = checked_editor(L);
    int selection = checked_selection(L, hEdit, 1);
    push_range(L, selection_start(hEdit, selection), selection_end(hEdit, selection));
    return 2;
}

static int editor_set_selection(lua_State *L) {
    HWND hEdit = checked_editor(L);
    Sci_Position from, to;
    checked_range(L, hEdit, 1, &from, &to);
    if (lua_isnoneornil(L, 3)) {
        SendMessageA(hEdit, SCI_SETSEL, (WPARAM)from, (LPARAM)to);
    } else {
        int selection = checked_selection(L, hEdit, 3);
        SendMessageA(hEdit, SCI_SETSELECTIONNANCHOR, (WPARAM)selection, (LPARAM)from);
        SendMessageA(hEdit, SCI_SETSELECTIONNCARET, (WPARAM)selection, (LPARAM)to);
    }
    SendMessageA(hEdit, SCI_SCROLLCARET, 0, 0);
    return 0;
}

static int editor_selection_count(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)SendMessageA(checked_editor(L), SCI_GETSELECTIONS, 0, 0));
    return 1;
}

static int editor_get_selections(lua_State *L) {
    HWND hEdit = checked_editor(L);
    int count = (int)SendMessageA(hEdit, SCI_GETSELECTIONS, 0, 0);
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; i++) {
        Sci_Position start = selection_start(hEdit, i);
        Sci_Position finish = selection_end(hEdit, i);
        Sci_Position caret = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONNCARET, (WPARAM)i, 0);
        Sci_Position anchor = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONNANCHOR, (WPARAM)i, 0);
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, (lua_Integer)start + 1); lua_setfield(L, -2, "start");
        lua_pushinteger(L, (lua_Integer)finish); lua_setfield(L, -2, "finish");
        lua_pushinteger(L, (lua_Integer)caret + 1); lua_setfield(L, -2, "caret");
        lua_pushinteger(L, (lua_Integer)anchor + 1); lua_setfield(L, -2, "anchor");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

typedef struct {
    Sci_Position start;
    Sci_Position finish;
    Sci_Position finalCaret;
    const char *text;
    size_t length;
    int originalIndex;
} SelectionReplacement;

static int compare_replacements_descending(const void *left, const void *right) {
    const SelectionReplacement *a = (const SelectionReplacement *)left;
    const SelectionReplacement *b = (const SelectionReplacement *)right;
    return a->start < b->start ? 1 : a->start > b->start ? -1 : 0;
}

/* Replaces every current selection from the end of the document backwards,
   then recreates one caret per replacement. The argument is either one
   string used for every selection or a table containing one string each. */
static int editor_replace_selections(lua_State *L) {
    HWND hEdit = checked_editor(L);
    int count = (int)SendMessageA(hEdit, SCI_GETSELECTIONS, 0, 0);
    int mainSelection = (int)SendMessageA(hEdit, SCI_GETMAINSELECTION, 0, 0);
    int oneText = lua_type(L, 1) == LUA_TSTRING;
    luaL_argcheck(L, oneText || lua_istable(L, 1), 1, "expected a string or table of strings");
    if (!oneText)
        luaL_argcheck(L, (lua_Integer)lua_rawlen(L, 1) == count, 1,
                      "table length must match selection count");
    if (!oneText) {
        for (int i = 0; i < count; i++) {
            lua_rawgeti(L, 1, i + 1);
            luaL_argcheck(L, lua_type(L, -1) == LUA_TSTRING, 1,
                          "every replacement must be a string");
            lua_pop(L, 1);
        }
    }

    SelectionReplacement *items = (SelectionReplacement *)calloc((size_t)count, sizeof(*items));
    Sci_Position *carets = (Sci_Position *)calloc((size_t)count, sizeof(*carets));
    if (!items || !carets) {
        free(items); free(carets);
        return luaL_error(L, "out of memory");
    }
    for (int i = 0; i < count; i++) {
        items[i].start = selection_start(hEdit, i);
        items[i].finish = selection_end(hEdit, i);
        items[i].originalIndex = i;
        if (oneText) {
            items[i].text = lua_tolstring(L, 1, &items[i].length);
        } else {
            lua_rawgeti(L, 1, i + 1);
            items[i].text = lua_tolstring(L, -1, &items[i].length);
            lua_pop(L, 1);
        }
    }

    /* Lua owns the strings above and the argument keeps them reachable. */
    qsort(items, (size_t)count, sizeof(*items), compare_replacements_descending);
    for (int i = 0; i < count; i++)
        replace_text_range(hEdit, items[i].start, items[i].finish, items[i].text, items[i].length);

    /* Positions in the final document include every edit that was originally
       before this selection. Rebuild in document order; Scintilla may merge
       coincident carets, which is the same behaviour as interactive editing. */
    int finalMainSelection = 0;
    for (int i = count - 1; i >= 0; i--) {
        Sci_Position shift = 0;
        for (int j = count - 1; j > i; j--)
            shift += (Sci_Position)items[j].length - (items[j].finish - items[j].start);
        items[i].finalCaret = items[i].start + shift + (Sci_Position)items[i].length;
        carets[count - 1 - i] = items[i].finalCaret;
        if (items[i].originalIndex == mainSelection) finalMainSelection = count - 1 - i;
    }
    SendMessageA(hEdit, SCI_SETSEL, (WPARAM)carets[0], (LPARAM)carets[0]);
    for (int i = 1; i < count; i++)
        SendMessageA(hEdit, SCI_ADDSELECTION, (WPARAM)carets[i], (LPARAM)carets[i]);
    SendMessageA(hEdit, SCI_SETMAINSELECTION, (WPARAM)finalMainSelection, 0);
    SendMessageA(hEdit, SCI_SCROLLCARET, 0, 0);
    free(carets);
    free(items);
    return 0;
}

static int editor_add_selection(lua_State *L) {
    HWND hEdit = checked_editor(L);
    Sci_Position from, to;
    checked_range(L, hEdit, 1, &from, &to);
    SendMessageA(hEdit, SCI_ADDSELECTION, (WPARAM)to, (LPARAM)from);
    lua_pushinteger(L, (lua_Integer)SendMessageA(hEdit, SCI_GETSELECTIONS, 0, 0));
    return 1;
}

static int editor_drop_selection(lua_State *L) {
    HWND hEdit = checked_editor(L);
    int selection = checked_selection(L, hEdit, 1);
    luaL_argcheck(L, SendMessageA(hEdit, SCI_GETSELECTIONS, 0, 0) > 1, 1,
                  "cannot drop the only selection");
    SendMessageA(hEdit, SCI_DROPSELECTIONN, (WPARAM)selection, 0);
    return 0;
}

static int editor_get_main_selection(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)SendMessageA(checked_editor(L), SCI_GETMAINSELECTION, 0, 0) + 1);
    return 1;
}

static int editor_set_main_selection(lua_State *L) {
    HWND hEdit = checked_editor(L);
    int selection = checked_selection(L, hEdit, 1);
    SendMessageA(hEdit, SCI_SETMAINSELECTION, (WPARAM)selection, 0);
    return 0;
}

static int editor_get_caret(lua_State *L) {
    HWND hEdit = checked_editor(L);
    int selection = checked_selection(L, hEdit, 1);
    lua_pushinteger(L, (lua_Integer)SendMessageA(hEdit, SCI_GETSELECTIONNCARET,
                                                (WPARAM)selection, 0) + 1);
    return 1;
}

static int editor_set_caret(lua_State *L) {
    HWND hEdit = checked_editor(L);
    Sci_Position position = checked_position(L, hEdit, 1);
    if (lua_isnoneornil(L, 2)) {
        SendMessageA(hEdit, SCI_GOTOPOS, (WPARAM)position, 0);
    } else {
        int selection = checked_selection(L, hEdit, 2);
        SendMessageA(hEdit, SCI_SETSELECTIONNANCHOR, (WPARAM)selection, (LPARAM)position);
        SendMessageA(hEdit, SCI_SETSELECTIONNCARET, (WPARAM)selection, (LPARAM)position);
        SendMessageA(hEdit, SCI_SCROLLCARET, 0, 0);
    }
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

static const luaL_Reg editor_functions[] = {
    {"get_text", editor_get_text},
    {"set_text", editor_set_text},
    {"get_selection", editor_get_selection},
    {"replace_selection", editor_replace_selection},
    {"get_selection_range", editor_get_selection_range},
    {"set_selection", editor_set_selection},
    {"selection_count", editor_selection_count},
    {"get_selections", editor_get_selections},
    {"replace_selections", editor_replace_selections},
    {"add_selection", editor_add_selection},
    {"drop_selection", editor_drop_selection},
    {"get_main_selection", editor_get_main_selection},
    {"set_main_selection", editor_set_main_selection},
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


void lua_editor_api_push(lua_State *L) {
    luaL_newlib(L, editor_functions);
}
