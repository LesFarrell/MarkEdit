#ifndef EDITOR_LANGUAGE_H
#define EDITOR_LANGUAGE_H

#include <windows.h>

typedef enum {
    LANG_MARKDOWN = 0, LANG_JSON, LANG_XML, LANG_HTML, LANG_YAML,
    LANG_CPP, LANG_PYTHON, LANG_BASH, LANG_POWERSHELL, LANG_LUA, LANG_CSS, LANG_SQL, LANG_INI,
    LANG_TOML, LANG_BATCH, LANG_MAKEFILE, LANG_TEXT, LANGUAGE_COUNT
} Language;

typedef struct LanguageInfo {
    Language id;
    const char *lexer;
    const WCHAR *extensions; /* semicolon-delimited, each dot-prefixed */
    const char *keywords;    /* primary keyword list, NULL when the lexer needs none */
    const char *fenceTag;    /* Markdown info-string; lexer names are not always valid ones */
    int folds;               /* the vendored Markdown lexer emits no fold levels at all */
    const WCHAR *fileNames;  /* semicolon-delimited whole names, such as "Makefile", or NULL */
} LanguageInfo;

/* Indexed by Language; shared by lexing, previews, and workspace filtering. */
extern const LanguageInfo LANGUAGES[LANGUAGE_COUNT];

int extension_in_list(const WCHAR *extension, const WCHAR *list);
/* The language whose extension or file name matches path, or NULL. */
const LanguageInfo *language_for_known_path(const WCHAR *path);
const LanguageInfo *language_for_path(const WCHAR *path);
/* Apply after SCI_STYLECLEARALL; does not change the document's lexer. */
void apply_language_styles(HWND hEdit, Language language, int darkMode);

#endif
