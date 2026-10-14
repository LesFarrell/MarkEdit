#include <stdio.h>
#include "../src/editor_language.h"

static int check_language(const WCHAR *path, Language expected) {
    Language actual = language_for_path(path)->id;
    if (actual != expected) {
        fprintf(stderr, "%ls mapped to language %d, expected %d\n", path ? path : L"(untitled)",
                (int)actual, (int)expected);
        return 0;
    }
    return 1;
}

int main(void) {
    int ok = 1;
    ok &= check_language(NULL, LANG_TEXT);
    ok &= check_language(L"notes.md", LANG_MARKDOWN);
    ok &= check_language(L"notes.txt", LANG_TEXT);
    /* Logs are plain text: the Markdown fallback restyled indented log
       lines as code blocks in a different font. */
    ok &= check_language(L"backend.log", LANG_TEXT);
    ok &= check_language(L"C:\\Logs\\SERIAL.LOG", LANG_TEXT);
    ok &= check_language(L"README", LANG_MARKDOWN);
    ok &= check_language(L"notes.unknown", LANG_MARKDOWN);
    ok &= check_language(L"main.c", LANG_CPP);
    if (ok) puts("All language tests passed.");
    return ok ? 0 : 1;
}
