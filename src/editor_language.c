/* Language metadata, extension matching, and Scintilla syntax colours. */
#include "editor_language.h"

#include <shlwapi.h>
#include <wchar.h>

#include "theme.h"
#include "../third_party/scintilla/include/Scintilla.h"
#include "../third_party/lexilla/include/SciLexer.h"

const LanguageInfo LANGUAGES[LANGUAGE_COUNT] = {
    { LANG_MARKDOWN, "markdown", L".md;.markdown;.mdown;.mkd;.mdwn;.text", NULL, "markdown", 0, NULL },
    { LANG_JSON, "json", L".json;.jsonc;.jsonl;.geojson;.webmanifest", "true false null", "json", 1, NULL },
    { LANG_XML, "xml", L".xml;.xsd;.xsl;.xslt;.svg;.rss;.atom;.plist;.csproj;.props;.targets", NULL, "xml", 1, NULL },
    { LANG_HTML, "hypertext", L".html;.htm;.xhtml", NULL, "html", 1, NULL },
    { LANG_YAML, "yaml", L".yml;.yaml", "true false null yes no on off", "yaml", 1, NULL },
    { LANG_CPP, "cpp", L".c;.h;.cc;.cpp;.cxx;.hpp;.hxx;.js;.mjs;.cjs;.ts;.tsx;.jsx;.cs;.java;.go;.rs;.php",
      "auto break case char const continue default do double else enum extern float for goto if"
      " inline int long register return short signed sizeof static struct switch typedef union"
      " unsigned void volatile while bool catch class const_cast delete dynamic_cast explicit"
      " export false friend mutable namespace new operator private protected public"
      " reinterpret_cast static_cast template this throw true try typeid typename using virtual"
      " nullptr constexpr decltype noexcept override final function var let await async"
      " import from interface type extends implements", "c", 1, NULL },
    { LANG_PYTHON, "python", L".py;.pyw;.pyi",
      "and as assert async await break class continue def del elif else except finally for from"
      " global if import in is lambda nonlocal not or pass raise return try while with yield"
      " None True False self", "python", 1, NULL },
    { LANG_BASH, "bash", L".sh;.bash;.zsh;.ksh;.bashrc;.profile",
      "if then else elif fi case esac for while until do done in function select time coproc"
      " break continue return exit export local readonly declare unset shift eval exec source",
      "bash", 1, NULL },
    { LANG_POWERSHELL, "powershell", L".ps1;.psm1;.psd1",
      "begin break catch continue data do dynamicparam else elseif end exit filter finally for"
      " foreach from function if in local param process return switch throw trap try until"
      " where while", "powershell", 1, NULL },
    { LANG_LUA, "lua", L".lua",
      "and break do else elseif end false for function goto if in local nil not or repeat return"
      " then true until while", "lua", 1, NULL },
    /* Properties a CSS lexer doesn't know are styled like known ones (see
       apply_css_styles()), so it needs no property list. */
    { LANG_CSS, "css", L".css", NULL, "css", 1, NULL },
    /* LexSQL matches keywords in lower case, whatever case they're typed in. */
    { LANG_SQL, "sql", L".sql",
      "add all alter and any as asc begin between by case cascade check column commit constraint"
      " create cross database default delete desc distinct drop else end exec execute exists"
      " foreign from full function group having if in index inner insert into is join key left"
      " like limit not null offset on or order outer primary procedure references replace"
      " return returns right rollback select set table then top transaction trigger truncate"
      " union unique update use values view when where while with", "sql", 1, NULL },
    { LANG_INI, "props", L".ini;.cfg;.conf;.properties;.editorconfig;.gitconfig", NULL, "ini", 1, NULL },
    { LANG_TOML, "toml", L".toml", "true false inf nan", "toml", 1, NULL },
    /* LexBatch matches commands in lower case, whatever case they're typed in. */
    { LANG_BATCH, "batch", L".bat;.cmd",
      "assoc break call cd chdir cls color copy date defined del dir do echo else endlocal equ"
      " erase errorlevel exist exit for ftype geq goto gtr if in leq lss md mkdir mklink move neq"
      " not path pause popd prompt pushd rd rem ren rename rmdir set setlocal shift start time"
      " title type ver verify vol", "plaintext", 0, NULL },
    { LANG_MAKEFILE, "makefile", L".mk;.mak",
      "define else endef endif export ifdef ifeq ifndef ifneq include override private"
      " sinclude undefine unexport vpath", "makefile", 0, L"Makefile;GNUmakefile" },
    /* "text" deliberately names no compiled-in lexer (see lexilla-sources.rsp) -
       CreateLexer() returns NULL for it, which apply_document_language() passes
       to SCI_SETILEXER to clear any previous lexer: genuinely
       unstyled plain text rather than a lexer that happens to add no colour. */
    { LANG_TEXT, "text", L".txt;.log", NULL, "text", 0, NULL },
};

/* Case-insensitive match of `extension` against one semicolon-delimited list. */
int extension_in_list(const WCHAR *extension, const WCHAR *list) {
    size_t length = wcslen(extension);
    for (const WCHAR *entry = list; *entry; ) {
        const WCHAR *end = wcschr(entry, L';');
        size_t entryLength = end ? (size_t)(end - entry) : wcslen(entry);
        if (entryLength == length && _wcsnicmp(entry, extension, length) == 0) return 1;
        if (!end) break;
        entry = end + 1;
    }
    return 0;
}

/* Picks the language for a document path. A document with no path yet (a
   new, untitled tab) defaults to plain text rather than assuming Markdown
   (its preview still renders as Markdown - see update_preview()) -
   see show_help_page() for the one deliberate exception, which forces
   Markdown for its own untitled tab since its content always is Markdown.
   An unrecognized extension on an actual saved path still falls back to
   Markdown, unchanged: that's a real file whose language merely isn't in
   the table, not a blank document that hasn't been given one yet. */
const LanguageInfo *language_for_path(const WCHAR *path) {
    if (!path) return &LANGUAGES[LANG_TEXT];
    const LanguageInfo *known = language_for_known_path(path);
    return known ? known : &LANGUAGES[LANG_MARKDOWN];
}

/* Whole file names, such as Makefile, are checked first: a name like
   "makefile.in" still goes by its extension. */
const LanguageInfo *language_for_known_path(const WCHAR *path) {
    if (!path) return NULL;
    const WCHAR *name = PathFindFileNameW(path);
    for (int l = 0; l < LANGUAGE_COUNT; l++) {
        if (LANGUAGES[l].fileNames && extension_in_list(name, LANGUAGES[l].fileNames)) return &LANGUAGES[l];
    }
    const WCHAR *extension = PathFindExtensionW(path);
    if (!extension || !*extension) return NULL;
    for (int l = 0; l < LANGUAGE_COUNT; l++) {
        if (extension_in_list(extension, LANGUAGES[l].extensions)) return &LANGUAGES[l];
    }
    return NULL;
}
/* Color the Markdown lexer's styles (see third_party/lexilla/include/
   SciLexer.h, SCE_MARKDOWN_*) to match the GitHub light/dark palette used
   everywhere else in the app. Called from apply_editor_theme() after every
   SCI_STYLECLEARALL, since that resets every style back to STYLE_DEFAULT. */
static void apply_markdown_lexer_styles(HWND hEdit, const ThemePalette *p) {
    COLORREF text = p->fg, heading = p->heading, link = p->link, code = p->code;
    COLORREF quote = p->quote, list = p->list, strike = p->quote, rule = p->lineNumber;

    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MARKDOWN_STRONG1, text);
    SendMessageA(hEdit, SCI_STYLESETBOLD, SCE_MARKDOWN_STRONG1, TRUE);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MARKDOWN_STRONG2, text);
    SendMessageA(hEdit, SCI_STYLESETBOLD, SCE_MARKDOWN_STRONG2, TRUE);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MARKDOWN_EM1, text);
    SendMessageA(hEdit, SCI_STYLESETITALIC, SCE_MARKDOWN_EM1, TRUE);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MARKDOWN_EM2, text);
    SendMessageA(hEdit, SCI_STYLESETITALIC, SCE_MARKDOWN_EM2, TRUE);

    int headerStyles[6] = { SCE_MARKDOWN_HEADER1, SCE_MARKDOWN_HEADER2, SCE_MARKDOWN_HEADER3,
                             SCE_MARKDOWN_HEADER4, SCE_MARKDOWN_HEADER5, SCE_MARKDOWN_HEADER6 };
    for (int i = 0; i < 6; i++) {
        SendMessageA(hEdit, SCI_STYLESETFORE, headerStyles[i], heading);
        SendMessageA(hEdit, SCI_STYLESETBOLD, headerStyles[i], TRUE);
    }

    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MARKDOWN_ULIST_ITEM, list);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MARKDOWN_OLIST_ITEM, list);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MARKDOWN_BLOCKQUOTE, quote);
    SendMessageA(hEdit, SCI_STYLESETITALIC, SCE_MARKDOWN_BLOCKQUOTE, TRUE);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MARKDOWN_STRIKEOUT, strike);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MARKDOWN_HRULE, rule);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MARKDOWN_LINK, link);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MARKDOWN_CODE, code);
    SendMessageA(hEdit, SCI_STYLESETFONT, SCE_MARKDOWN_CODE, (LPARAM)"Consolas");
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MARKDOWN_CODE2, code);
    SendMessageA(hEdit, SCI_STYLESETFONT, SCE_MARKDOWN_CODE2, (LPARAM)"Consolas");
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MARKDOWN_CODEBK, code);
    SendMessageA(hEdit, SCI_STYLESETFONT, SCE_MARKDOWN_CODEBK, (LPARAM)"Consolas");
}

static void style_range(HWND hEdit, const int *styles, int count, COLORREF colour) {
    for (int i = 0; i < count; i++)
        SendMessageA(hEdit, SCI_STYLESETFORE, (WPARAM)styles[i], (LPARAM)colour);
}

static void apply_json_styles(HWND hEdit, const ThemePalette *p) {
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_JSON_PROPERTYNAME, (LPARAM)p->attribute);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_JSON_STRING, (LPARAM)p->string);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_JSON_NUMBER, (LPARAM)p->number);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_JSON_KEYWORD, (LPARAM)p->keyword);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_JSON_LDKEYWORD, (LPARAM)p->keyword);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_JSON_OPERATOR, (LPARAM)p->operatorColour);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_JSON_ESCAPESEQUENCE, (LPARAM)p->variable);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_JSON_URI, (LPARAM)p->string);
    const int comments[] = { SCE_JSON_LINECOMMENT, SCE_JSON_BLOCKCOMMENT };
    style_range(hEdit, comments, 2, p->comment);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_JSON_ERROR, (LPARAM)p->keyword);
}

static void apply_markup_styles(HWND hEdit, const ThemePalette *p) {
    const int tags[] = { SCE_H_TAG, SCE_H_TAGEND, SCE_H_TAGUNKNOWN, SCE_H_XMLSTART, SCE_H_XMLEND };
    style_range(hEdit, tags, 5, p->tag);
    const int attributes[] = { SCE_H_ATTRIBUTE, SCE_H_ATTRIBUTEUNKNOWN };
    style_range(hEdit, attributes, 2, p->attribute);
    const int strings[] = { SCE_H_DOUBLESTRING, SCE_H_SINGLESTRING, SCE_H_VALUE };
    style_range(hEdit, strings, 3, p->string);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_H_NUMBER, (LPARAM)p->number);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_H_COMMENT, (LPARAM)p->comment);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_H_ENTITY, (LPARAM)p->variable);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_H_CDATA, (LPARAM)p->comment);
}

static void apply_yaml_styles(HWND hEdit, const ThemePalette *p) {
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_YAML_IDENTIFIER, (LPARAM)p->attribute);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_YAML_KEYWORD, (LPARAM)p->keyword);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_YAML_NUMBER, (LPARAM)p->number);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_YAML_COMMENT, (LPARAM)p->comment);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_YAML_REFERENCE, (LPARAM)p->variable);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_YAML_DOCUMENT, (LPARAM)p->comment);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_YAML_OPERATOR, (LPARAM)p->operatorColour);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_YAML_ERROR, (LPARAM)p->keyword);
}

static void apply_cpp_styles(HWND hEdit, const ThemePalette *p) {
    const int comments[] = { SCE_C_COMMENT, SCE_C_COMMENTLINE, SCE_C_COMMENTDOC, SCE_C_COMMENTLINEDOC };
    style_range(hEdit, comments, 4, p->comment);
    const int strings[] = { SCE_C_STRING, SCE_C_CHARACTER, SCE_C_VERBATIM, SCE_C_STRINGEOL };
    style_range(hEdit, strings, 4, p->string);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_C_WORD, (LPARAM)p->keyword);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_C_WORD2, (LPARAM)p->functionName);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_C_NUMBER, (LPARAM)p->number);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_C_PREPROCESSOR, (LPARAM)p->variable);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_C_OPERATOR, (LPARAM)p->operatorColour);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_C_REGEX, (LPARAM)p->string);
}

static void apply_python_styles(HWND hEdit, const ThemePalette *p) {
    const int comments[] = { SCE_P_COMMENTLINE, SCE_P_COMMENTBLOCK };
    style_range(hEdit, comments, 2, p->comment);
    const int strings[] = { SCE_P_STRING, SCE_P_CHARACTER, SCE_P_TRIPLE, SCE_P_TRIPLEDOUBLE, SCE_P_FSTRING };
    style_range(hEdit, strings, 5, p->string);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_P_WORD, (LPARAM)p->keyword);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_P_NUMBER, (LPARAM)p->number);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_P_DEFNAME, (LPARAM)p->functionName);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_P_CLASSNAME, (LPARAM)p->functionName);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_P_DECORATOR, (LPARAM)p->variable);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_P_OPERATOR, (LPARAM)p->operatorColour);
}

static void apply_bash_styles(HWND hEdit, const ThemePalette *p) {
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_SH_COMMENTLINE, (LPARAM)p->comment);
    const int strings[] = { SCE_SH_STRING, SCE_SH_CHARACTER, SCE_SH_HERE_Q, SCE_SH_BACKTICKS };
    style_range(hEdit, strings, 4, p->string);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_SH_WORD, (LPARAM)p->keyword);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_SH_NUMBER, (LPARAM)p->number);
    const int variables[] = { SCE_SH_SCALAR, SCE_SH_PARAM, SCE_SH_HERE_DELIM };
    style_range(hEdit, variables, 3, p->variable);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_SH_OPERATOR, (LPARAM)p->operatorColour);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_SH_ERROR, (LPARAM)p->keyword);
}

static void apply_powershell_styles(HWND hEdit, const ThemePalette *p) {
    const int comments[] = { SCE_POWERSHELL_COMMENT, SCE_POWERSHELL_COMMENTSTREAM,
                             SCE_POWERSHELL_COMMENTDOCKEYWORD };
    style_range(hEdit, comments, 3, p->comment);
    const int strings[] = { SCE_POWERSHELL_STRING, SCE_POWERSHELL_CHARACTER,
                            SCE_POWERSHELL_HERE_STRING, SCE_POWERSHELL_HERE_CHARACTER };
    style_range(hEdit, strings, 4, p->string);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_POWERSHELL_KEYWORD, (LPARAM)p->keyword);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_POWERSHELL_NUMBER, (LPARAM)p->number);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_POWERSHELL_VARIABLE, (LPARAM)p->variable);
    const int functions[] = { SCE_POWERSHELL_CMDLET, SCE_POWERSHELL_FUNCTION, SCE_POWERSHELL_ALIAS };
    style_range(hEdit, functions, 3, p->functionName);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_POWERSHELL_OPERATOR, (LPARAM)p->operatorColour);
}

static void apply_lua_styles(HWND hEdit, const ThemePalette *p) {
    const int comments[] = { SCE_LUA_COMMENT, SCE_LUA_COMMENTLINE, SCE_LUA_COMMENTDOC };
    style_range(hEdit, comments, 3, p->comment);
    const int strings[] = { SCE_LUA_STRING, SCE_LUA_CHARACTER, SCE_LUA_LITERALSTRING, SCE_LUA_STRINGEOL };
    style_range(hEdit, strings, 4, p->string);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_LUA_WORD, (LPARAM)p->keyword);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_LUA_NUMBER, (LPARAM)p->number);
    const int functions[] = { SCE_LUA_WORD2, SCE_LUA_WORD3 };
    style_range(hEdit, functions, 2, p->functionName);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_LUA_IDENTIFIER, (LPARAM)p->variable);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_LUA_PREPROCESSOR, (LPARAM)p->variable);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_LUA_OPERATOR, (LPARAM)p->operatorColour);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_LUA_LABEL, (LPARAM)p->functionName);
}

static void apply_css_styles(HWND hEdit, const ThemePalette *p) {
    const int selectors[] = { SCE_CSS_TAG, SCE_CSS_CLASS, SCE_CSS_ID, SCE_CSS_ATTRIBUTE };
    style_range(hEdit, selectors, 4, p->tag);
    const int pseudo[] = { SCE_CSS_PSEUDOCLASS, SCE_CSS_UNKNOWN_PSEUDOCLASS, SCE_CSS_PSEUDOELEMENT,
                           SCE_CSS_EXTENDED_PSEUDOCLASS, SCE_CSS_EXTENDED_PSEUDOELEMENT };
    style_range(hEdit, pseudo, 5, p->functionName);
    const int properties[] = { SCE_CSS_IDENTIFIER, SCE_CSS_IDENTIFIER2, SCE_CSS_IDENTIFIER3,
                               SCE_CSS_UNKNOWN_IDENTIFIER, SCE_CSS_EXTENDED_IDENTIFIER };
    style_range(hEdit, properties, 5, p->attribute);
    const int strings[] = { SCE_CSS_DOUBLESTRING, SCE_CSS_SINGLESTRING };
    style_range(hEdit, strings, 2, p->string);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_CSS_VALUE, (LPARAM)p->number);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_CSS_COMMENT, (LPARAM)p->comment);
    const int rules[] = { SCE_CSS_DIRECTIVE, SCE_CSS_GROUP_RULE, SCE_CSS_IMPORTANT };
    style_range(hEdit, rules, 3, p->keyword);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_CSS_VARIABLE, (LPARAM)p->variable);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_CSS_OPERATOR, (LPARAM)p->operatorColour);
}

static void apply_sql_styles(HWND hEdit, const ThemePalette *p) {
    const int comments[] = { SCE_SQL_COMMENT, SCE_SQL_COMMENTLINE, SCE_SQL_COMMENTDOC,
                             SCE_SQL_COMMENTLINEDOC, SCE_SQL_SQLPLUS_COMMENT };
    style_range(hEdit, comments, 5, p->comment);
    const int strings[] = { SCE_SQL_STRING, SCE_SQL_CHARACTER };
    style_range(hEdit, strings, 2, p->string);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_SQL_WORD, (LPARAM)p->keyword);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_SQL_WORD2, (LPARAM)p->functionName);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_SQL_NUMBER, (LPARAM)p->number);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_SQL_QUOTEDIDENTIFIER, (LPARAM)p->variable);
    const int operators[] = { SCE_SQL_OPERATOR, SCE_SQL_QOPERATOR };
    style_range(hEdit, operators, 2, p->operatorColour);
}

static void apply_ini_styles(HWND hEdit, const ThemePalette *p) {
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_PROPS_COMMENT, (LPARAM)p->comment);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_PROPS_SECTION, (LPARAM)p->heading);
    SendMessageA(hEdit, SCI_STYLESETBOLD, SCE_PROPS_SECTION, TRUE);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_PROPS_KEY, (LPARAM)p->attribute);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_PROPS_ASSIGNMENT, (LPARAM)p->operatorColour);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_PROPS_DEFVAL, (LPARAM)p->variable);
}

static void apply_toml_styles(HWND hEdit, const ThemePalette *p) {
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_TOML_COMMENT, (LPARAM)p->comment);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_TOML_TABLE, (LPARAM)p->heading);
    SendMessageA(hEdit, SCI_STYLESETBOLD, SCE_TOML_TABLE, TRUE);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_TOML_KEY, (LPARAM)p->attribute);
    const int strings[] = { SCE_TOML_STRING_SQ, SCE_TOML_STRING_DQ, SCE_TOML_TRIPLE_STRING_SQ,
                            SCE_TOML_TRIPLE_STRING_DQ, SCE_TOML_STRINGEOL };
    style_range(hEdit, strings, 5, p->string);
    const int numbers[] = { SCE_TOML_NUMBER, SCE_TOML_DATETIME };
    style_range(hEdit, numbers, 2, p->number);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_TOML_KEYWORD, (LPARAM)p->keyword);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_TOML_ESCAPECHAR, (LPARAM)p->variable);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_TOML_OPERATOR, (LPARAM)p->operatorColour);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_TOML_ERROR, (LPARAM)p->keyword);
}

static void apply_batch_styles(HWND hEdit, const ThemePalette *p) {
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_BAT_COMMENT, (LPARAM)p->comment);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_BAT_WORD, (LPARAM)p->keyword);
    const int labels[] = { SCE_BAT_LABEL, SCE_BAT_AFTER_LABEL };
    style_range(hEdit, labels, 2, p->functionName);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_BAT_HIDE, (LPARAM)p->operatorColour);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_BAT_COMMAND, (LPARAM)p->functionName);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_BAT_IDENTIFIER, (LPARAM)p->variable);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_BAT_OPERATOR, (LPARAM)p->operatorColour);
}

static void apply_makefile_styles(HWND hEdit, const ThemePalette *p) {
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MAKE_COMMENT, (LPARAM)p->comment);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MAKE_PREPROCESSOR, (LPARAM)p->keyword);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MAKE_IDENTIFIER, (LPARAM)p->variable);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MAKE_OPERATOR, (LPARAM)p->operatorColour);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MAKE_TARGET, (LPARAM)p->functionName);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_MAKE_IDEOL, (LPARAM)p->keyword);
}

/* Applies the current theme colours for whichever language is active. */
void apply_language_styles(HWND hEdit, Language language, int darkMode) {
    const ThemePalette *palette = theme_palette_for(darkMode);
    if (language == LANG_MARKDOWN) {
        apply_markdown_lexer_styles(hEdit, palette);
        return;
    }

    switch (language) {
        case LANG_JSON: apply_json_styles(hEdit, palette); break;
        case LANG_XML:
        case LANG_HTML: apply_markup_styles(hEdit, palette); break;
        case LANG_YAML: apply_yaml_styles(hEdit, palette); break;
        case LANG_CPP: apply_cpp_styles(hEdit, palette); break;
        case LANG_PYTHON: apply_python_styles(hEdit, palette); break;
        case LANG_BASH: apply_bash_styles(hEdit, palette); break;
        case LANG_POWERSHELL: apply_powershell_styles(hEdit, palette); break;
        case LANG_LUA: apply_lua_styles(hEdit, palette); break;
        case LANG_CSS: apply_css_styles(hEdit, palette); break;
        case LANG_SQL: apply_sql_styles(hEdit, palette); break;
        case LANG_INI: apply_ini_styles(hEdit, palette); break;
        case LANG_TOML: apply_toml_styles(hEdit, palette); break;
        case LANG_BATCH: apply_batch_styles(hEdit, palette); break;
        case LANG_MAKEFILE: apply_makefile_styles(hEdit, palette); break;
        default: break;
    }
}

