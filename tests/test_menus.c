#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/menu_commands.h"
#include "../src/app_accelerators.h"
#include "../src/command_ids.h"
#include "../src/plugin_host.h"
#include "../src/file_io.h"

static int s_ok = 1;

static void expect(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", what);
        s_ok = 0;
    }
}

static char *load(const char *code) {
    plugin_host_shutdown();
    return plugin_host_load_menu("menus.lua", code, strlen(code));
}

static void expect_error(const char *code, const char *expected, const char *what) {
    char *error = load(code);
    int matched = error && strstr(error, expected);
    if (!matched) fprintf(stderr, "Unexpected result for %s: %s\n", what, error ? error : "(loaded)");
    expect(matched, what);
    free(error);
    int count;
    expect(plugin_host_menu(&count) == NULL && count == 0, "a failed definition leaves no menu");
}

static void mark_used(const PluginMenuItem *items, int count, int *used) {
    for (int i = 0; i < count; i++) {
        if (items[i].kind == PLUGIN_MENU_COMMAND) {
            for (int c = 0; menu_command_at(c); c++)
                if (strcmp(menu_command_at(c)->name, items[i].name) == 0) used[c] = 1;
        }
        mark_used(items[i].items, items[i].itemCount, used);
    }
}

/* The built-in definition must load, name only real commands and lists,
   and offer every built-in command somewhere. */
static void test_default_definition(void) {
    char *code = read_utf8_text_file(L"src\\menus.lua", 1024 * 1024);
    expect(code != NULL, "src/menus.lua is readable");
    if (!code) return;
    char *error = load(code);
    if (error) fprintf(stderr, "%s\n", error);
    expect(error == NULL, "the default menus load");
    free(error);
    free(code);

    int count;
    const PluginMenuItem *menus = plugin_host_menu(&count);
    expect(count == 9 && strcmp(menus[0].label, "&File") == 0 &&
           strcmp(menus[3].label, "&Tools") == 0 && strcmp(menus[4].label, "&Git") == 0 &&
           strcmp(menus[8].label, "&Help") == 0,
           "nine top-level menus, with Git after Tools and Help last");
    char *problems = menu_definition_problems(menus, count);
    if (problems) fprintf(stderr, "%s\n", problems);
    expect(problems == NULL, "every command and list name is known");
    free(problems);

    int used[512] = {0};
    mark_used(menus, count, used);
    for (int c = 0; menu_command_at(c); c++) {
        if (!used[c]) fprintf(stderr, "Not in the default menus: %s\n", menu_command_at(c)->name);
        expect(used[c], "every built-in command is in the default menus");
    }

    /* Plugin-owned tool families must not grow a second registry/menu path
       in the executable. Their bundled-plugin tests verify the replacements. */
    static const char *const pluginOnly[] = {
        "edit.uppercase", "edit.lowercase", "edit.title_case",
        "edit.sort_ascending", "edit.sort_descending", "edit.remove_duplicate_lines",
        "edit.tabs_to_spaces", "edit.spaces_to_tabs", "edit.eol_crlf", "edit.eol_lf",
        "edit.eol_cr", "edit.encoding_utf8", "edit.encoding_utf8_bom",
        "edit.encoding_utf16le", "edit.encoding_utf16be", "edit.encoding_ansi",
        "json.format", "json.minify", "html.html", "html.head", "html.body",
        "html.paragraph", "html.bold", "html.italic", "html.underline", "html.div",
        "html.span", "html.link", "html.image", "html.table", "html.bullet_list",
        "html.numbered_list", "html.line_break"
    };
    for (size_t i = 0; i < sizeof(pluginOnly) / sizeof(pluginOnly[0]); i++)
        expect(menu_command_find(pluginOnly[i]) == NULL,
               "plugin-owned commands have no built-in registry entry");
}

/* Shortcut labels and actual accelerators are deliberately separate because
   native editor commands stay with the focused Scintilla/WebView control and
   the backtick key is keyboard-layout dependent. Everything else advertised
   by a built-in command must have a fixed application accelerator. */
static void test_builtin_accelerators(void) {
    size_t acceleratorCount = 0;
    const ACCEL *accelerators = app_fixed_accelerators(&acceleratorCount);
    expect(acceleratorCount <= APP_FIXED_ACCELERATOR_MAX, "fixed accelerator count fits its capacity");
    for (size_t i = 0; i < acceleratorCount; i++) {
        const MenuCommand *command = menu_command_for_id(accelerators[i].cmd);
        expect(command && command->shortcut, "every fixed accelerator has displayed shortcut text");
        for (size_t j = i + 1; j < acceleratorCount; j++)
            expect(accelerators[i].fVirt != accelerators[j].fVirt ||
                   accelerators[i].key != accelerators[j].key,
                   "fixed accelerators do not reuse the same key combination");
    }
    for (int i = 0; menu_command_at(i); i++) {
        const MenuCommand *command = menu_command_at(i);
        if (!command->shortcut) continue;
        int elsewhere = command->id == IDM_EDIT_UNDO || command->id == IDM_EDIT_CUT ||
                        command->id == IDM_EDIT_COPY || command->id == IDM_EDIT_DELETE ||
                        command->id == IDM_VIEW_OUTPUT_PANEL || command->id == IDM_VIEW_TERMINAL;
        int found = 0;
        for (size_t j = 0; j < acceleratorCount; j++)
            if (accelerators[j].cmd == command->id) { found = 1; break; }
        if (!found && !elsewhere) fprintf(stderr, "No accelerator for shortcut: %s (%s)\n",
                                          command->name, command->shortcut);
        expect(found || elsewhere, "every displayed shortcut is handled");
    }
}

static void test_definitions(void) {
    char *error = load(
        "return {\n"
        "  { '&One', {\n"
        "    { 'Built in', 'file.new' },\n"
        "    '-',\n"
        "    { 'Sub', { { 'Lua', function() end }, { list = 'recent_files' } } },\n"
        "    { 'Empty', {} },\n"
        "  } },\n"
        "}\n");
    expect(error == NULL, "a valid definition loads");
    free(error);
    int count;
    const PluginMenuItem *menus = plugin_host_menu(&count);
    expect(count == 1 && menus[0].kind == PLUGIN_MENU_SUBMENU && menus[0].itemCount == 4, "top-level shape");
    if (count == 1 && menus[0].itemCount == 4) {
        const PluginMenuItem *items = menus[0].items;
        expect(items[0].kind == PLUGIN_MENU_COMMAND && strcmp(items[0].name, "file.new") == 0, "command item");
        expect(items[1].kind == PLUGIN_MENU_SEPARATOR, "separator item");
        expect(items[2].kind == PLUGIN_MENU_SUBMENU && items[2].itemCount == 2, "submenu item");
        expect(items[3].kind == PLUGIN_MENU_SUBMENU && items[3].itemCount == 0, "empty submenu");
        if (items[2].itemCount == 2) {
            const PluginMenuItem *lua = &items[2].items[0];
            expect(lua->kind == PLUGIN_MENU_FUNCTION, "function item");
            expect(strcmp(plugin_host_command_label(lua->command), "/&One/Sub/Lua") == 0,
                   "a function's command is labelled with its path");
            expect(plugin_host_command_is_placed(lua->command), "a function's command is already placed");
            expect(items[2].items[1].kind == PLUGIN_MENU_LIST && strcmp(items[2].items[1].name, "recent_files") == 0,
                   "list item");
        }
    }

    char *problems = NULL;
    load("return { { 'M', { { 'X', 'no.such' }, { list = 'nothing' } } } }");
    menus = plugin_host_menu(&count);
    problems = menu_definition_problems(menus, count);
    expect(problems && strstr(problems, "command 'no.such'") && strstr(problems, "list 'nothing'"),
           "unknown names are reported");
    free(problems);

    expect_error("return 5", "must return a table", "non-table result");
    expect_error("return { { 'File', 'file.new' } }", "top-level item must be a menu", "top-level command");
    expect_error("return { '-' }", "must be a menu", "top-level separator");
    expect_error("return { { 'M', { 'oops' } } }", "M: a text item must be \"-\"", "stray string, with its path");
    expect_error("return { { 'M', { { 'Sub', { { 'X', 5 } } } } } }", "M > Sub > X: the action must be",
                 "bad action, with its path");
    expect_error("return { { 'M', { { 5, 'file.new' } } } }", "needs a text label", "missing label");
    expect_error("return { { 'M' .. string.char(0), {} } }", "must not contain NUL bytes",
                 "embedded NUL in a menu label");
    expect_error("return { { 'M', { { 'X', 'file.' .. string.char(0) .. 'new' } } } }",
                 "must not contain NUL bytes", "embedded NUL in a command name");
    expect_error("local t = { 'M' }; t[2] = { t }; return { t }", "nested too deeply", "self-reference");
    expect_error("return { { 'M', { { 'F', function() end } } } } + nil", "arithmetic", "a runtime error");

    /* A failed definition keeps none of the commands it had registered. */
    error = load("return { { 'M', { { 'F', function() end }, { 'Bad', 5 } } } }");
    free(error);
    expect(plugin_host_command_count() == 0, "a failed definition registers no commands");
}

/* Renders src/help/keyboard-shortcuts.lua against the menus definition code
   (NULL keeps whatever is loaded). Returns the page, or NULL. */
static char *render_shortcuts(const char *menus) {
    if (menus) free(load(menus));
    PluginShortcut shortcuts[512];
    int count = 0;
    for (int c = 0; menu_command_at(c); c++)
        if (menu_command_at(c)->shortcut)
            shortcuts[count++] = (PluginShortcut){ menu_command_at(c)->name, menu_command_at(c)->shortcut };
    char *code = read_utf8_text_file(L"src\\help\\keyboard-shortcuts.lua", 1024 * 1024);
    expect(code != NULL, "src/help/keyboard-shortcuts.lua is readable");
    if (!code) return NULL;
    char *page = NULL;
    char *error = plugin_host_render_page("keyboard-shortcuts.lua", code, strlen(code), shortcuts, count, &page);
    if (error) fprintf(stderr, "%s\n", error);
    expect(error == NULL && page != NULL, "the shortcuts page renders");
    free(error);
    free(code);
    return page;
}

/* Help > Keyboard Shortcuts is generated, so it lists every shortcut in the
   command table, under the menu holding the command. */
static void test_shortcuts_page(void) {
    char *menus = read_utf8_text_file(L"src\\menus.lua", 1024 * 1024);
    char *page = menus ? render_shortcuts(menus) : NULL;
    free(menus);
    if (!page) return;
    expect(strncmp(page, "# Tinker Editor Keyboard Shortcuts\n", 30) == 0, "the page has its title");
    for (int command_index = 0; menu_command_at(command_index); command_index++) {
        const char *shortcut = menu_command_at(command_index)->shortcut;
        if (!shortcut) continue;
        /* Cells escape Markdown punctuation as keyboard-shortcuts.lua's
           cell() does: Ctrl+\ is written Ctrl+\\ and Ctrl+` is Ctrl+\`. */
        char row[64] = "| ";
        size_t used = 2;
        for (const char *c = shortcut; *c && used + 4 < sizeof(row); c++) {
            if (strchr("\\|`*_", *c)) row[used++] = '\\';
            row[used++] = *c;
        }
        snprintf(row + used, sizeof(row) - used, " |");
        if (!strstr(page, row)) fprintf(stderr, "Shortcut missing from the page: %s\n", shortcut);
        expect(strstr(page, row) != NULL, "every shortcut is on the page");
    }
    expect(strstr(page, "## File\n") && strstr(page, "| Ctrl+N | New |"), "rows sit under their menu");
    expect(strstr(page, "| Ctrl+F2 | Bookmarks > Toggle Bookmark |") != NULL, "submenu items show their path");
    expect(strstr(page, "| Tab / Shift+Tab |") != NULL, "keys that aren't commands are listed");
    expect(strstr(page, "Not in the menus") == NULL, "the default menus hold every shortcut");
    free(page);

    /* A command left out of a customised menu bar still has its shortcut. */
    page = render_shortcuts("return { { '&File', { { '&New', 'file.new' } } } }");
    if (page) {
        expect(strstr(page, "## Not in the menus") && strstr(page, "| Ctrl+O | file.open |"),
               "shortcuts of commands not in the menus are listed separately");
        free(page);
    }

    /* Plugin shortcuts arrive keyed by label, and are listed by menu path. */
    static const PluginShortcut plugins[] = {
        { "file.new", "Ctrl+N" },
        { "/Tools/JSON/Format &JSON", "Ctrl+Alt+J" },
        { "Line Tools/Salt && &Pepper", "Ctrl+Alt+P" },
    };
    char *code = read_utf8_text_file(L"src\\help\\keyboard-shortcuts.lua", 1024 * 1024);
    page = NULL;
    if (code) free(plugin_host_render_page("keyboard-shortcuts.lua", code, strlen(code), plugins, 3, &page));
    free(code);
    expect(page && strstr(page, "## Plugin commands") &&
           strstr(page, "| Ctrl+Alt+J | Tools > JSON > Format JSON |") &&
           strstr(page, "| Ctrl+Alt+P | Plugins > Line Tools > Salt & Pepper |") &&
           !strstr(page, "/Tools/JSON"),
           "plugin shortcuts are listed by their plain menu path");
    free(page);

    /* A script error is reported, not crashed on. */
    char *bad = NULL;
    char *error = plugin_host_render_page("bad.lua", "return 5", 8, NULL, 0, &bad);
    expect(error && strstr(error, "must return the page") && bad == NULL, "a non-string result is an error");
    free(error);
}

int main(void) {
    test_default_definition();
    test_builtin_accelerators();
    test_definitions();
    test_shortcuts_page();
    plugin_host_shutdown();
    if (s_ok) puts("All menu tests passed.");
    return s_ok ? 0 : 1;
}
