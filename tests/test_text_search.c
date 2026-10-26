#include <stdio.h>
#include <string.h>
#include "../src/text_search.h"

static int s_ok = 1;

static void expect(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", what);
        s_ok = 0;
    }
}

static void set_text(HWND editor, const char *text, Sci_Position selStart, Sci_Position selEnd) {
    SendMessageA(editor, SCI_SETTEXT, 0, (LPARAM)text);
    SendMessageA(editor, SCI_EMPTYUNDOBUFFER, 0, 0);
    SendMessageA(editor, SCI_SETSEL, (WPARAM)selStart, (LPARAM)selEnd);
}

static int text_is(HWND editor, const char *expected) {
    char text[256];
    SendMessageA(editor, SCI_GETTEXT, sizeof(text), (LPARAM)text);
    if (strcmp(text, expected) == 0) return 1;
    fprintf(stderr, "Text is \"%s\", expected \"%s\"\n", text, expected);
    return 0;
}

static int selection_is(HWND editor, Sci_Position start, Sci_Position end) {
    return SendMessageA(editor, SCI_GETSELECTIONSTART, 0, 0) == start &&
           SendMessageA(editor, SCI_GETSELECTIONEND, 0, 0) == end;
}

static Sci_Position length_of(HWND editor) {
    return (Sci_Position)SendMessageA(editor, SCI_GETLENGTH, 0, 0);
}

static void test_find(HWND editor) {
    set_text(editor, "cat Cat cat", 0, 0);
    expect(search_find(editor, "cat", 0, 1) == SEARCH_FOUND && selection_is(editor, 0, 3),
           "finds the first match after the caret");
    expect(search_find(editor, "cat", 0, 1) == SEARCH_FOUND && selection_is(editor, 4, 7),
           "ignores case unless asked");
    expect(search_find(editor, "cat", SEARCH_MATCH_CASE, 1) == SEARCH_FOUND && selection_is(editor, 8, 11),
           "match case skips other cases");
    expect(search_find(editor, "cat", SEARCH_MATCH_CASE, 1) == SEARCH_WRAPPED && selection_is(editor, 0, 3),
           "wraps to the start, saying so");
    expect(search_find(editor, "cat", SEARCH_MATCH_CASE, 0) == SEARCH_WRAPPED && selection_is(editor, 8, 11),
           "finds backwards, wrapping to the end");
    expect(search_find(editor, "cat", SEARCH_MATCH_CASE, 0) == SEARCH_FOUND && selection_is(editor, 0, 3),
           "finds the previous match");
    expect(search_find(editor, "dog", 0, 1) == SEARCH_NOT_FOUND && selection_is(editor, 0, 3),
           "leaves the selection alone when nothing matches");

    set_text(editor, "concat cat", 0, 0);
    expect(search_find(editor, "cat", SEARCH_WHOLE_WORD, 1) == SEARCH_FOUND && selection_is(editor, 7, 10),
           "whole word skips matches inside words");
    set_text(editor, "concat cot", 0, 0);
    expect(search_find(editor, "c.t", SEARCH_REGEX | SEARCH_WHOLE_WORD, 1) == SEARCH_FOUND &&
           selection_is(editor, 7, 10),
           "whole word applies to regular expressions");
    expect(search_find(editor, "c[ao]t", SEARCH_REGEX, 1) == SEARCH_WRAPPED && selection_is(editor, 3, 6),
           "finds regular expressions");
    expect(search_find(editor, "(", SEARCH_REGEX, 1) == SEARCH_BAD_PATTERN, "reports a bad regular expression");
    expect(search_find(editor, "(", 0, 1) == SEARCH_NOT_FOUND, "plain text isn't a regular expression");

    set_text(editor, "a\nb\nc", 0, 0);
    expect(search_find(editor, "^", SEARCH_REGEX, 1) == SEARCH_FOUND && selection_is(editor, 2, 2),
           "steps past an empty match at the caret");
    expect(search_find(editor, "^", SEARCH_REGEX, 1) == SEARCH_FOUND && selection_is(editor, 4, 4),
           "keeps moving through empty matches");

    set_text(editor, "caf\xC3\xA9 \xC3\xA9t\xC3\xA9", 0, 0);
    expect(search_find(editor, "\xC3\xA9t", 0, 1) == SEARCH_FOUND && selection_is(editor, 6, 9),
           "finds UTF-8 text by its bytes");
}

static void test_count(HWND editor) {
    set_text(editor, "banana", 0, 0);
    expect(search_count(editor, "a", 0, 0, length_of(editor)) == 3, "counts every match");
    expect(search_count(editor, "ana", 0, 0, length_of(editor)) == 1, "counts matches without overlapping");
    expect(search_count(editor, "a", 0, 2, 5) == 1 && search_count(editor, "a", 0, 2, 6) == 2,
           "counts only matches wholly within a range");
    set_text(editor, "a\nb\nc", 0, 0);
    expect(search_count(editor, "^", SEARCH_REGEX, 0, length_of(editor)) == 3,
           "counts each empty match once");
    expect(search_count(editor, "[", SEARCH_REGEX, 0, length_of(editor)) == SEARCH_BAD_PATTERN,
           "count reports a bad regular expression");
}

static void test_replace(HWND editor) {
    set_text(editor, "cat cat", 0, 3);
    expect(search_replace_selection(editor, "cat", "dog", 0) == 1 && text_is(editor, "dog cat") &&
           selection_is(editor, 3, 3),
           "replaces a selection that is a match, leaving the caret after it");
    set_text(editor, "cat cat", 0, 2);
    expect(search_replace_selection(editor, "cat", "dog", 0) == 0 && text_is(editor, "cat cat"),
           "leaves a selection that isn't a whole match");
    set_text(editor, "x=12", 2, 4);
    expect(search_replace_selection(editor, "(\\d)(\\d)", "\\2\\1", SEARCH_REGEX) == 1 &&
           text_is(editor, "x=21"),
           "replacing a regular expression match expands groups");

    set_text(editor, "banana", 0, 0);
    Sci_Position end = length_of(editor);
    expect(search_replace_all(editor, "a", "aa", 0, 0, &end) == 3 && text_is(editor, "baanaanaa") && end == 9,
           "replaces every match once, even when the replacement contains it");
    SendMessageA(editor, SCI_UNDO, 0, 0);
    expect(text_is(editor, "banana") && !SendMessageA(editor, SCI_CANUNDO, 0, 0),
           "replace all is one undo step");

    set_text(editor, "ann@home bob@work", 0, 0);
    end = length_of(editor);
    expect(search_replace_all(editor, "(\\w+)@(\\w+)", "\\2:\\1", SEARCH_REGEX, 0, &end) == 2 &&
           text_is(editor, "home:ann work:bob"),
           "replace all expands regular expression groups");

    set_text(editor, "a\nb", 0, 0);
    end = length_of(editor);
    expect(search_replace_all(editor, "^", "> ", SEARCH_REGEX, 0, &end) == 2 && text_is(editor, "> a\n> b"),
           "replace all handles empty matches");

    set_text(editor, "a a a a", 0, 0);
    end = 5;
    expect(search_replace_all(editor, "a", "b", 0, 2, &end) == 2 && text_is(editor, "a b b a") && end == 5,
           "replace all stays within its range");

    set_text(editor, "cat", 0, 0);
    end = length_of(editor);
    expect(search_replace_all(editor, ")", "x", SEARCH_REGEX, 0, &end) == SEARCH_BAD_PATTERN &&
           text_is(editor, "cat"),
           "replace all reports a bad regular expression and changes nothing");
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
    SendMessageA(editor, SCI_SETCODEPAGE, SC_CP_UTF8, 0);

    test_find(editor);
    test_count(editor);
    test_replace(editor);

    DestroyWindow(editor);
    FreeLibrary(scintilla);
    if (s_ok) printf("All text search tests passed.\n");
    return s_ok ? 0 : 1;
}
