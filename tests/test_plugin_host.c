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
static char s_configPlugin[64], s_configKey[64], s_configValue[4096];
static char s_binaryName[64], s_binaryData[256];
static size_t s_binaryLength;
static int s_deferredToken;
static char s_documentEncoding[16] = "utf-8";
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
    if (strcmp(label, "Keep events containing:") == 0) {
        char *reply = (char *)malloc(4);
        if (reply) strcpy(reply, "ADA");
        return reply;
    }
    if (strcmp(label, "Remove the lines containing:") == 0) {
        char *reply = (char *)malloc(5);
        if (reply) strcpy(reply, "drop");
        return reply;
    }
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
    if (strcmp(label, "Keep events at or above:") == 0 && count == 6) return 4;
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

static int fake_schedule_deferred(int token, unsigned delayMs, int cancel) {
    (void)delayMs;
    if (cancel) { if (s_deferredToken == token) s_deferredToken = 0; return 1; }
    s_deferredToken = token;
    return 1;
}

static void fake_message(const char *title, const char *text, const char *kind) {
    (void)kind;
    snprintf(s_lastTitle, sizeof(s_lastTitle), "%s", title);
    snprintf(s_lastMessage, sizeof(s_lastMessage), "%s", text);
}

static void fake_status(const char *text) {
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

static const char *fake_document_encoding(void) {
    return s_documentEncoding;
}

static int fake_document_set_encoding(const char *encoding) {
    snprintf(s_documentEncoding, sizeof(s_documentEncoding), "%s", encoding);
    return 1;
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
        "  config.set('options', {enabled=true, names={'one','two'}, nested={count=3}})\n"
        "  local value = config.get('options', {})\n"
        "  assert(value.enabled and value.names[2] == 'two' and value.nested.count == 3)\n"
        "end)\n");
    expect(plugin_host_command_is_checkable(first), "checkbox command is checkable");
    expect(!plugin_host_command_is_checked(first), "checkbox uses its default state");
    expect(run_ok(first, editor), "checkbox command runs with its new state");
    expect(plugin_host_command_is_checked(first) && strcmp(s_configPlugin, "checkbox.lua") == 0 &&
           strcmp(s_configKey, "enabled") == 0 && strcmp(s_configValue, "1") == 0,
           "checkbox state is persisted in its plugin namespace");
    expect(run_ok(first + 1, editor) && strcmp(s_configKey, "options") == 0 &&
           strncmp(s_configValue, "@ME1:", 5) == 0,
           "config.set persists primitive and structured values");

    int failing = load_ok("bad_checkbox.lua",
        "menu.checkbox('Broken', 'enabled', true, function() error('nope') end)");
    expect(run_fails(failing, editor, "nope"), "failing checkbox command reports its error");
    expect(plugin_host_command_is_checked(failing), "a failing checkbox command restores its setting");
}

static void test_editor_api(HWND editor) {
    int first = load_ok("api.lua",
        "menu.add('Lines', function()\n"
        "  assert(editor.line_count() == 3 and editor.get_line(2) == 'two')\n"
        "  assert(editor.tab_width() > 0)\n"
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
        "menu.add('Undo balance', function() editor.begin_undo(); editor.insert('nested ') end)\n"
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
    set_document(editor, "plain");
    expect(run_ok(first + 4, editor) && document_is(editor, "nested plain"),
           "unmatched explicit undo groups are closed after a callback");
    SendMessageA(editor, SCI_UNDO, 0, 0);
    expect(document_is(editor, "plain"), "explicit undo nesting remains one command undo step");
    set_document(editor, "zero one two");
    expect(run_ok(first + 5, editor) && document_is(editor, "zero one TWO"),
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

    int positions = load_ok("find-scroll.lua",
        "menu.add('Find and scroll', function()\n"
        "  local s, f = editor.find('abc', 1)\n"
        "  assert(s == 1 and f == 3, 'find from the first position')\n"
        "  s, f = editor.find('abc', 2)\n"
        "  assert(s == 4 and f == 6, 'find starts at the given position')\n"
        "  assert(editor.find('abc', 5) == nil, 'find skips matches before the start')\n"
        "  editor.scroll_to(4)\n"
        "  assert(editor.get_caret() == 4, 'scroll_to moves the caret to the position')\n"
        "  editor.scroll_to(1)\n"
        "  assert(editor.get_caret() == 1, 'scroll_to the first position')\n"
        "end)");
    set_document(editor, "abcabc");
    expect(run_ok(positions, editor), "find and scroll_to take 1-based positions");
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

static void test_deferred(HWND editor) {
    static const PluginHostCallbacks callbacks = { .schedule_deferred = fake_schedule_deferred };
    plugin_host_set_callbacks(&callbacks);
    s_deferredToken = 0;
    int first = load_ok("defer.lua",
        "menu.add('Defer', function()\n"
        "  app.defer(10, function() editor.insert('later') end)\n"
        "end)\n");
    set_document(editor, "now ");
    SendMessageA(editor, SCI_GOTOPOS, 4, 0);
    expect(run_ok(first, editor) && s_deferredToken > 0, "app.defer schedules a callback");
    char *error = plugin_host_run_deferred(s_deferredToken, editor);
    expect(!error && document_is(editor, "now later"), "a deferred callback runs on the editor");
    free(error);
    s_deferredToken = 0;
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
        "menu.add('Reenter', function() ui.prompt('x') end)\n"
        "local removed = events.on('open', function() error('removed handler ran') end)\n"
        "assert(events.off(removed))\n"
        "local self, calls = nil, 0\n"
        "self = events.on('open', function() calls = calls + 1; events.off(self) end)\n"
        "events.on('open', function() calls = calls + 1 end)\n"
        "menu.add('Event removal', function() assert(calls == 3) end)\n");

    set_document(editor, "x");
    char *report = plugin_host_fire("save", "C:\\a.md", editor);
    expect(report && strstr(report, "second handler fails"), "a failing handler is reported");
    expect(document_is(editor, "saved x"), "handlers edit; a failing one's edits are undone");
    free(report);
    expect(run_ok(first, editor), "handlers receive the path");
    expect(plugin_host_fire("open", NULL, editor) == NULL, "nil path for an unsaved document");
    expect(plugin_host_fire("open", NULL, editor) == NULL, "self-removing handlers do not corrupt dispatch");
    expect(run_ok(first + 2, editor), "event removal preserves later handlers");
    expect(plugin_host_fire("tab_changed", NULL, editor) == NULL, "no handlers, no report");

    const char *unknown = "events.on('not_an_event', function() end)";
    char *error = plugin_host_load_chunk("unknown.lua", unknown, strlen(unknown));
    expect(error && strstr(error, "unknown event"), "unknown event names are rejected");
    free(error);
    const char *partial = "events.on('open', function() error('should not run') end)\nerror('late')\n";
    error = plugin_host_load_chunk("partial_events.lua", partial, strlen(partial));
    free(error);
    expect(plugin_host_fire("open", NULL, editor) == NULL, "a failed script's handlers are dropped");

    static const PluginHostCallbacks reentrant = {
        .prompt = reentrant_prompt, .config_get = fake_config_get, .config_set = fake_config_set
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
    load_ok("metadata.lua",
        "assert(app.api_version() >= 3 and app.platform() == 'windows')\n"
        "assert(type(app.version()) == 'string' and app.memory_used() > 0)\n"
        "local c = app.capabilities(); assert(c.plugin_environments and c.lazy_require)\n"
        "assert(c.eol_conversion and c.save_encoding and c.tab_width)\n");
}

static void test_isolation(HWND editor) {
    plugin_host_shutdown();
    int first = load_ok("isolated-a.lua",
        "private_value = 'a'; string.plugin_change = true\n"
        "menu.add('Isolated A', function() assert(private_value == 'a' and string.plugin_change) end)");
    int second = load_ok("isolated-b.lua",
        "assert(private_value == nil and string.plugin_change == nil)\n"
        "private_value = 'b'\n"
        "menu.add('Isolated B', function() assert(private_value == 'b') end)");
    expect(run_ok(first, editor) && run_ok(second, editor),
           "plugins have isolated globals and library tables");
}

static void test_time_limit(void) {
    const char *loop = "while true do end";
    char *error = plugin_host_load_chunk("loop.lua", loop, strlen(loop));
    expect(error && strstr(error, "stopped after"), "runaway script is stopped");
    free(error);
    const char *allocation = "return string.rep('x', 70 * 1024 * 1024)";
    error = plugin_host_load_chunk("memory.lua", allocation, strlen(allocation));
    expect(error && (strstr(error, "memory") || strstr(error, "allocate")),
           "plugin heap exhaustion is reported instead of exhausting the process");
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

static void test_directory(HWND editor) {
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
    write_script(folder, L"late.lua", "return { value = 99 }");
    write_script(nested, L"value.lua", "return 42");
    write_script(folder, L"init.lua",
        "local helper = require('helper')\n"
        "assert(require('helper') == helper)\n"
        "assert(require('nested.value') == 42)\n"
        "menu.add(helper.label, function() end, 'Ctrl+Alt+M')\n"
        "menu.add('Lazy Module', function() assert(require('late').value == 99) end)");

    plugin_host_shutdown();
    char *report = plugin_host_load_directory(directory, "test");
    expect(report && strstr(report, "broken.lua") && strstr(report, "nul.lua"),
           "load errors are reported by file, including source after a NUL byte");
    expect(plugin_host_command_count() == 4, "only the good scripts and folder extensions load");
    expect(plugin_host_command_count() == 4 &&
           strcmp(plugin_host_command_label(0), "From A") == 0 &&
           strcmp(plugin_host_command_label(1), "From B") == 0 &&
           strcmp(plugin_host_command_label(2), "From Folder") == 0,
           "scripts and folder extensions load in name order");
    expect(run_ok(3, editor), "folder modules can be required lazily from a command");
    expect(strcmp(plugin_host_command_shortcut(2), "Ctrl+Alt+M") == 0,
           "plugin commands retain shortcuts");
    expect(plugin_host_extension_count() == 5 && plugin_host_error_path() != NULL,
           "discovered extensions and the first failing path are available to the UI");
    free(report);

    static const PluginHostCallbacks filtered = {
        .plugin_enabled = test_plugin_enabled
    };
    plugin_host_shutdown();
    plugin_host_set_callbacks(&filtered);
    report = plugin_host_load_directory(directory, "test");
    expect(plugin_host_command_count() == 3 &&
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
    WCHAR *late = path_join(folder, L"late.lua");
    WCHAR *init = path_join(folder, L"init.lua");
    WCHAR *nestedValue = path_join(nested, L"value.lua");
    DeleteFileW(helper); DeleteFileW(late); DeleteFileW(init); DeleteFileW(nestedValue);
    RemoveDirectoryW(nested); RemoveDirectoryW(folder);
    free(nestedValue); free(nested); free(helper); free(late); free(init); free(folder);
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
        .prompt = fake_prompt, .choose = fake_choose, .file_path = fake_file_path,
        .language = fake_language, .message = fake_message, .config_get = fake_config_get,
        .config_set = fake_config_set, .save_binary = fake_save_binary,
        .status = fake_status,
        .document_encoding = fake_document_encoding,
        .document_set_encoding = fake_document_set_encoding,
        .schedule_deferred = fake_schedule_deferred
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
    int csvInspect = find_command("/Tools/CSV/Inspect and Validate");
    int csvFormat = find_command("/Tools/CSV/Format Columns");
    int csvToComma = find_command("/Tools/CSV/Convert/To Comma-Separated");
    int csvToTab = find_command("/Tools/CSV/Convert/To Tab-Separated");
    int csvToJson = find_command("/Tools/CSV/Convert/To JSON Objects");
    int csvDedupe = find_command("/Tools/CSV/Rows/Remove Duplicate Rows");
    int csvTranspose = find_command("/Tools/CSV/Columns/Transpose Rows and Columns");
    int jsonPretty = find_command("/Tools/JSON/Format JSON");
    int jsonMinify = find_command("/Tools/JSON/Minify JSON");
    int jsonValidate = find_command("/Tools/JSON/Validate");
    int jsonSort = find_command("/Tools/JSON/Sort Object Keys");
    int jsonToYaml = find_command("/Tools/JSON/Convert JSON to YAML");
    int yamlToJson = find_command("/Tools/JSON/Convert YAML to JSON");
    int jsonEscape = find_command("/Tools/JSON/Escape Selection as JSON String");
    int jsonUnescape = find_command("/Tools/JSON/Unescape JSON String");
    int editUpper = find_command("/Edit/Convert/&UPPERCASE");
    int editLower = find_command("/Edit/Convert/&lowercase");
    int editTitle = find_command("/Edit/Convert/&Title Case");
    int editSort = find_command("/Edit/Convert/Sort Lines &Ascending");
    int editSortDescending = find_command("/Edit/Convert/Sort Lines Descend&ing");
    int editDedupe = find_command("/Edit/Convert/Remove &Duplicate Lines");
    int editTabs = find_command("/Edit/Convert/Tabs to &Spaces");
    int editSpaces = find_command("/Edit/Convert/Spaces to Ta&bs");
    int editCrlf = find_command("/Edit/Convert/Line &Endings/&CRLF (Windows)");
    int editLf = find_command("/Edit/Convert/Line &Endings/&LF (Unix/macOS)");
    int editCr = find_command("/Edit/Convert/Line &Endings/C&R (Classic Mac)");
    int editUtf8 = find_command("/Edit/Convert/En&coding/UTF-&8");
    int editUtf8Bom = find_command("/Edit/Convert/En&coding/UTF-8 with &BOM");
    int editUtf16Le = find_command("/Edit/Convert/En&coding/UTF-16 &LE");
    int editUtf16Be = find_command("/Edit/Convert/En&coding/UTF-16 B&E");
    int editAnsi = find_command("/Edit/Convert/En&coding/&ANSI (system code page)");
    int naturalSort = find_command("/Edit/Convert/Natural Sort Lines Ascending");
    int numberHex = find_command("/Tools/Number Converter/To Hexadecimal");
    int numberDecimal = find_command("/Tools/Number Converter/To Decimal");
    int numberRoman = find_command("/Tools/Number Converter/To Roman Numerals");
    int sqlFormat = find_command("/Tools/SQL/Format SQL");
    int sqlMinify = find_command("/Tools/SQL/Minify SQL");
    int sqlUpper = find_command("/Tools/SQL/Uppercase Keywords");
    int sqlLower = find_command("/Tools/SQL/Lowercase Keywords");
    int removeContaining = find_command("Lines/Remove Lines Containing...");
    int jsonLinesToArray = find_command("/Tools/Data Converter/JSON Lines to JSON Array");
    int jsonArrayToLines = find_command("/Tools/Data Converter/JSON Array to JSON Lines");
    int queryToJson = find_command("/Tools/Data Converter/Query String to JSON");
    int clefFormat = find_command("/Tools/Logs/CLEF/Format Events");
    int clefWarnings = find_command("/Tools/Logs/CLEF/Keep Warnings and Above");
    int clefFormatProperties = find_command("/Tools/Logs/CLEF/Format Events with Properties");
    int clefInspect = find_command("/Tools/Logs/CLEF/Inspect");
    int clefLevel = find_command("/Tools/Logs/CLEF/Keep Level and Above...");
    int clefSearch = find_command("/Tools/Logs/CLEF/Keep Events Containing...");
    int clefSort = find_command("/Tools/Logs/CLEF/Sort by Timestamp");
    int clefExceptions = find_command("/Tools/Logs/CLEF/Extract Exceptions");
    int grammarCheck = find_command("/Tools/Grammar Checker/Check");
    int grammarHighlight = find_command("/Tools/Grammar Checker/Highlight Issues");
    int grammarFix = find_command("/Tools/Grammar Checker/Fix Safe Issues");
    int rot13 = find_command("Secret Message Lab/ROT13");
    int atbash = find_command("Secret Message Lab/Atbash");
    int encodeMorse = find_command("Secret Message Lab/Morse Code/Encode");
    int decodeMorse = find_command("Secret Message Lab/Morse Code/Decode");
    int encodeNato = find_command("Secret Message Lab/NATO Phonetics/Encode");
    int decodeNato = find_command("Secret Message Lab/NATO Phonetics/Decode");
    int encodeBinary = find_command("Secret Message Lab/Binary/Encode UTF-8");
    int decodeBinary = find_command("Secret Message Lab/Binary/Decode UTF-8");
    int pigLatin = find_command("Secret Message Lab/Pig Latin");
    int personality = find_command("Document Personality/Analyse");
    int clearPersonality = find_command("Document Personality/Clear Highlights");
    int checkConstraints = find_command("Constraint Writer/Check Constraints");
    int highlightConstraints = find_command("Constraint Writer/Highlight Issues");
    int clearConstraints = find_command("Constraint Writer/Clear Highlights");
    int toc = find_command("/Snippets/Markdown/Table of Contents...");
    int calculator = find_command("Calculate Selection");
    if (calculator < 0) calculator = find_command("/Tools/Calculate Selection");
    int removeBlank = find_command("Line Tools/Clean/Remove Blank Lines");
    int sortLines = find_command("Line Tools/Order/Sort A to Z");
    int sortLength = find_command("Line Tools/Order/Sort by Length");
    int shuffle = find_command("Line Tools/Order/Shuffle");
    int buildTap = find_command("Z80 Assembler/Compile");
    if (buildTap < 0) buildTap = find_command("/Tools/Z80 Assembler/Compile");
    expect(encode64 >= 0 && decode64 >= 0 && encodeUrl >= 0 && decodeUrl >= 0 &&
           encodeHex >= 0 && decodeHex >= 0 && encodeHtml >= 0 && decodeHtml >= 0 &&
           csvInspect >= 0 && csvFormat >= 0 && csvToComma >= 0 && csvToTab >= 0 && csvToJson >= 0 &&
           csvDedupe >= 0 && csvTranspose >= 0 &&
           jsonPretty >= 0 && jsonMinify >= 0 && jsonValidate >= 0 && jsonSort >= 0 &&
           jsonToYaml >= 0 && yamlToJson >= 0 && jsonEscape >= 0 && jsonUnescape >= 0 &&
           editUpper >= 0 && editLower >= 0 && editTitle >= 0 && editSort >= 0 &&
           editSortDescending >= 0 && editDedupe >= 0 && editTabs >= 0 && editSpaces >= 0 &&
           editCrlf >= 0 && editLf >= 0 && editCr >= 0 && editUtf8 >= 0 &&
           editUtf8Bom >= 0 && editUtf16Le >= 0 && editUtf16Be >= 0 && editAnsi >= 0 &&
           naturalSort >= 0 && numberHex >= 0 && numberDecimal >= 0 && numberRoman >= 0 &&
           sqlFormat >= 0 && sqlMinify >= 0 && sqlUpper >= 0 && sqlLower >= 0 &&
           removeContaining >= 0 && jsonLinesToArray >= 0 &&
           jsonArrayToLines >= 0 && queryToJson >= 0 && clefFormat >= 0 &&
           clefWarnings >= 0 && clefFormatProperties >= 0 && clefInspect >= 0 &&
           clefLevel >= 0 && clefSearch >= 0 && clefSort >= 0 && clefExceptions >= 0 &&
           grammarCheck >= 0 && grammarHighlight >= 0 && grammarFix >= 0 &&
           rot13 >= 0 && atbash >= 0 && encodeMorse >= 0 && decodeMorse >= 0 &&
           encodeNato >= 0 && decodeNato >= 0 && encodeBinary >= 0 &&
           decodeBinary >= 0 && pigLatin >= 0 &&
           personality >= 0 && clearPersonality >= 0 &&
           checkConstraints >= 0 && highlightConstraints >= 0 && clearConstraints >= 0 &&
           toc >= 0 && calculator >= 0 && removeBlank >= 0 &&
           sortLines >= 0 && sortLength >= 0 && shuffle >= 0 && buildTap >= 0,
           "bundled plugin commands are registered");

    set_document(editor, "one two three");
    SendMessageA(editor, SCI_SETSEL, 4, 7);
    expect(run_ok(editUpper, editor) && document_is(editor, "one TWO three") &&
           SendMessageA(editor, SCI_GETSELECTIONSTART, 0, 0) == 4 &&
           SendMessageA(editor, SCI_GETSELECTIONEND, 0, 0) == 7,
           "Edit Convert case command preserves its selection");
    expect(run_ok(editLower, editor) && document_is(editor, "one two three"),
           "Edit Convert lowercases a selection");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(editTitle, editor) && document_is(editor, "One Two Three"),
           "Edit Convert title-cases a selection");
    set_document(editor, "it's DON'T o'clock won\xE2\x80\x99t");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(editTitle, editor) && document_is(editor, "It's Don't O'clock Won\xE2\x80\x99t"),
           "Edit Convert title case keeps apostrophes inside words");

    SendMessageA(editor, SCI_SETEOLMODE, SC_EOL_CRLF, 0);
    set_document(editor, "b\r\na\r\nb\r\n");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(editSort, editor) && document_is(editor, "a\r\nb\r\nb\r\n"),
           "Edit Convert sorts selected lines");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(editSortDescending, editor) && document_is(editor, "b\r\nb\r\na\r\n"),
           "Edit Convert sorts selected lines descending");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(editDedupe, editor) && document_is(editor, "b\r\na\r\n"),
           "Edit Convert removes duplicate selected lines");

    SendMessageA(editor, SCI_SETTABWIDTH, 4, 0);
    set_document(editor, "\tx\n  \ty\ninside\ttext");
    expect(run_ok(editTabs, editor) && document_is(editor, "    x\n    y\ninside\ttext"),
           "Edit Convert changes only leading indentation");
    expect(run_ok(editSpaces, editor) && document_is(editor, "\tx\n\ty\ninside\ttext"),
           "Edit Convert converts leading spaces back to tabs");
    expect(run_ok(editLf, editor) && document_is(editor, "\tx\n\ty\ninside\ttext") &&
           SendMessageA(editor, SCI_GETEOLMODE, 0, 0) == SC_EOL_LF,
           "Edit Convert changes existing and future line endings");
    expect(run_ok(editCr, editor) && document_is(editor, "\tx\r\ty\rinside\ttext") &&
           SendMessageA(editor, SCI_GETEOLMODE, 0, 0) == SC_EOL_CR,
           "Edit Convert changes line endings to CR");
    expect(run_ok(editCrlf, editor) && document_is(editor, "\tx\r\n\ty\r\ninside\ttext") &&
           SendMessageA(editor, SCI_GETEOLMODE, 0, 0) == SC_EOL_CRLF,
           "Edit Convert changes line endings to CRLF");
    expect(run_ok(editLf, editor) && SendMessageA(editor, SCI_GETEOLMODE, 0, 0) == SC_EOL_LF,
           "Edit Convert can return the document to LF endings");
    strcpy(s_documentEncoding, "ansi");
    expect(run_ok(editUtf8, editor) && strcmp(s_documentEncoding, "utf-8") == 0 &&
           run_ok(editUtf8Bom, editor) && strcmp(s_documentEncoding, "utf-8-bom") == 0 &&
           run_ok(editUtf16Le, editor) && strcmp(s_documentEncoding, "utf-16le") == 0 &&
           run_ok(editUtf16Be, editor) && strcmp(s_documentEncoding, "utf-16be") == 0 &&
           run_ok(editAnsi, editor) && strcmp(s_documentEncoding, "ansi") == 0,
           "Edit Convert exposes every save encoding through the plugin API");

    set_document(editor, "file10\nfile2\nfile01\nfile1");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(naturalSort, editor) && document_is(editor, "file1\nfile01\nfile2\nfile10"),
           "natural sort compares embedded digit runs numerically");

    set_document(editor, "255");
    SendMessageA(editor, SCI_SETSEL, 0, 3);
    expect(run_ok(numberHex, editor) && document_is(editor, "0xFF"),
           "number converter converts decimal to hexadecimal");
    SendMessageA(editor, SCI_SETSEL, 0, 4);
    expect(run_ok(numberDecimal, editor) && document_is(editor, "255"),
           "number converter reads prefixed hexadecimal");
    SendMessageA(editor, SCI_SETSEL, 0, 3);
    expect(run_ok(numberRoman, editor) && document_is(editor, "CCLV"),
           "number converter creates canonical Roman numerals");

    set_document(editor, "select a,b from users where id=1 and active=true;");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(sqlFormat, editor) &&
           document_is(editor, "SELECT a, b\nFROM users\nWHERE id = 1 AND active = TRUE;"),
           "SQL plugin formats clauses without changing identifiers");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(sqlMinify, editor) &&
           document_is(editor, "SELECT a, b FROM users WHERE id = 1 AND active = TRUE;"),
           "SQL plugin minifies formatted SQL");
    set_document(editor, "select count(*) from users;");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(sqlFormat, editor) && document_is(editor, "SELECT count(*)\nFROM users;"),
           "SQL plugin keeps function parentheses on the same line");
    set_document(editor, "select a from t where id=$1 and n=@p and s=N'x' and k=:key and v=a::int");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(sqlMinify, editor) &&
           document_is(editor, "SELECT a FROM t WHERE id = $1 AND n = @p AND s = N'x' AND k = :key AND v = a::int"),
           "SQL plugin keeps parameters, variables and prefixed strings whole");
    set_document(editor, "select a,\n  b\nfrom t -- note\nwhere x=1");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(sqlUpper, editor) && document_is(editor, "SELECT a,\n  b\nFROM t -- note\nWHERE x=1"),
           "SQL plugin changes keyword case without changing the layout");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(sqlLower, editor) && document_is(editor, "select a,\n  b\nfrom t -- note\nwhere x=1"),
           "SQL plugin lowercases keywords in place");

    set_document(editor, "{\"a\":1}\n{\"a\":2}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(jsonLinesToArray, editor) &&
           document_is(editor, "[\n  {\"a\":1},\n  {\"a\":2}\n]"),
           "data converter turns JSON Lines into an array");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(jsonArrayToLines, editor) && document_is(editor, "{\"a\":1}\n{\"a\":2}"),
           "data converter turns a JSON array back into JSON Lines");
    set_document(editor, "name=Ada+Lovelace&lang=Lua");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(queryToJson, editor) &&
           document_is(editor, "{\n  \"name\": \"Ada Lovelace\",\n  \"lang\": \"Lua\"\n}"),
           "data converter decodes URL query strings");

    set_document(editor,
        "{\"@t\":\"2026-01-01T00:00:00Z\",\"@mt\":\"Hello {Name}\","
        "\"@l\":\"Warning\",\"Name\":\"Ada\"}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefFormat, editor) &&
           document_is(editor, "2026-01-01T00:00:00Z [WRN] Hello Ada"),
           "log plugin renders CLEF message templates");
    set_document(editor,
        "{\"@t\":\"1\",\"@m\":\"ok\",\"@l\":\"Information\"}\n"
        "{\"@t\":\"2\",\"@m\":\"bad\",\"@l\":\"Error\"}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefWarnings, editor) &&
           document_is(editor, "{\"@t\":\"2\",\"@m\":\"bad\",\"@l\":\"Error\"}"),
           "log plugin filters CLEF by severity");
    set_document(editor,
        "{\"@t\":\"1\",\"@mt\":\"{{x}} {$Name,-5}| {Missing} [{Empty}] {Quote} {Face}\","
        "\"Name\":\"Ada\",\"Empty\":\"\",\"Quote\":\"\\\"q\\\"\",\"Face\":\"\\ud83d\\ude00\"}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefFormat, editor) &&
           document_is(editor, "1 [INF] {x} Ada  | {Missing} [] \"q\" \xF0\x9F\x98\x80"),
           "log plugin renders CLEF escapes, alignment, empty values and surrogate pairs");
    set_document(editor,
        "{\"@t\":\"1\",\"@mt\":\"Saw {raw}\",\"@@t\":\"user\",\"raw\":\"x\",\"@l\":\"Error\"}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefFormatProperties, editor) &&
           document_is(editor, "1 [ERR] Saw x\n    @t = \"user\"\n    raw = \"x\""),
           "log plugin keeps @@ properties apart from reserved CLEF fields");
    set_document(editor, "{\"@m\":\"a\"}\n\nnot json");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(clefFormat, editor) && strstr(s_lastMessage, "line 3."),
           "log plugin reports the real line number of an invalid CLEF event");
    set_document(editor,
        "{\"@t\":\"2026-01-01T01:00:00+02:00\",\"@m\":\"b\"}\n"
        "{\"@m\":\"none\"}\n"
        "{\"@t\":\"2026-01-01T00:00:00.5Z\",\"@m\":\"d\"}\n"
        "{\"@t\":\"2026-01-01T00:00:00Z\",\"@m\":\"c\"}\n"
        "{\"@t\":\"2025-12-31T23:00:00.25Z\",\"@m\":\"a\"}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefSort, editor) &&
           document_is(editor,
               "{\"@t\":\"2026-01-01T01:00:00+02:00\",\"@m\":\"b\"}\n"
               "{\"@t\":\"2025-12-31T23:00:00.25Z\",\"@m\":\"a\"}\n"
               "{\"@t\":\"2026-01-01T00:00:00Z\",\"@m\":\"c\"}\n"
               "{\"@t\":\"2026-01-01T00:00:00.5Z\",\"@m\":\"d\"}\n"
               "{\"@m\":\"none\"}"),
           "log plugin sorts CLEF events by UTC timestamp");
    s_lastMessage[0] = 0;
    expect(run_ok(clefInspect, editor) && strstr(s_lastMessage, "CLEF events: 5") &&
           strstr(s_lastMessage, "First: 2026-01-01T01:00:00+02:00") &&
           strstr(s_lastMessage, "Last: 2026-01-01T00:00:00.5Z"),
           "log plugin inspect reports the CLEF time range");
    set_document(editor,
        "{\"@m\":\"i\",\"@l\":\"Information\"}\n"
        "{\"@m\":\"e\",\"@l\":\"Error\"}\n"
        "{\"@m\":\"f\",\"@l\":\"Fatal\",\"User\":\"ada\"}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefLevel, editor) &&
           document_is(editor, "{\"@m\":\"e\",\"@l\":\"Error\"}\n"
                               "{\"@m\":\"f\",\"@l\":\"Fatal\",\"User\":\"ada\"}"),
           "log plugin filters CLEF by a chosen level");
    expect(run_ok(clefSearch, editor) &&
           document_is(editor, "{\"@m\":\"f\",\"@l\":\"Fatal\",\"User\":\"ada\"}"),
           "log plugin keeps CLEF events containing text");
    set_document(editor,
        "{\"@t\":\"1\",\"@m\":\"ok\"}\n"
        "{\"@t\":\"2\",\"@m\":\"boom\",\"@x\":\"System.Exception: boom\\n   at Main\"}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefExceptions, editor) &&
           document_is(editor, "2 boom\nSystem.Exception: boom\n   at Main"),
           "log plugin extracts CLEF exceptions with their message");
    set_document(editor,
        "{\"@t\":\"1\",\"@mt\":\"Placed {@Order} for {Tags} in {Elapsed:0.0} ms\","
        "\"Order\":{\"$type\":\"Order\",\"Id\":5,\"Lines\":[{\"Sku\":\"}A\\\"1\"},null]},"
        "\"Tags\":[\"x\",true],\"Elapsed\":12.345,\"@r\":[\"12.3\"]}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefFormat, editor) &&
           document_is(editor, "1 [INF] Placed Order { Id: 5, Lines: [{ Sku: \"}A\\\"1\" }, null] }"
                               " for [\"x\", true] in 12.3 ms"),
           "log plugin renders nested CLEF objects, arrays and @r renderings");
    set_document(editor,
        "{\"@t\":\"1\",\"@m\":\"m\",\"Order\":{\"Id\":5,\"Tags\":[]},"
        "\"Body\":\"{\\\"a\\\":[1,2]}\"}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(clefFormatProperties, editor) &&
           document_is(editor, "1 [INF] m\n"
                               "    Order = {\n      \"Id\": 5,\n      \"Tags\": []\n    }\n"
                               "    Body = {\n      \"a\": [\n        1,\n        2\n      ]\n    }"),
           "log plugin lists nested and embedded JSON properties as indented JSON");
    set_document(editor, "{\"@m\":\"x\",\"A\":{\"b\":}}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(clefFormat, editor) && strstr(s_lastMessage, "line 1."),
           "log plugin rejects invalid nested JSON in a CLEF event");

    set_document(editor, "this is is a error.");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(grammarCheck, editor) && strstr(s_lastMessage, "possible issues") &&
           strstr(s_lastMessage, "Line 1, column 1") && strstr(s_lastMessage, "this is is a error.") &&
           run_ok(grammarHighlight, editor) && SendMessageA(editor, SCI_INDICATORVALUEAT, 12, 0),
           "grammar checker reports source lines and highlights heuristic issues");
    set_document(editor, "This is is badly ,spaced!");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(grammarFix, editor) && document_is(editor, "This is badly, spaced!"),
           "grammar checker fixes safe spacing and repeated-word issues");

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

    s_calculatorExpression = "max(1, 2) * 3";
    set_document(editor, s_calculatorExpression);
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(calculator, editor) && document_contains(editor, " = 6"),
           "bundled calculator accepts commas between function arguments");

    s_calculatorExpression = "1,000 * 2";
    set_document(editor, s_calculatorExpression);
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    s_lastMessage[0] = 0;
    expect(run_ok(calculator, editor) && document_is(editor, "1,000 * 2") &&
           strstr(s_lastMessage, "Commas can only separate function arguments"),
           "bundled calculator rejects thousands separators instead of misreading them");

    set_document(editor, "hello world");
    SendMessageA(editor, SCI_SETSEL, 0, 5);
    expect(encode64 >= 0 && run_ok(encode64, editor) && document_is(editor, "aGVsbG8= world"),
           "bundled Base64 encoder");
    SendMessageA(editor, SCI_SETSEL, 0, 8);
    expect(decode64 >= 0 && run_ok(decode64, editor) && document_is(editor, "hello world"),
           "bundled Base64 decoder");

    set_document(editor, "AB==");
    SendMessageA(editor, SCI_SETSEL, 0, 4);
    s_lastMessage[0] = 0;
    expect(run_ok(decode64, editor) && document_is(editor, "AB==") &&
           strstr(s_lastMessage, "not valid Base64"),
           "bundled Base64 decoder rejects non-canonical padding bits");

    set_document(editor, "AAEC/w==");
    SendMessageA(editor, SCI_SETSEL, 0, 8);
    s_lastMessage[0] = 0;
    expect(run_ok(decode64, editor) && document_is(editor, "AAEC/w==") &&
           strstr(s_lastMessage, "binary data"),
           "bundled Base64 decoder refuses to insert binary data");
    set_document(editor, "ff fe");
    SendMessageA(editor, SCI_SETSEL, 0, 5);
    s_lastMessage[0] = 0;
    expect(run_ok(decodeHex, editor) && document_is(editor, "ff fe") &&
           strstr(s_lastMessage, "binary data"),
           "bundled hexadecimal decoder refuses to insert binary data");
    set_document(editor, "%FF");
    SendMessageA(editor, SCI_SETSEL, 0, 3);
    s_lastMessage[0] = 0;
    expect(run_ok(decodeUrl, editor) && document_is(editor, "%FF") &&
           strstr(s_lastMessage, "binary data"),
           "bundled URL decoder refuses to insert binary data");

    set_document(editor, "keep\ndrop 1\ndrop 2\nkeep too");
    SendMessageA(editor, SCI_SETSEL, 5, 18);
    expect(removeContaining >= 0 && run_ok(removeContaining, editor) &&
           document_is(editor, "keep\nkeep too"),
           "removing every selected line leaves no blank line behind");
    set_document(editor, "keep\ndrop 1\ndrop 2");
    SendMessageA(editor, SCI_SETSEL, 5, 18);
    expect(run_ok(removeContaining, editor) && document_is(editor, "keep"),
           "removing the last lines also removes the line ending before them");

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

    set_document(editor, "name;note\nAda;\"hello; world\"\n");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvToTab, editor) && document_is(editor, "name\tnote\nAda\thello; world\n"),
           "bundled CSV plugin parses quoted semicolon data and converts it to TSV");

    set_document(editor, "name\tage\nAda\t36\nBob\t40");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvToComma, editor) && document_is(editor, "name,age\nAda,36\nBob,40"),
           "bundled CSV plugin converts TSV to CSV");

    set_document(editor, "before\nname;age\nAda;36\nafter");
    SendMessageA(editor, SCI_SETSEL, 7, 22);
    expect(run_ok(csvToComma, editor) &&
           document_is(editor, "before\nname,age\nAda,36\nafter") &&
           SendMessageA(editor, SCI_GETSELECTIONSTART, 0, 0) == 7 &&
           SendMessageA(editor, SCI_GETSELECTIONEND, 0, 0) == 22,
           "bundled CSV plugin transforms only the selected data range");

    set_document(editor, "name age\nAda 36\nBob 40");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvToComma, editor) && document_is(editor, "name age\nAda 36\nBob 40"),
           "bundled CSV plugin never auto-detects whitespace as the separator");

    set_document(editor, "name;age\nAda;36\nBob;40");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvToJson, editor) &&
           document_is(editor, "[\n  {\n    \"name\": \"Ada\",\n    \"age\": \"36\"\n  },\n  {\n    \"name\": \"Bob\",\n    \"age\": \"40\"\n  }\n]"),
           "bundled CSV plugin converts delimited rows to JSON objects");

    set_document(editor, "name,age\nAda,36\nAda,36\nBob,40");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvDedupe, editor) && document_is(editor, "name,age\nAda,36\nBob,40"),
           "bundled CSV plugin removes duplicate data rows");

    set_document(editor, "name,age\nAda,36\nBob,40");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvTranspose, editor) && document_is(editor, "name,Ada,Bob\nage,36,40"),
           "bundled CSV plugin transposes rows and columns");

    set_document(editor, "name,age\nAda,36\nBob,40");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(csvInspect, editor) && strstr(s_lastMessage, "Separator: comma") &&
           strstr(s_lastMessage, "Rows: 3") && strstr(s_lastMessage, "Columns: 2"),
           "bundled CSV plugin inspects delimited data");

    set_document(editor, "name,note\nAda,un\"quoted");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(csvInspect, editor) && document_is(editor, "name,note\nAda,un\"quoted") &&
           strstr(s_lastMessage, "line 2, column 7") &&
           strstr(s_lastMessage, "quoted field must begin"),
           "bundled CSV plugin rejects quotes inside unquoted fields");

    set_document(editor, "name,age\nAlexandra,9\nBo,12");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvFormat, editor) &&
           document_is(editor, "name     ,age\nAlexandra,9\nBo       ,12"),
           "bundled CSV plugin aligns columns without adding quotes");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvFormat, editor) &&
           document_is(editor, "name     ,age\nAlexandra,9\nBo       ,12"),
           "formatting aligned CSV again leaves it unchanged");
    set_document(editor, "a,b\n\"x,y\",\"say \"\"hi\"\"\"");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvFormat, editor) &&
           document_is(editor, "a    ,b\n\"x,y\",\"say \"\"hi\"\"\""),
           "CSV formatting keeps fields that were quoted as they were");
    set_document(editor, "id, name, city\n1, Alexandra, Oslo\n22, Bo, \"Rome, IT\"");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvFormat, editor) &&
           document_is(editor, "id,name     ,city\n1 ,Alexandra,Oslo\n22,Bo       ,\"Rome, IT\""),
           "CSV formatting aligns spaced-out separators without adding quotes");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvFormat, editor) &&
           document_is(editor, "id,name     ,city\n1 ,Alexandra,Oslo\n22,Bo       ,\"Rome, IT\""),
           "formatting spaced-out CSV twice leaves it unchanged");
    set_document(editor, "  a ,  b  \n ccc,d   ");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(csvFormat, editor) && document_is(editor, "a  ,b\nccc,d"),
           "CSV formatting trims blanks around fields and at line ends");

    set_document(editor, "{\"z\":0,\"a\":[1,true,null]}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(jsonPretty, editor) &&
           document_is(editor, "{\n  \"z\": 0,\n  \"a\": [\n    1,\n    true,\n    null\n  ]\n}"),
           "bundled JSON plugin pretty-prints the whole document");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(jsonMinify, editor) && document_is(editor, "{\"z\":0,\"a\":[1,true,null]}"),
           "bundled JSON plugin minifies a selection");

    set_document(editor, "{\"z\":0,\"a\":{\"z\":1,\"a\":2}}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(jsonSort, editor) &&
           document_is(editor, "{\n  \"a\": {\n    \"a\": 2,\n    \"z\": 1\n  },\n  \"z\": 0\n}"),
           "bundled JSON plugin recursively sorts object keys");

    set_document(editor, "{\"name\":\"MarkEdit\",\"enabled\":true,\"items\":[1,{\"value\":null}]}");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(jsonToYaml, editor) &&
           document_is(editor, "\"name\": \"MarkEdit\"\n\"enabled\": true\n\"items\":\n  - 1\n  -\n    \"value\": null"),
           "bundled JSON plugin converts JSON to YAML");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(yamlToJson, editor) &&
           document_is(editor, "{\n  \"name\": \"MarkEdit\",\n  \"enabled\": true,\n  \"items\": [\n    1,\n    {\n      \"value\": null\n    }\n  ]\n}"),
           "bundled JSON plugin converts its YAML output back to JSON");

    set_document(editor, "name: Example # comment\nnote: 'it''s # data'\nports:\n  - 80\n  - 443\nowner:\n  first: Ada\n  active: TRUE");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    expect(run_ok(yamlToJson, editor) &&
           document_is(editor, "{\n  \"name\": \"Example\",\n  \"note\": \"it's # data\",\n  \"ports\": [\n    80,\n    443\n  ],\n  \"owner\": {\n    \"first\": \"Ada\",\n    \"active\": true\n  }\n}"),
           "bundled JSON plugin reads common YAML mappings, sequences, comments and scalars");

    set_document(editor, "[1,]");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(jsonValidate, editor) && document_is(editor, "[1,]") &&
           strstr(s_lastMessage, "line 1, column 4") && strstr(s_lastMessage, "Trailing commas"),
           "bundled JSON plugin reports the precise validation error without changing text");
    expect(SendMessageA(editor, SCI_INDICATORVALUEAT, 17, 3) != 0,
           "bundled JSON plugin highlights the invalid token");

    const char *invalidJson[] = { "[1 2]", "{\"a\" 1}", "{\"a\":}", "[truefalse]",
        "[01]", "[1.]", "[1e+]", "[+1]", "[NaN]", "[undefined]", "[,1]",
        "{1:2}", "{\"a\":1 \"b\":2}", "[\"\\q\"]", "[\"\\u12xz\"]",
        "[\"a\nb\"]", "{}{}", "[1}", "true false", "[\"unterminated]" };
    int invalidJsonCount = (int)(sizeof(invalidJson) / sizeof(invalidJson[0]));
    int invalidJsonRejected = 1;
    for (int i = 0; i < invalidJsonCount; i++) {
        set_document(editor, invalidJson[i]);
        SendMessageA(editor, SCI_SETSEL, 0, 0);
        s_lastMessage[0] = 0;
        invalidJsonRejected = invalidJsonRejected && run_ok(jsonValidate, editor) &&
            strstr(s_lastMessage, "Invalid JSON") != NULL && document_is(editor, invalidJson[i]);
    }
    expect(invalidJsonRejected, "bundled JSON plugin rejects malformed JSON forms");

    set_document(editor, "before\n```json\n{\"a\":1}\n```\nafter");
    SendMessageA(editor, SCI_SETSEL, 18, 18);
    expect(run_ok(jsonPretty, editor) &&
           document_is(editor, "before\n```json\n{\n  \"a\": 1\n}\n```\nafter"),
           "bundled JSON plugin formats the fenced block at the caret");

    set_document(editor, "line 1\n\"line 2\"");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(jsonEscape, editor) && document_is(editor, "\"line 1\\n\\\"line 2\\\"\""),
           "bundled JSON plugin escapes a selection");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(jsonUnescape, editor) && document_is(editor, "line 1\n\"line 2\""),
           "bundled JSON plugin unescapes a JSON string");

    set_document(editor, "Hello, World!");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(rot13, editor) && document_is(editor, "Uryyb, Jbeyq!"),
           "bundled ROT13 transform");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(rot13, editor) && document_is(editor, "Hello, World!"),
           "bundled ROT13 transform is reversible");

    set_document(editor, "Abc-Z");
    SendMessageA(editor, SCI_SETSEL, 0, 5);
    expect(run_ok(atbash, editor) && document_is(editor, "Zyx-A"),
           "bundled Atbash transform preserves case and punctuation");

    set_document(editor, "SOS 2");
    SendMessageA(editor, SCI_SETSEL, 0, 5);
    expect(run_ok(encodeMorse, editor) && document_is(editor, "... --- ... / ..---"),
           "bundled Morse encoder");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(decodeMorse, editor) && document_is(editor, "SOS 2"),
           "bundled Morse decoder");

    set_document(editor, "Hi 5");
    SendMessageA(editor, SCI_SETSEL, 0, 4);
    expect(run_ok(encodeNato, editor) && document_is(editor, "Hotel India / Five"),
           "bundled NATO phonetics encoder");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(decodeNato, editor) && document_is(editor, "HI 5"),
           "bundled NATO phonetics decoder");

    set_document(editor, "Hi!");
    SendMessageA(editor, SCI_SETSEL, 0, 3);
    expect(run_ok(encodeBinary, editor) &&
           document_is(editor, "01001000 01101001 00100001"), "bundled binary encoder");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(decodeBinary, editor) && document_is(editor, "Hi!"),
           "bundled binary decoder");

    set_document(editor, "Hello, apple and rhythm!");
    SendMessageA(editor, SCI_SETSEL, 0, SendMessageA(editor, SCI_GETLENGTH, 0, 0));
    expect(run_ok(pigLatin, editor) && document_is(editor, "Ellohay, appleway andway rhythmay!"),
           "bundled Pig Latin transform");

    set_document(editor, "Bright sparks dance! Bright ideas sing! Bright colours shine!");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(personality, editor) && strstr(s_lastMessage, "Document mood: Enthusiastic") &&
           strstr(s_lastMessage, "Favourite word: bright (3 times)"),
           "bundled document personality reports mood and favourite word");
    expect(run_ok(clearPersonality, editor), "bundled document personality clears highlights");

    set_document(editor, "Short sentences work. Clear prose helps.");
    SendMessageA(editor, SCI_SETSEL, 0, 0);
    s_lastMessage[0] = 0;
    expect(run_ok(checkConstraints, editor) &&
           strstr(s_lastMessage, "All active constraints are satisfied."),
           "bundled constraint writer checks its default sentence limit");
    set_document(editor,
        "one two three four five six seven eight nine ten eleven twelve thirteen fourteen "
        "fifteen sixteen seventeen eighteen nineteen twenty twentyone.");
    s_lastMessage[0] = 0;
    expect(run_ok(checkConstraints, editor) &&
           strstr(s_lastMessage, "Sentence limit (20 words): 1 too long"),
           "bundled constraint writer reports a long sentence");
    expect(run_ok(highlightConstraints, editor) && run_ok(clearConstraints, editor),
           "bundled constraint writer manages issue highlights");

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
        .prompt = fake_prompt, .choose = fake_choose, .file_path = fake_file_path,
        .language = fake_language, .message = fake_message, .config_get = fake_config_get,
        .config_set = fake_config_set, .save_binary = fake_save_binary,
        .schedule_deferred = fake_schedule_deferred
    };
    plugin_host_set_callbacks(&fakes);
    test_commands(editor);
    test_separators(editor);
    test_editor_api(editor);
    test_checkboxes_and_config(editor);
    test_ui(editor);
    test_deferred(editor);
    test_events(editor);
    test_load_failures();
    test_environment();
    test_isolation(editor);
    test_time_limit();
    test_directory(editor);
    test_bundled_plugins(editor);
    plugin_host_shutdown();

    DestroyWindow(editor);
    FreeLibrary(scintilla);
    if (s_ok) puts("All plugin host tests passed.");
    return s_ok ? 0 : 1;
}
