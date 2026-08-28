#include "editor_language.h"

#include <shlwapi.h>
#include <wchar.h>

static const EditorLanguageInfo LANGUAGES[] = {
    { LANG_MARKDOWN, "markdown", L".md;.markdown;.mdown;.mkd;.mdwn;.text", NULL, "markdown", 0 },
    { LANG_JSON, "json", L".json;.jsonc;.jsonl;.geojson;.webmanifest", "true false null", "json", 1 },
    { LANG_XML, "xml", L".xml;.xsd;.xsl;.xslt;.svg;.rss;.atom;.plist;.csproj;.props;.targets", NULL, "xml", 1 },
    { LANG_HTML, "hypertext", L".html;.htm;.xhtml", NULL, "html", 1 },
    { LANG_YAML, "yaml", L".yml;.yaml", "true false null yes no on off", "yaml", 1 },
    { LANG_CPP, "cpp", L".c;.h;.cc;.cpp;.cxx;.hpp;.hxx;.js;.mjs;.cjs;.ts;.tsx;.jsx;.cs;.java;.go;.rs;.php",
      "auto break case char const continue default do double else enum extern float for goto if"
      " inline int long register return short signed sizeof static struct switch typedef union"
      " unsigned void volatile while bool catch class const_cast delete dynamic_cast explicit"
      " export false friend mutable namespace new operator private protected public"
      " reinterpret_cast static_cast template this throw true try typeid typename using virtual"
      " nullptr constexpr decltype noexcept override final function var let await async"
      " import from interface type extends implements", "c", 1 },
    { LANG_PYTHON, "python", L".py;.pyw;.pyi",
      "and as assert async await break class continue def del elif else except finally for from"
      " global if import in is lambda nonlocal not or pass raise return try while with yield"
      " None True False self", "python", 1 },
    { LANG_BASH, "bash", L".sh;.bash;.zsh;.ksh;.bashrc;.profile",
      "if then else elif fi case esac for while until do done in function select time coproc"
      " break continue return exit export local readonly declare unset shift eval exec source",
      "bash", 1 },
    { LANG_POWERSHELL, "powershell", L".ps1;.psm1;.psd1",
      "begin break catch continue data do dynamicparam else elseif end exit filter finally for"
      " foreach from function if in local param process return switch throw trap try until"
      " where while", "powershell", 1 },
    { LANG_LUA, "lua", L".lua",
      "and break do else elseif end false for function goto if in local nil not or repeat return"
      " then true until while", "lua", 1 },
};

static int extension_in_list(const WCHAR *extension, const WCHAR *list) {
    size_t length = wcslen(extension);
    for (const WCHAR *entry = list; *entry;) {
        const WCHAR *end = wcschr(entry, L';');
        size_t entryLength = end ? (size_t)(end - entry) : wcslen(entry);
        if (entryLength == length && _wcsnicmp(entry, extension, length) == 0) return 1;
        if (!end) break;
        entry = end + 1;
    }
    return 0;
}

const EditorLanguageInfo *editor_language_for_path(const WCHAR *path) {
    if (path) {
        const WCHAR *extension = PathFindExtensionW(path);
        if (extension && *extension) {
            for (size_t i = 0; i < sizeof(LANGUAGES) / sizeof(LANGUAGES[0]); i++) {
                if (extension_in_list(extension, LANGUAGES[i].extensions)) return &LANGUAGES[i];
            }
        }
    }
    return &LANGUAGES[LANG_MARKDOWN];
}
