#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/plugin_host.h"
#include "../src/file_io.h"
#include "../src/path_utils.h"
#include "../third_party/scintilla/include/Scintilla.h"

static int s_ok = 1;

static void expect(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", what);
        s_ok = 0;
    }
}

static void set_document(HWND editor, const char *text) {
    SendMessageA(editor, SCI_SETTEXT, 0, (LPARAM)text);
    SendMessageA(editor, SCI_EMPTYUNDOBUFFER, 0, 0);
    SendMessageA(editor, SCI_SETSAVEPOINT, 0, 0);
}

static int document_is(HWND editor, const char *expected) {
    char text[256];
    SendMessageA(editor, SCI_GETTEXT, sizeof(text), (LPARAM)text);
    return strcmp(text, expected) == 0;
}

static int document_contains(HWND editor, const char *expected) {
    char text[1024];
    SendMessageA(editor, SCI_GETTEXT, sizeof(text), (LPARAM)text);
    return strstr(text, expected) != NULL;
}

/* Loads a chunk that must succeed and returns the index of its first command. */
static int load_ok(const char *name, const char *code) {
    int first = plugin_host_command_count();
    char *error = plugin_host_load_chunk(name, code, strlen(code));
    if (error) fprintf(stderr, "%s\n", error);
    expect(error == NULL, name);
    free(error);
    return first;
}

static int run_fails(int index, HWND editor, const char *expectedText) {
    char *error = plugin_host_run(index, editor);
    int failed = error && strstr(error, expectedText);
    if (!failed) fprintf(stderr, "Unexpected result: %s\n", error ? error : "(success)");
    free(error);
    return failed;
}

static void test_commands(HWND editor) {
    int first = load_ok("commands.lua",
        "menu.add('Upper', function() editor.replace_selection(editor.get_selection():upper()) end)\n"
        "menu.add('Number', function()\n"
        "  local out, n = {}, 0\n"
        "  for line in (editor.get_text() .. editor.eol()):gmatch('(.-)' .. editor.eol()) do\n"
        "    n = n + 1; out[n] = n .. ' ' .. line\n"
        "  end\n"
        "  editor.set_text(table.concat(out, editor.eol()))\n"
        "end)\n"
        "menu.add('Edit then fail', function() editor.set_text('partial'); error('boom') end)\n"
        "menu.add('Fail', function() error('nothing changed') end)\n");
    expect(plugin_host_command_count() == first + 4, "four commands registered");
    expect(strcmp(plugin_host_command_label(first), "Upper") == 0, "label kept");
    expect(plugin_host_command_label(first + 4) == NULL, "out-of-range label is NULL");

    set_document(editor, "abc def");
    SendMessageA(editor, SCI_SETSEL, 0, 3);
    char *error = plugin_host_run(first, editor);
    expect(!error && document_is(editor, "ABC def"), "replace_selection");
    free(error);
    SendMessageA(editor, SCI_UNDO, 0, 0);
    expect(document_is(editor, "abc def") && !SendMessageA(editor, SCI_CANUNDO, 0, 0),
           "a command is one undo step");

    SendMessageA(editor, SCI_SETEOLMODE, SC_EOL_LF, 0);
    set_document(editor, "a\nb");
    error = plugin_host_run(first + 1, editor);
    expect(!error && document_is(editor, "1 a\n2 b"), "get_text/set_text/eol");
    free(error);

    set_document(editor, "keep");
    expect(run_fails(first + 2, editor, "boom"), "error is reported");
    expect(document_is(editor, "keep"), "a failing command's edits are undone");
    expect(!SendMessageA(editor, SCI_GETMODIFY, 0, 0),
           "rolling back a failing command restores the save point");

    /* A failing command that changed nothing mustn't undo the user's edit. */
    set_document(editor, "");
    SendMessageA(editor, SCI_REPLACESEL, 0, (LPARAM)"typed");
    expect(run_fails(first + 3, editor, "nothing changed"), "error without edits is reported");
    expect(document_is(editor, "typed"), "the user's own edit survives");

    expect(run_fails(-1, editor, "no longer loaded"), "bad index rejected");
}

/* menu.separator entries keep their place among the commands, carry their
   menu path as the label, and run nothing. */
static void test_separators(HWND editor) {
    int first = load_ok("separators.lua",
        "menu.add('Lines/A', function() end)\n"
        "menu.separator('Lines')\n"
        "menu.add('Lines/B', function() end)\n"
        "menu.separator()\n"
        "menu.separator('/Snippets/Markdown')\n");
    expect(plugin_host_command_count() == first + 5, "separators are listed with the commands");
    expect(!plugin_host_command_is_separator(first) && plugin_host_command_is_separator(first + 1) &&
           !plugin_host_command_is_separator(first + 2), "separators keep their order");
    expect(strcmp(plugin_host_command_label(first + 1), "Lines") == 0, "a separator's label is its path");
    expect(strcmp(plugin_host_command_label(first + 3), "") == 0, "no path means the Plugins menu");
    expect(strcmp(plugin_host_command_label(first + 4), "/Snippets/Markdown") == 0, "menu-bar path kept");
    expect(!plugin_host_command_is_placed(first + 1) && !plugin_host_command_overrides(first + 1),
           "a separator is placed by its path");
    expect(!plugin_host_command_is_separator(first + 5), "out-of-range is no separator");
    set_document(editor, "same");
    char *error = plugin_host_run(first + 1, editor);
    expect(!error && document_is(editor, "same"), "running a separator does nothing");
    free(error);

    error = plugin_host_load_chunk("bad.lua", "menu.separator({})", 18);
    expect(error && strstr(error, "bad argument"), "a separator path must be a string");
    free(error);
}

/* ---- stand-ins for the application's dialogs and document details ---- */
static char s_lastTitle[64], s_lastMessage[256];
static char s_configPlugin[64], s_configKey[64], s_configValue[128];
static char s_binaryName[64], s_binaryData[256];
static size_t s_binaryLength;
static const char *s_calculatorExpression =
    "0x10 + 0b11 + (5 << 1) + min(4, 9)";

static char *fake_config_get(const char *plugin, const char *key) {
    if (strcmp(plugin, s_configPlugin) != 0 || strcmp(key, s_configKey) != 0 || !s_configValue[0])
        return NULL;
    size_t length = strlen(s_configValue) + 1;
    char *copy = (char *)malloc(length);
    if (copy) memcpy(copy, s_configValue, length);
    return copy;
}

static int fake_config_set(const char *plugin, const char *key, const char *value) {
    snprintf(s_configPlugin, sizeof(s_configPlugin), "%s", plugin);
    snprintf(s_configKey, sizeof(s_configKey), "%s", key);
    snprintf(s_configValue, sizeof(s_configValue), "%s", value ? value : "");
    return 1;
}

static char *fake_prompt(const char *title, const char *label, const char *initial) {
    snprintf(s_lastTitle, sizeof(s_lastTitle), "%s", title);
    if (strcmp(label, "cancel") == 0) return NULL;
    if (strncmp(label, "Expression ", 11) == 0) {
        char *reply = (char *)malloc(strlen(s_calculatorExpression) + 1);
        if (reply) strcpy(reply, s_calculatorExpression);
        return reply;
    }
    char *reply = (char *)malloc(strlen(label) + strlen(initial) + 16);
    if (reply) sprintf(reply, "reply to %s %s", label, initial);
    return reply;
}

static int fake_choose(const char *title, const char *label, const char *const *items, int count) {
    snprintf(s_lastTitle, sizeof(s_lastTitle), "%s", title);
    if (strstr(label, " = ") && count == 2 && strcmp(items[0], "Insert result") == 0)
        return 0;
    if (strcmp(label, "Include headings at:") == 0 && count == 4) return 1;
    return strcmp(label, "Level:") == 0 && count == 3 && strcmp(items[1], "h2") == 0 ? 1 : -1;
}

static int fake_save_binary(const char *title, const char *defaultName,
                            const char *data, size_t length) {
    snprintf(s_lastTitle, sizeof(s_lastTitle), "%s", title);
    if (strcmp(defaultName, "cancel.tap") == 0) return 0;
    snprintf(s_binaryName, sizeof(s_binaryName), "%s", defaultName);
    s_binaryLength = length < sizeof(s_binaryData) ? length : sizeof(s_binaryData);
    memcpy(s_binaryData, data, s_binaryLength);
    return 1;
}

static void fake_message(const char *title, const char *text, const char *kind) {
    (void)kind;
    snprintf(s_lastTitle, sizeof(s_lastTitle), "%s", title);
    snprintf(s_lastMessage, sizeof(s_lastMessage), "%s", text);
}

static char *fake_file_path(void) {
    const char *path = "C:\\docs\\note.md";
    char *copy = (char *)malloc(strlen(path) + 1);
    if (copy) strcpy(copy, path);
    return copy;
}

static const char *fake_language(void) {
    return "markdown";
}

static int run_ok(int index, HWND editor) {
    char *error = plugin_host_run(index, editor);
    if (error) fprintf(stderr, "%s\n", error);
    free(error);
    return error == NULL;
}

static void test_checkboxes_and_config(HWND editor) {
    s_configPlugin[0] = s_configKey[0] = s_configValue[0] = 0;
    int first = load_ok("checkbox.lua",
        "assert(config.get('enabled', false) == false)\n"
        "menu.checkbox('Options/Enabled', 'enabled', false, function(checked)\n"
        "  assert(checked == true)\n"
        "end)\n"
        "menu.add('Options/Save Value', function()\n"
        "  config.set('answer', 42)\n"
        "  assert(config.get('answer', 0) == 42)\n"
        "end)\n");
    expect(plugin_host_command_is_checkable(first), "checkbox command is checkable");
    expect(!plugin_host_command_is_checked(first), "checkbox uses its default state");
    expect(run_ok(first, editor), "checkbox command runs with its new state");
    expect(plugin_host_command_is_checked(first) && strcmp(s_configPlugin, "checkbox.lua") == 0 &&
           strcmp(s_configKey, "enabled") == 0 && strcmp(s_configValue, "1") == 0,
           "checkbox state is persisted in its plugin namespace");
    expect(run_ok(first + 1, editor) && strcmp(s_configKey, "answer") == 0 &&
           strcmp(s_configValue, "42") == 0, "config.set persists typed values");

    int failing = load_ok("bad_checkbox.lua",
        "menu.checkbox('Broken', 'enabled', true, function() error('nope') end)");
    expect(run_fails(failing, editor, "nope"), "failing checkbox command reports its error");
    expect(plugin_host_command_is_checked(failing), "a failing checkbox command restores its setting");
}

static void test_editor_api(HWND editor) {
    int first = load_ok("api.lua",
        "menu.add('Lines', function()\n"
        "  assert(editor.line_count() == 3 and editor.get_line(2) == 'two')\n"
        "  local s, f = editor.line_range(2)\n"
        "  assert(s == 5 and f == 7 and editor.get_text():sub(s, f) == 'two')\n"
        "  editor.set_selection(s, f)\n"
        "  assert(editor.get_selection() == 'two' and editor.current_line() == 2)\n"
        "  local a, b = editor.get_selection_range()\n"
        "  assert(a == 5 and b == 7)\n"
        "  editor.replace_selection('TWO')\n"
        "  assert(editor.get_caret() == 8, 'caret after replaced text')\n"
        "  editor.replace_range(1, 3, 'ONE')\n"
        "  editor.insert('>', 1)\n"
        "  editor.set_caret(#editor.get_text() + 1)\n"
        "  editor.insert('!')\n"
        "  editor.set_selection(3, 2)\n"
        "  assert(editor.get_selection() == '', 'empty range')\n"
        "end)\n"
        "menu.add('Bounds', function()\n"
        "  assert(not pcall(editor.get_line, 4), 'line past the end')\n"
        "  assert(not pcall(editor.get_line, 0), 'line zero')\n"
        "  assert(not pcall(editor.set_selection, 3, 1), 'finish before start')\n"
        "  assert(not pcall(editor.replace_range, 0, 1, 'x'), 'start before the document')\n"
        "  assert(not pcall(editor.set_caret, #editor.get_text() + 2), 'caret past the end')\n"
        "end)\n"
        "menu.add('Binary', function() editor.replace_range(1, 0, 'a\\0b') end)\n"
        "menu.add('Document', function()\n"
        "  assert(editor.file_path() == 'C:\\\\docs\\\\note.md')\n"
        "  assert(editor.language() == 'markdown')\n"
        "end)\n"
        "menu.add('Multiple selections', function()\n"
        "  assert(editor.selection_count() == 1)\n"
        "  editor.set_selection(1, 4)\n"
        "  local second = editor.add_selection(10, 12)\n"
        "  assert(second == 2 and editor.selection_count() == 2)\n"
        "  local all = editor.get_selections()\n"
        "  assert(#all == 2 and all[1].start == 1 and all[1].finish == 4)\n"
        "  assert(all[2].caret == 13 and all[2].anchor == 10)\n"
        "  assert(editor.get_selection(1) == 'zero' and editor.get_selection(2) == 'two')\n"
        "  local s, f = editor.get_selection_range(2)\n"
        "  assert(s == 10 and f == 12 and editor.get_caret(2) == 13)\n"
        "  editor.replace_selection('TWO', 2)\n"
        "  assert(editor.selection_count() == 2 and editor.get_caret(2) == 13)\n"
        "  editor.set_selection(6, 8, 1)\n"
        "  editor.set_main_selection(2)\n"
        "  assert(editor.get_main_selection() == 2 and editor.get_selection() == '')\n"
        "  editor.set_caret(12, 2)\n"
        "  assert(editor.get_caret() == 12 and editor.selection_count() == 2)\n"
        "  editor.drop_selection(1)\n"
        "  assert(editor.selection_count() == 1 and editor.get_main_selection() == 1)\n"
        "  assert(not pcall(editor.drop_selection, 1), 'cannot drop only selection')\n"
        "  assert(not pcall(editor.get_caret, 2), 'selection past the end')\n"
        "end)\n");

    SendMessageA(editor, SCI_SETEOLMODE, SC_EOL_LF, 0);
    set_document(editor, "one\ntwo\nthree");
    expect(run_ok(first, editor) && document_is(editor, ">ONE\nTWO\nthree!"), "line and range editing");
    set_document(editor, "one\ntwo\nthree");
    expect(run_ok(first + 1, editor) && document_is(editor, "one\ntwo\nthree"), "out-of-range arguments raise errors");
    set_document(editor, "");
    expect(run_ok(first + 2, editor) && SendMessageA(editor, SCI_GETLENGTH, 0, 0) == 3,
           "text containing NUL is written whole");
    expect(run_ok(first + 3, editor), "file_path and language");
    set_document(editor, "zero one two");
    expect(run_ok(first + 4, editor) && document_is(editor, "zero one TWO"),
           "multiple selections and carets through the Lua API");

    int bulk = load_ok("bulk-selections.lua",
        "menu.add('Bulk selections', function()\n"
        "  assert(editor.selection_count() == 3)\n"
        "  assert(not pcall(editor.replace_selections, {'only one'}))\n"
        "  assert(not pcall(editor.replace_selections, {'A', 2, 'C'}))\n"
        "  editor.replace_selections({'A', 'middle', 'C'})\n"
        "  assert(editor.selection_count() == 3)\n"
        "  local all = editor.get_selections()\n"
        "  assert(all[1].caret == 2 and all[2].caret == 9 and all[3].caret == 11)\n"
        "  assert(editor.get_main_selection() == 2)\n"
        "  editor.replace_selections('!')\n"
        "  assert(editor.selection_count() == 3 and editor.get_main_selection() == 2)\n"
        "end)");
    set_document(editor, "aa bb cc");
    SendMessageA(editor, SCI_SETSEL, 0, 2);
    SendMessageA(editor, SCI_ADDSELECTION, 5, 3);
    SendMessageA(editor, SCI_ADDSELECTION, 8, 6);
    SendMessageA(editor, SCI_SETMAINSELECTION, 1, 0);
    expect(run_ok(bulk, editor) && document_is(editor, "A! middle! C!") &&
           SendMessageA(editor, SCI_GETSELECTIONS, 0, 0) == 3,
           "bulk replacement keeps multiple carets stable");
}

static void test_ui(HWND editor) {
    int first = load_ok("ui.lua",
        "menu.add('Ask', function()\n"
        "  assert(ui.prompt('Width:', '80') == 'reply to Width: 80')\n"
        "  assert(ui.prompt('cancel') == nil, 'cancel gives nil')\n"
        "  local i, item = ui.choose({'h1', 'h2', 'h3'}, 'Level:')\n"
        "  assert(i == 2 and item == 'h2')\n"
        "  assert(ui.choose({'x'}) == nil, 'cancelled choice gives nil')\n"
        "  assert(not pcall(ui.choose, {}), 'no items')\n"
        "  assert(not pcall(ui.choose, {'a', 5}), 'non-string item')\n"
        "end)\n"
        "menu.add('Prompt', function() ui.prompt('Width:') end)\n"
        "menu.add('Binary', function()\n"
        "  assert(ui.save_binary('output.tap', string.char(0, 1, 255)))\n"
        "  assert(ui.save_binary('cancel.tap', 'x') == false)\n"
        "end)\n");
    expect(run_ok(first, editor), "ui.prompt and ui.choose");
    expect(strcmp(s_lastTitle, "Ask") == 0, "dialogs are titled with the command label");
    expect(run_ok(first + 2, editor) && strcmp(s_binaryName, "output.tap") == 0 &&
           s_binaryLength == 3 && (unsigned char)s_binaryData[0] == 0 &&
           (unsigned char)s_binaryData[1] == 1 && (unsigned char)s_binaryData[2] == 255,
           "ui.save_binary preserves every byte and reports cancellation");

    plugin_host_set_callbacks(NULL);
    expect(run_fails(first + 1, editor, "not available"), "missing callbacks raise errors");
    expect(run_fails(first + 2, editor, "not available"), "missing binary callback raises an error");
}

static HWND s_reentryEditor;
static int s_reentryBlocked;

/* Stands in for a dialog whose message loop delivers an event and a menu
   command while a plugin is still running. */
static char *reentrant_prompt(const char *title, const char *label, const char *initial) {
    (void)title; (void)label; (void)initial;
    char *fired = plugin_host_fire("save", NULL, s_reentryEditor);
    char *run = plugin_host_run(0, s_reentryEditor);
    s_reentryBlocked = fired == NULL && run && strstr(run, "still running");
    free(fired);
    free(run);
    return NULL;
}

static void test_events(HWND editor) {
    plugin_host_shutdown();
    int first = load_ok("events.lua",
        "saves = {}\n"
        "events.on('save', function(path) saves[#saves + 1] = path; editor.insert('saved ', 1) end)\n"
        "events.on('save', function() editor.insert('lost'); error('second handler fails') end)\n"
        "events.on('open', function(path) assert(path == nil) end)\n"
        "menu.add('Saves', function() assert(#saves == 1 and saves[1] == 'C:\\\\a.md') end)\n"
        "menu.add('Reenter', function() ui.prompt('x') end)\n");

    set_document(editor, "x");
    char *report = plugin_host_fire("save", "C:\\a.md", editor);
    expect(report && strstr(report, "second handler fails"), "a failing handler is reported");
    expect(document_is(editor, "saved x"), "handlers edit; a failing one's edits are undone");
    free(report);
    expect(run_ok(first, editor), "handlers receive the path");
    expect(plugin_host_fire("open", NULL, editor) == NULL, "nil path for an unsaved document");
    expect(plugin_host_fire("tab_changed", NULL, editor) == NULL, "no handlers, no report");

    const char *unknown = "events.on('close', function() end)";
    char *error = plugin_host_load_chunk("unknown.lua", unknown, strlen(unknown));
    expect(error && strstr(error, "unknown event"), "unknown event names are rejected");
    free(error);
    const char *partial = "events.on('open', function() error('should not run') end)\nerror('late')\n";
    error = plugin_host_load_chunk("partial_events.lua", partial, strlen(partial));
    free(error);
    expect(plugin_host_fire("open", NULL, editor) == NULL, "a failed script's handlers are dropped");

    static const PluginHostCallbacks reentrant = {
        reentrant_prompt, NULL, NULL, NULL, NULL, fake_config_get, fake_config_set, NULL, NULL
    };
    plugin_host_set_callbacks(&reentrant);
    s_reentryEditor = editor;
    expect(run_ok(first + 1, editor) && s_reentryBlocked, "no re-entry while a plugin runs");
    plugin_host_set_callbacks(NULL);
}

static void test_load_failures(void) {
    int before = plugin_host_command_count();
    char *error = plugin_host_load_chunk("syntax.lua", "menu.add(", 9);
    expect(error && strstr(error, "syntax.lua"), "syntax error names the script");
    free(error);

    const char *badShortcut = "menu.add('Bad', function() end, 'Shift+Q')";
    error = plugin_host_load_chunk("shortcut.lua", badShortcut, strlen(badShortcut));
    expect(error && strstr(error, "shortcut needs Ctrl or Alt"),
           "plugin shortcuts require a safe modifier and supported key");
    free(error);
    const char *partial = "menu.add('Lost', function() end)\nerror('late failure')\n";
    error = plugin_host_load_chunk("partial.lua", partial, strlen(partial));
    expect(error && strstr(error, "late failure"), "runtime error at load is reported");
    expect(plugin_host_command_count() == before, "a failed script adds no commands");
    free(error);
    const char *outside = "editor.get_text()";
    error = plugin_host_load_chunk("outside.lua", outside, strlen(outside));
    expect(error && strstr(error, "only available while"), "editor API needs a running command");
    free(error);

    const char *promptOutside = "ui.prompt('x')";
    error = plugin_host_load_chunk("prompt_outside.lua", promptOutside, strlen(promptOutside));
    expect(error && strstr(error, "only available while"), "ui.prompt needs a running command");
    free(error);
    const char *chooseOutside = "ui.choose({'x'})";
    error = plugin_host_load_chunk("choose_outside.lua", chooseOutside, strlen(chooseOutside));
    expect(error && strstr(error, "only available while"), "ui.choose needs a running command");
    free(error);

    const char *nulName =
        "commands.register('file.save' .. string.char(0) .. 'suffix', function() end)";
    error = plugin_host_load_chunk("nul_name.lua", nulName, strlen(nulName));
    expect(error && strstr(error, "must not contain NUL bytes"),
           "C-string API arguments reject embedded NUL bytes");
    expect(plugin_host_command_count() == before, "a rejected NUL name registers no command");
    free(error);

    const char *invalidUtf8 = "menu.add(string.char(255), function() end)";
    error = plugin_host_load_chunk("invalid_utf8.lua", invalidUtf8, strlen(invalidUtf8));
    expect(error && strstr(error, "must be valid UTF-8"),
           "labels constructed with invalid UTF-8 are rejected");
    expect(plugin_host_command_count() == before, "a rejected UTF-8 label registers no command");
    free(error);
}

static void test_environment(void) {
    load_ok("environment.lua",
        "assert(io == nil and os == nil and package == nil and debug == nil)\n"
        "assert(dofile == nil and loadfile == nil)\n"
        "assert(load('return 1')() == 1)\n"
        "assert(load('return string.upper(\"a\")')() == 'A', 'load keeps the globals without an env')\n"
        "assert(load(string.dump(function() end)) == nil, 'binary chunks refused')\n"
        "assert(not pcall(require, 'outside'), 'single-file plugins cannot require files')\n");

    load_ok("clock.lua",
        "assert(app.date():match('^%d%d%d%d%-%d%d%-%d%d$'))\n"
        "assert(app.time():match('^%d%d:%d%d:%d%d$'))\n"
        "assert(app.date_time():match('^%d%d%d%d%-%d%d%-%d%d %d%d:%d%d:%d%d$'))\n");
}

static void test_time_limit(void) {
    const char *loop = "while true do end";
    char *error = plugin_host_load_chunk("loop.lua", loop, strlen(loop));
    expect(error && strstr(error, "stopped after"), "runaway script is stopped");
    free(error);
}

static void write_script(const WCHAR *directory, const WCHAR *name, const char *code) {
    WCHAR *path = path_join(directory, name);
    expect(path && write_bytes_file(path, code, strlen(code)), "write test script");
    free(path);
}

static void write_script_bytes(const WCHAR *directory, const WCHAR *name,
                               const char *code, size_t length) {
    WCHAR *path = path_join(directory, name);
    expect(path && write_bytes_file(path, code, length), "write binary test script");
    free(path);
}

static int test_plugin_enabled(const char *scope, const char *name) {
    return strcmp(scope, "test") != 0 || _stricmp(name, "b.lua") != 0;
}

static void test_directory(void) {
    WCHAR temp[MAX_PATH], directory[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    swprintf(directory, MAX_PATH, L"%lsmarkedit_plugins_%lu", temp, GetCurrentProcessId());
    CreateDirectoryW(directory, NULL);
    write_script(directory, L"b.lua", "menu.add('From B', function() end)");
    write_script(directory, L"a.lua", "menu.add('From A', function() end)");
    write_script(directory, L"broken.lua", "this is not lua");
    write_script(directory, L"ignored.luac", "menu.add('Ignored', function() end)");
    static const char nulCode[] =
        "menu.add('Before NUL', function() end)\0menu.add('After NUL', function() end)";
    write_script_bytes(directory, L"nul.lua", nulCode, sizeof(nulCode) - 1);
    WCHAR *folder = path_join(directory, L"folder");
    CreateDirectoryW(folder, NULL);
    WCHAR *nested = path_join(folder, L"nested");
    CreateDirectoryW(nested, NULL);
    write_script(folder, L"helper.lua", "return { label = 'From Folder' }");
    write_script(nested, L"value.lua", "return 42");
    write_script(folder, L"init.lua",
        "local helper = require('helper')\n"
        "assert(require('helper') == helper)\n"
        "assert(require('nested.value') == 42)\n"
        "menu.add(helper.label, function() end, 'Ctrl+Alt+M')");

    plugin_host_shutdown();
    char *report = plugin_host_load_directory(directory, "test");
    expect(report && strstr(report, "broken.lua") && strstr(report, "nul.lua"),
           "load errors are reported by file, including source after a NUL byte");
    expect(plugin_host_command_count() == 3, "only the good scripts and folder extensions load");
    expect(plugin_host_command_count() == 3 &&
           strcmp(plugin_host_command_label(0), "From A") == 0 &&
           strcmp(plugin_host_command_label(1), "From B") == 0 &&
           strcmp(plugin_host_command_label(2), "From Folder") == 0,
           "scripts and folder extensions load in name order");
    expect(strcmp(plugin_host_command_shortcut(2), "Ctrl+Alt+M") == 0,
           "plugin commands retain shortcuts");
    expect(plugin_host_extension_count() == 5 && plugin_host_error_path() != NULL,
           "discovered extensions and the first failing path are available to the UI");
    free(report);

    static const PluginHostCallbacks filtered = {
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, test_plugin_enabled, NULL
    };
    plugin_host_shutdown();
    plugin_host_set_callbacks(&filtered);
    report = plugin_host_load_directory(directory, "test");
    expect(plugin_host_command_count() == 2 &&
           strcmp(plugin_host_command_label(0), "From A") == 0 &&
           strcmp(plugin_host_command_label(1), "From Folder") == 0 &&
           plugin_host_extension_count() == 5,
           "disabled extensions are discovered but not executed");
    free(report);
    plugin_host_set_callbacks(NULL);

    const WCHAR *names[] = { L"a.lua", L"b.lua", L"broken.lua", L"ignored.luac", L"nul.lua" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        WCHAR *path = path_join(directory, names[i]);
        if (path) DeleteFileW(path);
        free(path);
    }
    WCHAR *helper = path_join(folder, L"helper.lua");
    WCHAR *init = path_join(folder, L"init.lua");
    WCHAR *nestedValue = path_join(nested, L"value.lua");
    DeleteFileW(helper); DeleteFileW(init); DeleteFileW(nestedValue);
    RemoveDirectoryW(nested); RemoveDirectoryW(folder);
    free(nestedValue); free(nested); free(helper); free(init); free(folder);
    RemoveDirectoryW(directory);

    plugin_host_shutdown();
    expect(plugin_host_load_directory(directory, "test") == NULL && plugin_host_command_count() == 0,
           "a missing folder loads nothing, silently");
}

static int find_command(const char *label) {
    for (int index = 0; index < plugin_host_command_count(); index++) {
        const char *candidate = plugin_host_command_label(index);
        if (candidate && strcmp(candidate, label) == 0) return index;
    }
    return -1;
}

static void test_bundled_plugins(HWND editor) {
    static const PluginHostCallbacks fakes = {
        fake_prompt, fake_choose, fake_file_path, fake_language, fake_message,
        fake_config_get, fake_config_set, NULL, fake_save_binary
    };
    plugin_host_set_callbacks(&fakes);
    plugin_host_shutdown();
    char *report = plugin_host_load_directory(L"plugins", "bundled");
    if (report) fprintf(stderr, "%s\n", report);
    expect(report == NULL, "bundled plugins load without errors");
    free(report);

    int encode64 = find_command("Developer Tools/Base64 Encode");
    int decode64 = find_command("Developer Tools/Base64 Decode");
    int encodeUrl = find_command("Developer Tools/URL Encode");
    int decodeUrl = find_command("Developer Tools/URL Decode");
    int encodeHex = find_command("Developer Tools/Hex Encode");
    int decodeHex = find_command("Developer Tools/Hex Decode");
    int encodeHtml = find_command("Developer Tools/HTML Entity Encode");
    int decodeHtml = find_command("Developer Tools/HTML Entity Decode");
    int toc = find_command("/Snippets/Markdown/Table of Contents...");
    int calculator = find_command("Calculate Selection");
    int removeBlank = find_command("Line Tools/Clean/Remove Blank Lines");
    int sortLines = find_command("Line Tools/Order/Sort A to Z");
    int sortLength = find_command("Line Tools/Order/Sort by Length");
    int shuffle = find_command("Line Tools/Order/Shuffle");
    int buildTap = find_command("Z80 Assembler/Compile");
    expect(encode64 >= 0 && decode64 >= 0 && encodeUrl >= 0 && decodeUrl >= 0 &&
           encodeHex >= 0 && decodeHex >= 0 && encodeHtml >= 0 && decodeHtml >= 0 &&
           toc >= 0 && calculator >= 0 && removeBlank >= 0 &&
           sortLines >= 0 && sortLength >= 0 && shuffle >= 0 && buildTap >= 0,
           "bundled plugin commands are registered");

    set_document(editor, "ORG $8000\nstart: LD A,42\nLD ($9000),A\nJR start\n");
    s_binaryLength = 0;
    expect(buildTap >= 0 && run_ok(buildTap, editor) &&
           strcmp(s_binaryName, "C:\\docs\\note.tap") == 0 && s_binaryLength == 93 &&
           (unsigned char)s_binaryData[0] == 19 && (unsigned char)s_binaryData[3] == 0 &&
           (unsigned char)s_binaryData[14] == 36 && (unsigned char)s_binaryData[16] == 10 &&
           (unsigned char)s_binaryData[18] == 36 && (unsigned char)s_binaryData[23] == 0xff &&
           (unsigned char)s_binaryData[24] == 0 && (unsigned char)s_binaryData[25] == 10 &&
           (unsigned char)s_binaryData[28] == 0xfd && memcmp(s_binaryData + 29, "32767", 5) == 0 &&
           (unsigned char)s_binaryData[34] == 0x0e && (unsigned char)s_binaryData[37] == 0xff &&
           (unsigned char)s_binaryData[38] == 0x7f && (unsigned char)s_binaryData[41] == 0xef &&
           (unsigned char)s_binaryData[44] == 0xaf && (unsigned char)s_binaryData[46] == 0xf9 &&
           (unsigned char)s_binaryData[47] == 0xc0 && memcmp(s_binaryData + 48, "32768", 5) == 0 &&
           (unsigned char)s_binaryData[64] == 3 && (unsigned char)s_binaryData[75] == 7 &&
           (unsigned char)s_binaryData[77] == 0 && (unsigned char)s_binaryData[78] == 0x80 &&
           (unsigned char)s_binaryData[84] == 0xff && (unsigned char)s_binaryData[85] == 0x3e &&
           (unsigned char)s_binaryData[86] == 42 && (unsigned char)s_binaryData[87] == 0x32 &&
           (unsigned char)s_binaryData[88] == 0 && (unsigned char)s_binaryData[89] == 0x90 &&
           (unsigned char)s_binaryData[90] == 0x18 && (unsigned char)s_binaryData[91] == 0xf9,
           "bundled Z80 assembler creates an auto-running Spectrum TAP");

    set_document(editor,
        "ORG $8000\nCOUNT EQU 5\nLD BC,$1234\nLD IX,$9000\nLD (IX-2),A\n"
        "LD B,(IY+3)\nINC (IX+1)\nADD A,(IY-1)\nADC HL,SP\nBIT 7,(IX-8)\n"
        "JP NZ,done\nJR C,done\nCALL Z,done\nPUSH IX\nEX AF,AF'\nEX (SP),IY\nIN A,($FE)\n"
        "IN B,(C)\nOUT (C),D\nIM 1\nLDIR\ndone: RET\nDB \"OK\",0\nDW done\nDS COUNT,$FF\n");
    s_binaryLength = 0;
    expect(run_ok(buildTap, editor) && s_binaryLength > 40,
           "bundled Z80 assembler handles directives and indexed/extended instructions");

    set_document(editor, "org 3000\nld a,255\nloop: dec a\njp nz loop:\nret\n");
    s_binaryLength = 0;
    expect(run_ok(buildTap, editor) && s_binaryLength == 79 &&
           (unsigned char)s_binaryData[39] == 0x0e && (unsigned char)s_binaryData[42] == 0xb8 &&
           (unsigned char)s_binaryData[43] == 0x0b &&
           (unsigned char)s_binaryData[63] == 0xb8 && (unsigned char)s_binaryData[64] == 0x0b &&
           (unsigned char)s_binaryData[71] == 0x3e && (unsigned char)s_binaryData[72] == 0xff &&
           (unsigned char)s_binaryData[73] == 0x3d && (unsigned char)s_binaryData[74] == 0xc2 &&
           (unsigned char)s_binaryData[75] == 0xba && (unsigned char)s_binaryData[76] == 0x0b &&
           (unsigned char)s_binaryData[77] == 0xc9,
           "Z80 assembler accepts whitespace-separated conditions and colon references");

    set_document(editor, "        org 8000\n        ld z,255\nloop:   dec a\n"
                         "        jp nz, loop:\n        ret\n");
    s_lastMessage[0] = 0;
    expect(run_ok(buildTap, editor) && strstr(s_lastMessage, "Line 2:") &&
           strstr(s_lastMessage, "unsupported LD operands") &&
           SendMessageA(editor, SCI_GETSELECTIONSTART, 0, 0) == 17 &&
           SendMessageA(editor, SCI_GETSELECTIONEND, 0, 0) == 33,
           "bundled Z80 assembler highlights and reports the source line for an error");

    set_document(editor, s_calculatorExpression);
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(calculator >= 0 && run_ok(calculator, editor) && document_contains(editor, " = 33"),
           "bundled calculator evaluates bases, shifts and functions");

    s_calculatorExpression = "((0xF0 >> 4) xor 0b0011) + (10 % 4)";
    set_document(editor, s_calculatorExpression);
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(calculator, editor) && document_contains(editor, " = 14"),
           "bundled calculator evaluates bitwise xor and modulo");

    s_calculatorExpression =
        "deg(pi) + sqrt(16)";
    set_document(editor, s_calculatorExpression);
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(calculator, editor) && document_contains(editor, " = 184"),
           "bundled calculator evaluates trigonometry and angle conversions");

    set_document(editor, "hello world");
    SendMessageA(editor, SCI_SETSEL, 0, 5);
    expect(encode64 >= 0 && run_ok(encode64, editor) && document_is(editor, "aGVsbG8= world"),
           "bundled Base64 encoder");
    SendMessageA(editor, SCI_SETSEL, 0, 8);
    expect(decode64 >= 0 && run_ok(decode64, editor) && document_is(editor, "hello world"),
           "bundled Base64 decoder");

    set_document(editor, "a b+c");
    SendMessageA(editor, SCI_SETSEL, 0, 5);
    expect(encodeUrl >= 0 && run_ok(encodeUrl, editor) && document_is(editor, "a%20b%2Bc"),
           "bundled URL encoder");
    SendMessageA(editor, SCI_SETSEL, 0, 9);
    expect(decodeUrl >= 0 && run_ok(decodeUrl, editor) && document_is(editor, "a b+c"),
           "bundled URL decoder");

    set_document(editor, "Hi!");
    SendMessageA(editor, SCI_SETSEL, 0, 3);
    expect(encodeHex >= 0 && run_ok(encodeHex, editor) && document_is(editor, "486921"),
           "bundled hexadecimal encoder");
    SendMessageA(editor, SCI_SETSEL, 0, 6);
    expect(decodeHex >= 0 && run_ok(decodeHex, editor) && document_is(editor, "Hi!"),
           "bundled hexadecimal decoder");

    set_document(editor, "<a&b>");
    SendMessageA(editor, SCI_SETSEL, 0, 5);
    expect(encodeHtml >= 0 && run_ok(encodeHtml, editor) &&
           document_is(editor, "&lt;a&amp;b&gt;"), "bundled HTML entity encoder");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(decodeHtml >= 0 && run_ok(decodeHtml, editor) && document_is(editor, "<a&b>"),
           "bundled HTML entity decoder");

    SendMessageA(editor, SCI_SETEOLMODE, SC_EOL_LF, 0);
    set_document(editor, "# Intro\n\n## Details\n\n```md\n# Hidden\n```");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(toc >= 0 && run_ok(toc, editor) &&
           document_contains(editor, "- [Intro](#intro)\n  - [Details](#details)") &&
           !document_contains(editor, "[Hidden](#hidden)"),
           "bundled TOC generator ignores fenced headings");

    set_document(editor, "alpha\n\n  \nbeta\n");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(removeBlank >= 0 && run_ok(removeBlank, editor) && document_is(editor, "alpha\nbeta\n"),
           "bundled line tool removes blank selected lines");

    set_document(editor, "zebra\nApple\nbanana\n");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(sortLines >= 0 && run_ok(sortLines, editor) &&
           document_is(editor, "Apple\nbanana\nzebra\n"),
           "bundled line tool sorts selected lines case-insensitively");

    set_document(editor, "longest\nx\nmedium\n");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(sortLength >= 0 && run_ok(sortLength, editor) &&
           document_is(editor, "x\nmedium\nlongest\n"),
           "bundled line tool sorts selected lines by length");

    set_document(editor, "one\ntwo\nthree\nfour\n");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(shuffle >= 0 && run_ok(shuffle, editor) &&
           !document_is(editor, "one\ntwo\nthree\nfour\n") &&
           document_contains(editor, "one") && document_contains(editor, "two") &&
           document_contains(editor, "three") && document_contains(editor, "four"),
           "bundled line tool shuffles selected lines");
}

int main(void) {
    /* Resolve the DLL relative to this executable, including Premake builds. */
    WCHAR path[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return 1;
    WCHAR *slash = wcsrchr(path, L'\\');
    if (!slash || (size_t)(slash - path) + 19 >= MAX_PATH) return 1;
    wcscpy(slash + 1, L"..\\Scintilla.dll");
    HMODULE scintilla = LoadLibraryW(path);
    if (!scintilla) { fprintf(stderr, "Cannot load Scintilla.dll\n"); return 1; }
    HWND editor = CreateWindowExA(0, "Scintilla", "", WS_POPUP, 0, 0, 100, 100,
                                  NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!editor) { FreeLibrary(scintilla); return 1; }

    static const PluginHostCallbacks fakes = {
        fake_prompt, fake_choose, fake_file_path, fake_language, fake_message,
        fake_config_get, fake_config_set, NULL, fake_save_binary
    };
    plugin_host_set_callbacks(&fakes);
    test_commands(editor);
    test_separators(editor);
    test_editor_api(editor);
    test_checkboxes_and_config(editor);
    test_ui(editor);
    test_events(editor);
    test_load_failures();
    test_environment();
    test_time_limit();
    test_directory();
    test_bundled_plugins(editor);
    plugin_host_shutdown();

    DestroyWindow(editor);
    FreeLibrary(scintilla);
    if (s_ok) puts("All plugin host tests passed.");
    return s_ok ? 0 : 1;
}
