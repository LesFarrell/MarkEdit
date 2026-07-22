/*
 * markedit - a tiny live-preview Markdown editor for Win32.
 * Left pane: plain-text edit control for Markdown source.
 * Right pane: WebView2-hosted HTML preview (supports Mermaid diagrams,
 * with native browser zoom/pan/scroll), re-rendered on every edit.
 *
 * Build with a MinGW-w64 toolchain by running build.bat.
 */

#define COBJMACROS
#define CINTERFACE

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <shlwapi.h>
#include <shellapi.h>
#include "markdown.h"

#include <WebView2.h>
#include "third_party/scintilla/include/Scintilla.h"

#define ID_EDIT    101
#define ID_PREVIEW 102
#define IDT_PREVIEW_UPDATE 1

#define IDM_FILE_NEW      201
#define IDM_FILE_OPEN     202
#define IDM_FILE_SAVE     203
#define IDM_FILE_SAVEAS   204
#define IDM_FILE_EXIT     205
#define IDM_FILE_RECENT_CLEAR 206
#define IDM_FILE_RECENT_0 250 /* IDM_FILE_RECENT_0 .. IDM_FILE_RECENT_0+MAX_RECENT_FILES-1 */
#define IDM_EDIT_UNDO     210
#define IDM_EDIT_REDO     219
#define IDM_EDIT_CUT      211
#define IDM_EDIT_COPY     212
#define IDM_EDIT_PASTE    213
#define IDM_EDIT_DELETE   214
#define IDM_EDIT_SELECTALL 215
#define IDM_EDIT_FIND     216
#define IDM_EDIT_REPLACE  217
#define IDM_EDIT_FINDNEXT 218
#define IDM_FORMAT_FONT   220
#define IDM_VIEW_TOGGLEPREVIEW 230
#define IDM_VIEW_TOGGLEEDITOR  231
#define IDM_VIEW_TOGGLETHEME   232
#define IDM_HELP_REFERENCE 240

#define IDI_APPICON 1

static HWND g_hEdit; /* Scintilla control */
static HWND g_hPreview; /* WebView2 host child window */
static HWND g_hMainWnd;
static HFONT g_fontMono; /* used only by the plain-EDIT help-reference window */
static char g_fontFaceName[LF_FACESIZE] = "Consolas";
static int g_fontPointSize = 10;

static ICoreWebView2 *g_webview = NULL;
static ICoreWebView2Controller *g_webviewController = NULL;
static ICoreWebView2_3 *g_webview3 = NULL;
static int g_webviewReady = 0;
static HBRUSH g_previewBkBrush = NULL; /* matches the preview host's WM_CTLCOLORSTATIC background to the theme, so it doesn't flash white before WebView2 paints */
static char *g_pendingHtmlBody = NULL; /* set if update requested before webview ready */
static int g_previewDarkMode = 0;

/* Help window: a second top-level window with a read-only textbox showing a
   plain-text Markdown-syntax reference. Created on demand. */
static HWND g_hHelpWnd = NULL;
static HWND g_hHelpEdit = NULL;

static WCHAR *g_currentFile = NULL;
static int g_dirty = 0;
static int g_previewVisible = 1;
static int g_editorVisible = 1;

#define MAX_RECENT_FILES 10
static WCHAR *g_recentFiles[MAX_RECENT_FILES];
static int g_recentFileCount = 0;

static HWND g_hFindDlg = NULL;
static FINDREPLACEA g_fr;
static char g_findWhat[256] = "";
static char g_replaceWith[256] = "";
static UINT g_msgFindReplace;

void do_save(void);
static void update_preview(void);
void push_html_to_webview(const char *htmlFragment);
static void apply_preview_theme(void);
static void apply_editor_theme(HWND hEdit);
static void apply_preview_host_theme(void);
static void refresh_menu(HWND hwnd);
static void update_document_mapping(void);

static WCHAR *wcsdup_heap(const WCHAR *value) {
    size_t bytes = (wcslen(value) + 1) * sizeof(WCHAR);
    WCHAR *copy = (WCHAR *)malloc(bytes);
    if (copy) memcpy(copy, value, bytes);
    return copy;
}

static void replace_wstring(WCHAR **target, const WCHAR *value) {
    WCHAR *copy = value ? wcsdup_heap(value) : NULL;
    if (value && !copy) return;
    free(*target);
    *target = copy;
}

static WCHAR *get_module_path(void) {
    DWORD cap = 512;
    for (;;) {
        WCHAR *path = (WCHAR *)malloc((size_t)cap * sizeof(WCHAR));
        if (!path) return NULL;
        DWORD n = GetModuleFileNameW(NULL, path, cap);
        if (n > 0 && n < cap - 1) return path;
        free(path);
        if (cap >= 32768) return NULL;
        cap *= 2;
    }
}

static WCHAR *path_directory(const WCHAR *path) {
    WCHAR *dir = wcsdup_heap(path);
    if (!dir) return NULL;
    WCHAR *slash = wcsrchr(dir, L'\\');
    WCHAR *forward = wcsrchr(dir, L'/');
    if (forward && (!slash || forward > slash)) slash = forward;
    if (slash) {
        if (slash == dir + 2 && dir[1] == L':') slash[1] = 0;
        else *slash = 0;
    } else {
        free(dir);
        return wcsdup_heap(L".");
    }
    return dir;
}

static WCHAR *path_join(const WCHAR *directory, const WCHAR *name) {
    size_t a = wcslen(directory), b = wcslen(name);
    int addSlash = a > 0 && directory[a - 1] != L'\\' && directory[a - 1] != L'/';
    WCHAR *joined = (WCHAR *)malloc((a + (size_t)addSlash + b + 1) * sizeof(WCHAR));
    if (!joined) return NULL;
    memcpy(joined, directory, a * sizeof(WCHAR));
    if (addSlash) joined[a++] = L'\\';
    memcpy(joined + a, name, (b + 1) * sizeof(WCHAR));
    return joined;
}

static WCHAR *absolute_path(const WCHAR *path) {
    DWORD cap = GetFullPathNameW(path, 0, NULL, NULL);
    if (!cap) return wcsdup_heap(path);
    WCHAR *full = (WCHAR *)malloc((size_t)cap * sizeof(WCHAR));
    if (!full) return NULL;
    if (!GetFullPathNameW(path, cap, full, NULL)) {
        free(full);
        return NULL;
    }
    return full;
}

static WCHAR *extended_path(const WCHAR *path) {
    WCHAR *full = absolute_path(path);
    if (!full || wcsncmp(full, L"\\\\?\\", 4) == 0) return full;
    const WCHAR *prefix = wcsncmp(full, L"\\\\", 2) == 0 ? L"\\\\?\\UNC\\" : L"\\\\?\\";
    const WCHAR *tail = wcsncmp(full, L"\\\\", 2) == 0 ? full + 2 : full;
    size_t n = wcslen(prefix) + wcslen(tail) + 1;
    WCHAR *extended = (WCHAR *)malloc(n * sizeof(WCHAR));
    if (extended) swprintf(extended, n, L"%ls%ls", prefix, tail);
    free(full);
    return extended;
}

/* ---- simple growable text buffer, used to pull text out of the Scintilla control ---- */
static char *get_edit_text(HWND hEdit, int *outLen) {
    int len = (int)SendMessageA(hEdit, SCI_GETLENGTH, 0, 0);
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { *outLen = 0; return NULL; }
    SendMessageA(hEdit, SCI_GETTEXT, (WPARAM)(len + 1), (LPARAM)buf);
    *outLen = len;
    return buf;
}

/* Replace the Scintilla control's entire contents. Note SCI_SETTEXT is itself
   undoable (a delete-all + insert wrapped in one undo group); callers loading
   a fresh document (New/Open) should follow up with SCI_EMPTYUNDOBUFFER so the
   previous document's content isn't reachable via Undo. */
static void set_edit_text(HWND hEdit, const char *text) {
    SendMessageA(hEdit, SCI_SETTEXT, 0, (LPARAM)text);
}

/* Configure the Scintilla control as a plain Markdown source editor: no lexer
   (so it never applies its own styling/highlighting rules that could color or
   otherwise transform text unexpectedly), real tab characters at a width of 4,
   CRLF line endings to match the rest of the app's file I/O, and only the
   change-notifications we actually act on. Nothing here is auto-triggered by
   typing (autocomplete/call tips are entirely opt-in in Scintilla and are
   simply never invoked), so no unexpected characters get inserted. */
#define MARGIN_LINENUMBERS 0

/* Widen/narrow the line-number margin to fit the current line count (so "9"
   and "9999" both have comfortably-sized gutters, and it grows as the
   document grows past another power of ten). */
static void update_linenumber_margin_width(HWND hEdit) {
    int lineCount = (int)SendMessageA(hEdit, SCI_GETLINECOUNT, 0, 0);
    char digits[16];
    wsprintfA(digits, "_%d", lineCount); /* leading '_' gives a little breathing room */
    int width = (int)SendMessageA(hEdit, SCI_TEXTWIDTH, STYLE_LINENUMBER, (LPARAM)digits);
    SendMessageA(hEdit, SCI_SETMARGINWIDTHN, MARGIN_LINENUMBERS, width);
}

/* Apply the editor's color scheme (text, background, caret line, line-number
   margin, selection) to match g_previewDarkMode. Called once at startup and
   again whenever the theme is toggled, so it must be idempotent and safe to
   call after the control already has text/styles set up. */
static void apply_editor_theme(HWND hEdit) {
    if (g_previewDarkMode) {
        SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_DEFAULT, 0x00D4D4D4 /* light gray, BGR */);
        SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_DEFAULT, 0x001E1E1E /* dark gray, BGR */);
        SendMessageA(hEdit, SCI_STYLECLEARALL, 0, 0);

        SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_LINENUMBER, 0x00808080 /* mid gray, BGR */);
        SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_LINENUMBER, 0x002B2B2B /* dark gray, BGR */);

        SendMessageA(hEdit, SCI_SETCARETLINEBACK, 0x003A3A3A /* lighter dark gray, BGR */, 0);
        SendMessageA(hEdit, SCI_SETCARETFORE, 0x00FFFFFF /* white, BGR */, 0);
        SendMessageA(hEdit, SCI_SETSELBACK, TRUE, 0x00666666 /* gray, BGR */);
        SendMessageA(hEdit, SCI_SETSELFORE, TRUE, 0x00FFFFFF /* white, BGR */);
    } else {
        SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_DEFAULT, 0x00000000 /* black, BGR */);
        SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_DEFAULT, 0x00FFFFFF /* white, BGR */);
        SendMessageA(hEdit, SCI_STYLECLEARALL, 0, 0);

        SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_LINENUMBER, 0x00000000 /* black, BGR */);
        SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_LINENUMBER, 0x00F0F0F0 /* light gray, BGR */);

        SendMessageA(hEdit, SCI_SETCARETLINEBACK, 0x00E8E8E8 /* light gray, BGR */, 0);
        SendMessageA(hEdit, SCI_SETCARETFORE, 0x00000000 /* black, BGR */, 0);
        SendMessageA(hEdit, SCI_SETSELBACK, TRUE, 0x00C8C8C8 /* light gray, BGR */);
        SendMessageA(hEdit, SCI_SETSELFORE, TRUE, 0x00000000 /* black, BGR */);
    }
    InvalidateRect(hEdit, NULL, TRUE);
}

/* (Re)creates g_previewBkBrush to match g_previewDarkMode, so the preview
   host STATIC control's WM_CTLCOLORSTATIC background doesn't flash white/
   light when starting (or switching to) dark mode, before WebView2 itself
   has painted anything. */
static void apply_preview_host_theme(void) {
    if (g_previewBkBrush) DeleteObject(g_previewBkBrush);
    g_previewBkBrush = CreateSolidBrush(g_previewDarkMode ? RGB(0x0D, 0x11, 0x17) : RGB(0xFF, 0xFF, 0xFF));
    if (g_hPreview) InvalidateRect(g_hPreview, NULL, TRUE);
}

static void configure_scintilla(HWND hEdit) {
    SendMessageA(hEdit, SCI_SETCODEPAGE, SC_CP_UTF8, 0);
    SendMessageA(hEdit, SCI_SETEOLMODE, SC_EOL_CRLF, 0);
    SendMessageA(hEdit, SCI_SETUSETABS, TRUE, 0);
    SendMessageA(hEdit, SCI_SETTABWIDTH, 4, 0);
    SendMessageA(hEdit, SCI_SETINDENT, 0, 0); /* 0 = indent width follows tab width */
    SendMessageA(hEdit, SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT, 0);
    SendMessageA(hEdit, SCI_SETSCROLLWIDTHTRACKING, TRUE, 0);
    SendMessageA(hEdit, SCI_SETSCROLLWIDTH, 1, 0);
    SendMessageA(hEdit, SCI_SETWRAPMODE, 0 /* SC_WRAP_NONE */, 0);

    /* Line numbers: a dedicated margin, sized to fit the current line count. */
    SendMessageA(hEdit, SCI_SETMARGINTYPEN, MARGIN_LINENUMBERS, SC_MARGIN_NUMBER);
    update_linenumber_margin_width(hEdit);

    /* Explicit text colors, rather than relying on Scintilla's own defaults,
       so the caret-line highlight below is guaranteed to contrast with them.
       apply_editor_theme() sets the actual color values (light or dark). */
    apply_editor_theme(hEdit);

    /* Highlight the line the caret is on. SC_ALPHA_NOALPHA (rather than
       SC_ALPHA_OPAQUE) is required here: passing anything other than "no
       alpha" switches the caret line to Layer::OverText internally, which
       paints the highlight *over* the glyphs instead of behind them and is
       what was making the text unreadable. */
    SendMessageA(hEdit, SCI_SETCARETLINEVISIBLE, TRUE, 0);
    SendMessageA(hEdit, SCI_SETCARETLINEVISIBLEALWAYS, TRUE, 0); /* keep it visible even without focus */
    SendMessageA(hEdit, SCI_SETCARETLINEBACKALPHA, SC_ALPHA_NOALPHA, 0);
}

/* Apply a font face/point size to the Scintilla control by setting the
   default style (Scintilla styles text via numeric style IDs rather than an
   HFONT; with no lexer active, everything uses STYLE_DEFAULT). */
static void apply_editor_font(const char *faceName, int pointSize) {
    SendMessageA(g_hEdit, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)faceName);
    SendMessageA(g_hEdit, SCI_STYLESETSIZE, STYLE_DEFAULT, pointSize);
    SendMessageA(g_hEdit, SCI_STYLECLEARALL, 0, 0); /* copies STYLE_DEFAULT (fore/back included) to all other styles, including STYLE_LINENUMBER */

    /* STYLECLEARALL just overwrote STYLE_LINENUMBER's fore/back with
       STYLE_DEFAULT's, undoing apply_editor_theme()'s margin colors (which
       are deliberately different from the text area). Re-apply the theme so
       the gutter doesn't revert to the wrong colors after a font change. */
    apply_editor_theme(g_hEdit);

    /* The margin's digit width depends on the font, so keep both in sync. */
    SendMessageA(g_hEdit, SCI_STYLESETFONT, STYLE_LINENUMBER, (LPARAM)faceName);
    SendMessageA(g_hEdit, SCI_STYLESETSIZE, STYLE_LINENUMBER, pointSize);
    update_linenumber_margin_width(g_hEdit);
}

/* ---- persisted settings, stored in markedit.ini next to the executable ---- */
static WCHAR *get_settings_path(void) {
    WCHAR *module = get_module_path();
    if (!module) return NULL;
    WCHAR *directory = path_directory(module);
    free(module);
    if (!directory) return NULL;
    WCHAR *settings = path_join(directory, L"markedit.ini");
    free(directory);
    return settings;
}

static void load_font_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WCHAR face[LF_FACESIZE] = L"Consolas";
    GetPrivateProfileStringW(L"Font", L"FaceName", face, face, LF_FACESIZE, path);
    WideCharToMultiByte(CP_UTF8, 0, face, -1, g_fontFaceName, LF_FACESIZE, NULL, NULL);
    g_fontPointSize = GetPrivateProfileIntW(L"Font", L"PointSize", g_fontPointSize, path);
    if (g_fontPointSize < 1) g_fontPointSize = 1;
    free(path);
}

static void load_theme_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WCHAR value[16];
    GetPrivateProfileStringW(L"Preview", L"DarkMode", L"0", value, 16, path);
    g_previewDarkMode = (value[0] == L'1' || value[0] == L't' || value[0] == L'T' || value[0] == L'y' || value[0] == L'Y');
    free(path);
}

static void save_theme_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WritePrivateProfileStringW(L"Preview", L"DarkMode", g_previewDarkMode ? L"1" : L"0", path);
    free(path);
}

static void save_font_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WCHAR face[LF_FACESIZE];
    MultiByteToWideChar(CP_UTF8, 0, g_fontFaceName, -1, face, LF_FACESIZE);
    WritePrivateProfileStringW(L"Font", L"FaceName", face, path);
    WCHAR sizeBuf[16];
    swprintf(sizeBuf, 16, L"%d", g_fontPointSize);
    WritePrivateProfileStringW(L"Font", L"PointSize", sizeBuf, path);
    free(path);
}

/* ---- recently opened files, most-recent-first, stored in the same INI ---- */
static void load_recent_files(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    for (int i = 0; i < g_recentFileCount; i++) free(g_recentFiles[i]);
    g_recentFileCount = 0;
    for (int i = 0; i < MAX_RECENT_FILES; i++) {
        WCHAR key[16];
        swprintf(key, 16, L"File%d", i);
        DWORD cap = 512, n = 0;
        WCHAR *value = NULL;
        do {
            free(value);
            value = (WCHAR *)malloc((size_t)cap * sizeof(WCHAR));
            if (!value) break;
            n = GetPrivateProfileStringW(L"RecentFiles", key, L"", value, cap, path);
            if (n < cap - 1) break;
            cap *= 2;
        } while (cap <= 32768);
        if (n == 0) { free(value); break; }
        g_recentFiles[g_recentFileCount++] = value;
        value = NULL;
    }
    free(path);
}

static void save_recent_files(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    /* clear the whole section first so a shrinking list doesn't leave stale
       trailing entries from a previous, longer run */
    WritePrivateProfileStringW(L"RecentFiles", NULL, NULL, path);
    for (int i = 0; i < g_recentFileCount; i++) {
        WCHAR key[16];
        swprintf(key, 16, L"File%d", i);
        WritePrivateProfileStringW(L"RecentFiles", key, g_recentFiles[i], path);
    }
    free(path);
}

/* Move (or insert) path to the front of the recent-files list, deduping
   case-insensitively (Windows paths), capping at MAX_RECENT_FILES. */
static void add_recent_file(const WCHAR *path) {
    WCHAR *copy = wcsdup_heap(path);
    if (!copy) return;
    int existing = -1;
    for (int i = 0; i < g_recentFileCount; i++) {
        if (_wcsicmp(g_recentFiles[i], path) == 0) { existing = i; break; }
    }
    int start = (existing >= 0) ? existing : (g_recentFileCount < MAX_RECENT_FILES ? g_recentFileCount : MAX_RECENT_FILES - 1);
    if (existing >= 0) free(g_recentFiles[existing]);
    else if (g_recentFileCount == MAX_RECENT_FILES) free(g_recentFiles[MAX_RECENT_FILES - 1]);
    for (int i = start; i > 0; i--) {
        g_recentFiles[i] = g_recentFiles[i - 1];
    }
    g_recentFiles[0] = copy;
    if (existing < 0 && g_recentFileCount < MAX_RECENT_FILES) g_recentFileCount++;
    save_recent_files();
}

static void clear_recent_files(void) {
    for (int i = 0; i < g_recentFileCount; i++) {
        free(g_recentFiles[i]);
        g_recentFiles[i] = NULL;
    }
    g_recentFileCount = 0;
    save_recent_files();
}

/* Remove path from the recent-files list (e.g. because it no longer exists
   on disk) and persist the change. */
static void remove_recent_file(const WCHAR *path) {
    int idx = -1;
    for (int i = 0; i < g_recentFileCount; i++) {
        if (_wcsicmp(g_recentFiles[i], path) == 0) { idx = i; break; }
    }
    if (idx < 0) return;
    free(g_recentFiles[idx]);
    for (int i = idx; i < g_recentFileCount - 1; i++) g_recentFiles[i] = g_recentFiles[i + 1];
    g_recentFileCount--;
    g_recentFiles[g_recentFileCount] = NULL;
    save_recent_files();
}

/* Drop any recent-file entries whose file no longer exists on disk. Called
   right before the menu is (re)built so the list stays accurate without
   needing to stat every entry on every keystroke. */
static void prune_missing_recent_files(void) {
    int i = 0;
    while (i < g_recentFileCount) {
        WCHAR *ioPath = extended_path(g_recentFiles[i]);
        DWORD attrs = ioPath ? GetFileAttributesW(ioPath) : INVALID_FILE_ATTRIBUTES;
        free(ioPath);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            remove_recent_file(g_recentFiles[i]);
        } else {
            i++;
        }
    }
}



/* ================= WebView2 host page ================= */

static const char *HTML_SHELL =
"<!doctype html><html><head><meta charset=\"utf-8\">"
"<style>"
"html,body{margin:0;padding:0;background:#ffffff;font-family:'Segoe UI',sans-serif;color:#141414;height:100%;}"
"#content{padding:12px 20px 60px 20px;min-height:calc(100% - 72px);display:flex;flex-direction:column;}"
"#content.solo-diagram{padding:12px 20px;height:calc(100vh - 24px);min-height:calc(100vh - 24px);overflow:hidden;}"
"h1{font-size:1.9em;font-weight:700;margin:0.5em 0 0.3em 0;}"
"h2{font-size:1.5em;font-weight:700;margin:0.5em 0 0.3em 0;}"
"h3{font-size:1.2em;font-weight:700;margin:0.5em 0 0.3em 0;}"
"p{margin:0.4em 0;line-height:1.5;}"
"code{background:#f0f0f0;padding:1px 4px;border-radius:3px;font-family:Consolas,monospace;font-size:0.95em;}"
"pre.codeblock{background:#f5f5f5;padding:8px 10px;border-radius:4px;overflow-x:auto;}"
"pre.codeblock code{background:none;padding:0;}"
"blockquote{border-left:3px solid #ccc;margin:0.4em 0;padding:0.1em 1em;color:#5a5a5a;font-style:italic;}"
"hr{border:none;border-top:1px solid #bbb;margin:1em 0;}"
"ul,ol{margin:0.3em 0;padding-left:1.6em;}"
"li{margin:0.15em 0;}"
"li:has(> input[type=\"checkbox\"]){list-style:none;margin-left:-1.2em;}"
"li>input[type=\"checkbox\"]{margin-right:0.4em;}"
"table{border-collapse:collapse;margin:0.6em 0;}"
"th,td{border:1px solid #ccc;padding:4px 10px;text-align:left;}"
"th{background:#f0f0f0;font-weight:700;}"
"a{color:#0969da;}"
"img{max-width:100%;}"
".mermaid-wrap{position:relative;border:1px solid #e0e0e0;border-radius:6px;margin:0.6em 0;overflow:auto;background:#fff;max-height:70vh;}"
"#content.solo-diagram .mermaid-wrap{flex:1 1 auto;max-height:none;margin:0;min-height:0;}"
".mermaid-inner{transform-origin:0 0;display:inline-block;padding:12px;}"
".mermaid-toolbar{position:sticky;top:0;left:0;display:flex;gap:4px;justify-content:flex-end;padding:4px;background:rgba(255,255,255,0.9);border-bottom:1px solid #eee;z-index:5;}"
".mermaid-toolbar button{cursor:pointer;border:1px solid #ccc;background:#fafafa;border-radius:4px;width:26px;height:24px;font-size:14px;line-height:1;}"
".mermaid-toolbar .zoom-readout{width:52px;font-variant-numeric:tabular-nums;}"
".mermaid-toolbar button:hover{background:#eee;}"
"</style>"
"</head><body>"
"<div id=\"content\"></div>"
"<script src=\"mermaid.min.js\"></script>"
"<script>"
"mermaid.initialize({startOnLoad:false, securityLevel:'strict'});"
"let zoomSeq = 0;"
"function wrapMermaidBlocks(){"
"  const blocks = document.querySelectorAll('pre.mermaid:not([data-wrapped]), pre > code.language-mermaid');"
"  blocks.forEach((block) => {"
"    const el = block.matches('code') ? block.parentElement : block;"
"    if (!el || el.hasAttribute('data-wrapped')) return;"
"    el.setAttribute('data-wrapped','1');"
"    const src = el.textContent;"
"    const id = 'mmd-' + (zoomSeq++);"
"    const wrap = document.createElement('div');"
"    wrap.className = 'mermaid-wrap';"
"    const toolbar = document.createElement('div');"
"    toolbar.className = 'mermaid-toolbar';"
"    toolbar.innerHTML = '<button data-act=\"out\">-</button><button class=\"zoom-readout\" data-act=\"reset\">100%</button><button data-act=\"in\">+</button>';"
"    const readout = toolbar.querySelector('.zoom-readout');"
"    const inner = document.createElement('div');"
"    inner.className = 'mermaid-inner';"
"    inner.id = id;"
"    inner.textContent = src;"
"    inner.dataset.scale = '1';"
"    wrap.appendChild(toolbar);"
"    wrap.appendChild(inner);"
"    el.replaceWith(wrap);"
"    function applyScale(scale) {"
"      inner.dataset.scale = String(scale);"
"      inner.style.transform = 'scale(' + scale + ')';"
"      readout.textContent = Math.round(scale * 100) + '%';"
"    }"
"    toolbar.addEventListener('click', (e) => {"
"      const act = e.target.getAttribute('data-act');"
"      if (!act) return;"
"      let scale = parseFloat(inner.dataset.scale || '1');"
"      if (act === 'in') scale = Math.min(scale * 1.2, 6);"
"      else if (act === 'out') scale = Math.max(scale / 1.2, 0.15);"
"      else scale = 1;"
"      applyScale(scale);"
"    });"
"    wrap.addEventListener('wheel', (e) => {"
"      if (!e.ctrlKey) return;"
"      e.preventDefault();"
"      let scale = parseFloat(inner.dataset.scale || '1');"
"      scale *= (e.deltaY < 0) ? 1.1 : (1/1.1);"
"      scale = Math.min(Math.max(scale, 0.15), 6);"
"      applyScale(scale);"
"    }, { passive: false });"
"  });"
"}"
"function updateSoloDiagramLayout(){"
"  const content = document.getElementById('content');"
"  const children = Array.from(content.children).filter((el) => !(el.tagName === 'P' && el.textContent.trim() === ''));"
"  const solo = children.length === 1 && children[0].classList.contains('mermaid-wrap');"
"  content.classList.toggle('solo-diagram', solo);"
"}"
"function setContent(html){"
"  document.getElementById('content').innerHTML = html;"
"  wrapMermaidBlocks();"
"  mermaid.run({querySelector: '.mermaid-inner'}).catch(()=>{}).then(()=>updateSoloDiagramLayout());"
"  updateSoloDiagramLayout();"
"}"
"window.setContent = setContent;"
"</script>"
"</body></html>";

/* Convert a UTF-8 (well, ASCII in practice from our EDIT control) C string to a wide string. */
static WCHAR *to_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    WCHAR *w = (WCHAR *)malloc((size_t)n * sizeof(WCHAR));
    if (!w) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n)) {
        free(w);
        return NULL;
    }
    return w;
}

/* JSON-escape a UTF-8 string for embedding inside a JS string literal (single-quoted). */
static char *json_escape(const char *s) {
    size_t n = strlen(s);
    char *out = (char *)malloc(n * 6 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\'': out[o++] = '\\'; out[o++] = '\''; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
            case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
            case '\t': out[o++] = '\\'; out[o++] = 't'; break;
            case '\b': out[o++] = '\\'; out[o++] = 'b'; break;
            case '\f': out[o++] = '\\'; out[o++] = 'f'; break;
            default:
                if (c < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    out[o++] = '\\'; out[o++] = 'u'; out[o++] = '0'; out[o++] = '0';
                    out[o++] = hex[c >> 4]; out[o++] = hex[c & 15];
                } else if (c == 0xE2 && i + 2 < n && (unsigned char)s[i + 1] == 0x80 &&
                           ((unsigned char)s[i + 2] == 0xA8 || (unsigned char)s[i + 2] == 0xA9)) {
                    memcpy(out + o, (unsigned char)s[i + 2] == 0xA8 ? "\\u2028" : "\\u2029", 6);
                    o += 6;
                    i += 2;
                } else out[o++] = (char)c;
                break;
        }
    }
    out[o] = 0;
    return out;
}

/* ---- generic single-use COM completion handler (QueryInterface/AddRef/Release are no-ops) ---- */

typedef HRESULT (STDMETHODCALLTYPE *EnvInvokeFn)(void *self, HRESULT errorCode, ICoreWebView2Environment *env);
typedef HRESULT (STDMETHODCALLTYPE *CtrlInvokeFn)(void *self, HRESULT errorCode, ICoreWebView2Controller *ctrl);
typedef HRESULT (STDMETHODCALLTYPE *ScriptInvokeFn)(void *self, HRESULT errorCode, LPCWSTR result);

static ULONG STDMETHODCALLTYPE Handler_AddRef(void *this_) { (void)this_; return 1; }
static ULONG STDMETHODCALLTYPE Handler_Release(void *this_) { (void)this_; return 1; }
static HRESULT STDMETHODCALLTYPE Handler_QI(void *this_, REFIID riid, void **out) {
    (void)this_; (void)riid;
    *out = NULL;
    return E_NOINTERFACE;
}

/* Environment created handler */
static HRESULT STDMETHODCALLTYPE EnvCreated_Invoke(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *this_, HRESULT errorCode, ICoreWebView2Environment *env);
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl g_envVtbl = {
    (void*)Handler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)EnvCreated_Invoke
};
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler g_envHandler = { &g_envVtbl };

/* Controller created handler */
static HRESULT STDMETHODCALLTYPE CtrlCreated_Invoke(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *this_, HRESULT errorCode, ICoreWebView2Controller *ctrl);
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl g_ctrlVtbl = {
    (void*)Handler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)CtrlCreated_Invoke
};
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandler g_ctrlHandler = { &g_ctrlVtbl };

/* ExecuteScript completed handler: no-op, we don't need the result */
static HRESULT STDMETHODCALLTYPE ScriptDone_Invoke(ICoreWebView2ExecuteScriptCompletedHandler *this_, HRESULT errorCode, LPCWSTR result) {
    (void)this_; (void)errorCode; (void)result;
    return S_OK;
}
static ICoreWebView2ExecuteScriptCompletedHandlerVtbl g_scriptVtbl = {
    (void*)Handler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)ScriptDone_Invoke
};
static ICoreWebView2ExecuteScriptCompletedHandler g_scriptHandler = { &g_scriptVtbl };

/* NavigationCompleted handler: the initial shell page is ready, flush any pending content */
static HRESULT STDMETHODCALLTYPE NavDone_Invoke(ICoreWebView2NavigationCompletedEventHandler *this_, ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) {
    (void)this_; (void)sender; (void)args;
    g_webviewReady = 1;
    apply_preview_theme();
    if (g_pendingHtmlBody) {
        char *body = g_pendingHtmlBody;
        g_pendingHtmlBody = NULL;
        push_html_to_webview(body);
        free(body);
    }
    return S_OK;
}
static ICoreWebView2NavigationCompletedEventHandlerVtbl g_navVtbl = {
    (void*)Handler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)NavDone_Invoke
};
static ICoreWebView2NavigationCompletedEventHandler g_navHandler = { &g_navVtbl };

static WCHAR *decode_document_url(const WCHAR *uri) {
    static const WCHAR prefix[] = L"https://document.local/";
    if (_wcsnicmp(uri, prefix, wcslen(prefix)) != 0) return NULL;
    const WCHAR *encoded = uri + wcslen(prefix);
    const WCHAR *end = encoded + wcscspn(encoded, L"?#");
    size_t byteCap = (size_t)(end - encoded) * 4 + 1;
    char *bytes = (char *)malloc(byteCap);
    if (!bytes) return NULL;
    size_t o = 0;
    for (const WCHAR *p = encoded; p < end;) {
        if (*p == L'%' && p + 2 < end) {
            int hi = (p[1] >= L'0' && p[1] <= L'9') ? p[1] - L'0' :
                     (p[1] >= L'a' && p[1] <= L'f') ? p[1] - L'a' + 10 :
                     (p[1] >= L'A' && p[1] <= L'F') ? p[1] - L'A' + 10 : -1;
            int lo = (p[2] >= L'0' && p[2] <= L'9') ? p[2] - L'0' :
                     (p[2] >= L'a' && p[2] <= L'f') ? p[2] - L'a' + 10 :
                     (p[2] >= L'A' && p[2] <= L'F') ? p[2] - L'A' + 10 : -1;
            if (hi >= 0 && lo >= 0) {
                bytes[o++] = (char)((hi << 4) | lo);
                p += 3;
                continue;
            }
        }
        WCHAR one[2] = { *p++, 0 };
        o += (size_t)WideCharToMultiByte(CP_UTF8, 0, one, 1, bytes + o, (int)(byteCap - o), NULL, NULL);
    }
    bytes[o] = 0;
    int wideLen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, (int)o, NULL, 0);
    if (wideLen <= 0) { free(bytes); return NULL; }
    WCHAR *relative = (WCHAR *)malloc(((size_t)wideLen + 1) * sizeof(WCHAR));
    if (!relative) { free(bytes); return NULL; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, (int)o, relative, wideLen);
    relative[wideLen] = 0;
    free(bytes);
    for (int i = 0; i < wideLen; i++) if (relative[i] == L'/') relative[i] = L'\\';
    WCHAR *directory = g_currentFile ? path_directory(g_currentFile) : NULL;
    if (!directory) {
        WCHAR *module = get_module_path();
        directory = module ? path_directory(module) : NULL;
        free(module);
    }
    WCHAR *full = directory ? path_join(directory, relative) : NULL;
    free(directory);
    free(relative);
    return full;
}

/* NavigationStarting handler: links clicked inside the preview (Markdown links,
   images-as-links, etc.) should open in the user's default browser instead of
   navigating the embedded WebView away from the preview shell. Our own shell
   page load is allowed through - either the virtual-host URL
   (https://markedit.local/preview_shell.html) or, on older WebView2 Runtimes
   lacking ICoreWebView2_3, the "about:..." page produced by NavigateToString.
   Anything else is cancelled here and handed to ShellExecuteW instead. */
static HRESULT STDMETHODCALLTYPE NavStarting_Invoke(ICoreWebView2NavigationStartingEventHandler *this_, ICoreWebView2 *sender, ICoreWebView2NavigationStartingEventArgs *args) {
    (void)this_; (void)sender;
    LPWSTR uri = NULL;
    if (SUCCEEDED(ICoreWebView2NavigationStartingEventArgs_get_Uri(args, &uri)) && uri) {
        /* Prefix match (not exact-equality) so the "?theme=dark|light" query
           string passed on the initial navigation still counts as our shell
           page, rather than being treated as an external link. */
        static const WCHAR shellPrefix[] = L"https://markedit.local/preview_shell.html";
        size_t shellLen = wcslen(shellPrefix);
        int isShellPage = (_wcsnicmp(uri, shellPrefix, shellLen) == 0 &&
                           (uri[shellLen] == 0 || uri[shellLen] == L'?' || uri[shellLen] == L'#')) ||
                           (!g_webviewReady && _wcsicmp(uri, L"about:blank") == 0);
        if (!isShellPage) {
            ICoreWebView2NavigationStartingEventArgs_put_Cancel(args, TRUE);
            WCHAR *localPath = decode_document_url(uri);
            if (localPath) {
                ShellExecuteW(NULL, L"open", localPath, NULL, NULL, SW_SHOWNORMAL);
                free(localPath);
            } else if (_wcsnicmp(uri, L"https://", 8) == 0 ||
                       _wcsnicmp(uri, L"http://", 7) == 0 ||
                       _wcsnicmp(uri, L"mailto:", 7) == 0) {
                ShellExecuteW(NULL, L"open", uri, NULL, NULL, SW_SHOWNORMAL);
            }
        }
        CoTaskMemFree(uri);
    }
    return S_OK;
}
static ICoreWebView2NavigationStartingEventHandlerVtbl g_navStartVtbl = {
    (void*)Handler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)NavStarting_Invoke
};
static ICoreWebView2NavigationStartingEventHandler g_navStartHandler = { &g_navStartVtbl };

static void update_document_mapping(void) {
    if (!g_webview3) return;
    WCHAR *directory = g_currentFile ? path_directory(g_currentFile) : NULL;
    if (!directory) {
        WCHAR *module = get_module_path();
        directory = module ? path_directory(module) : NULL;
        free(module);
    }
    if (directory) {
        ICoreWebView2_3_SetVirtualHostNameToFolderMapping(g_webview3, L"document.local", directory,
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        free(directory);
    }
}

static void apply_preview_theme(void) {
    if (!g_webviewReady || !g_webview) return;
    char script[64];
    wsprintfA(script, "window.setTheme('%s');", g_previewDarkMode ? "dark" : "light");
    WCHAR *wscript = to_wide(script);
    if (wscript) ICoreWebView2_ExecuteScript(g_webview, wscript, &g_scriptHandler);
    free(wscript);
}

void push_html_to_webview(const char *htmlFragment) {
    if (!g_webviewReady || !g_webview) {
        free(g_pendingHtmlBody);
        g_pendingHtmlBody = _strdup(htmlFragment);
        return;
    }
    char *escaped = json_escape(htmlFragment);
    if (!escaped) return;
    size_t escapedLen = strlen(escaped);
    const char *prefix = "setContent('";
    const char *suffix = "')";
    size_t scriptLen = strlen(prefix) + escapedLen + strlen(suffix);
    char *script = (char *)malloc(scriptLen + 1);
    memcpy(script, prefix, strlen(prefix));
    memcpy(script + strlen(prefix), escaped, escapedLen);
    memcpy(script + strlen(prefix) + escapedLen, suffix, strlen(suffix) + 1);
    WCHAR *wscript = to_wide(script);
    if (wscript) ICoreWebView2_ExecuteScript(g_webview, wscript, &g_scriptHandler);
    free(wscript);
    free(script);
    free(escaped);
}

static HRESULT STDMETHODCALLTYPE CtrlCreated_Invoke(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *this_, HRESULT errorCode, ICoreWebView2Controller *ctrl) {
    (void)this_;
    if (FAILED(errorCode) || !ctrl) return S_OK;

    g_webviewController = ctrl;
    ICoreWebView2Controller_AddRef(ctrl);

    RECT rc;
    GetClientRect(g_hPreview, &rc);
    ICoreWebView2Controller_put_Bounds(ctrl, rc);

    /* Set the controller's own background before anything is visible, so a
       dark-mode start shows a dark WebView2 host immediately instead of
       flashing its default white background while the shell page loads. */
    ICoreWebView2Controller2 *ctrl2 = NULL;
    ICoreWebView2Controller_QueryInterface(ctrl, &IID_ICoreWebView2Controller2, (void **)&ctrl2);
    if (ctrl2) {
        COREWEBVIEW2_COLOR bg = g_previewDarkMode
            ? (COREWEBVIEW2_COLOR){255, 0x0D, 0x11, 0x17}
            : (COREWEBVIEW2_COLOR){255, 0xFF, 0xFF, 0xFF};
        ICoreWebView2Controller2_put_DefaultBackgroundColor(ctrl2, bg);
        ICoreWebView2Controller2_Release(ctrl2);
    }

    ICoreWebView2Controller_put_IsVisible(ctrl, TRUE);

    ICoreWebView2 *webview = NULL;
    ICoreWebView2Controller_get_CoreWebView2(ctrl, &webview);
    g_webview = webview;

    ICoreWebView2Settings *settings = NULL;
    if (SUCCEEDED(ICoreWebView2_get_Settings(webview, &settings)) && settings) {
        ICoreWebView2Settings_put_AreDevToolsEnabled(settings, FALSE);
        ICoreWebView2Settings_put_AreDefaultScriptDialogsEnabled(settings, FALSE);
        ICoreWebView2Settings_put_IsWebMessageEnabled(settings, FALSE);
        ICoreWebView2Settings_Release(settings);
    }

    EventRegistrationToken token;
    ICoreWebView2_add_NavigationCompleted(webview, &g_navHandler, &token);
    EventRegistrationToken navStartToken;
    ICoreWebView2_add_NavigationStarting(webview, &g_navStartHandler, &navStartToken);

    /* Serve the shell + mermaid.min.js from the exe's folder via a virtual host,
       so the page has a real origin and can load mermaid.min.js by relative URL. */
    ICoreWebView2_QueryInterface(webview, &IID_ICoreWebView2_3, (void **)&g_webview3);
    if (g_webview3) {
        WCHAR *module = get_module_path();
        WCHAR *exeDir = module ? path_directory(module) : NULL;
        free(module);
        if (!exeDir) return S_OK;
        ICoreWebView2_3_SetVirtualHostNameToFolderMapping(g_webview3, L"markedit.local", exeDir, COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
        free(exeDir);
        update_document_mapping();
        /* Pass the starting theme via query string so the page can apply it
           immediately on load, instead of defaulting to light and flipping
           to dark only once apply_preview_theme() runs after navigation
           completes (which caused a visible light-then-dark flash). */
        const WCHAR *shellUrl = g_previewDarkMode
            ? L"https://markedit.local/preview_shell.html?theme=dark"
            : L"https://markedit.local/preview_shell.html?theme=light";
        ICoreWebView2_Navigate(webview, shellUrl);
    } else {
        WCHAR *wHtml = to_wide(HTML_SHELL);
        ICoreWebView2_NavigateToString(webview, wHtml);
        free(wHtml);
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE EnvCreated_Invoke(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *this_, HRESULT errorCode, ICoreWebView2Environment *env) {
    (void)this_;
    if (FAILED(errorCode) || !env) {
        char msg[256];
        wsprintfA(msg, "Failed to create WebView2 environment (hr=0x%08lX). Is the WebView2 Runtime installed?", (unsigned long)errorCode);
        MessageBoxA(g_hMainWnd, msg, "MarkEdit", MB_OK | MB_ICONERROR);
        return S_OK;
    }
    ICoreWebView2Environment_CreateCoreWebView2Controller(env, g_hPreview, &g_ctrlHandler);
    return S_OK;
}

static void init_webview(HWND hostWnd) {
    WCHAR *module = get_module_path();
    WCHAR *exeDir = module ? path_directory(module) : NULL;
    WCHAR *userDataDir = exeDir ? path_join(exeDir, L"webview2_data") : NULL;
    free(module);
    free(exeDir);
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(NULL, userDataDir, NULL, &g_envHandler);
    free(userDataDir);
    if (FAILED(hr)) {
        char msg[256];
        wsprintfA(msg, "CreateCoreWebView2EnvironmentWithOptions call failed (hr=0x%08lX).", (unsigned long)hr);
        MessageBoxA(g_hMainWnd, msg, "MarkEdit", MB_OK | MB_ICONERROR);
    }
    (void)hostWnd;
}

static void update_preview(void) {
    if (g_hMainWnd) KillTimer(g_hMainWnd, IDT_PREVIEW_UPDATE);
    int len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;
    char *html = markdown_to_html(text);
    free(text);
    if (!html) return;
    push_html_to_webview(html);
    free(html);
}

/* ================= rest of the app (menus, file I/O, find/replace, font) ================= */

static void update_title(void) {
    const WCHAR *name = g_currentFile ? g_currentFile : L"Untitled";
    size_t cap = wcslen(name) + 32;
    WCHAR *title = (WCHAR *)malloc(cap * sizeof(WCHAR));
    if (!title) return;
    swprintf(title, cap, L"%ls%ls - MarkEdit", name, g_dirty ? L"*" : L"");
    SetWindowTextW(g_hMainWnd, title);
    free(title);
}

static void set_dirty(int dirty) {
    g_dirty = dirty;
    update_title();
}

static void do_new(void) {
    if (g_dirty) {
        int r = MessageBoxW(g_hMainWnd, L"Save changes to the current document?", L"MarkEdit", MB_YESNOCANCEL | MB_ICONWARNING);
        if (r == IDCANCEL) return;
        if (r == IDYES) {
            do_save();
            if (g_dirty) return; /* save was cancelled */
        }
    }
    set_edit_text(g_hEdit, "");
    SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    replace_wstring(&g_currentFile, NULL);
    update_document_mapping();
    update_preview();
    set_dirty(0);
}

static int load_file(const WCHAR *path) {
    WCHAR *displayPath = absolute_path(path);
    WCHAR *ioPath = extended_path(path);
    HANDLE hFile = ioPath ? CreateFileW(ioPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL)
        : INVALID_HANDLE_VALUE;
    free(ioPath);
    if (hFile == INVALID_HANDLE_VALUE) {
        MessageBoxW(g_hMainWnd, L"Could not open file.", L"MarkEdit", MB_OK | MB_ICONERROR);
        remove_recent_file(path);
        refresh_menu(g_hMainWnd);
        free(displayPath);
        return 0;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart < 0 || size.QuadPart > INT_MAX) {
        CloseHandle(hFile);
        MessageBoxW(g_hMainWnd, L"The file is too large to edit safely.", L"MarkEdit", MB_OK | MB_ICONERROR);
        free(displayPath);
        return 0;
    }
    size_t length = (size_t)size.QuadPart;
    char *buf = (char *)malloc(length + 1);
    size_t total = 0;
    int ok = 0;
    while (buf && total < length) {
        DWORD chunk = (DWORD)((length - total) > 1024 * 1024 ? 1024 * 1024 : (length - total));
        DWORD got = 0;
        if (!ReadFile(hFile, buf + total, chunk, &got, NULL) || got == 0) break;
        total += got;
    }
    if (buf && total == length) {
        buf[length] = 0;
        const char *content = buf;
        if (length >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
            content += 3;
        set_edit_text(g_hEdit, content);
        SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
        SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
        replace_wstring(&g_currentFile, displayPath ? displayPath : path);
        update_document_mapping();
        update_preview();
        set_dirty(0);
        add_recent_file(displayPath ? displayPath : path);
        refresh_menu(g_hMainWnd);
        ok = 1;
    } else {
        MessageBoxW(g_hMainWnd, L"Could not read file.", L"MarkEdit", MB_OK | MB_ICONERROR);
    }
    if (buf) free(buf);
    CloseHandle(hFile);
    free(displayPath);
    return ok;
}

/* Shared "is it OK to replace the current document" prompt used by New/Open/
   opening a recent file. Returns 0 if the caller should abort. */
static int confirm_discard_if_dirty(void) {
    if (!g_dirty) return 1;
    int r = MessageBoxW(g_hMainWnd, L"Save changes to the current document?", L"MarkEdit", MB_YESNOCANCEL | MB_ICONWARNING);
    if (r == IDCANCEL) return 0;
    if (r == IDYES) {
        do_save();
        if (g_dirty) return 0; /* save was cancelled */
    }
    return 1;
}

static void do_open_recent(int index) {
    if (index < 0 || index >= g_recentFileCount) return;
    if (!confirm_discard_if_dirty()) return;
    WCHAR *path = wcsdup_heap(g_recentFiles[index]);
    if (path) load_file(path); /* load_file() may mutate the recent-files array */
    free(path);
}

static void do_open(void) {
    if (!confirm_discard_if_dirty()) return;
    DWORD cap = 32768;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"Markdown Files (*.md;*.markdown)\0*.md;*.markdown\0Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"md";
    if (GetOpenFileNameW(&ofn)) load_file(path);
    free(path);
}

/* Opens a file dropped onto the main window, prompting to save any dirty
   document first, same as File > Open. */
static void do_open_dropped_file(const WCHAR *path) {
    if (!confirm_discard_if_dirty()) return;
    load_file(path);
}

static void handle_dropfiles(HDROP hDrop) {
    UINT cap = DragQueryFileW(hDrop, 0, NULL, 0) + 1;
    WCHAR *path = (WCHAR *)malloc((size_t)cap * sizeof(WCHAR));
    if (path && DragQueryFileW(hDrop, 0, path, cap)) do_open_dropped_file(path);
    free(path);
    DragFinish(hDrop);
}

static int save_to_path(const WCHAR *path) {
    int len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) {
        MessageBoxW(g_hMainWnd, L"Not enough memory to save the document.", L"MarkEdit", MB_OK | MB_ICONERROR);
        return 0;
    }
    WCHAR *displayPath = absolute_path(path);
    WCHAR *ioPath = extended_path(path);
    const WCHAR *target = ioPath ? ioPath : path;
    size_t pathLen = wcslen(target);
    WCHAR *temporary = (WCHAR *)malloc((pathLen + 64) * sizeof(WCHAR));
    HANDLE hFile = INVALID_HANDLE_VALUE;
    if (temporary) {
        for (unsigned attempt = 0; attempt < 100 && hFile == INVALID_HANDLE_VALUE; attempt++) {
            swprintf(temporary, pathLen + 64, L"%ls.markedit-%lu-%u.tmp", target, GetCurrentProcessId(), attempt);
            hFile = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, NULL);
            if (hFile == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) break;
        }
    }
    if (hFile == INVALID_HANDLE_VALUE) {
        free(temporary);
        free(displayPath);
        free(ioPath);
        free(text);
        MessageBoxW(g_hMainWnd, L"Could not create a temporary file for saving.", L"MarkEdit", MB_OK | MB_ICONERROR);
        return 0;
    }
    size_t total = 0;
    BOOL ok = TRUE;
    while (total < (size_t)len) {
        DWORD chunk = (DWORD)(((size_t)len - total) > 1024 * 1024 ? 1024 * 1024 : ((size_t)len - total));
        DWORD written = 0;
        if (!WriteFile(hFile, text + total, chunk, &written, NULL) || written != chunk) { ok = FALSE; break; }
        total += written;
    }
    if (ok) ok = FlushFileBuffers(hFile);
    if (!CloseHandle(hFile)) ok = FALSE;
    free(text);
    if (ok) {
        DWORD attrs = GetFileAttributesW(target);
        if (attrs != INVALID_FILE_ATTRIBUTES)
            ok = ReplaceFileW(target, temporary, NULL, REPLACEFILE_WRITE_THROUGH, NULL, NULL);
        else
            ok = MoveFileExW(temporary, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    if (!ok) {
        DeleteFileW(temporary);
        free(temporary);
        free(displayPath);
        free(ioPath);
        MessageBoxW(g_hMainWnd, L"Could not write the complete file; the original was left unchanged.", L"MarkEdit", MB_OK | MB_ICONERROR);
        return 0;
    }
    free(temporary);
    replace_wstring(&g_currentFile, displayPath ? displayPath : path);
    free(ioPath);
    update_document_mapping();
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    set_dirty(0);
    add_recent_file(displayPath ? displayPath : path);
    free(displayPath);
    refresh_menu(g_hMainWnd);
    return 1;
}

static int do_saveas(void) {
    DWORD cap = 32768;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return 0;
    if (g_currentFile) wcsncpy(path, g_currentFile, cap - 1);
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"Markdown Files (*.md)\0*.md\0Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"md";
    int result = GetSaveFileNameW(&ofn) ? save_to_path(path) : 0;
    free(path);
    return result;
}

void do_save(void) {
    if (g_currentFile) {
        save_to_path(g_currentFile);
    } else {
        do_saveas();
    }
}

static void do_exit(void) {
    if (g_dirty) {
        int r = MessageBoxW(g_hMainWnd, L"Save changes before exiting?", L"MarkEdit", MB_YESNOCANCEL | MB_ICONWARNING);
        if (r == IDCANCEL) return;
        if (r == IDYES) {
            do_save();
            if (g_dirty) return;
        }
    }
    DestroyWindow(g_hMainWnd);
}

/* ---- Find / Replace (operates on the markdown source in g_hEdit) ---- */
static void find_in_edit(const char *needle, int matchCase, int down) {
    if (!needle || !needle[0]) return;
    int len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;

    int selStart = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int selEnd = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);

    int needleLen = (int)strlen(needle);
    int foundAt = -1;

    if (down) {
        int startFrom = (int)selEnd;
        for (int i = startFrom; i <= len - needleLen; i++) {
            int match = matchCase ? (strncmp(text + i, needle, needleLen) == 0)
                                   : (_strnicmp(text + i, needle, needleLen) == 0);
            if (match) { foundAt = i; break; }
        }
        if (foundAt < 0) {
            for (int i = 0; i <= (int)selStart && i <= len - needleLen; i++) {
                int match = matchCase ? (strncmp(text + i, needle, needleLen) == 0)
                                       : (_strnicmp(text + i, needle, needleLen) == 0);
                if (match) { foundAt = i; break; }
            }
        }
    } else {
        int startFrom = (int)selStart - 1;
        for (int i = startFrom; i >= 0; i--) {
            int match = matchCase ? (strncmp(text + i, needle, needleLen) == 0)
                                   : (_strnicmp(text + i, needle, needleLen) == 0);
            if (match) { foundAt = i; break; }
        }
        if (foundAt < 0) {
            for (int i = len - needleLen; i >= (int)selStart; i--) {
                int match = matchCase ? (strncmp(text + i, needle, needleLen) == 0)
                                       : (_strnicmp(text + i, needle, needleLen) == 0);
                if (match) { foundAt = i; break; }
            }
        }
    }

    if (foundAt >= 0) {
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)foundAt, (LPARAM)(foundAt + needleLen));
        SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
        SetFocus(g_hEdit);
    } else {
        MessageBeep(MB_ICONASTERISK);
    }
    free(text);
}

static void replace_selection_and_find(const char *findWhat, const char *replaceWith, int matchCase) {
    int selStart = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int selEnd = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (selEnd > selStart) {
        int len;
        char *text = get_edit_text(g_hEdit, &len);
        if (text) {
            int selLen = selEnd - selStart;
            char sel[256];
            if (selLen >= (int)sizeof(sel)) selLen = (int)sizeof(sel) - 1;
            memcpy(sel, text + selStart, (size_t)selLen);
            sel[selLen] = 0;
            int match = matchCase ? (strcmp(sel, findWhat) == 0) : (_stricmp(sel, findWhat) == 0);
            if (match) {
                SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)replaceWith);
            }
            free(text);
        }
    }
    find_in_edit(findWhat, matchCase, 1);
}

static void show_find_dialog(int replace) {
    if (g_hFindDlg) {
        SetFocus(g_hFindDlg);
        return;
    }
    ZeroMemory(&g_fr, sizeof(g_fr));
    g_fr.lStructSize = sizeof(FINDREPLACEA);
    g_fr.hwndOwner = g_hMainWnd;
    g_fr.lpstrFindWhat = g_findWhat;
    g_fr.wFindWhatLen = sizeof(g_findWhat);
    g_fr.lpstrReplaceWith = g_replaceWith;
    g_fr.wReplaceWithLen = sizeof(g_replaceWith);
    g_fr.Flags = FR_DOWN;

    if (replace) {
        g_hFindDlg = ReplaceTextA(&g_fr);
    } else {
        g_hFindDlg = FindTextA(&g_fr);
    }
}

static void handle_findmsg(FINDREPLACEA *fr) {
    if (fr->Flags & FR_DIALOGTERM) {
        g_hFindDlg = NULL;
        return;
    }
    int matchCase = (fr->Flags & FR_MATCHCASE) ? 1 : 0;
    int down = (fr->Flags & FR_DOWN) ? 1 : 0;
    if (fr->Flags & FR_FINDNEXT) {
        find_in_edit(fr->lpstrFindWhat, matchCase, down);
    } else if (fr->Flags & FR_REPLACE) {
        replace_selection_and_find(fr->lpstrFindWhat, fr->lpstrReplaceWith, matchCase);
    } else if (fr->Flags & FR_REPLACEALL) {
        int count = 0;
        SendMessageA(g_hEdit, SCI_SETSEL, 0, 0);
        for (;;) {
            int before = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
            find_in_edit(fr->lpstrFindWhat, matchCase, 1);
            int selStart = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
            int selEnd = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
            if (selEnd == selStart) break;
            if (selStart < before && count > 0) break; /* wrapped around, stop */
            SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)fr->lpstrReplaceWith);
            count++;
            if (count > 100000) break;
        }
        char msg[64];
        wsprintfA(msg, "Replaced %d occurrence(s).", count);
        MessageBoxA(g_hMainWnd, msg, "MarkEdit", MB_OK | MB_ICONINFORMATION);
    }
}

static void do_choose_font(void) {
    LOGFONTA lf;
    ZeroMemory(&lf, sizeof(lf));
    snprintf(lf.lfFaceName, sizeof(lf.lfFaceName), "%s", g_fontFaceName);
    HDC hdc = GetDC(g_hEdit);
    lf.lfHeight = -MulDiv(g_fontPointSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(g_hEdit, hdc);

    CHOOSEFONTA cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = g_hMainWnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOSCRIPTSEL;

    if (ChooseFontA(&cf)) {
        strncpy(g_fontFaceName, lf.lfFaceName, LF_FACESIZE - 1);
        g_fontFaceName[LF_FACESIZE - 1] = 0;
        g_fontPointSize = cf.iPointSize / 10;
        if (g_fontPointSize < 1) g_fontPointSize = 1;
        apply_editor_font(g_fontFaceName, g_fontPointSize);
        save_font_settings();
    }
}

/* ---- Help > Markdown Reference: a plain-text syntax cheat sheet. ---- */

static const char *HELP_REFERENCE_TEXT =
"MARKDOWN REFERENCE\r\n"
"===================\r\n"
"\r\n"
"HEADINGS\r\n"
"    # Heading 1\r\n"
"    ## Heading 2\r\n"
"    ### Heading 3\r\n"
"    #### Heading 4 (continue with # through Heading 6)\r\n"
"\r\n"
"    Or, for H1/H2, underline the line above:\r\n"
"    Heading 1          Heading 2\r\n"
"    =========          ---------\r\n"
"\r\n"
"EMPHASIS\r\n"
"    **bold**, *italic*, ***bold italic***, ~~strikethrough~~, `inline code`\r\n"
"\r\n"
"    Escape a character with a backslash to show it literally: \\*not italic\\*\r\n"
"\r\n"
"LINKS, IMAGES, AUTOLINKS\r\n"
"    [link text](https://example.com)\r\n"
"    ![alt text](https://example.com/image.png)\r\n"
"    <https://example.com>\r\n"
"    https://example.com (bare URLs are linked automatically)\r\n"
"\r\n"
"    Reference-style links:\r\n"
"    [link text][id]\r\n"
"    [id]: https://example.com\r\n"
"\r\n"
"LISTS\r\n"
"    - bullet item\r\n"
"    - another item\r\n"
"      - nested item (indent two spaces or more)\r\n"
"\r\n"
"    1. first\r\n"
"    2. second\r\n"
"       1. nested\r\n"
"\r\n"
"    5. an ordered list can start at any number\r\n"
"\r\n"
"    - [ ] an unchecked task\r\n"
"    - [x] a checked task\r\n"
"\r\n"
"BLOCKQUOTES\r\n"
"    > a quoted line\r\n"
"\r\n"
"CODE BLOCKS\r\n"
"    Indent with 4 spaces, or fence with triple backticks:\r\n"
"    ```\r\n"
"    code goes here\r\n"
"    ```\r\n"
"\r\n"
"    Fence with \"mermaid\" as the language to render a diagram:\r\n"
"    ```mermaid\r\n"
"    graph TD\r\n"
"      A --> B\r\n"
"    ```\r\n"
"\r\n"
"TABLES\r\n"
"    | Column A | Column B |\r\n"
"    |----------|----------|\r\n"
"    | 1        | 2        |\r\n"
"\r\n"
"FOOTNOTES\r\n"
"    A sentence with a note.[^1]\r\n"
"\r\n"
"    [^1]: The footnote text.\r\n"
"\r\n"
"HORIZONTAL RULES\r\n"
"    ---\r\n"
"    ***\r\n";

static void layout_help_window(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (g_hHelpEdit) MoveWindow(g_hHelpEdit, 0, 0, rc.right, rc.bottom, TRUE);
}

static LRESULT CALLBACK HelpWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hHelpEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                0, 0, 0, 0, hwnd, NULL, GetModuleHandleA(NULL), NULL);
            SendMessageA(g_hHelpEdit, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
            SetWindowTextA(g_hHelpEdit, HELP_REFERENCE_TEXT);
            return 0;
        }
        case WM_SIZE:
            layout_help_window(hwnd);
            return 0;
        case WM_TIMER:
            if (wParam == IDT_PREVIEW_UPDATE) {
                update_preview();
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_hHelpEdit = NULL;
            g_hHelpWnd = NULL;
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void show_help_reference(void) {
    if (g_hHelpWnd) {
        SetForegroundWindow(g_hHelpWnd);
        return;
    }

    static int classRegistered = 0;
    if (!classRegistered) {
        WNDCLASSEXA wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = HelpWndProc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "MarkEditHelpWndClass";
        wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassExA(&wc);
        classRegistered = 1;
    }

    g_hHelpWnd = CreateWindowExA(0, "MarkEditHelpWndClass", "Markdown Reference - MarkEdit",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 780,
        g_hMainWnd, NULL, GetModuleHandleA(NULL), NULL);

    if (!g_hHelpWnd) return;

    ShowWindow(g_hHelpWnd, SW_SHOW);
    UpdateWindow(g_hHelpWnd);
}

/* Menu text treats '&' as an accelerator marker; escape it as '&&' so a path
   containing '&' (rare, but legal on Windows) displays literally. */
static void escape_ampersands(const WCHAR *in, WCHAR *out, size_t outSize) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < outSize; i++) {
        if (in[i] == L'&' && o + 3 < outSize) out[o++] = L'&';
        out[o++] = in[i];
    }
    out[o] = 0;
}

static HMENU build_menu(void) {
    HMENU hMenuBar = CreateMenu();

    HMENU hFile = CreatePopupMenu();
    AppendMenuA(hFile, MF_STRING, IDM_FILE_NEW, "&New\tCtrl+N");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_OPEN, "&Open...\tCtrl+O");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_SAVE, "&Save\tCtrl+S");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_SAVEAS, "Save &As...\tCtrl+Shift+S");
    AppendMenuA(hFile, MF_SEPARATOR, 0, NULL);

    HMENU hRecent = CreatePopupMenu();
    if (g_recentFileCount == 0) {
        AppendMenuA(hRecent, MF_STRING | MF_GRAYED, 0, "(no recent files)");
    } else {
        for (int i = 0; i < g_recentFileCount; i++) {
            size_t cap = wcslen(g_recentFiles[i]) * 2 + 16;
            WCHAR *escaped = (WCHAR *)malloc(cap * sizeof(WCHAR));
            WCHAR *label = (WCHAR *)malloc((cap + 16) * sizeof(WCHAR));
            if (escaped && label) {
                escape_ampersands(g_recentFiles[i], escaped, cap);
                swprintf(label, cap + 16, L"&%d %ls", i + 1, escaped);
                AppendMenuW(hRecent, MF_STRING, IDM_FILE_RECENT_0 + i, label);
            }
            free(escaped);
            free(label);
        }
        AppendMenuA(hRecent, MF_SEPARATOR, 0, NULL);
        AppendMenuA(hRecent, MF_STRING, IDM_FILE_RECENT_CLEAR, "&Clear Recent Files");
    }
    AppendMenuA(hFile, MF_POPUP, (UINT_PTR)hRecent, "Recent &Files");
    AppendMenuA(hFile, MF_SEPARATOR, 0, NULL);

    AppendMenuA(hFile, MF_STRING, IDM_FILE_EXIT, "E&xit");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hFile, "&File");

    HMENU hEditMenu = CreatePopupMenu();
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_UNDO, "&Undo\tCtrl+Z");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_REDO, "&Redo\tCtrl+Y");
    AppendMenuA(hEditMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_CUT, "Cu&t\tCtrl+X");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_COPY, "&Copy\tCtrl+C");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_PASTE, "&Paste\tCtrl+V");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_DELETE, "De&lete\tDel");
    AppendMenuA(hEditMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_SELECTALL, "Select &All\tCtrl+A");
    AppendMenuA(hEditMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_FIND, "&Find...\tCtrl+F");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_FINDNEXT, "Find &Next\tF3");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_REPLACE, "&Replace...\tCtrl+H");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hEditMenu, "&Edit");

    HMENU hFormat = CreatePopupMenu();
    AppendMenuA(hFormat, MF_STRING, IDM_FORMAT_FONT, "&Font...");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hFormat, "F&ormat");

    HMENU hView = CreatePopupMenu();
    AppendMenuA(hView, MF_STRING | (g_previewVisible ? MF_CHECKED : MF_UNCHECKED), IDM_VIEW_TOGGLEPREVIEW, "Show &Preview\tCtrl+Shift+V");
    AppendMenuA(hView, MF_STRING | (g_editorVisible ? MF_CHECKED : MF_UNCHECKED), IDM_VIEW_TOGGLEEDITOR, "Show &Editor\tCtrl+Shift+E");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hView, MF_STRING | (g_previewDarkMode ? MF_CHECKED : MF_UNCHECKED), IDM_VIEW_TOGGLETHEME, "Use &Dark Mode");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hView, "&View");

    HMENU hHelp = CreatePopupMenu();
    AppendMenuA(hHelp, MF_STRING, IDM_HELP_REFERENCE, "&Markdown Reference");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hHelp, "&Help");

    return hMenuBar;
}

/* Rebuild and install the menu bar (used after the recent-files list changes,
   since Win32 menus aren't backed by a live model - the HMENU has to be
   replaced whenever its contents differ). */
static void refresh_menu(HWND hwnd) {
    HMENU oldMenu = GetMenu(hwnd);
    SetMenu(hwnd, build_menu());
    if (oldMenu) DestroyMenu(oldMenu);
}

#define SPLITTER_WIDTH 6
static int g_splitterPos = -1; /* pixel x of splitter, -1 = uninitialized (use 50%) */
static int g_dragging = 0;

static void set_webview_bounds(RECT wvRc, BOOL visible) {
    if (!g_webviewController) return;
    ICoreWebView2Controller_put_Bounds(g_webviewController, wvRc);
    ICoreWebView2Controller_put_IsVisible(g_webviewController, visible);
}

static void layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;

    if (g_editorVisible && !g_previewVisible) {
        MoveWindow(g_hEdit, 0, 0, w, h, TRUE);
        ShowWindow(g_hEdit, SW_SHOW);
        ShowWindow(g_hPreview, SW_HIDE);
        RECT zero = { 0, 0, 0, 0 };
        set_webview_bounds(zero, FALSE);
        return;
    }

    if (!g_editorVisible && g_previewVisible) {
        ShowWindow(g_hEdit, SW_HIDE);
        ShowWindow(g_hPreview, SW_SHOW);
        MoveWindow(g_hPreview, 0, 0, w, h, TRUE);
        RECT wvRc = { 0, 0, w, h };
        set_webview_bounds(wvRc, TRUE);
        return;
    }

    /* both visible (the !g_editorVisible && !g_previewVisible case is prevented by the toggle handlers) */
    if (g_splitterPos < 0) g_splitterPos = w / 2;
    if (g_splitterPos < 100) g_splitterPos = 100;
    if (g_splitterPos > w - 100) g_splitterPos = (w - 100 > 100) ? w - 100 : 100;

    ShowWindow(g_hEdit, SW_SHOW);
    ShowWindow(g_hPreview, SW_SHOW);
    MoveWindow(g_hEdit, 0, 0, g_splitterPos, h, TRUE);
    MoveWindow(g_hPreview, g_splitterPos + SPLITTER_WIDTH, 0, w - g_splitterPos - SPLITTER_WIDTH, h, TRUE);
    RECT wvRc = { 0, 0, w - g_splitterPos - SPLITTER_WIDTH, h };
    set_webview_bounds(wvRc, TRUE);
}

static void update_view_menu(HWND hwnd) {
    HMENU hMenuBar = GetMenu(hwnd);
    HMENU hView = GetSubMenu(hMenuBar, 3); /* File, Edit, Format, View */
    CheckMenuItem(hView, IDM_VIEW_TOGGLEPREVIEW, MF_BYCOMMAND | (g_previewVisible ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_TOGGLEEDITOR, MF_BYCOMMAND | (g_editorVisible ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_TOGGLETHEME, MF_BYCOMMAND | (g_previewDarkMode ? MF_CHECKED : MF_UNCHECKED));
}

static void toggle_preview(HWND hwnd) {
    if (g_previewVisible && !g_editorVisible) return; /* would leave nothing visible */
    g_previewVisible = !g_previewVisible;
    update_view_menu(hwnd);
    layout(hwnd);
}

static void toggle_editor(HWND hwnd) {
    if (g_editorVisible && !g_previewVisible) return; /* would leave nothing visible */
    g_editorVisible = !g_editorVisible;
    update_view_menu(hwnd);
    layout(hwnd);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_msgFindReplace) {
        handle_findmsg((FINDREPLACEA *)lParam);
        return 0;
    }
    switch (msg) {
        case WM_CREATE: {
            g_hMainWnd = hwnd;

            HDC hdc = GetDC(hwnd);
            int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
            g_fontMono = CreateFontA(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");
            ReleaseDC(hwnd, hdc);

            SetMenu(hwnd, build_menu());

            g_hEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "Scintilla", "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL,
                0, 0, 0, 0, hwnd, (HMENU)ID_EDIT, GetModuleHandleA(NULL), NULL);
            configure_scintilla(g_hEdit);
            apply_editor_font(g_fontFaceName, g_fontPointSize);

            // static const char *welcome =
            //     "# Welcome to MarkEdit\r\n\r\n"
            //     "Type **Markdown** on the left, see the *live preview* on the right.\r\n\r\n"
            //     "## Features\r\n"
            //     "- Headings (# ## ###)\r\n"
            //     "- **Bold**, *italic*, ***both***\r\n"
            //     "- `inline code` and indented code blocks\r\n"
            //     "- Bullet and numbered lists\r\n"
            //     "- > blockquotes\r\n"
            //     "- Horizontal rules with ---\r\n"
            //     "- Mermaid diagrams, with zoom (buttons or Ctrl+wheel) and scroll\r\n\r\n"
            //     "```mermaid\r\n"
            //     "graph TD\r\n"
            //     "  A[Type Markdown] --> B[Live HTML preview]\r\n"
            //     "  B --> C{Contains mermaid fence?}\r\n"
            //     "  C -- yes --> D[Render diagram]\r\n"
            //     "  C -- no --> E[Render text]\r\n"
            //     "```\r\n\r\n"
            //     "> Edits render instantly in the preview window.\r\n";
            // set_edit_text(g_hEdit, welcome);
            SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
            SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
            /* SCI_SETTEXT above fires SCN_MODIFIED notifications like any other edit,
               which flip g_dirty on via WM_NOTIFY; put the fresh document back to a
               clean/untitled state before the window is shown. */
            set_dirty(0);
            update_preview();

            apply_preview_host_theme();
            g_hPreview = CreateWindowExA(WS_EX_CLIENTEDGE, "STATIC", "",
                WS_CHILD | WS_VISIBLE,
                0, 0, 0, 0, hwnd, (HMENU)ID_PREVIEW, GetModuleHandleA(NULL), NULL);

            init_webview(hwnd);

            layout(hwnd);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            if ((HWND)lParam == g_hPreview && g_previewBkBrush) {
                HDC hdcStatic = (HDC)wParam;
                COLORREF bk = g_previewDarkMode ? RGB(0x0D, 0x11, 0x17) : RGB(0xFF, 0xFF, 0xFF);
                COLORREF fg = g_previewDarkMode ? RGB(0xE6, 0xED, 0xF3) : RGB(0x00, 0x00, 0x00);
                SetBkColor(hdcStatic, bk);
                SetTextColor(hdcStatic, fg);
                return (LRESULT)g_previewBkBrush;
            }
            break;
        }
        case WM_SIZE:
            layout(hwnd);
            return 0;
        case WM_SETCURSOR: {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (g_previewVisible && g_editorVisible && pt.x >= g_splitterPos && pt.x <= g_splitterPos + SPLITTER_WIDTH) {
                SetCursor(LoadCursorA(NULL, (LPCSTR)IDC_SIZEWE));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam);
            if (g_previewVisible && g_editorVisible && x >= g_splitterPos && x <= g_splitterPos + SPLITTER_WIDTH) {
                g_dragging = 1;
                SetCapture(hwnd);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_dragging) {
                g_splitterPos = GET_X_LPARAM(lParam);
                layout(hwnd);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_dragging) {
                g_dragging = 0;
                ReleaseCapture();
            }
            return 0;
        }
        case WM_DROPFILES:
            handle_dropfiles((HDROP)wParam);
            return 0;
        case WM_NOTIFY: {
            NMHDR *nm = (NMHDR *)lParam;
            if (nm->hwndFrom == g_hEdit && nm->code == SCN_MODIFIED) {
                struct SCNotification *scn = (struct SCNotification *)lParam;
                if (scn->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) {
                    set_dirty(1);
                    SetTimer(hwnd, IDT_PREVIEW_UPDATE, 150, NULL);
                    if (scn->linesAdded != 0) update_linenumber_margin_width(g_hEdit);
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            WORD cmd = LOWORD(wParam);
            if (cmd >= IDM_FILE_RECENT_0 && cmd < IDM_FILE_RECENT_0 + MAX_RECENT_FILES) {
                do_open_recent(cmd - IDM_FILE_RECENT_0);
                return 0;
            }
            switch (cmd) {
                case IDM_FILE_NEW: do_new(); break;
                case IDM_FILE_OPEN: do_open(); break;
                case IDM_FILE_SAVE: do_save(); break;
                case IDM_FILE_SAVEAS: do_saveas(); break;
                case IDM_FILE_RECENT_CLEAR:
                    clear_recent_files();
                    refresh_menu(hwnd);
                    break;
                case IDM_FILE_EXIT: do_exit(); break;
                case IDM_EDIT_UNDO: SendMessageA(g_hEdit, SCI_UNDO, 0, 0); break;
                case IDM_EDIT_REDO: SendMessageA(g_hEdit, SCI_REDO, 0, 0); break;
                case IDM_EDIT_CUT: SendMessageA(g_hEdit, SCI_CUT, 0, 0); break;
                case IDM_EDIT_COPY: SendMessageA(g_hEdit, SCI_COPY, 0, 0); break;
                case IDM_EDIT_PASTE: SendMessageA(g_hEdit, SCI_PASTE, 0, 0); break;
                case IDM_EDIT_DELETE: SendMessageA(g_hEdit, SCI_CLEAR, 0, 0); break;
                case IDM_EDIT_SELECTALL: SendMessageA(g_hEdit, SCI_SELECTALL, 0, 0); break;
                case IDM_EDIT_FIND: show_find_dialog(0); break;
                case IDM_EDIT_REPLACE: show_find_dialog(1); break;
                case IDM_EDIT_FINDNEXT:
                    if (g_findWhat[0]) find_in_edit(g_findWhat, (g_fr.Flags & FR_MATCHCASE) ? 1 : 0, 1);
                    else show_find_dialog(0);
                    break;
                case IDM_FORMAT_FONT: do_choose_font(); break;
                case IDM_VIEW_TOGGLEPREVIEW: toggle_preview(hwnd); break;
                case IDM_VIEW_TOGGLEEDITOR: toggle_editor(hwnd); break;
                case IDM_VIEW_TOGGLETHEME:
                    g_previewDarkMode = !g_previewDarkMode;
                    save_theme_settings();
                    apply_preview_theme();
                    apply_editor_theme(g_hEdit);
                    apply_preview_host_theme();
                    refresh_menu(hwnd);
                    break;
                case IDM_HELP_REFERENCE: show_help_reference(); break;
            }
            return 0;
        }
        case WM_CLOSE:
            do_exit();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, IDT_PREVIEW_UPDATE);
            if (g_webview3) { ICoreWebView2_3_Release(g_webview3); g_webview3 = NULL; }
            if (g_webview) { ICoreWebView2_Release(g_webview); g_webview = NULL; }
            if (g_webviewController) { ICoreWebView2Controller_Release(g_webviewController); g_webviewController = NULL; }
            for (int i = 0; i < g_recentFileCount; i++) free(g_recentFiles[i]);
            free(g_currentFile);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    WCHAR *cliFile = (argv && argc > 1) ? wcsdup_heap(argv[1]) : NULL;
    if (argv) LocalFree(argv);
    if (cliFile) {
        /* command-line launch to view a file: show just the rendered preview */
        g_previewVisible = 1;
        g_editorVisible = 0;
    } else {
        /* plain launch with no file: show just the editor */
        g_previewVisible = 0;
        g_editorVisible = 1;
    }

    if (!LoadLibraryW(L"Scintilla.dll")) {
        MessageBoxW(NULL, L"Could not load Scintilla.dll (expected next to markedit.exe).", L"MarkEdit", MB_OK | MB_ICONERROR);
        free(cliFile);
        return 1;
    }

    load_font_settings();
    load_theme_settings();
    load_recent_files();
    prune_missing_recent_files();

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    g_msgFindReplace = RegisterWindowMessageA(FINDMSGSTRINGA);

    HICON hIconBig = LoadIconA(hInstance, MAKEINTRESOURCEA(IDI_APPICON));
    HICON hIconSmall = (HICON)LoadImageA(hInstance, MAKEINTRESOURCEA(IDI_APPICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);

    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "MarkEditWndClass";
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = hIconBig;
    wc.hIconSm = hIconSmall;
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(WS_EX_ACCEPTFILES, "MarkEditWndClass", "Untitled - MarkEdit",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 750,
        NULL, NULL, hInstance, NULL);

    if (hIconBig) SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    if (hIconSmall) SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);

    if (cliFile) {
        load_file(cliFile);
    }
    free(cliFile);

    ACCEL accels[] = {
        { FVIRTKEY | FCONTROL, 'N', IDM_FILE_NEW },
        { FVIRTKEY | FCONTROL, 'O', IDM_FILE_OPEN },
        { FVIRTKEY | FCONTROL, 'S', IDM_FILE_SAVE },
        { FVIRTKEY | FCONTROL | FSHIFT, 'S', IDM_FILE_SAVEAS },
        { FVIRTKEY | FCONTROL, 'A', IDM_EDIT_SELECTALL },
        { FVIRTKEY | FCONTROL, 'Y', IDM_EDIT_REDO },
        { FVIRTKEY | FCONTROL, 'F', IDM_EDIT_FIND },
        { FVIRTKEY | FCONTROL, 'H', IDM_EDIT_REPLACE },
        { FVIRTKEY, VK_F3, IDM_EDIT_FINDNEXT },
        { FVIRTKEY | FCONTROL | FSHIFT, 'V', IDM_VIEW_TOGGLEPREVIEW },
        { FVIRTKEY | FCONTROL | FSHIFT, 'E', IDM_VIEW_TOGGLEEDITOR },
    };
    HACCEL hAccel = CreateAcceleratorTableA(accels, sizeof(accels) / sizeof(accels[0]));

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (g_hFindDlg && IsDialogMessage(g_hFindDlg, &msg)) continue;
        if (TranslateAcceleratorA(hwnd, hAccel, &msg)) continue;
        if (IsDialogMessage(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
