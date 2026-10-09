#include <stdio.h>
#include <string.h>
#include "../src/editor_commands.h"

static int check_indentation(HWND editor, const char *input, int toSpaces, const char *expected) {
    char text[256];
    SendMessageA(editor, SCI_SETTEXT, 0, (LPARAM)input);
    SendMessageA(editor, SCI_EMPTYUNDOBUFFER, 0, 0);
    convert_indentation(editor, toSpaces);
    SendMessageA(editor, SCI_GETTEXT, sizeof(text), (LPARAM)text);
    if (strcmp(text, expected)) {
        fprintf(stderr, "Incorrect indentation conversion of \"%s\": \"%s\"\n", input, text);
        return 0;
    }
    SendMessageA(editor, SCI_UNDO, 0, 0);
    SendMessageA(editor, SCI_GETTEXT, sizeof(text), (LPARAM)text);
    if (strcmp(text, input) || SendMessageA(editor, SCI_CANUNDO, 0, 0)) {
        fprintf(stderr, "Indentation conversion was not one undo step\n");
        return 0;
    }
    return 1;
}

static int check_brace(HWND editor, const char *input, Sci_Position caret, Sci_Position expected) {
    SendMessageA(editor, SCI_SETTEXT, 0, (LPARAM)input);
    SendMessageA(editor, SCI_GOTOPOS, (WPARAM)caret, 0);
    jump_to_matching_brace(editor);
    Sci_Position landed = (Sci_Position)SendMessageA(editor, SCI_GETCURRENTPOS, 0, 0);
    if (landed != expected) {
        fprintf(stderr, "Brace match in \"%s\" from %d landed at %d\n", input, (int)caret, (int)landed);
        return 0;
    }
    return 1;
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
    SendMessageA(editor, SCI_SETTABWIDTH, 4, 0);
    int ok = 1;

    /* Only leading whitespace changes; a tab advances to the next tab stop. */
    ok &= check_indentation(editor, "\tx\n  \ty\n", 1, "    x\n    y\n");
    ok &= check_indentation(editor, "      x\n\t\ty", 0, "\t  x\n\t\ty");
    ok &= check_indentation(editor, "a\tb\n  \"\t\"", 1, "a\tb\n  \"\t\"");
    ok &= check_indentation(editor, "        deep", 0, "\t\tdeep");

    /* The brace just before the caret is preferred, then the one after it. */
    ok &= check_brace(editor, "f(a[b])", 2, 6);
    ok &= check_brace(editor, "f(a[b])", 3, 5);
    ok &= check_brace(editor, "f(a[b])", 7, 1);
    ok &= check_brace(editor, "no braces", 3, 3);

    DestroyWindow(editor);
    FreeLibrary(scintilla);
    if (ok) puts("All editor command tests passed.");
    return ok ? 0 : 1;
}
