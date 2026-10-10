/* src/lua/formatting_*.lua: the Markdown, HTML and JSON formatting commands. Most
   of their behaviour was checked against the C versions they replaced; this
   keeps the JSON checks those had, and that every command is registered. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/menu_commands.h"
#include "../src/plugin_host.h"
#include "../src/file_io.h"
#include "../third_party/scintilla/include/Scintilla.h"

static int s_ok = 1;
static char s_lastMessage[256];

static char *read_formatting_source(void) {
    static const WCHAR *const paths[] = {
        L"src\\lua\\formatting_base.lua", L"src\\lua\\formatting_json.lua",
        L"src\\lua\\formatting_tables.lua", L"src\\lua\\formatting_text.lua",
    };
    char *joined = NULL;
    size_t used = 0;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        char *part = read_utf8_text_file(paths[i], 1024 * 1024);
        if (!part) { free(joined); return NULL; }
        size_t length = strlen(part);
        char *grown = (char *)realloc(joined, used + length + 2);
        if (!grown) { free(part); free(joined); return NULL; }
        joined = grown;
        memcpy(joined + used, part, length);
        used += length;
        joined[used++] = '\n';
        joined[used] = 0;
        free(part);
    }
    return joined;
}

static void expect(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", what);
        s_ok = 0;
    }
}

/* Answers for ui.prompt, in order; NULL cancels. */
static const char *s_answers[4];
static int s_answerCount, s_nextAnswer;

static char *fake_prompt(const char *title, const char *label, const char *initial) {
    (void)title;
    (void)label;
    (void)initial;
    if (s_nextAnswer >= s_answerCount || !s_answers[s_nextAnswer]) return NULL;
    const char *answer = s_answers[s_nextAnswer++];
    char *copy = (char *)malloc(strlen(answer) + 1);
    if (copy) strcpy(copy, answer);
    return copy;
}

static void answer_with(const char *rows, const char *columns) {
    s_answers[0] = rows;
    s_answers[1] = columns;
    s_answerCount = 2;
    s_nextAnswer = 0;
}

static void record_message(const char *title, const char *text, const char *kind) {
    (void)title;
    (void)kind;
    snprintf(s_lastMessage, sizeof(s_lastMessage), "%s", text);
}

/* Runs a built-in command by name on the whole of text (selected), and
   returns the document afterwards. */
static const char *run_with_selection(HWND editor, const char *name, const char *text,
                                      size_t length, Sci_Position start,
                                      Sci_Position finish, int eolMode) {
    static char result[8192];
    s_lastMessage[0] = 0;
    SendMessageA(editor, SCI_SETEOLMODE, (WPARAM)eolMode, 0);
    SendMessageA(editor, SCI_CLEARALL, 0, 0);
    SendMessageA(editor, SCI_ADDTEXT, (WPARAM)length, (LPARAM)text);
    SendMessageA(editor, SCI_SETSEL, (WPARAM)start, (LPARAM)finish);
    int index = plugin_host_find_override(name);
    char *error = index >= 0 ? plugin_host_run(index, editor) : NULL;
    if (error) fprintf(stderr, "%s: %s\n", name, error);
    expect(index >= 0 && !error, name);
    free(error);
    SendMessageA(editor, SCI_GETTEXT, sizeof(result), (LPARAM)result);
    return result;
}

static const char *run(HWND editor, const char *name, const char *text, size_t length, int eolMode) {
    return run_with_selection(editor, name, text, length, 0, (Sci_Position)length, eolMode);
}

static const char *run_at(HWND editor, const char *name, const char *text,
                          Sci_Position caret, int eolMode) {
    return run_with_selection(editor, name, text, strlen(text), caret, caret, eolMode);
}

static void test_registration(void) {
    static const char *const textCommands[] = { "edit.title_case", "edit.sort_ascending",
        "edit.sort_descending", "edit.remove_duplicate_lines", "edit.trim_whitespace", "edit.toggle_comment" };
    for (int i = 0; menu_command_at(i); i++) {
        const char *name = menu_command_at(i)->name;
        int inLua = strncmp(name, "markdown.", 9) == 0 || strncmp(name, "html.", 5) == 0 ||
                    strncmp(name, "json.", 5) == 0;
        for (size_t t = 0; t < sizeof(textCommands) / sizeof(textCommands[0]); t++)
            if (strcmp(name, textCommands[t]) == 0) inLua = 1;
        if (inLua && plugin_host_find_override(name) < 0) fprintf(stderr, "Not registered: %s\n", name);
        if (inLua) expect(plugin_host_find_override(name) >= 0, "every formatting and text command is in Lua");
    }
}

static void test_json(HWND editor) {
    const char *invalid[] = { "[1 2]", "{\"a\" 1}", "{\"a\":}", "[truefalse]",
        "[01]", "[1.]", "[1e+]", "[+1]", "[NaN]", "[undefined]", "[1,]", "[,1]",
        "{1:2}", "{\"a\":1 \"b\":2}", "[\"\\q\"]", "[\"\\u12xz\"]", "[\"a\nb\"]",
        "{}{}", "[1}", "true false", "[\"unterminated]", "[\"\xFF\"]" };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        for (int pretty = 0; pretty < 2; pretty++) {
            const char *after = run(editor, pretty ? "json.format" : "json.minify", invalid[i], strlen(invalid[i]), SC_EOL_LF);
            if (strcmp(after, invalid[i]) != 0) fprintf(stderr, "Changed: %s\n", invalid[i]);
            expect(strcmp(after, invalid[i]) == 0 && strstr(s_lastMessage, "could not be read as JSON"),
                   "invalid JSON is reported and left alone");
        }
    }
    const char *valid[] = { "null", "true", "false", "-0.25E+3", "\"hello\\u1234\"",
        "{}", "[]", "{\"a\":[1,true,null,{\"b\":\"\\\"\"}]}" };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        char pretty[512];
        snprintf(pretty, sizeof(pretty), "%s", run(editor, "json.format", valid[i], strlen(valid[i]), SC_EOL_CRLF));
        const char *compact = run(editor, "json.minify", pretty, strlen(pretty), SC_EOL_LF);
        if (strcmp(compact, valid[i]) != 0) fprintf(stderr, "Round trip: %s -> %s -> %s\n", valid[i], pretty, compact);
        expect(strcmp(compact, valid[i]) == 0, "pretty then minified JSON is the original");
    }
    expect(strcmp(run(editor, "json.format", "{\"a\":[1,{}]}", 12, SC_EOL_LF), "{\n  \"a\": [\n    1,\n    {}\n  ]\n}") == 0,
           "pretty layout");

    const char *after = run(editor, "json.minify", "[1\0]", 4, SC_EOL_LF);
    expect(strstr(s_lastMessage, "could not be read as JSON") != NULL && after[0] == '[', "a NUL byte is invalid");

    char deep[600];
    memset(deep, '[', 257);
    memset(deep + 257, ']', 257);
    deep[514] = 0;
    run(editor, "json.minify", deep, 514, SC_EOL_LF);
    expect(strstr(s_lastMessage, "could not be read as JSON") != NULL, "257 levels deep is too deep");
    memset(deep, '[', 256);
    memset(deep + 256, ']', 256);
    deep[512] = 0;
    run(editor, "json.minify", deep, 512, SC_EOL_LF);
    expect(s_lastMessage[0] == 0, "256 levels deep is fine");
}

static void test_json_fences(HWND editor) {
    const char *inside = "```json\n{\"a\":1}\n```\n";
    const char *insideExpected = "```json\n{\n  \"a\": 1\n}\n```\n";
    expect(strcmp(run_at(editor, "json.format", inside,
                         (Sci_Position)(strstr(inside, "{\"") - inside), SC_EOL_LF),
                  insideExpected) == 0,
           "JSON inside a matched fence is formatted");

    const char *between = "```json\n{}\n```\n{\"a\":1}\n```json\n[]\n```\n";
    expect(strcmp(run_at(editor, "json.format", between,
                         (Sci_Position)(strstr(between, "{\"a\"") - between), SC_EOL_LF),
                  between) == 0 && strstr(s_lastMessage, "inside a fenced code block"),
           "a closing fence is not mistaken for a new opening fence");

    const char *mismatched = "```json\n{\"a\":1}\n~~~\n";
    expect(strcmp(run_at(editor, "json.format", mismatched,
                         (Sci_Position)(strstr(mismatched, "{\"") - mismatched), SC_EOL_LF),
                  mismatched) == 0 && strstr(s_lastMessage, "inside a fenced code block"),
           "a different fence marker does not close a block");

    const char *badInfo = "```bad`info\n{\"a\":1}\n```\n";
    expect(strcmp(run_at(editor, "json.format", badInfo,
                         (Sci_Position)(strstr(badInfo, "{\"") - badInfo), SC_EOL_LF),
                  badInfo) == 0 && strstr(s_lastMessage, "inside a fenced code block"),
           "a backtick fence with a backtick in its info string is not an opener");
}

static void test_table_commands(HWND editor) {
    const char *prose = "a | b\nc | d";
    expect(strcmp(run_at(editor, "markdown.table_column_delete", prose, 0, SC_EOL_LF), prose) == 0 &&
           strstr(s_lastMessage, "inside a Markdown table"),
           "pipe-containing prose is not treated as a table");

    const char *unequal = "a | b | c\n--- | ---\n1 | 2";
    expect(strcmp(run_at(editor, "markdown.table_column_delete", unequal, 0, SC_EOL_LF), unequal) == 0 &&
           strstr(s_lastMessage, "inside a Markdown table"),
           "a delimiter with the wrong number of columns does not form a table");

    const char *table = "| a | b |\n| --- | --- |\n| 1 | 2 |";
    expect(strcmp(run_at(editor, "markdown.table_column_delete", table,
                         (Sci_Position)(strstr(table, "2 |") - table), SC_EOL_LF),
                  "| a |\n| --- |\n| 1 |") == 0,
           "a table with a delimiter row is recognised");

    const char *escaped = "| a \\| b | c |\n| --- | --- |\n| x | y |";
    expect(strcmp(run_at(editor, "markdown.table_column_delete", escaped,
                         (Sci_Position)(strstr(escaped, "c |") - escaped), SC_EOL_LF),
                  "| a \\| b |\n| --- |\n| x |") == 0,
           "escaped pipes remain cell text in a Markdown table");

    run_at(editor, "markdown.table_row_below", table,
           (Sci_Position)(strstr(table, "1 |") - table), SC_EOL_LF);
    expect(SendMessageA(editor, SCI_GETCURRENTPOS, 0, 0) == (LRESULT)strlen(table) + 1,
           "the caret enters an appended table row after its inserted line ending");
}

/* Runs name on the whole of input, checks the result, then that one Undo
   restores input. */
static void check_text_command(HWND editor, const char *name, const char *input, int eolMode,
                               const char *expected) {
    const char *after = run(editor, name, input, strlen(input), eolMode);
    if (strcmp(after, expected) != 0) fprintf(stderr, "%s on \"%s\" gave \"%s\"\n", name, input, after);
    expect(strcmp(after, expected) == 0, name);
    SendMessageA(editor, SCI_UNDO, 0, 0);
    char undone[512];
    SendMessageA(editor, SCI_GETTEXT, sizeof(undone), (LPARAM)undone);
    expect(strcmp(undone, input) == 0, "a text command is one undo step");
}

/* The Edit menu's text commands (these cases came from the C versions'
   tests: the selection ends at the start of the final empty line, which
   leaves that line out). */
static void test_text_commands(HWND editor) {
    const int modes[] = { SC_EOL_CR, SC_EOL_LF, SC_EOL_CRLF };
    const char *endings[] = { "\r", "\n", "\r\n" };
    for (int i = 0; i < 3; i++) {
        char input[64], ascending[64], descending[64], unique[64];
        const char *e = endings[i];
        snprintf(input, sizeof(input), "b%sa%sb%s", e, e, e);
        snprintf(ascending, sizeof(ascending), "a%sb%sb%s", e, e, e);
        snprintf(descending, sizeof(descending), "b%sb%sa%s", e, e, e);
        snprintf(unique, sizeof(unique), "b%sa%s", e, e);
        check_text_command(editor, "edit.sort_ascending", input, modes[i], ascending);
        check_text_command(editor, "edit.sort_descending", input, modes[i], descending);
        check_text_command(editor, "edit.remove_duplicate_lines", input, modes[i], unique);
    }
    check_text_command(editor, "edit.sort_ascending", "c\rb\na\r\n", SC_EOL_LF, "a\nb\nc\r\n");
    check_text_command(editor, "edit.sort_ascending", "b\nB\na", SC_EOL_LF, "B\na\nb");

    check_text_command(editor, "edit.title_case", "hello WORLD x1y", SC_EOL_LF, "Hello World X1Y");
    /* An apostrophe after a letter is part of the word; elsewhere it isn't. */
    check_text_command(editor, "edit.title_case", "it's DON'T o'clock", SC_EOL_LF, "It's Don't O'clock");
    check_text_command(editor, "edit.title_case", "won\xE2\x80\x99t stop", SC_EOL_LF, "Won\xE2\x80\x99t Stop");
    check_text_command(editor, "edit.title_case", "'twas rock 'n' roll", SC_EOL_LF, "'Twas Rock 'N' Roll");
    check_text_command(editor, "edit.title_case", "dogs' bowls", SC_EOL_LF, "Dogs' Bowls");
    check_text_command(editor, "edit.trim_whitespace", "a  \nb\t\n  \nc", SC_EOL_LF, "a\nb\n\nc");
    check_text_command(editor, "edit.toggle_comment", "a\nb", SC_EOL_LF, "<!--\na\nb\n-->");
    check_text_command(editor, "edit.toggle_comment", "<!--\na\nb\n-->", SC_EOL_LF, "a\nb");
    check_text_command(editor, "edit.toggle_comment", " <!-- \n\t-->", SC_EOL_CRLF, "");
}

/* Insert Table and Insert HTML Table ask for a size; the defaults give the
   tables they always inserted. */
static void test_insert_tables(HWND editor) {
    answer_with("2", "2");
    expect(strcmp(run(editor, "markdown.table", "", 0, SC_EOL_LF),
                  "| Column 1 | Column 2 |\n| --- | --- |\n| Cell | Cell |\n| Cell | Cell |\n") == 0,
           "Markdown table of the size asked for");
    answer_with("1", "3");
    expect(strcmp(run(editor, "markdown.table", "", 0, SC_EOL_CRLF),
                  "| Column 1 | Column 2 | Column 3 |\r\n| --- | --- | --- |\r\n| Cell | Cell | Cell |\r\n") == 0,
           "the default Markdown table is the one it always was");
    answer_with("1", "1");
    expect(strcmp(run(editor, "html.table", "", 0, SC_EOL_LF),
                  "<table>\n  <tr>\n    <th>Column 1</th>\n  </tr>\n  <tr>\n    <td>Cell</td>\n  </tr>\n</table>\n") == 0,
           "HTML table of the size asked for");
    answer_with("1", "3");
    expect(strcmp(run(editor, "html.table", "", 0, SC_EOL_LF),
                  "<table>\n  <tr>\n    <th>Column 1</th>\n    <th>Column 2</th>\n    <th>Column 3</th>\n  </tr>\n"
                  "  <tr>\n    <td>Cell</td>\n    <td>Cell</td>\n    <td>Cell</td>\n  </tr>\n</table>\n") == 0,
           "the default HTML table is the one it always was");
    answer_with("2", NULL);
    expect(strcmp(run(editor, "markdown.table", "", 0, SC_EOL_LF), "") == 0, "cancelling inserts nothing");
    answer_with("0", "3");
    expect(strcmp(run(editor, "html.table", "", 0, SC_EOL_LF), "") == 0 && strstr(s_lastMessage, "1–50"),
           "an out-of-range size is reported");
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

    static const PluginHostCallbacks callbacks = {
        fake_prompt, NULL, NULL, NULL, record_message, NULL, NULL
    };
    plugin_host_set_callbacks(&callbacks);
    char *code = read_formatting_source();
    char *error = code ? plugin_host_load_builtin("formatting.lua", code, strlen(code)) : NULL;
    if (error) fprintf(stderr, "%s\n", error);
    expect(code && !error, "built-in formatting Lua files load together");
    free(error);
    free(code);

    test_registration();
    test_json(editor);
    test_json_fences(editor);
    test_insert_tables(editor);
    test_table_commands(editor);
    test_text_commands(editor);
    plugin_host_shutdown();

    DestroyWindow(editor);
    FreeLibrary(scintilla);
    if (s_ok) puts("All formatting tests passed.");
    return s_ok ? 0 : 1;
}
