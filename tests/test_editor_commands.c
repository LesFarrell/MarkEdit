#include <stdio.h>
#include <string.h>
#include "../editor_commands.h"

static int check_transform(HWND editor, const char *input, const char *expected,
                           int mode, const char *eol) {
    char text[256];
    SendMessageA(editor, SCI_SETTEXT, 0, (LPARAM)input);
    SendMessageA(editor, SCI_EMPTYUNDOBUFFER, 0, 0);
    SendMessageA(editor, SCI_SETSEL, 0, (LPARAM)strlen(input));
    transform_selected_lines(editor, mode, eol);
    SendMessageA(editor, SCI_GETTEXT, sizeof(text), (LPARAM)text);
    if (strcmp(text, expected)) {
        fprintf(stderr, "Incorrect transform for mode %d\n", mode);
        return 0;
    }
    SendMessageA(editor, SCI_UNDO, 0, 0);
    SendMessageA(editor, SCI_GETTEXT, sizeof(text), (LPARAM)text);
    if (strcmp(text, input) || SendMessageA(editor, SCI_CANUNDO, 0, 0)) {
        fprintf(stderr, "Transform was not one undo step\n");
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
    int ok = 1;
    const char *endings[] = { "\r", "\n", "\r\n" };
    for (int i = 0; i < 3; i++) {
        char input[128], ascending[128], descending[128], unique[128];
        const char *eol = endings[i];
        snprintf(input, sizeof(input), "b%sa%sb%s", eol, eol, eol);
        snprintf(ascending, sizeof(ascending), "a%sb%sb%s", eol, eol, eol);
        snprintf(descending, sizeof(descending), "b%sb%sa%s", eol, eol, eol);
        snprintf(unique, sizeof(unique), "b%sa%s", eol, eol);
        ok &= check_transform(editor, input, ascending, 0, eol);
        ok &= check_transform(editor, input, descending, 1, eol);
        ok &= check_transform(editor, input, unique, 2, eol);
    }
    ok &= check_transform(editor, "c\rb\na\r\n", "a\nb\nc\r\n", 0, "\n");
    DestroyWindow(editor);
    FreeLibrary(scintilla);
    if (ok) puts("All editor command tests passed.");
    return ok ? 0 : 1;
}
