/* The names menus.lua uses for built-in commands, and the shortcut text the
   menu shows beside each. No application state. */
#include "menu_commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "command_ids.h"

static const MenuCommand COMMANDS[] = {
    {"file.new", IDM_FILE_NEW, "Ctrl+N"},
    {"file.open", IDM_FILE_OPEN, "Ctrl+O"},
    {"file.reopen_utf8", IDM_FILE_REOPEN_UTF8, NULL},
    {"file.reopen_utf16le", IDM_FILE_REOPEN_UTF16LE, NULL},
    {"file.reopen_utf16be", IDM_FILE_REOPEN_UTF16BE, NULL},
    {"file.reopen_ansi", IDM_FILE_REOPEN_ANSI, NULL},
    {"file.quick_open", IDM_FILE_QUICK_OPEN, "Ctrl+K"},
    {"file.find_in_folder", IDM_FILE_FIND_FOLDER, NULL},
    {"file.replace_in_folder", IDM_REPLACE_WORKSPACE, NULL},
    {"file.open_workspace", IDM_OPEN_WORKSPACE, NULL},
    {"file.insert_file", IDM_FILE_INSERT, NULL},
    {"file.save", IDM_FILE_SAVE, "Ctrl+S"},
    {"file.save_as", IDM_FILE_SAVEAS, "Ctrl+Shift+S"},
    {"file.save_all", IDM_FILE_SAVE_ALL, "Ctrl+Alt+S"},
    {"file.export_html", IDM_FILE_EXPORT_HTML, NULL},
    {"file.export_pdf", IDM_FILE_EXPORT_PDF, NULL},
    {"file.print", IDM_FILE_PRINT, "Ctrl+P"},
    {"file.open_terminal", IDM_FILE_OPEN_TERMINAL, NULL},
    {"file.show_in_explorer", IDM_FILE_SHOW_EXPLORER, NULL},
    {"file.close_tab", IDM_FILE_CLOSE, "Ctrl+W"},
    {"file.reopen_closed_tab", IDM_FILE_REOPEN_CLOSED, "Ctrl+Shift+T"},
    {"file.go_back", IDM_FILE_GO_BACK, "Alt+Left"},
    {"file.exit", IDM_FILE_EXIT, NULL},

    {"edit.undo", IDM_EDIT_UNDO, "Ctrl+Z"},
    {"edit.redo", IDM_EDIT_REDO, "Ctrl+Y"},
    {"edit.cut", IDM_EDIT_CUT, "Ctrl+X"},
    {"edit.copy", IDM_EDIT_COPY, "Ctrl+C"},
    {"edit.paste", IDM_EDIT_PASTE, "Ctrl+V"},
    {"edit.delete", IDM_EDIT_DELETE, "Del"},
    {"edit.select_all", IDM_EDIT_SELECTALL, "Ctrl+A"},
    {"edit.duplicate_line", IDM_EDIT_DUPLICATE_LINE, "Ctrl+D"},
    {"edit.move_line_up", IDM_EDIT_MOVE_LINE_UP, "Alt+Up"},
    {"edit.move_line_down", IDM_EDIT_MOVE_LINE_DOWN, "Alt+Down"},
    {"edit.delete_line", IDM_EDIT_DELETE_LINE, "Ctrl+Shift+K"},
    {"edit.join_lines", IDM_EDIT_JOIN_LINES, "Ctrl+J"},
    {"edit.toggle_comment", IDM_EDIT_TOGGLE_COMMENT, "Ctrl+/"},
    {"edit.trim_whitespace", IDM_EDIT_TRIM_WHITESPACE, NULL},
    {"edit.select_next", IDM_EDIT_SELECT_NEXT, "Ctrl+Shift+D"},
    {"edit.select_all_occurrences", IDM_EDIT_SELECT_ALL_OCCURRENCES, "Ctrl+Shift+L"},
    {"edit.match_brace", IDM_EDIT_MATCH_BRACE, "Ctrl+]"},
    {"edit.bookmark_toggle", IDM_EDIT_BOOKMARK_TOGGLE, "Ctrl+F2"},
    {"edit.bookmark_next", IDM_EDIT_BOOKMARK_NEXT, "F2"},
    {"edit.bookmark_previous", IDM_EDIT_BOOKMARK_PREV, "Shift+F2"},
    {"edit.bookmark_clear", IDM_EDIT_BOOKMARK_CLEAR, NULL},
    {"edit.uppercase", IDM_EDIT_UPPERCASE, NULL},
    {"edit.lowercase", IDM_EDIT_LOWERCASE, NULL},
    {"edit.title_case", IDM_EDIT_TITLECASE, NULL},
    {"edit.sort_ascending", IDM_EDIT_SORT_ASC, NULL},
    {"edit.sort_descending", IDM_EDIT_SORT_DESC, NULL},
    {"edit.remove_duplicate_lines", IDM_EDIT_SORT_DEDUPE, NULL},
    {"edit.tabs_to_spaces", IDM_EDIT_TABS_TO_SPACES, NULL},
    {"edit.spaces_to_tabs", IDM_EDIT_SPACES_TO_TABS, NULL},
    {"edit.eol_crlf", IDM_EDIT_EOL_CRLF, NULL},
    {"edit.eol_lf", IDM_EDIT_EOL_LF, NULL},
    {"edit.eol_cr", IDM_EDIT_EOL_CR, NULL},
    {"edit.encoding_utf8", IDM_EDIT_ENCODING_UTF8, NULL},
    {"edit.encoding_utf8_bom", IDM_EDIT_ENCODING_UTF8_BOM, NULL},
    {"edit.encoding_utf16le", IDM_EDIT_ENCODING_UTF16LE, NULL},
    {"edit.encoding_utf16be", IDM_EDIT_ENCODING_UTF16BE, NULL},
    {"edit.encoding_ansi", IDM_EDIT_ENCODING_ANSI, NULL},
    {"edit.find", IDM_EDIT_FIND, "Ctrl+F"},
    {"edit.find_next", IDM_EDIT_FINDNEXT, "F3"},
    {"edit.replace", IDM_EDIT_REPLACE, "Ctrl+H"},
    {"edit.go_to_line", IDM_EDIT_GOTOLINE, "Ctrl+G"},

    {"view.preview", IDM_VIEW_TOGGLEPREVIEW, "Ctrl+Shift+V"},
    {"view.editor", IDM_VIEW_TOGGLEEDITOR, "Ctrl+Shift+E"},
    {"view.next_tab", IDM_VIEW_NEXT_TAB, "Ctrl+Tab"},
    {"view.previous_tab", IDM_VIEW_PREVIOUS_TAB, "Ctrl+Shift+Tab"},
    {"view.cycle_panes", IDM_VIEW_CYCLE_PANE, "Ctrl+F6"},
    {"view.theme_light", IDM_VIEW_THEME_GITHUB_LIGHT, NULL},
    {"view.theme_dark", IDM_VIEW_THEME_GITHUB_DARK, NULL},
    {"view.preview_theme_light", IDM_VIEW_PREVIEW_THEME_LIGHT, NULL},
    {"view.preview_theme_dark", IDM_VIEW_PREVIEW_THEME_DARK, NULL},
    {"view.custom_css", IDM_VIEW_CUSTOM_CSS, NULL},
    {"view.clear_custom_css", IDM_VIEW_CLEAR_CUSTOM_CSS, NULL},
    {"view.font", IDM_FORMAT_FONT, NULL},
    {"view.preview_on_left", IDM_VIEW_PREVIEW_ON_LEFT, NULL},
    {"view.focus_mode", IDM_VIEW_FOCUS_MODE, "Ctrl+Shift+F11"},
    {"view.word_wrap", IDM_VIEW_WORD_WRAP, "Alt+Z"},
    {"view.spell_check", IDM_VIEW_SPELL_CHECK, NULL},
    {"view.spell_check_language", IDM_VIEW_SPELL_LANGUAGE, NULL},
    {"view.outline", IDM_VIEW_OUTLINE, NULL},
    {"view.file_explorer", IDM_VIEW_EXPLORER, "Ctrl+Shift+X"},
    {"view.minimap", IDM_VIEW_MINIMAP, NULL},
    {"view.split_editor", IDM_VIEW_TOGGLESPLIT, "Ctrl+\\"},
    {"view.reset_split_editor", IDM_VIEW_RESET_SPLIT_EDITOR, NULL},
    {"view.show_whitespace", IDM_VIEW_SHOW_WHITESPACE, NULL},
    {"view.indent_guides", IDM_VIEW_INDENT_GUIDES, NULL},
    {"view.use_spaces", IDM_VIEW_USE_SPACES_INDENT, NULL},
    {"view.toggle_fold", IDM_VIEW_FOLD_TOGGLE, NULL},
    {"view.fold_all", IDM_VIEW_FOLD_ALL, NULL},
    {"view.unfold_all", IDM_VIEW_UNFOLD_ALL, NULL},
    {"view.zoom_in", IDM_VIEW_ZOOM_IN, "Ctrl+="},
    {"view.zoom_out", IDM_VIEW_ZOOM_OUT, "Ctrl+-"},
    {"view.zoom_reset", IDM_VIEW_ZOOM_RESET, "Ctrl+0"},

    {"markdown.bold", IDM_FMT_BOLD, NULL},
    {"markdown.italic", IDM_FMT_ITALIC, NULL},
    {"markdown.strikethrough", IDM_FMT_STRIKETHROUGH, NULL},
    {"markdown.inline_code", IDM_FMT_CODE, NULL},
    {"markdown.heading1", IDM_FMT_H1, NULL},
    {"markdown.heading2", IDM_FMT_H2, NULL},
    {"markdown.heading3", IDM_FMT_H3, NULL},
    {"markdown.blockquote", IDM_FMT_QUOTE, NULL},
    {"markdown.bullet_list", IDM_FMT_BULLETLIST, NULL},
    {"markdown.numbered_list", IDM_FMT_NUMBERLIST, NULL},
    {"markdown.task_list", IDM_FMT_TASKLIST, NULL},
    {"markdown.link", IDM_FMT_LINK, NULL},
    {"markdown.image", IDM_FMT_IMAGE, NULL},
    {"markdown.table", IDM_FMT_TABLE, NULL},
    {"markdown.table_editor", IDM_FMT_TABLE_EDITOR, NULL},
    {"markdown.table_row_above", IDM_FMT_TABLE_ROW_ABOVE, NULL},
    {"markdown.table_row_below", IDM_FMT_TABLE_ROW_BELOW, NULL},
    {"markdown.table_row_delete", IDM_FMT_TABLE_ROW_DELETE, NULL},
    {"markdown.table_column_left", IDM_FMT_TABLE_COL_LEFT, NULL},
    {"markdown.table_column_right", IDM_FMT_TABLE_COL_RIGHT, NULL},
    {"markdown.table_column_delete", IDM_FMT_TABLE_COL_DELETE, NULL},
    {"markdown.table_sort_ascending", IDM_TABLE_SORT_ASC, NULL},
    {"markdown.table_sort_descending", IDM_TABLE_SORT_DESC, NULL},
    {"markdown.table_align_left", IDM_TABLE_ALIGN_LEFT, NULL},
    {"markdown.table_align_center", IDM_TABLE_ALIGN_CENTER, NULL},
    {"markdown.table_align_right", IDM_TABLE_ALIGN_RIGHT, NULL},
    {"markdown.code_block", IDM_FMT_CODEBLOCK, NULL},
    {"markdown.footnote", IDM_FMT_FOOTNOTE, NULL},
    {"markdown.horizontal_rule", IDM_FMT_HRULE, NULL},
    {"markdown.alert", IDM_FMT_ALERT, NULL},

    {"json.format", IDM_FMT_JSON_PRETTY, NULL},
    {"json.minify", IDM_FMT_JSON_MINIFY, NULL},

    {"html.html", IDM_FMT_HTML_HTML, NULL},
    {"html.head", IDM_FMT_HTML_HEAD, NULL},
    {"html.body", IDM_FMT_HTML_BODY, NULL},
    {"html.paragraph", IDM_FMT_HTML_PARAGRAPH, NULL},
    {"html.bold", IDM_FMT_HTML_BOLD, NULL},
    {"html.italic", IDM_FMT_HTML_ITALIC, NULL},
    {"html.underline", IDM_FMT_HTML_UNDERLINE, NULL},
    {"html.div", IDM_FMT_HTML_DIV, NULL},
    {"html.span", IDM_FMT_HTML_SPAN, NULL},
    {"html.link", IDM_FMT_HTML_LINK, NULL},
    {"html.image", IDM_FMT_HTML_IMAGE, NULL},
    {"html.table", IDM_FMT_HTML_TABLE, NULL},
    {"html.bullet_list", IDM_FMT_HTML_BULLETLIST, NULL},
    {"html.numbered_list", IDM_FMT_HTML_NUMBERLIST, NULL},
    {"html.line_break", IDM_FMT_HTML_BREAK, NULL},

    {"tools.settings", IDM_SETTINGS, NULL},

    {"run.run_file", IDM_RUN_FILE, "Ctrl+F5"},
    {"run.debug_file", IDM_DEBUG_FILE, "F5"},
    {"run.continue", IDM_DEBUG_CONTINUE, "F6"},
    {"run.step_over", IDM_DEBUG_STEP_OVER, "F10"},
    {"run.step_into", IDM_DEBUG_STEP_INTO, "F11"},
    {"run.step_out", IDM_DEBUG_STEP_OUT, "Shift+F11"},
    {"run.toggle_breakpoint", IDM_DEBUG_BREAKPOINT, "F9"},
    {"run.clear_breakpoints", IDM_DEBUG_CLEAR_BREAKPOINTS, NULL},

    {"plugins.reload", IDM_PLUGIN_RELOAD, NULL},
    {"plugins.customize_menus", IDM_PLUGIN_CUSTOMIZE_MENUS, NULL},

    {"help.markdown_reference", IDM_HELP_REFERENCE, NULL},
    {"help.keyboard_shortcuts", IDM_HELP_SHORTCUTS, NULL},
    {"help.plugin_guide", IDM_HELP_PLUGIN_GUIDE, NULL},
    {"help.about", IDM_HELP_ABOUT, NULL},
};

#define COMMAND_COUNT ((int)(sizeof(COMMANDS) / sizeof(COMMANDS[0])))

static const char *const LISTS[] = { "recent_files", "recent_workspaces", "plugins" };

const MenuCommand *menu_command_find(const char *name) {
    for (int i = 0; i < COMMAND_COUNT; i++)
        if (strcmp(COMMANDS[i].name, name) == 0) return &COMMANDS[i];
    return NULL;
}

const MenuCommand *menu_command_for_id(WORD id) {
    for (int i = 0; i < COMMAND_COUNT; i++)
        if (COMMANDS[i].id == id) return &COMMANDS[i];
    return NULL;
}

const MenuCommand *menu_command_at(int index) {
    return index >= 0 && index < COMMAND_COUNT ? &COMMANDS[index] : NULL;
}

/* Whether { list = name } in menus.lua names a generated list. */
static int menu_list_known(const char *name) {
    for (size_t i = 0; i < sizeof(LISTS) / sizeof(LISTS[0]); i++)
        if (strcmp(LISTS[i], name) == 0) return 1;
    return 0;
}

/* Appends "<what> '<name>'" to a newline-separated report. */
static char *report_unknown(char *report, const char *what, const char *name) {
    size_t used = report ? strlen(report) : 0;
    size_t needed = used + strlen(what) + strlen(name) + 32;
    char *grown = (char *)realloc(report, needed);
    if (!grown) return report;
    snprintf(grown + used, needed - used, "%sunknown %s '%s'", used ? "\n" : "", what, name);
    return grown;
}

static char *collect_problems(char *report, const PluginMenuItem *items, int count) {
    for (int i = 0; i < count; i++) {
        const PluginMenuItem *item = &items[i];
        if (item->kind == PLUGIN_MENU_COMMAND && !menu_command_find(item->name))
            report = report_unknown(report, "command", item->name);
        else if (item->kind == PLUGIN_MENU_LIST && !menu_list_known(item->name))
            report = report_unknown(report, "list", item->name);
        else if (item->kind == PLUGIN_MENU_SUBMENU)
            report = collect_problems(report, item->items, item->itemCount);
    }
    return report;
}

char *menu_definition_problems(const PluginMenuItem *items, int count) {
    return collect_problems(NULL, items, count);
}
