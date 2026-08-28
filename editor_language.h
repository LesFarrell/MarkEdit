#ifndef EDITOR_LANGUAGE_H
#define EDITOR_LANGUAGE_H

#include <windows.h>

typedef enum EditorLanguage {
    LANG_MARKDOWN = 0,
    LANG_JSON,
    LANG_XML,
    LANG_HTML,
    LANG_YAML,
    LANG_CPP,
    LANG_PYTHON,
    LANG_BASH,
    LANG_POWERSHELL,
    LANG_LUA
} EditorLanguage;

typedef struct EditorLanguageInfo {
    EditorLanguage id;
    const char *lexer;
    const WCHAR *extensions;
    const char *keywords;
    const char *fenceTag;
    int folds;
} EditorLanguageInfo;

const EditorLanguageInfo *editor_language_for_path(const WCHAR *path);

#endif
