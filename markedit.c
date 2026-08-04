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
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <shlwapi.h>
#include <shellapi.h>
#include "markdown.h"

#include <WebView2.h>
#include "third_party/scintilla/include/Scintilla.h"
#include "third_party/lexilla/include/Lexilla.h"
#include "third_party/lexilla/include/SciLexer.h"

#define ID_EDIT    101
#define ID_PREVIEW 102
#define ID_TABS    103
#define IDT_PREVIEW_UPDATE 1
#define IDT_FILE_WATCH 2
#define IDT_SCROLL_SYNC 3
#define WM_APP_CLOSE_TAB (WM_APP + 1)
#define SPLITTER_WIDTH 6
#define TOOLBAR_HEIGHT 32
#define TAB_HEIGHT 28

#define IDM_FILE_NEW      201
#define IDM_FILE_OPEN     202
#define IDM_FILE_SAVE     203
#define IDM_FILE_SAVEAS   204
#define IDM_FILE_EXIT     205
#define IDM_FILE_RECENT_CLEAR 206
#define IDM_FILE_EXPORT_HTML 207
#define IDM_FILE_CLOSE     208
#define IDM_FILE_REOPEN_CLOSED 241
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
#define IDM_EDIT_GOTOLINE  209
#define IDM_FORMAT_FONT   220
#define IDM_VIEW_TOGGLEPREVIEW 230
#define IDM_VIEW_TOGGLEEDITOR  231
#define IDM_VIEW_THEME_GITHUB_LIGHT 232
#define IDM_VIEW_THEME_GITHUB_DARK  233
#define IDM_VIEW_PREVIEW_ON_LEFT 234
#define IDM_VIEW_NEXT_TAB        235
#define IDM_VIEW_PREVIOUS_TAB    236
#define IDM_HELP_REFERENCE 240

#define IDM_FMT_BOLD          260
#define IDM_FMT_ITALIC        261
#define IDM_FMT_STRIKETHROUGH 262
#define IDM_FMT_CODE          263
#define IDM_FMT_H1            264
#define IDM_FMT_H2            265
#define IDM_FMT_H3            266
#define IDM_FMT_QUOTE         267
#define IDM_FMT_BULLETLIST    268
#define IDM_FMT_NUMBERLIST    269
#define IDM_FMT_TASKLIST      270
#define IDM_FMT_LINK          271
#define IDM_FMT_IMAGE         272
#define IDM_FMT_TABLE         273
#define IDM_FMT_CODEBLOCK     274
#define IDM_FMT_FOOTNOTE      275
#define IDM_FMT_HRULE         276
#define IDM_FMT_ALERT         277

#define IDI_APPICON 1

static HWND g_hEdit; /* Scintilla control */
static HWND g_hPreview; /* WebView2 host child window */
static HWND g_hTabs;
static HWND g_hMainWnd;
static HFONT g_fontMono; /* used only by the plain-EDIT help-reference window */
static WCHAR g_fontFaceName[LF_FACESIZE] = L"Consolas";
static int g_fontPointSize = 10;

static ICoreWebView2 *g_webview = NULL;
static ICoreWebView2Controller *g_webviewController = NULL;
static ICoreWebView2_3 *g_webview3 = NULL;
static int g_webviewReady = 0;
static int g_ignoreNextEditorScroll = 0;
static EventRegistrationToken g_webMessageToken;
static int g_webMessageRegistered = 0;
static HBRUSH g_previewBkBrush = NULL; /* matches the preview host's WM_CTLCOLORSTATIC background to the theme, so it doesn't flash white before WebView2 paints */
static HBRUSH g_helpBkBrush = NULL;
static HBRUSH g_mainBkBrush = NULL; /* fills the toolbar strip and splitter gap in the main window, matching the theme instead of the system COLOR_WINDOW default */
static char *g_pendingHtmlBody = NULL; /* set if update requested before webview ready */
static WCHAR *g_exportPath = NULL;
static int g_previewDarkMode = 0;
/* Help window: a second top-level window with a read-only textbox showing a
   plain-text Markdown-syntax reference. Created on demand. */
static HWND g_hHelpWnd = NULL;
static HWND g_hHelpEdit = NULL;

#define MAX_DOCUMENTS 32
typedef struct Document {
    sptr_t sciDocument;
    WCHAR *path;
    int dirty;
    int hasBom;
    FILETIME fileTime;
    int hasFileTime;
    int externalChanged;
    int caret;
    int anchor;
    int firstVisibleLine;
    int xOffset;
    unsigned untitledNumber;
    int closedIndex;
} Document;

static Document g_documents[MAX_DOCUMENTS];
static int g_documentCount = 0;
static int g_activeDocument = 0;
static unsigned g_nextUntitledNumber = 1;
static int g_switchingDocument = 0;
static char g_tabTooltipAnsi[32768];

#define MAX_CLOSED_DOCUMENTS 10
static Document g_closedDocuments[MAX_CLOSED_DOCUMENTS];
static int g_closedDocumentCount = 0;

/* Most editor operations naturally target the selected tab. These aliases
   keep that code direct while the storage itself remains per-document. */
#define g_currentFile (g_documents[g_activeDocument].path)
#define g_dirty       (g_documents[g_activeDocument].dirty)
#define g_hasBom      (g_documents[g_activeDocument].hasBom)
static int g_previewVisible = 0;
static int g_editorVisible = 1;
static int g_previewOnLeft = 0;
static int g_splitterRatio = 500; /* left pane width as thousandths of the available split width */
static int g_splitterPos = -1; /* pixel x of splitter, initialized from g_splitterRatio */
static int g_commandLinePreviewMode = 0;
static RECT g_savedWindowRect;
static int g_haveSavedWindowRect = 0;
static int g_startMaximized = 0;

#define MAX_RECENT_FILES 10
static WCHAR *g_recentFiles[MAX_RECENT_FILES];
static int g_recentFileCount = 0;

static HWND g_hFindDlg = NULL;
static FINDREPLACEW g_fr;
static WCHAR g_findWhat[256] = L"";
static WCHAR g_replaceWith[256] = L"";
static UINT g_msgFindReplace;

void do_save(void);
static void update_preview(void);
void push_html_to_webview(const char *htmlFragment);
static void apply_preview_theme(void);
static void apply_editor_theme(HWND hEdit);
static void apply_markdown_lexer_styles(HWND hEdit);
static void apply_preview_host_theme(void);
static void apply_help_theme(void);
static void refresh_menu(HWND hwnd);
static void update_document_mapping(void);
static void update_title(void);
static void update_tab_label(int index);
static void switch_document(int index);
static void check_external_file_change(void);
static void prompt_reload_active_external_change(void);
static char *to_utf8(const WCHAR *s);
static void do_close_document_at(int index);
static void do_reopen_closed_tab(void);
static void save_session_documents(void);
static void restore_session_documents(void);
static void sync_preview_to_editor_scroll(void);

static WCHAR *wcsdup_heap(const WCHAR *value) {
    if (!value) return NULL;
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

static const char *editor_eol_sequence(void) {
    switch ((int)SendMessageA(g_hEdit, SCI_GETEOLMODE, 0, 0)) {
        case SC_EOL_CR: return "\r";
        case SC_EOL_LF: return "\n";
        default: return "\r\n";
    }
}

static int position_needs_leading_eol(int position) {
    if (position <= 0) return 0;
    int previous = (int)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)(position - 1), 0);
    return previous != '\r' && previous != '\n';
}

/* ================= formatting toolbar actions (operate on g_hEdit) ================= */

/* Wrap the current selection in a symmetric Markdown marker (e.g. "**" for
   bold), toggling it off if the selection is already wrapped in exactly that
   marker. With no selection, insert an empty marker pair and place the caret
   between them so typing continues inside it. */
static void toolbar_wrap_inline(const char *marker) {
    int selStart = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int selEnd = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    int markerLen = (int)strlen(marker);

    if (selEnd == selStart) {
        char pair[16];
        snprintf(pair, sizeof(pair), "%s%s", marker, marker);
        SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)pair);
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)(selStart + markerLen), (LPARAM)(selStart + markerLen));
        SetFocus(g_hEdit);
        return;
    }

    int len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;

    int innerStart = selStart + markerLen, innerEnd = selEnd - markerLen;
    int alreadyWrapped = selEnd - selStart >= markerLen * 2 &&
        memcmp(text + selStart, marker, (size_t)markerLen) == 0 &&
        memcmp(text + innerEnd, marker, (size_t)markerLen) == 0;

    if (alreadyWrapped) {
        char *inner = (char *)malloc((size_t)(innerEnd - innerStart) + 1);
        if (inner) {
            memcpy(inner, text + innerStart, (size_t)(innerEnd - innerStart));
            inner[innerEnd - innerStart] = 0;
            SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)selStart, (LPARAM)selEnd);
            SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)inner);
            SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)selStart, (LPARAM)(selStart + (innerEnd - innerStart)));
            free(inner);
        }
    } else {
        size_t selLen = (size_t)(selEnd - selStart);
        char *wrapped = (char *)malloc(selLen + (size_t)markerLen * 2 + 1);
        if (wrapped) {
            memcpy(wrapped, marker, (size_t)markerLen);
            memcpy(wrapped + markerLen, text + selStart, selLen);
            memcpy(wrapped + markerLen + selLen, marker, (size_t)markerLen);
            wrapped[markerLen * 2 + selLen] = 0;
            SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)wrapped);
            SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)(selStart + markerLen), (LPARAM)(selStart + markerLen + (int)selLen));
            free(wrapped);
        }
    }
    free(text);
    SetFocus(g_hEdit);
}

/* Returns the 0-based [firstLine, lastLine] range touched by the current
   selection (a single line if there is no selection). */
static void toolbar_selected_line_range(int *firstLine, int *lastLine) {
    int selStart = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int selEnd = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    *firstLine = (int)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)selStart, 0);
    *lastLine = (int)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)selEnd, 0);
    /* A selection ending exactly at the start of a line (e.g. after pressing
       Home then Shift+Down once) shouldn't pull that line into the range. */
    if (*lastLine > *firstLine && selEnd == (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)*lastLine, 0))
        (*lastLine)--;
}

/* How many bytes of `line` (of length lineLen) are one of the recognized
   line-prefix markers this toolbar manages (heading, quote, bullet, numbered,
   task). Returns 0 if none match. Used so re-clicking a different button on
   an already-prefixed line replaces the old prefix instead of stacking. */
static int toolbar_prefix_length(const char *line, int lineLen) {
    int i = 0;
    while (i < lineLen && (line[i] == ' ') && i < 3) i++; /* tolerate up to 3 leading spaces, like CommonMark */
    if (i < lineLen && line[i] == '>' ) {
        int n = i + 1;
        if (n < lineLen && (line[n] == ' ' || line[n] == '\t')) n++;
        return n;
    }
    if (i < lineLen && line[i] == '#') {
        int n = i;
        while (n < lineLen && line[n] == '#' && n - i < 6) n++;
        if (n < lineLen && line[n] != ' ' && line[n] != '\t') return 0;
        if (n < lineLen) n++;
        return n;
    }
    if (i < lineLen && (line[i] == '-' || line[i] == '*') ) {
        int n = i + 1;
        if (n == lineLen) return n;
        if (line[n] != ' ' && line[n] != '\t') return 0;
        if (line[n] == ' ' && n + 3 < lineLen && line[n + 1] == '[' &&
            (line[n + 2] == ' ' || line[n + 2] == 'x' || line[n + 2] == 'X') &&
            line[n + 3] == ']' &&
            (n + 4 == lineLen || line[n + 4] == ' ' || line[n + 4] == '\t')) {
            n += 4;
            if (n < lineLen) n++;
            return n;
        }
        return n + 1;
    }
    if (i < lineLen && line[i] >= '0' && line[i] <= '9') {
        int n = i;
        while (n < lineLen && line[n] >= '0' && line[n] <= '9') n++;
        if (n < lineLen && line[n] == '.') {
            n++;
            if (n < lineLen && line[n] != ' ' && line[n] != '\t') return 0;
            if (n < lineLen) n++;
            return n;
        }
    }
    return 0;
}

/* Apply `prefix` to every line in [firstLine, lastLine], replacing any
   existing recognized prefix on each line first (so switching from, say,
   H1 to H2, or bullet to numbered, doesn't leave the old marker behind).
   `numbered` renumbers sequential lines 1., 2., 3., etc.; when true, `prefix`
   is ignored and "N. " is generated per line instead. */
static void toolbar_apply_line_prefix(const char *prefix, int numbered) {
    int firstLine, lastLine;
    toolbar_selected_line_range(&firstLine, &lastLine);

    int len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;

    /* Build the replacement text for the whole line range in one pass, then
       replace it in a single SCI_REPLACESEL so this is one undo step. */
    size_t cap = (size_t)len + (size_t)(lastLine - firstLine + 1) * 16 + 1;
    char *out = (char *)malloc(cap);
    if (!out) { free(text); return; }
    size_t o = 0;
    int number = 1;

    int rangeStart = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)firstLine, 0);
    int rangeEnd = (int)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)lastLine, 0);

    /* For the numbered-list case, toggle the whole range off only if every
       line in it is already numbered (so clicking again removes numbering). */
    int allNumbered = 1;
    if (numbered) {
        for (int line = firstLine; line <= lastLine && allNumbered; line++) {
            int lineStart = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
            int lineEnd = (int)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)line, 0);
            const char *lineText = text + lineStart;
            int lineLen = lineEnd - lineStart;
            int indentLen = 0;
            while (indentLen < lineLen && lineText[indentLen] == ' ' && indentLen < 3) indentLen++;
            int prefixLen = toolbar_prefix_length(lineText, lineLen);
            int isNumeric = prefixLen > indentLen &&
                lineText[indentLen] >= '0' && lineText[indentLen] <= '9';
            if (!isNumeric) allNumbered = 0;
        }
    }

    const char *eol = editor_eol_sequence();
    size_t eolLen = strlen(eol);
    for (int line = firstLine; line <= lastLine; line++) {
        int lineStart = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
        int lineEnd = (int)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)line, 0);
        int lineLen = lineEnd - lineStart;
        const char *lineText = text + lineStart;

        int indentLen = 0;
        while (indentLen < lineLen && lineText[indentLen] == ' ' && indentLen < 3) indentLen++;
        int existingLen = toolbar_prefix_length(lineText, lineLen);
        int markerLen = existingLen > 0 ? existingLen - indentLen : 0;
        int hasExactPrefix = !numbered && prefix && markerLen == (int)strlen(prefix) &&
            memcmp(lineText + indentLen, prefix, (size_t)markerLen) == 0;

        char newPrefix[16];
        int newPrefixLen;
        if (hasExactPrefix || (numbered && allNumbered)) {
            newPrefixLen = 0; /* toggle off */
        } else if (numbered) {
            newPrefixLen = snprintf(newPrefix, sizeof(newPrefix), "%d. ", number++);
        } else {
            newPrefixLen = snprintf(newPrefix, sizeof(newPrefix), "%s", prefix);
        }

        memcpy(out + o, lineText, (size_t)indentLen);
        o += (size_t)indentLen;
        if (newPrefixLen > 0) {
            memcpy(out + o, newPrefix, (size_t)newPrefixLen);
            o += (size_t)newPrefixLen;
        }
        int contentStart = existingLen > 0 ? existingLen : indentLen;
        memcpy(out + o, lineText + contentStart, (size_t)(lineLen - contentStart));
        o += (size_t)(lineLen - contentStart);
        if (line < lastLine) {
            memcpy(out + o, eol, eolLen);
            o += eolLen;
        }
    }
    out[o] = 0;

    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)rangeStart, (LPARAM)rangeEnd);
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)out);
    free(out);
    free(text);
    SetFocus(g_hEdit);
}

/* Insert `before` + selection (or `placeholder` if empty) + `after`, then
   select the literal substring "url" inside `after` (both current callers'
   `after` is "](url)") so the user can immediately type the destination.
   Used for link/image inserts. */
static void toolbar_insert_template(const char *before, const char *placeholder, const char *after) {
    int selStart = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int selEnd = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    int len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;

    int hasSelection = selEnd > selStart;
    const char *middle = hasSelection ? text + selStart : placeholder;
    int middleLen = hasSelection ? (selEnd - selStart) : (int)strlen(placeholder);

    size_t beforeLen = strlen(before), afterLen = strlen(after);
    char *whole = (char *)malloc(beforeLen + (size_t)middleLen + afterLen + 1);
    if (whole) {
        memcpy(whole, before, beforeLen);
        memcpy(whole + beforeLen, middle, (size_t)middleLen);
        memcpy(whole + beforeLen + (size_t)middleLen, after, afterLen);
        whole[beforeLen + (size_t)middleLen + afterLen] = 0;
        SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)whole);

        const char *urlPlaceholder = strstr(after, "url");
        if (urlPlaceholder) {
            int urlStart = selStart + (int)beforeLen + middleLen + (int)(urlPlaceholder - after);
            SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)urlStart, (LPARAM)(urlStart + 3));
        } else if (!hasSelection) {
            int caret = selStart + (int)beforeLen + middleLen + (int)afterLen;
            SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)caret, (LPARAM)caret);
        }
        free(whole);
    }
    free(text);
    SetFocus(g_hEdit);
}

static void toolbar_insert_table(void) {
    const char *eol = editor_eol_sequence();
    int selStart = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);

    const char *leading = position_needs_leading_eol(selStart) ? eol : "";
    char table[168];
    snprintf(table, sizeof(table),
        "%s| Column 1 | Column 2 | Column 3 |%s"
        "| --- | --- | --- |%s"
        "| Cell | Cell | Cell |%s", leading, eol, eol, eol);
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)table);
    SetFocus(g_hEdit);
}

/* Wrap the current selection in a fenced code block (``` on its own line
   before and after). With no selection, insert an empty fence pair and
   place the caret right after the opening "```" so a language hint (e.g.
   "c") can be typed immediately. */
static void toolbar_insert_codeblock(void) {
    int selStart = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int selEnd = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    const char *eol = editor_eol_sequence();
    size_t eolLen = strlen(eol);
    const char *leading = position_needs_leading_eol(selStart) ? eol : "";
    size_t leadingLen = strlen(leading);

    if (selEnd == selStart) {
        char empty[36];
        snprintf(empty, sizeof(empty), "%s```%s%s```%s", leading, eol, eol, eol);
        SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)empty);
        int caret = selStart + (int)leadingLen + 3;
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)caret, (LPARAM)caret);
        SetFocus(g_hEdit);
        return;
    }

    int len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;
    size_t selLen = (size_t)(selEnd - selStart);
    size_t openLen = leadingLen + 3 + eolLen, closeLen = eolLen + 3 + eolLen;
    char *wrapped = (char *)malloc(openLen + selLen + closeLen + 1);
    if (wrapped) {
        memcpy(wrapped, leading, leadingLen);
        memcpy(wrapped + leadingLen, "```", 3);
        memcpy(wrapped + leadingLen + 3, eol, eolLen);
        memcpy(wrapped + openLen, text + selStart, selLen);
        memcpy(wrapped + openLen + selLen, eol, eolLen);
        memcpy(wrapped + openLen + selLen + eolLen, "```", 3);
        memcpy(wrapped + openLen + selLen + eolLen + 3, eol, eolLen);
        wrapped[openLen + selLen + closeLen] = 0;
        SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)wrapped);
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)(selStart + (int)openLen), (LPARAM)(selStart + (int)openLen + (int)selLen));
        free(wrapped);
    }
    free(text);
    SetFocus(g_hEdit);
}

static void toolbar_insert_horizontal_rule(void) {
    int position = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    const char *eol = editor_eol_sequence();
    const char *leading = position_needs_leading_eol(position) ? eol : "";
    char block[24];
    snprintf(block, sizeof(block), "%s---%s", leading, eol);
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)block);
    SetFocus(g_hEdit);
}

static void toolbar_insert_alert(void) {
    int selStart = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int selEnd = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    const char *eol = editor_eol_sequence();
    int len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;

    const char *body = selEnd > selStart ? text + selStart : "Alert text";
    int bodyLen = selEnd > selStart ? selEnd - selStart : 10;
    size_t cap = (size_t)bodyLen * 3 + strlen(eol) * 3 + 32;
    char *out = (char *)malloc(cap);
    if (!out) { free(text); return; }

    size_t o = 0;
    if (position_needs_leading_eol(selStart)) {
        size_t n = strlen(eol); memcpy(out + o, eol, n); o += n;
    }
    memcpy(out + o, "> [!NOTE]", 9); o += 9;
    size_t eolLen = strlen(eol); memcpy(out + o, eol, eolLen); o += eolLen;
    int lineStart = 1;
    for (int i = 0; i < bodyLen; i++) {
        if (lineStart) { memcpy(out + o, "> ", 2); o += 2; lineStart = 0; }
        char c = body[i];
        out[o++] = c;
        if (c == '\n') lineStart = 1;
    }
    if (o < 1 || (out[o - 1] != '\r' && out[o - 1] != '\n')) {
        memcpy(out + o, eol, eolLen); o += eolLen;
    }
    out[o] = 0;
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)out);
    free(out);
    free(text);
    SetFocus(g_hEdit);
}

static void toolbar_insert_footnote(void) {
    int len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;
    int next = 1;
    for (int i = 0; i + 3 < len; i++) {
        if (text[i] == '[' && text[i + 1] == '^' && text[i + 2] >= '0' && text[i + 2] <= '9') {
            int value = 0, j = i + 2;
            while (j < len && text[j] >= '0' && text[j] <= '9') value = value * 10 + (text[j++] - '0');
            if (j < len && text[j] == ']' && value >= next) next = value + 1;
        }
    }

    int selStart = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int selEnd = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    const char *note = selEnd > selStart ? text + selStart : "Footnote text";
    int noteLen = selEnd > selStart ? selEnd - selStart : 13;
    const char *eol = editor_eol_sequence();
    char reference[32], prefix[48];
    snprintf(reference, sizeof(reference), "[^%d]", next);
    snprintf(prefix, sizeof(prefix), "%s%s[^%d]: ", eol, eol, next);

    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)reference);
    int newLength = (int)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)newLength, (LPARAM)newLength);
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)prefix);
    int noteStart = (int)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    char *noteCopy = (char *)malloc((size_t)noteLen + 1);
    if (noteCopy) {
        memcpy(noteCopy, note, (size_t)noteLen); noteCopy[noteLen] = 0;
        SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)noteCopy);
        free(noteCopy);
    }
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)eol);
    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)noteStart, (LPARAM)(noteStart + noteLen));
    free(text);
    SetFocus(g_hEdit);
}

static void toolbar_command(WORD cmd) {
    switch (cmd) {
        case IDM_FMT_BOLD: toolbar_wrap_inline("**"); break;
        case IDM_FMT_ITALIC: toolbar_wrap_inline("*"); break;
        case IDM_FMT_STRIKETHROUGH: toolbar_wrap_inline("~~"); break;
        case IDM_FMT_CODE: toolbar_wrap_inline("`"); break;
        case IDM_FMT_H1: toolbar_apply_line_prefix("# ", 0); break;
        case IDM_FMT_H2: toolbar_apply_line_prefix("## ", 0); break;
        case IDM_FMT_H3: toolbar_apply_line_prefix("### ", 0); break;
        case IDM_FMT_QUOTE: toolbar_apply_line_prefix("> ", 0); break;
        case IDM_FMT_BULLETLIST: toolbar_apply_line_prefix("- ", 0); break;
        case IDM_FMT_NUMBERLIST: toolbar_apply_line_prefix(NULL, 1); break;
        case IDM_FMT_TASKLIST: toolbar_apply_line_prefix("- [ ] ", 0); break;
        case IDM_FMT_LINK: toolbar_insert_template("[", "link text", "](url)"); break;
        case IDM_FMT_IMAGE: toolbar_insert_template("![", "alt text", "](url)"); break;
        case IDM_FMT_TABLE: toolbar_insert_table(); break;
        case IDM_FMT_CODEBLOCK: toolbar_insert_codeblock(); break;
        case IDM_FMT_FOOTNOTE: toolbar_insert_footnote(); break;
        case IDM_FMT_HRULE: toolbar_insert_horizontal_rule(); break;
        case IDM_FMT_ALERT: toolbar_insert_alert(); break;
    }
}

/* Replace the Scintilla control's entire contents. Note SCI_SETTEXT is itself
   undoable (a delete-all + insert wrapped in one undo group); callers loading
   a fresh document (New/Open) should follow up with SCI_EMPTYUNDOBUFFER so the
   previous document's content isn't reachable via Undo. */
static void set_edit_text(HWND hEdit, const char *text) {
    int eolMode = SC_EOL_CRLF;
    const char *firstCr = strchr(text, '\r');
    const char *firstLf = strchr(text, '\n');
    if (firstLf && (!firstCr || firstLf < firstCr)) {
        eolMode = SC_EOL_LF;
    } else if (firstCr) {
        eolMode = firstCr[1] == '\n' ? SC_EOL_CRLF : SC_EOL_CR;
    }
    SendMessageA(hEdit, SCI_SETEOLMODE, (WPARAM)eolMode, 0);
    SendMessageA(hEdit, SCI_SETTEXT, 0, (LPARAM)text);
}

/* Configure the Scintilla control as a Markdown source editor: Lexilla's
   Markdown lexer for syntax highlighting (a passive read-only colorizer -
   it never edits the buffer), real tab characters at a width of 4, CRLF
   line endings to match the rest of the app's file I/O, and only the
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

/* Apply GitHub's light/dark palette to the editor so it matches the preview.
   Called once at startup and whenever the selected theme changes. */
static void apply_editor_theme(HWND hEdit) {
    if (g_previewDarkMode) {
        SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_DEFAULT, RGB(0xE6, 0xED, 0xF3));
        SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_DEFAULT, RGB(0x0D, 0x11, 0x17));
        SendMessageA(hEdit, SCI_STYLECLEARALL, 0, 0);

        SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_LINENUMBER, RGB(0x7D, 0x85, 0x90));
        SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_LINENUMBER, RGB(0x16, 0x1B, 0x22));

        SendMessageA(hEdit, SCI_SETCARETLINEBACK, RGB(0x16, 0x1B, 0x22), 0);
        SendMessageA(hEdit, SCI_SETCARETFORE, RGB(0xE6, 0xED, 0xF3), 0);
        SendMessageA(hEdit, SCI_SETSELBACK, TRUE, RGB(0x26, 0x4F, 0x78));
        SendMessageA(hEdit, SCI_SETSELFORE, TRUE, RGB(0xFF, 0xFF, 0xFF));
    } else {
        SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_DEFAULT, RGB(0x1F, 0x23, 0x28));
        SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_DEFAULT, RGB(0xFF, 0xFF, 0xFF));
        SendMessageA(hEdit, SCI_STYLECLEARALL, 0, 0);

        SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_LINENUMBER, RGB(0x65, 0x6D, 0x76));
        SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_LINENUMBER, RGB(0xF6, 0xF8, 0xFA));

        SendMessageA(hEdit, SCI_SETCARETLINEBACK, RGB(0xF0, 0xF3, 0xF6), 0);
        SendMessageA(hEdit, SCI_SETCARETFORE, RGB(0x1F, 0x23, 0x28), 0);
        SendMessageA(hEdit, SCI_SETSELBACK, TRUE, RGB(0xB6, 0xD7, 0xFF));
        SendMessageA(hEdit, SCI_SETSELFORE, TRUE, RGB(0x1F, 0x23, 0x28));
    }
    apply_markdown_lexer_styles(hEdit);
    InvalidateRect(hEdit, NULL, TRUE);
}

/* Color the Markdown lexer's styles (see third_party/lexilla/include/
   SciLexer.h, SCE_MARKDOWN_*) to match the GitHub light/dark palette used
   everywhere else in the app. Called from apply_editor_theme() after every
   SCI_STYLECLEARALL, since that resets every style back to STYLE_DEFAULT. */
static void apply_markdown_lexer_styles(HWND hEdit) {
    COLORREF text = g_previewDarkMode ? RGB(0xE6, 0xED, 0xF3) : RGB(0x1F, 0x23, 0x28);
    COLORREF heading = g_previewDarkMode ? RGB(0x79, 0xC0, 0xFF) : RGB(0x09, 0x69, 0xDA);
    COLORREF link = g_previewDarkMode ? RGB(0x58, 0xA6, 0xFF) : RGB(0x09, 0x69, 0xDA);
    COLORREF code = g_previewDarkMode ? RGB(0xFF, 0xA6, 0x57) : RGB(0xB3, 0x53, 0x00);
    COLORREF quote = g_previewDarkMode ? RGB(0x8B, 0x94, 0x9E) : RGB(0x5A, 0x5A, 0x5A);
    COLORREF list = g_previewDarkMode ? RGB(0xD2, 0xA8, 0xFF) : RGB(0x82, 0x50, 0xDF);
    COLORREF strike = g_previewDarkMode ? RGB(0x8B, 0x94, 0x9E) : RGB(0x5A, 0x5A, 0x5A);
    COLORREF rule = g_previewDarkMode ? RGB(0x7D, 0x85, 0x90) : RGB(0x65, 0x6D, 0x76);

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

/* (Re)creates g_previewBkBrush to match g_previewDarkMode, so the preview
   host STATIC control's WM_CTLCOLORSTATIC background doesn't flash white/
   light when starting (or switching to) dark mode, before WebView2 itself
   has painted anything. */
static void apply_preview_host_theme(void) {
    if (g_previewBkBrush) DeleteObject(g_previewBkBrush);
    g_previewBkBrush = CreateSolidBrush(g_previewDarkMode ? RGB(0x0D, 0x11, 0x17) : RGB(0xFF, 0xFF, 0xFF));
    if (g_hPreview) InvalidateRect(g_hPreview, NULL, TRUE);
}

/* (Re)creates g_mainBkBrush to match g_previewDarkMode, and repaints the
   parts of the main window's own background it's used for (the toolbar
   strip and the splitter gap - everywhere else is covered by child windows). */
static void apply_main_theme(HWND hwnd) {
    if (g_mainBkBrush) DeleteObject(g_mainBkBrush);
    g_mainBkBrush = CreateSolidBrush(g_previewDarkMode ? RGB(0x0D, 0x11, 0x17) : RGB(0xFF, 0xFF, 0xFF));
    if (hwnd) InvalidateRect(hwnd, NULL, FALSE);
}

static void apply_help_theme(void) {
    if (!g_hHelpEdit && !g_hHelpWnd) return;
    if (g_helpBkBrush) DeleteObject(g_helpBkBrush);
    g_helpBkBrush = CreateSolidBrush(g_previewDarkMode
        ? RGB(0x0D, 0x11, 0x17)
        : RGB(0xFF, 0xFF, 0xFF));
    if (g_hHelpEdit) InvalidateRect(g_hHelpEdit, NULL, TRUE);
    if (g_hHelpWnd) InvalidateRect(g_hHelpWnd, NULL, TRUE);
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

    /* Syntax-highlight the Markdown source using Lexilla's bundled lexer
       (vendored trimmed to just this one language - see third_party/lexilla).
       ILexer5* ownership passes to Scintilla once handed to SCI_SETILEXER. */
    void *lexer = CreateLexer("markdown");
    if (lexer) SendMessageA(hEdit, SCI_SETILEXER, 0, (LPARAM)lexer);

    /* Explicit text colors, rather than relying on Scintilla's own defaults,
       so the caret-line highlight below is guaranteed to contrast with them.
       apply_editor_theme() sets the actual color values (light or dark),
       including the Markdown lexer's per-construct colors set above. */
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
   HFONT). SCI_STYLECLEARALL below resets every style - including the
   Markdown lexer's per-construct styles - back to STYLE_DEFAULT's font, so
   apply_editor_theme() must run afterwards to reapply them. */
static void apply_editor_font(const WCHAR *faceName, int pointSize) {
    char *faceUtf8 = to_utf8(faceName);
    if (!faceUtf8) faceUtf8 = _strdup("Consolas");
    if (!faceUtf8) return;
    SendMessageA(g_hEdit, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)faceUtf8);
    SendMessageA(g_hEdit, SCI_STYLESETSIZE, STYLE_DEFAULT, pointSize);
    SendMessageA(g_hEdit, SCI_STYLECLEARALL, 0, 0); /* copies STYLE_DEFAULT (fore/back included) to all other styles, including STYLE_LINENUMBER */

    /* STYLECLEARALL just overwrote STYLE_LINENUMBER's fore/back with
       STYLE_DEFAULT's, undoing apply_editor_theme()'s margin colors (which
       are deliberately different from the text area). Re-apply the theme so
       the gutter doesn't revert to the wrong colors after a font change. */
    apply_editor_theme(g_hEdit);

    /* The margin's digit width depends on the font, so keep both in sync. */
    SendMessageA(g_hEdit, SCI_STYLESETFONT, STYLE_LINENUMBER, (LPARAM)faceUtf8);
    SendMessageA(g_hEdit, SCI_STYLESETSIZE, STYLE_LINENUMBER, pointSize);
    update_linenumber_margin_width(g_hEdit);
    free(faceUtf8);
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
    GetPrivateProfileStringW(L"Font", L"FaceName", L"Consolas",
                             g_fontFaceName, LF_FACESIZE, path);
    g_fontPointSize = GetPrivateProfileIntW(L"Font", L"PointSize", g_fontPointSize, path);
    if (g_fontPointSize < 1) g_fontPointSize = 1;
    free(path);
}

static void load_theme_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WCHAR value[32];
    GetPrivateProfileStringW(L"Preview", L"Theme", L"", value, 32, path);
    if (_wcsicmp(value, L"github-dark") == 0) {
        g_previewDarkMode = 1;
    } else if (_wcsicmp(value, L"github-light") == 0) {
        g_previewDarkMode = 0;
    } else {
        /* Migrate settings written by releases that exposed a DarkMode toggle. */
        GetPrivateProfileStringW(L"Preview", L"DarkMode", L"0", value, 32, path);
        g_previewDarkMode = (value[0] == L'1' || value[0] == L't' || value[0] == L'T' ||
                             value[0] == L'y' || value[0] == L'Y');
    }
    free(path);
}

static void save_theme_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WritePrivateProfileStringW(L"Preview", L"Theme",
                               g_previewDarkMode ? L"github-dark" : L"github-light", path);
    WritePrivateProfileStringW(L"Preview", L"DarkMode", NULL, path);
    free(path);
}

static int read_profile_int(const WCHAR *section, const WCHAR *key, int defaultValue,
                            const WCHAR *path) {
    WCHAR fallback[32], value[32], *end = NULL;
    swprintf(fallback, 32, L"%d", defaultValue);
    GetPrivateProfileStringW(section, key, fallback, value, 32, path);
    long parsed = wcstol(value, &end, 10);
    return end && end != value && *end == 0 ? (int)parsed : defaultValue;
}

static void write_profile_int(const WCHAR *section, const WCHAR *key, int value,
                              const WCHAR *path) {
    WCHAR buffer[32];
    swprintf(buffer, 32, L"%d", value);
    WritePrivateProfileStringW(section, key, buffer, path);
}

static void load_layout_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    g_previewOnLeft = read_profile_int(L"View", L"PreviewOnLeft", 0, path) != 0;
    g_previewVisible = read_profile_int(L"View", L"PreviewVisible", g_previewVisible, path) != 0;
    g_editorVisible = read_profile_int(L"View", L"EditorVisible", g_editorVisible, path) != 0;
    if (!g_previewVisible && !g_editorVisible) g_editorVisible = 1;

    g_splitterRatio = read_profile_int(L"View", L"SplitterRatio", 500, path);
    if (g_splitterRatio < 50) g_splitterRatio = 50;
    if (g_splitterRatio > 950) g_splitterRatio = 950;

    int left = read_profile_int(L"Window", L"Left", 0, path);
    int top = read_profile_int(L"Window", L"Top", 0, path);
    int width = read_profile_int(L"Window", L"Width", 0, path);
    int height = read_profile_int(L"Window", L"Height", 0, path);
    RECT candidate = { left, top, left + width, top + height };
    if (width >= 400 && height >= 300 &&
        MonitorFromRect(&candidate, MONITOR_DEFAULTTONULL) != NULL) {
        g_savedWindowRect = candidate;
        g_haveSavedWindowRect = 1;
    }
    g_startMaximized = read_profile_int(L"Window", L"Maximized", 0, path) != 0;
    free(path);
}

static void save_layout_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    write_profile_int(L"View", L"PreviewOnLeft", g_previewOnLeft, path);
    if (!g_commandLinePreviewMode) {
        write_profile_int(L"View", L"PreviewVisible", g_previewVisible, path);
        write_profile_int(L"View", L"EditorVisible", g_editorVisible, path);
    }

    if (g_hMainWnd && g_splitterPos >= 0) {
        RECT client;
        GetClientRect(g_hMainWnd, &client);
        int available = client.right - SPLITTER_WIDTH;
        if (available > 0) {
            g_splitterRatio = MulDiv(g_splitterPos, 1000, available);
            if (g_splitterRatio < 50) g_splitterRatio = 50;
            if (g_splitterRatio > 950) g_splitterRatio = 950;
        }
    }
    write_profile_int(L"View", L"SplitterRatio", g_splitterRatio, path);

    WINDOWPLACEMENT placement = {0};
    placement.length = sizeof(placement);
    if (g_hMainWnd && GetWindowPlacement(g_hMainWnd, &placement)) {
        RECT normal = placement.rcNormalPosition;
        int maximized = placement.showCmd == SW_SHOWMAXIMIZED ||
            (placement.showCmd == SW_SHOWMINIMIZED &&
             (placement.flags & WPF_RESTORETOMAXIMIZED) != 0);
        write_profile_int(L"Window", L"Left", normal.left, path);
        write_profile_int(L"Window", L"Top", normal.top, path);
        write_profile_int(L"Window", L"Width", normal.right - normal.left, path);
        write_profile_int(L"Window", L"Height", normal.bottom - normal.top, path);
        write_profile_int(L"Window", L"Maximized", maximized, path);
    }
    free(path);
}

static void save_font_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WritePrivateProfileStringW(L"Font", L"FaceName", g_fontFaceName, path);
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

/* Used only when the WebView2 Runtime lacks ICoreWebView2_3 (no virtual-host
   folder mapping), so NavigateToString() gives this page an opaque origin
   that cannot resolve relative script/stylesheet URLs. mermaid.min.js, KaTeX,
   and highlight.js are therefore left out entirely rather than added as
   broken references. Some shared feature styling remains for parity with
   preview_shell.html, although those features are inactive in this fallback. */
static const char *HTML_SHELL =
"<!doctype html><html><head><meta charset=\"utf-8\">"
"<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src https: http: data:; font-src data:; connect-src 'none'; object-src 'none'; frame-src 'none'; form-action 'none'\">"
"<style>"
"html,body{margin:0;padding:0;background:#ffffff;font-family:'Segoe UI',sans-serif;color:#141414;height:100%;color-scheme:light;}"
"#content{padding:12px 20px 60px 20px;min-height:calc(100% - 72px);display:flex;flex-direction:column;}"
"#content.solo-diagram{padding:12px 20px;height:calc(100vh - 24px);min-height:calc(100vh - 24px);}"
"h1{font-size:1.9em;font-weight:700;margin:0.5em 0 0.3em 0;}"
"h2{font-size:1.5em;font-weight:700;margin:0.5em 0 0.3em 0;}"
"h3{font-size:1.2em;font-weight:700;margin:0.5em 0 0.3em 0;}"
"p{margin:0.4em 0;line-height:1.5;}"
"code{background:#f0f0f0;padding:1px 4px;border-radius:3px;font-family:Consolas,monospace;font-size:0.95em;}"
"pre.codeblock{background:#eaeaea;padding:8px 10px;border-radius:4px;overflow-x:auto;}"
"pre.codeblock code{background:none;padding:0;}"
".markdown-body pre{position:relative;}"
".code-copy-button{position:absolute;top:6px;right:6px;border:1px solid #d0d7de;border-radius:5px;background:#f6f8fa;color:#1f2328;padding:3px 8px;cursor:pointer;}"
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
".mermaid-wrap{position:relative;border:1px solid #e0e0e0;border-radius:6px;margin:0.6em 0;overflow:auto;background:#fff;max-height:70vh;cursor:grab;}"
".mermaid-wrap.panning{cursor:grabbing;user-select:none;}"
".mermaid-inner{transform-origin:0 0;display:inline-block;padding:12px;}"
".mermaid-toolbar{position:sticky;top:0;left:0;display:flex;gap:4px;justify-content:flex-end;padding:4px;background:rgba(255,255,255,0.9);border-bottom:1px solid #eee;z-index:5;}"
".mermaid-toolbar button{cursor:pointer;border:1px solid #ccc;background:#fafafa;border-radius:4px;width:26px;height:24px;font-size:14px;line-height:1;}"
".mermaid-toolbar .zoom-readout{width:52px;font-variant-numeric:tabular-nums;}"
".mermaid-toolbar button:hover{background:#eee;}"
".markdown-body blockquote.markdown-alert{--alert-color:#0969da;margin:1rem 0;padding:.5rem 1rem;border-left:.25em solid var(--alert-color);color:inherit;font-style:normal;}"
".markdown-alert>:first-child{margin-top:0;}"
".markdown-alert>:last-child{margin-bottom:0;}"
".markdown-alert-title{margin:0 0 .35rem!important;color:var(--alert-color);font-weight:600;line-height:1.5;}"
".markdown-alert-note{--alert-color:#0969da;}"
".markdown-alert-tip{--alert-color:#1a7f37;}"
".markdown-alert-important{--alert-color:#8250df;}"
".markdown-alert-warning{--alert-color:#9a6700;}"
".markdown-alert-caution{--alert-color:#cf222e;}"
".heading-anchor{margin-left:.35em;color:inherit!important;text-decoration:none!important;opacity:0;}"
"h1:hover>.heading-anchor,h2:hover>.heading-anchor,h3:hover>.heading-anchor,h4:hover>.heading-anchor,h5:hover>.heading-anchor,h6:hover>.heading-anchor,.heading-anchor:focus{opacity:.55;}"
".document-outline{position:fixed;top:8px;right:12px;z-index:20;max-width:min(300px,70vw);border:1px solid #d0d7de;border-radius:6px;background:rgba(255,255,255,.96);box-shadow:0 3px 12px rgba(31,35,40,.15);font-size:13px;}"
".document-outline[hidden]{display:none;}"
".document-outline summary{cursor:pointer;padding:5px 9px;font-weight:600;user-select:none;}"
".document-outline nav{max-height:60vh;overflow:auto;border-top:1px solid #d8dee4;padding:6px 8px 8px;}"
".document-outline ol{margin:0;padding:0;list-style:none;}"
".document-outline li{margin:2px 0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}"
".document-outline a{color:#0969da;text-decoration:none;}"
".document-outline a:hover{text-decoration:underline;}"
".math-display{margin:1em 0;overflow-x:auto;overflow-y:hidden;text-align:center;}"
".math-display>.katex-display{margin:0;padding:.2em 0;}"
".theme-dark,.theme-dark body{background:#0d1117;color:#e6edf3;color-scheme:dark;}"
".theme-dark code{background:#161b22;color:#e6edf3;}"
".theme-dark pre.codeblock{background:#161b22;}"
".theme-dark .code-copy-button{border-color:#30363d;background:#21262d;color:#e6edf3;}"
".theme-dark blockquote{border-left-color:#30363d;color:#8b949e;}"
".theme-dark hr{border-top-color:#30363d;}"
".theme-dark th,.theme-dark td{border-color:#30363d;}"
".theme-dark th{background:#161b22;}"
".theme-dark a{color:#58a6ff;}"
".theme-dark .mermaid-wrap{border-color:#30363d;background:#0d1117;}"
".theme-dark .mermaid-toolbar{background:rgba(13,17,23,0.9);border-color:#30363d;}"
".theme-dark .mermaid-toolbar button{background:#161b22;color:#e6edf3;border-color:#30363d;}"
".theme-dark .mermaid-toolbar button:hover{background:#21262d;}"
".theme-dark .document-outline{border-color:#30363d;background:rgba(13,17,23,.96);box-shadow:0 3px 12px rgba(0,0,0,.45);}"
".theme-dark .document-outline nav{border-color:#30363d;}"
".theme-dark .document-outline a{color:#58a6ff;}"
".theme-dark .markdown-body blockquote.markdown-alert{border-left-color:var(--alert-color);color:inherit;}"
"</style>"
"</head><body>"
"<details id=\"document-outline\" class=\"document-outline\" hidden><summary>Outline</summary><nav aria-label=\"Document outline\"><ol></ol></nav></details>"
"<div id=\"content\"></div>"
"<script>"
/* mermaid.min.js is intentionally not <script src>'d here: this shell is
   only used via NavigateToString(), whose opaque origin cannot resolve a
   relative script URL. Diagrams render as plain fenced code on this path. */
"function applyTheme(theme){"
"  const isDark = theme === 'dark';"
"  document.documentElement.classList.toggle('theme-dark', isDark);"
"  document.body.classList.toggle('theme-dark', isDark);"
"}"
"window.setTheme = function(theme){ applyTheme(theme === 'dark' ? 'dark' : 'light'); };"
"let suppressScrollPost=false,scrollPostPending=false;"
"function sourceLine(el){const m=el&&el.getAttribute('data-sourcepos')?.match(/^(\\d+):/);return m?Number(m[1]):0;}"
"function sourceBlocks(){return Array.from(document.querySelectorAll('#content [data-sourcepos]')).filter((el)=>sourceLine(el)>0&&el.offsetParent!==null);}"
"function sourceTop(el){return el.getBoundingClientRect().top+window.scrollY;}"
"window.scrollToSourceLine=function(line){const blocks=sourceBlocks();if(!blocks.length)return;let current=blocks[0],next=null;for(let i=0;i<blocks.length;i++){if(sourceLine(blocks[i])>line){next=blocks[i];break;}current=blocks[i];}let top=sourceTop(current);if(next){const first=sourceLine(current),last=sourceLine(next),ratio=last>first?Math.max(0,Math.min(1,(line-first)/(last-first))):0;top+=(sourceTop(next)-sourceTop(current))*ratio;}suppressScrollPost=true;window.scrollTo(0,Math.max(0,top-12));setTimeout(()=>suppressScrollPost=false,80);};"
"window.addEventListener('scroll',()=>{if(suppressScrollPost||scrollPostPending)return;scrollPostPending=true;requestAnimationFrame(()=>{scrollPostPending=false;if(suppressScrollPost||!window.chrome?.webview)return;const blocks=sourceBlocks();if(!blocks.length)return;const top=window.scrollY+18;let current=blocks[0];for(const block of blocks){if(sourceTop(block)>top)break;current=block;}const line=sourceLine(current);if(line)window.chrome.webview.postMessage('scroll:'+line);});},{passive:true});"
"function enhanceAlerts(root){"
"  const names={NOTE:'Note',TIP:'Tip',IMPORTANT:'Important',WARNING:'Warning',CAUTION:'Caution'};"
"  root.querySelectorAll('blockquote').forEach((quote)=>{"
"    const p=quote.firstElementChild;const text=p&&p.tagName==='P'?p.firstChild:null;"
"    if(!text||text.nodeType!==Node.TEXT_NODE)return;"
"    const match=text.nodeValue.match(/^\\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\\](?:[ \\t]*\\n|[ \\t]+|$)/i);"
"    if(!match)return;const type=match[1].toUpperCase();text.nodeValue=text.nodeValue.slice(match[0].length);"
"    quote.classList.add('markdown-alert','markdown-alert-'+type.toLowerCase());"
"    const title=document.createElement('p');title.className='markdown-alert-title';title.textContent=names[type];quote.insertBefore(title,p);"
"    if(p.textContent.trim()===''&&p.children.length===0)p.remove();"
"  });"
"}"
"function headingSlug(text){return text.toLowerCase().normalize('NFKD').replace(/[\\u0300-\\u036f]/g,'').replace(/['’]/g,'').replace(/[^a-z0-9\\u00c0-\\uffff]+/g,'-').replace(/^-+|-+$/g,'')||'section';}"
"function navigateToHeading(e,heading,id){e.preventDefault();heading.scrollIntoView({behavior:'smooth',block:'start'});const url=new URL(window.location.href);url.hash=id;history.replaceState(null,'',url.href);}"
"function enhanceFragmentLinks(root){"
"  root.querySelectorAll('a[href^=\"#\"]').forEach((link)=>{const raw=link.getAttribute('href');if(!raw||raw.length<2)return;"
"    link.addEventListener('click',(e)=>{let id;try{id=decodeURIComponent(raw.slice(1));}catch(_){return;}const target=document.getElementById(id);if(target)navigateToHeading(e,target,id);});"
"  });"
"}"
"function enhanceHeadings(root){"
"  const headings=Array.from(root.querySelectorAll('h1,h2,h3,h4,h5,h6'));const outline=document.getElementById('document-outline');const list=outline.querySelector('ol');const used=new Map();list.replaceChildren();"
"  headings.forEach((heading)=>{const label=heading.textContent.trim();const base=headingSlug(label);const duplicate=used.get(base)||0;used.set(base,duplicate+1);const id=duplicate?base+'-'+duplicate:base;heading.id=id;"
"    const anchor=document.createElement('a');anchor.className='heading-anchor';anchor.href='#'+id;anchor.textContent='#';anchor.setAttribute('aria-label','Permalink to '+label);anchor.addEventListener('click',(e)=>navigateToHeading(e,heading,id));heading.appendChild(anchor);"
"    const item=document.createElement('li');item.style.marginLeft=((Number(heading.tagName.slice(1))-1)*10)+'px';const link=document.createElement('a');link.href='#'+id;link.textContent=label;link.addEventListener('click',(e)=>navigateToHeading(e,heading,id));item.appendChild(link);list.appendChild(item);"
"  });outline.hidden=headings.length<2;if(outline.hidden)outline.open=false;"
"}"
"function enhanceCodeCopy(root){root.querySelectorAll('pre > code').forEach((code)=>{const pre=code.parentElement;if(!pre||pre.querySelector('.code-copy-button'))return;"
"  const button=document.createElement('button');button.type='button';button.className='code-copy-button';button.textContent='Copy';button.onclick=()=>{"
"    const area=document.createElement('textarea');area.value=code.textContent;area.style.position='fixed';area.style.opacity='0';document.body.appendChild(area);area.select();document.execCommand('copy');area.remove();button.textContent='Copied';setTimeout(()=>button.textContent='Copy',1200);};pre.appendChild(button);});}"
"function setContent(html){"
"  document.getElementById('content').innerHTML = '<article class=\"markdown-body\">' + html + '</article>';"
"  const article=document.querySelector('#content > .markdown-body');enhanceAlerts(article);enhanceFragmentLinks(article);enhanceHeadings(article);enhanceCodeCopy(article);"
"}"
"window.setContent = setContent;"
"window.exportRenderedHTML=function(baseHref,title){const source=document.querySelector('#content > .markdown-body');if(!source)return '';const article=source.cloneNode(true);article.querySelectorAll('.code-copy-button,.heading-anchor').forEach((el)=>el.remove());"
"let css='';Array.from(document.styleSheets).forEach((sheet)=>{try{css+=Array.from(sheet.cssRules).map((rule)=>rule.cssText).join('\\n');}catch(_){}});"
"const esc=(v)=>String(v).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');const attr=(v)=>esc(v).replace(/\"/g,'&quot;');"
"return '<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>'+esc(title)+'</title><base href=\"'+attr(baseHref)+'\"><style>'+css+'</style></head><body><main style=\"max-width:980px;margin:0 auto;padding:24px\">'+article.outerHTML+'</main></body></html>';};"
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

static char *to_utf8(const WCHAR *s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *utf8 = (char *)malloc((size_t)n);
    if (!utf8) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, utf8, n, NULL, NULL)) {
        free(utf8);
        return NULL;
    }
    return utf8;
}

static int json_hex_digit(WCHAR c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

/* WebView2 returns ExecuteScript values as JSON. Decode its string result
   without involving a second scripting engine so exported Unicode remains
   byte-for-byte UTF-8. */
static WCHAR *decode_json_string(const WCHAR *json) {
    if (!json || json[0] != L'"') return NULL;
    size_t n = wcslen(json);
    WCHAR *out = (WCHAR *)malloc((n + 1) * sizeof(WCHAR));
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 1; i < n && json[i] != L'"'; i++) {
        WCHAR c = json[i];
        if (c != L'\\') { out[o++] = c; continue; }
        if (++i >= n) { free(out); return NULL; }
        c = json[i];
        if (c == L'"' || c == L'\\' || c == L'/') out[o++] = c;
        else if (c == L'b') out[o++] = L'\b';
        else if (c == L'f') out[o++] = L'\f';
        else if (c == L'n') out[o++] = L'\n';
        else if (c == L'r') out[o++] = L'\r';
        else if (c == L't') out[o++] = L'\t';
        else if (c == L'u' && i + 4 < n) {
            int value = 0;
            for (int j = 0; j < 4; j++) {
                int digit = json_hex_digit(json[++i]);
                if (digit < 0) { free(out); return NULL; }
                value = value * 16 + digit;
            }
            out[o++] = (WCHAR)value;
        } else { free(out); return NULL; }
    }
    out[o] = 0;
    return out;
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
            case '<': memcpy(out + o, "\\u003C", 6); o += 6; break;
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

static ULONG STDMETHODCALLTYPE Handler_AddRef(void *this_) { (void)this_; return 1; }
static ULONG STDMETHODCALLTYPE Handler_Release(void *this_) { (void)this_; return 1; }
static const IID MARKEDIT_IID_IUNKNOWN =
    {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static HRESULT Handler_QueryInterface(void *this_, REFIID riid, void **out, REFIID handlerIid) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualIID(riid, &MARKEDIT_IID_IUNKNOWN) || IsEqualIID(riid, handlerIid)) {
        *out = this_;
        Handler_AddRef(this_);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE EnvHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out,
        &IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler);
}

static HRESULT STDMETHODCALLTYPE CtrlHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out,
        &IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler);
}

static HRESULT STDMETHODCALLTYPE ScriptHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out,
        &IID_ICoreWebView2ExecuteScriptCompletedHandler);
}

static HRESULT STDMETHODCALLTYPE NavHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out,
        &IID_ICoreWebView2NavigationCompletedEventHandler);
}

static HRESULT STDMETHODCALLTYPE NavStartHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out,
        &IID_ICoreWebView2NavigationStartingEventHandler);
}

static HRESULT STDMETHODCALLTYPE WebMessageHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out,
        &IID_ICoreWebView2WebMessageReceivedEventHandler);
}

/* Environment created handler */
static HRESULT STDMETHODCALLTYPE EnvCreated_Invoke(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *this_, HRESULT errorCode, ICoreWebView2Environment *env);
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl g_envVtbl = {
    (void*)EnvHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)EnvCreated_Invoke
};
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler g_envHandler = { &g_envVtbl };

/* Controller created handler */
static HRESULT STDMETHODCALLTYPE CtrlCreated_Invoke(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *this_, HRESULT errorCode, ICoreWebView2Controller *ctrl);
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl g_ctrlVtbl = {
    (void*)CtrlHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)CtrlCreated_Invoke
};
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandler g_ctrlHandler = { &g_ctrlVtbl };

/* ExecuteScript completed handler: no-op, we don't need the result */
static HRESULT STDMETHODCALLTYPE ScriptDone_Invoke(ICoreWebView2ExecuteScriptCompletedHandler *this_, HRESULT errorCode, LPCWSTR result) {
    (void)this_; (void)errorCode; (void)result;
    return S_OK;
}
static ICoreWebView2ExecuteScriptCompletedHandlerVtbl g_scriptVtbl = {
    (void*)ScriptHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)ScriptDone_Invoke
};
static ICoreWebView2ExecuteScriptCompletedHandler g_scriptHandler = { &g_scriptVtbl };

static HRESULT STDMETHODCALLTYPE ExportDone_Invoke(ICoreWebView2ExecuteScriptCompletedHandler *this_, HRESULT errorCode, LPCWSTR result) {
    (void)this_;
    WCHAR *html = SUCCEEDED(errorCode) ? decode_json_string(result) : NULL;
    char *utf8 = html ? to_utf8(html) : NULL;
    int ok = 0;
    if (utf8 && g_exportPath) {
        WCHAR *ioPath = extended_path(g_exportPath);
        HANDLE file = CreateFileW(ioPath ? ioPath : g_exportPath, GENERIC_WRITE, 0, NULL,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        free(ioPath);
        if (file != INVALID_HANDLE_VALUE) {
            size_t length = strlen(utf8), total = 0;
            ok = 1;
            while (total < length) {
                DWORD chunk = (DWORD)((length - total) > 1024 * 1024 ? 1024 * 1024 : length - total);
                DWORD written = 0;
                if (!WriteFile(file, utf8 + total, chunk, &written, NULL) || written != chunk) {
                    ok = 0;
                    break;
                }
                total += written;
            }
            if (!CloseHandle(file)) ok = 0;
        }
    }
    if (!ok) MessageBoxW(g_hMainWnd, L"Could not export the rendered HTML.", L"MarkEdit", MB_OK | MB_ICONERROR);
    free(utf8);
    free(html);
    free(g_exportPath);
    g_exportPath = NULL;
    return S_OK;
}
static ICoreWebView2ExecuteScriptCompletedHandlerVtbl g_exportScriptVtbl = {
    (void*)ScriptHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)ExportDone_Invoke
};
static ICoreWebView2ExecuteScriptCompletedHandler g_exportScriptHandler = { &g_exportScriptVtbl };

static HRESULT STDMETHODCALLTYPE WebMessage_Invoke(ICoreWebView2WebMessageReceivedEventHandler *this_,
                                                     ICoreWebView2 *sender,
                                                     ICoreWebView2WebMessageReceivedEventArgs *args) {
    (void)this_; (void)sender;
    LPWSTR message = NULL;
    if (!g_editorVisible || !g_previewVisible ||
        FAILED(ICoreWebView2WebMessageReceivedEventArgs_TryGetWebMessageAsString(args, &message)) ||
        !message) return S_OK;
    if (wcsncmp(message, L"scroll:", 7) == 0) {
        WCHAR *end = NULL;
        long line = wcstol(message + 7, &end, 10);
        int lineCount = (int)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
        if (end && *end == 0 && line >= 1 && line <= lineCount) {
            g_ignoreNextEditorScroll = 1;
            KillTimer(g_hMainWnd, IDT_SCROLL_SYNC);
            SendMessageA(g_hEdit, SCI_SETFIRSTVISIBLELINE, (WPARAM)(line - 1), 0);
        }
    }
    CoTaskMemFree(message);
    return S_OK;
}
static ICoreWebView2WebMessageReceivedEventHandlerVtbl g_webMessageVtbl = {
    (void*)WebMessageHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)WebMessage_Invoke
};
static ICoreWebView2WebMessageReceivedEventHandler g_webMessageHandler = { &g_webMessageVtbl };

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
    (void*)NavHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)NavDone_Invoke
};
static ICoreWebView2NavigationCompletedEventHandler g_navHandler = { &g_navVtbl };

/* True if the backslash-separated relative path escapes its base directory
   via a ".." segment, or is itself rooted (drive letter or UNC share) rather
   than relative. Used to keep clicked document-relative links confined to
   the folder the open Markdown file lives in. */
static int path_escapes_base(const WCHAR *relative) {
    if (relative[0] == L'\\' || (relative[0] && relative[1] == L':')) return 1;
    const WCHAR *segment = relative;
    for (;;) {
        size_t len = wcscspn(segment, L"\\");
        if (len == 2 && segment[0] == L'.' && segment[1] == L'.') return 1;
        if (segment[len] == 0) break;
        segment += len + 1;
    }
    return 0;
}

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
        int units = 1;
        if (*p >= 0xD800 && *p <= 0xDBFF && p + 1 < end &&
            p[1] >= 0xDC00 && p[1] <= 0xDFFF) units = 2;
        int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, p, units,
                                           bytes + o, (int)(byteCap - o), NULL, NULL);
        if (written <= 0) { free(bytes); return NULL; }
        o += (size_t)written;
        p += units;
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
    if (path_escapes_base(relative)) { free(relative); return NULL; }
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
    (void*)NavStartHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)NavStarting_Invoke
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
    if (!htmlFragment) return;
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
    if (!script) {
        free(escaped);
        return;
    }
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
    HRESULT webviewResult = ICoreWebView2Controller_get_CoreWebView2(ctrl, &webview);
    if (FAILED(webviewResult) || !webview) return S_OK;
    g_webview = webview;

    ICoreWebView2Settings *settings = NULL;
    if (SUCCEEDED(ICoreWebView2_get_Settings(webview, &settings)) && settings) {
        ICoreWebView2Settings_put_AreDevToolsEnabled(settings, FALSE);
        ICoreWebView2Settings_put_AreDefaultScriptDialogsEnabled(settings, FALSE);
        ICoreWebView2Settings_put_IsWebMessageEnabled(settings, TRUE);
        ICoreWebView2Settings_Release(settings);
    }

    EventRegistrationToken token;
    ICoreWebView2_add_NavigationCompleted(webview, &g_navHandler, &token);
    EventRegistrationToken navStartToken;
    ICoreWebView2_add_NavigationStarting(webview, &g_navStartHandler, &navStartToken);
    if (SUCCEEDED(ICoreWebView2_add_WebMessageReceived(webview, &g_webMessageHandler,
                                                       &g_webMessageToken)))
        g_webMessageRegistered = 1;

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
        if (wHtml) ICoreWebView2_NavigateToString(webview, wHtml);
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

static void init_webview(void) {
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
}

/* Documents below this size render in well under a frame, so skipping the
   cursor change for them avoids an hourglass flicker on every keystroke
   pause; above it, rendering can take long enough to be worth signaling. */
#define PREVIEW_SLOW_RENDER_THRESHOLD (256 * 1024)

static void update_preview(void) {
    if (g_hMainWnd) KillTimer(g_hMainWnd, IDT_PREVIEW_UPDATE);
    int len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;

    int showWaitCursor = len >= PREVIEW_SLOW_RENDER_THRESHOLD;
    HCURSOR oldCursor = showWaitCursor ? SetCursor(LoadCursorA(NULL, IDC_WAIT)) : NULL;

    char *html = markdown_to_html(text);
    free(text);

    if (showWaitCursor) SetCursor(oldCursor);

    if (!html) return;
    push_html_to_webview(html);
    free(html);
    if (g_hMainWnd) SetTimer(g_hMainWnd, IDT_SCROLL_SYNC, 40, NULL);
}

static void sync_preview_to_editor_scroll(void) {
    if (g_hMainWnd) KillTimer(g_hMainWnd, IDT_SCROLL_SYNC);
    if (!g_webviewReady || !g_webview || !g_editorVisible || !g_previewVisible) return;
    int line = (int)SendMessageA(g_hEdit, SCI_GETFIRSTVISIBLELINE, 0, 0) + 1;
    char script[96];
    snprintf(script, sizeof(script),
             "if(window.scrollToSourceLine)window.scrollToSourceLine(%d);", line);
    WCHAR *wide = to_wide(script);
    if (wide) ICoreWebView2_ExecuteScript(g_webview, wide, &g_scriptHandler);
    free(wide);
}

/* ================= rest of the app (menus, file I/O, find/replace, font) ================= */

static const WCHAR *document_short_name(const Document *doc) {
    return doc->path ? PathFindFileNameW(doc->path) : NULL;
}

static void format_document_label(const Document *doc, WCHAR *label, size_t capacity) {
    const WCHAR *shortName = document_short_name(doc);
    if (shortName && shortName[0]) {
        swprintf(label, capacity, L"%ls%ls", shortName, doc->dirty ? L" *" : L"");
    } else {
        swprintf(label, capacity, L"Untitled %u%ls",
                 doc->untitledNumber, doc->dirty ? L" *" : L"");
    }
}

static void update_tab_label(int index) {
    if (!g_hTabs || index < 0 || index >= g_documentCount) return;
    Document *doc = &g_documents[index];
    WCHAR label[MAX_PATH + 32];
    format_document_label(doc, label, sizeof(label) / sizeof(label[0]));
    TCITEMW item = {0};
    item.mask = TCIF_TEXT;
    item.pszText = label;
    SendMessageW(g_hTabs, TCM_SETITEMW, (WPARAM)index, (LPARAM)&item);
    InvalidateRect(g_hTabs, NULL, FALSE);
}

static void save_active_document_view(void) {
    if (!g_hEdit || g_documentCount <= 0) return;
    Document *doc = &g_documents[g_activeDocument];
    doc->caret = (int)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    doc->anchor = (int)SendMessageA(g_hEdit, SCI_GETANCHOR, 0, 0);
    doc->firstVisibleLine = (int)SendMessageA(g_hEdit, SCI_GETFIRSTVISIBLELINE, 0, 0);
    doc->xOffset = (int)SendMessageA(g_hEdit, SCI_GETXOFFSET, 0, 0);
}

static void record_document_file_time(int index) {
    if (index < 0 || index >= g_documentCount) return;
    Document *doc = &g_documents[index];
    doc->hasFileTime = 0;
    if (!doc->path) return;
    WCHAR *ioPath = extended_path(doc->path);
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (ioPath && GetFileAttributesExW(ioPath, GetFileExInfoStandard, &data)) {
        doc->fileTime = data.ftLastWriteTime;
        doc->hasFileTime = 1;
    }
    doc->externalChanged = 0;
    if (g_hTabs) InvalidateRect(g_hTabs, NULL, FALSE);
    free(ioPath);
}

static void switch_document(int index) {
    if (!g_hEdit || index < 0 || index >= g_documentCount || index == g_activeDocument) return;
    save_active_document_view();
    g_switchingDocument = 1;
    g_activeDocument = index;
    SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_documents[index].sciDocument);
    apply_editor_font(g_fontFaceName, g_fontPointSize);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)g_documents[index].anchor, (LPARAM)g_documents[index].caret);
    SendMessageA(g_hEdit, SCI_SETFIRSTVISIBLELINE, (WPARAM)g_documents[index].firstVisibleLine, 0);
    SendMessageA(g_hEdit, SCI_SETXOFFSET, (WPARAM)g_documents[index].xOffset, 0);
    update_linenumber_margin_width(g_hEdit);
    g_switchingDocument = 0;
    if (g_hTabs) TabCtrl_SetCurSel(g_hTabs, index);
    update_document_mapping();
    update_preview();
    update_title();
    prompt_reload_active_external_change();
    SetFocus(g_hEdit);
}

static int create_document_tab(void) {
    if (g_documentCount >= MAX_DOCUMENTS) {
        MessageBoxW(g_hMainWnd, L"MarkEdit can keep up to 32 documents open at once.", L"MarkEdit", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (g_documentCount > 0) save_active_document_view();
    sptr_t sciDoc = (sptr_t)SendMessageA(g_hEdit, SCI_CREATEDOCUMENT, 0, 0);
    if (!sciDoc) return 0;
    int index = g_documentCount++;
    memset(&g_documents[index], 0, sizeof(g_documents[index]));
    g_documents[index].sciDocument = sciDoc;
    g_documents[index].untitledNumber = g_nextUntitledNumber++;
    g_activeDocument = index;
    g_switchingDocument = 1;
    SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)sciDoc);
    configure_scintilla(g_hEdit);
    apply_editor_font(g_fontFaceName, g_fontPointSize);
    SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    g_switchingDocument = 0;
    TCITEMW item = {0};
    item.mask = TCIF_TEXT;
    item.pszText = L"";
    SendMessageW(g_hTabs, TCM_INSERTITEMW, (WPARAM)index, (LPARAM)&item);
    TabCtrl_SetCurSel(g_hTabs, index);
    update_tab_label(index);
    update_document_mapping();
    update_preview();
    update_title();
    return 1;
}

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
    update_tab_label(g_activeDocument);
    update_title();
}

static void do_new(void) {
    create_document_tab();
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
        if (length >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
            content += 3;
            g_hasBom = 1;
        } else {
            g_hasBom = 0;
        }
        g_switchingDocument = 1;
        set_edit_text(g_hEdit, content);
        SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
        SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
        g_switchingDocument = 0;
        replace_wstring(&g_currentFile, displayPath ? displayPath : path);
        record_document_file_time(g_activeDocument);
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

static int active_document_is_reusable(void) {
    return !g_currentFile && !g_dirty &&
           (int)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0) == 0;
}

static int open_file_in_tab(const WCHAR *path) {
    WCHAR *full = absolute_path(path);
    if (full) {
        for (int i = 0; i < g_documentCount; i++) {
            if (g_documents[i].path && _wcsicmp(g_documents[i].path, full) == 0) {
                switch_document(i);
                free(full);
                return 1;
            }
        }
    }
    free(full);
    int created = 0;
    if (!active_document_is_reusable()) {
        if (!create_document_tab()) return 0;
        created = 1;
    }
    if (load_file(path)) return 1;
    if (created && g_documentCount > 1) {
        sptr_t failedDoc = g_documents[g_activeDocument].sciDocument;
        int removeIndex = g_activeDocument;
        int target = removeIndex > 0 ? removeIndex - 1 : 0;
        TabCtrl_DeleteItem(g_hTabs, removeIndex);
        g_documentCount--;
        g_activeDocument = target;
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_documents[target].sciDocument);
        apply_editor_font(g_fontFaceName, g_fontPointSize);
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)g_documents[target].anchor, (LPARAM)g_documents[target].caret);
        SendMessageA(g_hEdit, SCI_RELEASEDOCUMENT, 0, (LPARAM)failedDoc);
        TabCtrl_SetCurSel(g_hTabs, target);
        update_document_mapping();
        update_preview();
        update_title();
    }
    return 0;
}

static void save_session_documents(void) {
    if (g_commandLinePreviewMode) return;
    WCHAR *settings = get_settings_path();
    if (!settings) return;
    int savedCount = 0;
    for (int i = 0; i < g_documentCount; i++) {
        if (!g_documents[i].path) continue;
        WCHAR key[24];
        swprintf(key, sizeof(key) / sizeof(key[0]), L"File%d", savedCount++);
        WritePrivateProfileStringW(L"Session", key, g_documents[i].path, settings);
    }
    for (int i = savedCount; i < MAX_DOCUMENTS; i++) {
        WCHAR key[24];
        swprintf(key, sizeof(key) / sizeof(key[0]), L"File%d", i);
        WritePrivateProfileStringW(L"Session", key, NULL, settings);
    }
    WCHAR count[24];
    swprintf(count, sizeof(count) / sizeof(count[0]), L"%d", savedCount);
    WritePrivateProfileStringW(L"Session", L"Count", count, settings);
    WritePrivateProfileStringW(L"Session", L"ActiveFile",
        g_currentFile ? g_currentFile : L"", settings);
    free(settings);
}

static void restore_session_documents(void) {
    WCHAR *settings = get_settings_path();
    if (!settings) return;
    int count = GetPrivateProfileIntW(L"Session", L"Count", 0, settings);
    if (count < 0) count = 0;
    if (count > MAX_DOCUMENTS) count = MAX_DOCUMENTS;
    WCHAR activeFile[32768];
    GetPrivateProfileStringW(L"Session", L"ActiveFile", L"", activeFile,
                             sizeof(activeFile) / sizeof(activeFile[0]), settings);
    for (int i = 0; i < count; i++) {
        WCHAR key[24], file[32768];
        swprintf(key, sizeof(key) / sizeof(key[0]), L"File%d", i);
        GetPrivateProfileStringW(L"Session", key, L"", file,
                                 sizeof(file) / sizeof(file[0]), settings);
        if (file[0]) open_file_in_tab(file);
    }
    if (activeFile[0]) {
        for (int i = 0; i < g_documentCount; i++) {
            if (g_documents[i].path && _wcsicmp(g_documents[i].path, activeFile) == 0) {
                switch_document(i);
                break;
            }
        }
    }
    free(settings);
}

/* Prompt used when closing a dirty tab or exiting with dirty documents.
   Returns 0 if the caller should abort. */
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
    WCHAR *path = wcsdup_heap(g_recentFiles[index]);
    if (path) open_file_in_tab(path); /* opening may mutate the recent-files array */
    free(path);
}

static void do_open(void) {
    DWORD cap = 32768;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"Markdown Files (*.md;*.markdown)\0*.md;*.markdown\0Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    ofn.lpstrDefExt = L"md";
    if (GetOpenFileNameW(&ofn)) {
        WCHAR *next = path + wcslen(path) + 1;
        if (!*next) {
            open_file_in_tab(path);
        } else {
            while (*next) {
                WCHAR *full = path_join(path, next);
                if (full) open_file_in_tab(full);
                free(full);
                next += wcslen(next) + 1;
            }
        }
    }
    free(path);
}

/* Opens a dropped file in its own document tab. */
static void do_open_dropped_file(const WCHAR *path) {
    open_file_in_tab(path);
}

static void handle_dropfiles(HDROP hDrop) {
    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
    for (UINT i = 0; i < count; i++) {
        UINT cap = DragQueryFileW(hDrop, i, NULL, 0) + 1;
        WCHAR *path = (WCHAR *)malloc((size_t)cap * sizeof(WCHAR));
        if (path && DragQueryFileW(hDrop, i, path, cap)) do_open_dropped_file(path);
        free(path);
    }
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
    static const char bom[3] = { (char)0xEF, (char)0xBB, (char)0xBF };
    size_t total = 0;
    BOOL ok = TRUE;
    if (g_hasBom) {
        DWORD written = 0;
        if (!WriteFile(hFile, bom, 3, &written, NULL) || written != 3) ok = FALSE;
    }
    while (ok && total < (size_t)len) {
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
    /* Record the path before replace_wstring(): when saving the current file,
       `path` may alias g_currentFile, which replace_wstring() can free. */
    add_recent_file(displayPath ? displayPath : path);
    replace_wstring(&g_currentFile, displayPath ? displayPath : path);
    free(ioPath);
    record_document_file_time(g_activeDocument);
    update_document_mapping();
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    set_dirty(0);
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

static void do_export_html(void) {
    if (!g_webview || !g_webviewReady) {
        MessageBoxW(g_hMainWnd, L"The preview is not ready yet.", L"MarkEdit", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g_exportPath) {
        MessageBoxW(g_hMainWnd, L"An HTML export is already in progress.", L"MarkEdit", MB_OK | MB_ICONINFORMATION);
        return;
    }

    DWORD cap = 32768;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    if (g_currentFile) {
        wcsncpy(path, g_currentFile, cap - 1);
        PathRenameExtensionW(path, L".html");
    } else {
        wcsncpy(path, L"Untitled.html", cap - 1);
    }
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"HTML Files (*.html;*.htm)\0*.html;*.htm\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"html";
    if (!GetSaveFileNameW(&ofn)) { free(path); return; }

    WCHAR *directory = g_currentFile ? path_directory(g_currentFile) : NULL;
    if (!directory) {
        WCHAR *module = get_module_path();
        directory = module ? path_directory(module) : NULL;
        free(module);
    }
    WCHAR *directorySlash = NULL;
    if (directory) {
        size_t n = wcslen(directory);
        directorySlash = (WCHAR *)malloc((n + 2) * sizeof(WCHAR));
        if (directorySlash) {
            memcpy(directorySlash, directory, n * sizeof(WCHAR));
            if (n == 0 || (directory[n - 1] != L'\\' && directory[n - 1] != L'/')) directorySlash[n++] = L'\\';
            directorySlash[n] = 0;
        }
    }
    WCHAR url[32768];
    DWORD urlLength = sizeof(url) / sizeof(url[0]);
    if (!directorySlash || FAILED(UrlCreateFromPathW(directorySlash, url, &urlLength, 0))) wcscpy(url, L"");
    free(directorySlash);
    free(directory);

    const WCHAR *titleWide = document_short_name(&g_documents[g_activeDocument]);
    if (!titleWide) titleWide = L"Untitled";
    char *baseUtf8 = to_utf8(url);
    char *titleUtf8 = to_utf8(titleWide);
    char *baseEscaped = baseUtf8 ? json_escape(baseUtf8) : NULL;
    char *titleEscaped = titleUtf8 ? json_escape(titleUtf8) : NULL;
    if (!baseEscaped || !titleEscaped) {
        free(path); free(baseUtf8); free(titleUtf8); free(baseEscaped); free(titleEscaped);
        return;
    }
    size_t scriptCap = strlen(baseEscaped) + strlen(titleEscaped) + 128;
    char *script = (char *)malloc(scriptCap);
    if (!script) {
        free(path); free(baseUtf8); free(titleUtf8); free(baseEscaped); free(titleEscaped);
        return;
    }
    snprintf(script, scriptCap,
        "window.exportRenderedHTML ? window.exportRenderedHTML('%s','%s') : ''",
        baseEscaped, titleEscaped);
    WCHAR *wideScript = to_wide(script);
    g_exportPath = path;
    update_preview();
    HRESULT hr = wideScript
        ? ICoreWebView2_ExecuteScript(g_webview, wideScript, &g_exportScriptHandler)
        : E_OUTOFMEMORY;
    if (FAILED(hr)) {
        free(g_exportPath);
        g_exportPath = NULL;
        MessageBoxW(g_hMainWnd, L"Could not start the HTML export.", L"MarkEdit", MB_OK | MB_ICONERROR);
    }
    free(wideScript); free(script); free(baseUtf8); free(titleUtf8); free(baseEscaped); free(titleEscaped);
}

static void release_document_reference(Document *doc) {
    if (doc->sciDocument && g_hEdit)
        SendMessageA(g_hEdit, SCI_RELEASEDOCUMENT, 0, (LPARAM)doc->sciDocument);
    free(doc->path);
    memset(doc, 0, sizeof(*doc));
}

static void push_closed_document(Document removed, int index) {
    removed.closedIndex = index;
    if (g_closedDocumentCount == MAX_CLOSED_DOCUMENTS) {
        release_document_reference(&g_closedDocuments[0]);
        for (int i = 0; i + 1 < g_closedDocumentCount; i++)
            g_closedDocuments[i] = g_closedDocuments[i + 1];
        g_closedDocumentCount--;
    }
    g_closedDocuments[g_closedDocumentCount++] = removed;
}

static void do_close_document_at(int index) {
    if (index < 0 || index >= g_documentCount) return;
    if (index != g_activeDocument) switch_document(index);
    if (!confirm_discard_if_dirty()) return;
    save_active_document_view();
    int removeIndex = g_activeDocument;
    Document removed = g_documents[removeIndex];

    if (g_documentCount <= 1) {
        sptr_t newDoc = (sptr_t)SendMessageA(g_hEdit, SCI_CREATEDOCUMENT, 0, 0);
        if (!newDoc) return;
        memset(&g_documents[0], 0, sizeof(g_documents[0]));
        g_documents[0].sciDocument = newDoc;
        g_documents[0].untitledNumber = g_nextUntitledNumber++;
        g_switchingDocument = 1;
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)newDoc);
        configure_scintilla(g_hEdit);
        apply_editor_font(g_fontFaceName, g_fontPointSize);
        SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
        g_switchingDocument = 0;
        push_closed_document(removed, 0);
        update_tab_label(0);
        update_document_mapping();
        update_preview();
        update_title();
        refresh_menu(g_hMainWnd);
        SetFocus(g_hEdit);
        return;
    }

    TabCtrl_DeleteItem(g_hTabs, removeIndex);
    for (int i = removeIndex; i + 1 < g_documentCount; i++) g_documents[i] = g_documents[i + 1];
    memset(&g_documents[g_documentCount - 1], 0, sizeof(g_documents[0]));
    g_documentCount--;
    g_activeDocument = removeIndex < g_documentCount ? removeIndex : g_documentCount - 1;
    g_switchingDocument = 1;
    SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_documents[g_activeDocument].sciDocument);
    apply_editor_font(g_fontFaceName, g_fontPointSize);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)g_documents[g_activeDocument].anchor,
                 (LPARAM)g_documents[g_activeDocument].caret);
    g_switchingDocument = 0;
    push_closed_document(removed, removeIndex);
    TabCtrl_SetCurSel(g_hTabs, g_activeDocument);
    update_linenumber_margin_width(g_hEdit);
    update_document_mapping();
    update_preview();
    update_title();
    refresh_menu(g_hMainWnd);
    prompt_reload_active_external_change();
    SetFocus(g_hEdit);
}

static void do_close_document(void) {
    do_close_document_at(g_activeDocument);
}

static void do_reopen_closed_tab(void) {
    if (g_closedDocumentCount <= 0) {
        MessageBeep(MB_ICONASTERISK);
        return;
    }
    Document restored = g_closedDocuments[--g_closedDocumentCount];
    memset(&g_closedDocuments[g_closedDocumentCount], 0, sizeof(g_closedDocuments[0]));
    save_active_document_view();

    int replaceBlank = g_documentCount == 1 && active_document_is_reusable();
    int index = restored.closedIndex;
    if (index < 0) index = 0;
    if (index > g_documentCount) index = g_documentCount;
    if (replaceBlank) {
        Document blank = g_documents[0];
        g_documents[0] = restored;
        index = 0;
        g_activeDocument = 0;
        g_switchingDocument = 1;
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)restored.sciDocument);
        g_switchingDocument = 0;
        release_document_reference(&blank);
    } else {
        if (g_documentCount >= MAX_DOCUMENTS) {
            g_closedDocuments[g_closedDocumentCount++] = restored;
            MessageBoxW(g_hMainWnd, L"Close a tab before reopening this document.", L"MarkEdit", MB_OK | MB_ICONINFORMATION);
            return;
        }
        for (int i = g_documentCount; i > index; i--) g_documents[i] = g_documents[i - 1];
        g_documents[index] = restored;
        g_documentCount++;
        g_activeDocument = index;
        g_switchingDocument = 1;
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)restored.sciDocument);
        g_switchingDocument = 0;
        TCITEMW item = {0};
        item.mask = TCIF_TEXT;
        item.pszText = L"";
        SendMessageW(g_hTabs, TCM_INSERTITEMW, (WPARAM)index, (LPARAM)&item);
    }
    apply_editor_font(g_fontFaceName, g_fontPointSize);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)restored.anchor, (LPARAM)restored.caret);
    SendMessageA(g_hEdit, SCI_SETFIRSTVISIBLELINE, (WPARAM)restored.firstVisibleLine, 0);
    SendMessageA(g_hEdit, SCI_SETXOFFSET, (WPARAM)restored.xOffset, 0);
    TabCtrl_SetCurSel(g_hTabs, index);
    update_tab_label(index);
    update_linenumber_margin_width(g_hEdit);
    update_document_mapping();
    update_preview();
    update_title();
    refresh_menu(g_hMainWnd);
    prompt_reload_active_external_change();
    SetFocus(g_hEdit);
}

static void prompt_reload_active_external_change(void) {
    if (g_documentCount <= 0 || !g_currentFile || !g_documents[g_activeDocument].externalChanged) return;
    Document *doc = &g_documents[g_activeDocument];
    doc->externalChanged = 0;
    InvalidateRect(g_hTabs, NULL, FALSE);
    const WCHAR *message = g_dirty
        ? L"This file changed on disk. Reload it and discard your unsaved changes?"
        : L"This file changed on disk. Reload the updated version?";
    if (MessageBoxW(g_hMainWnd, message, L"MarkEdit", MB_YESNO | MB_ICONQUESTION) == IDYES) {
        WCHAR *path = wcsdup_heap(g_currentFile);
        if (path) load_file(path);
        free(path);
    }
}

static void check_external_file_change(void) {
    int changedVisual = 0;
    for (int i = 0; i < g_documentCount; i++) {
        Document *doc = &g_documents[i];
        if (!doc->path) continue;
        WCHAR *ioPath = extended_path(doc->path);
        WIN32_FILE_ATTRIBUTE_DATA data;
        int exists = ioPath && GetFileAttributesExW(ioPath, GetFileExInfoStandard, &data);
        free(ioPath);
        if (!exists) {
            doc->hasFileTime = 0;
            continue;
        }
        if (!doc->hasFileTime) {
            doc->fileTime = data.ftLastWriteTime;
            doc->hasFileTime = 1;
            continue;
        }
        if (CompareFileTime(&doc->fileTime, &data.ftLastWriteTime) != 0) {
            doc->fileTime = data.ftLastWriteTime;
            if (!doc->externalChanged) changedVisual = 1;
            doc->externalChanged = 1;
        }
    }
    if (changedVisual) InvalidateRect(g_hTabs, NULL, FALSE);
    prompt_reload_active_external_change();
}

static void do_exit(void) {
    int original = g_activeDocument;
    for (int i = 0; i < g_documentCount; i++) {
        if (!g_documents[i].dirty) continue;
        switch_document(i);
        if (!confirm_discard_if_dirty()) {
            if (original < g_documentCount) switch_document(original);
            return;
        }
    }
    if (original < g_documentCount) switch_document(original);
    save_layout_settings();
    save_session_documents();
    for (int i = 0; i < g_documentCount; i++) {
        SendMessageA(g_hEdit, SCI_RELEASEDOCUMENT, 0, (LPARAM)g_documents[i].sciDocument);
        g_documents[i].sciDocument = 0;
    }
    for (int i = 0; i < g_closedDocumentCount; i++)
        release_document_reference(&g_closedDocuments[i]);
    g_closedDocumentCount = 0;
    DestroyWindow(g_hMainWnd);
}

/* ---- Find / Replace (operates on the markdown source in g_hEdit) ---- */
static HWND g_hGotoEdit = NULL;

static LRESULT CALLBACK GotoLineWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (msg) {
        case WM_CREATE: {
            CreateWindowExA(0, "STATIC", "Line number:", WS_CHILD | WS_VISIBLE,
                12, 14, 90, 20, hwnd, NULL, GetModuleHandleA(NULL), NULL);
            g_hGotoEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                104, 11, 105, 24, hwnd, (HMENU)10, GetModuleHandleA(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Go to", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                48, 50, 76, 25, hwnd, (HMENU)IDOK, GetModuleHandleA(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                132, 50, 76, 25, hwnd, (HMENU)IDCANCEL, GetModuleHandleA(NULL), NULL);
            char current[32];
            int line = (int)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION,
                SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0), 0) + 1;
            snprintf(current, sizeof(current), "%d", line);
            SetWindowTextA(g_hGotoEdit, current);
            SendMessageA(g_hGotoEdit, EM_SETSEL, 0, -1);
            SetFocus(g_hGotoEdit);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                char value[32];
                GetWindowTextA(g_hGotoEdit, value, sizeof(value));
                long line = strtol(value, NULL, 10);
                int lineCount = (int)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
                if (line < 1 || line > lineCount) {
                    MessageBeep(MB_ICONWARNING);
                    SendMessageA(g_hGotoEdit, EM_SETSEL, 0, -1);
                    return 0;
                }
                SendMessageA(g_hEdit, SCI_GOTOLINE, (WPARAM)(line - 1), 0);
                SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void show_goto_line_dialog(void) {
    static int classRegistered = 0;
    if (!classRegistered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = GotoLineWndProc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "MarkEditGotoLine";
        if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        classRegistered = 1;
    }
    HWND dialog = CreateWindowExA(WS_EX_DLGMODALFRAME, "MarkEditGotoLine", "Go to Line",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 238, 118, g_hMainWnd, NULL, GetModuleHandleA(NULL), NULL);
    if (!dialog) return;
    RECT owner, rc;
    GetWindowRect(g_hMainWnd, &owner);
    GetWindowRect(dialog, &rc);
    SetWindowPos(dialog, HWND_TOP,
        owner.left + ((owner.right - owner.left) - (rc.right - rc.left)) / 2,
        owner.top + ((owner.bottom - owner.top) - (rc.bottom - rc.top)) / 2,
        0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    EnableWindow(g_hMainWnd, FALSE);
    MSG msg = {0};
    int result = -1;
    while (IsWindow(dialog) && (result = GetMessage(&msg, NULL, 0, 0)) > 0) {
        if (!IsDialogMessage(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    EnableWindow(g_hMainWnd, TRUE);
    SetForegroundWindow(g_hMainWnd);
    SetFocus(g_hEdit);
    if (result == 0) PostQuitMessage((int)msg.wParam);
}

static void find_in_edit(const char *needle, int matchCase, int down) {
    if (!needle || !needle[0]) return;
    int len = (int)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    int selStart = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int selEnd = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    int needleLen = (int)strlen(needle);
    int foundAt = -1;
    int foundEnd = -1;
    SendMessageA(g_hEdit, SCI_SETSEARCHFLAGS, matchCase ? SCFIND_MATCHCASE : 0, 0);

    if (down) {
        SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)selEnd, 0);
        SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)len, 0);
        foundAt = (int)SendMessageA(g_hEdit, SCI_SEARCHINTARGET, (WPARAM)needleLen, (LPARAM)needle);
        if (foundAt < 0) {
            SendMessageA(g_hEdit, SCI_SETTARGETSTART, 0, 0);
            SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)selStart, 0);
            foundAt = (int)SendMessageA(g_hEdit, SCI_SEARCHINTARGET, (WPARAM)needleLen, (LPARAM)needle);
        }
    } else {
        SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)selStart, 0);
        SendMessageA(g_hEdit, SCI_SETTARGETEND, 0, 0);
        foundAt = (int)SendMessageA(g_hEdit, SCI_SEARCHINTARGET, (WPARAM)needleLen, (LPARAM)needle);
        if (foundAt < 0) {
            SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)len, 0);
            SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)selEnd, 0);
            foundAt = (int)SendMessageA(g_hEdit, SCI_SEARCHINTARGET, (WPARAM)needleLen, (LPARAM)needle);
        }
    }

    if (foundAt >= 0) {
        foundEnd = (int)SendMessageA(g_hEdit, SCI_GETTARGETEND, 0, 0);
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)foundAt, (LPARAM)foundEnd);
        SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
        SetFocus(g_hEdit);
    } else {
        MessageBeep(MB_ICONASTERISK);
    }
}

static void replace_selection_and_find(const char *findWhat, const char *replaceWith, int matchCase) {
    int selStart = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int selEnd = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (selEnd > selStart) {
        int findLen = (int)strlen(findWhat);
        SendMessageA(g_hEdit, SCI_SETSEARCHFLAGS, matchCase ? SCFIND_MATCHCASE : 0, 0);
        SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)selStart, 0);
        SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)selEnd, 0);
        int found = (int)SendMessageA(g_hEdit, SCI_SEARCHINTARGET, (WPARAM)findLen, (LPARAM)findWhat);
        int foundEnd = (int)SendMessageA(g_hEdit, SCI_GETTARGETEND, 0, 0);
        if (found == selStart && foundEnd == selEnd) {
            SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)replaceWith);
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
    g_fr.lStructSize = sizeof(FINDREPLACEW);
    g_fr.hwndOwner = g_hMainWnd;
    g_fr.lpstrFindWhat = g_findWhat;
    g_fr.wFindWhatLen = sizeof(g_findWhat) / sizeof(g_findWhat[0]);
    g_fr.lpstrReplaceWith = g_replaceWith;
    g_fr.wReplaceWithLen = sizeof(g_replaceWith) / sizeof(g_replaceWith[0]);
    g_fr.Flags = FR_DOWN;

    if (replace) {
        g_hFindDlg = ReplaceTextW(&g_fr);
    } else {
        g_hFindDlg = FindTextW(&g_fr);
    }
}

static void handle_findmsg(FINDREPLACEW *fr) {
    if (fr->Flags & FR_DIALOGTERM) {
        g_hFindDlg = NULL;
        return;
    }
    char *findUtf8 = to_utf8(fr->lpstrFindWhat);
    char *replaceUtf8 = to_utf8(fr->lpstrReplaceWith);
    if (!findUtf8 || !replaceUtf8) {
        free(findUtf8);
        free(replaceUtf8);
        return;
    }
    int matchCase = (fr->Flags & FR_MATCHCASE) ? 1 : 0;
    int down = (fr->Flags & FR_DOWN) ? 1 : 0;
    if (fr->Flags & FR_FINDNEXT) {
        find_in_edit(findUtf8, matchCase, down);
    } else if (fr->Flags & FR_REPLACE) {
        replace_selection_and_find(findUtf8, replaceUtf8, matchCase);
    } else if (fr->Flags & FR_REPLACEALL) {
        int count = 0;
        int findLen = (int)strlen(findUtf8);
        int replaceLen = (int)strlen(replaceUtf8);
        int position = 0;
        int documentLength = (int)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
        SendMessageA(g_hEdit, SCI_SETSEARCHFLAGS, matchCase ? SCFIND_MATCHCASE : 0, 0);
        SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
        while (findLen > 0 && position <= documentLength && count <= 100000) {
            SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)position, 0);
            SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)documentLength, 0);
            int found = (int)SendMessageA(g_hEdit, SCI_SEARCHINTARGET,
                                           (WPARAM)findLen, (LPARAM)findUtf8);
            if (found < 0) break;
            int oldEnd = (int)SendMessageA(g_hEdit, SCI_GETTARGETEND, 0, 0);
            SendMessageA(g_hEdit, SCI_REPLACETARGET, (WPARAM)replaceLen, (LPARAM)replaceUtf8);
            documentLength += replaceLen - (oldEnd - found);
            position = found + replaceLen;
            count++;
        }
        SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
        WCHAR msg[64];
        swprintf(msg, sizeof(msg) / sizeof(msg[0]), L"Replaced %d occurrence(s).", count);
        MessageBoxW(g_hMainWnd, msg, L"MarkEdit", MB_OK | MB_ICONINFORMATION);
    }
    free(findUtf8);
    free(replaceUtf8);
}

static void do_choose_font(void) {
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    wcsncpy(lf.lfFaceName, g_fontFaceName, LF_FACESIZE - 1);
    HDC hdc = GetDC(g_hEdit);
    lf.lfHeight = -MulDiv(g_fontPointSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(g_hEdit, hdc);

    CHOOSEFONTW cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = g_hMainWnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOSCRIPTSEL;

    if (ChooseFontW(&cf)) {
        wcsncpy(g_fontFaceName, lf.lfFaceName, LF_FACESIZE - 1);
        g_fontFaceName[LF_FACESIZE - 1] = 0;
        g_fontPointSize = cf.iPointSize / 10;
        if (g_fontPointSize < 1) g_fontPointSize = 1;
        apply_editor_font(g_fontFaceName, g_fontPointSize);
        save_font_settings();
    }
}

/* ---- Help > Markdown Reference: a plain-text syntax cheat sheet. ---- */

static const char *HELP_REFERENCE_TEXT =
"MARKEDIT MARKDOWN REFERENCE\r\n"
"===========================\r\n"
"CommonMark with GitHub-flavoured extensions and MarkEdit preview features.\r\n"
"\r\n"
"HEADINGS\r\n"
"    # Heading 1\r\n"
"    ## Heading 2\r\n"
"    ### Heading 3\r\n"
"    ###### Heading 6\r\n"
"\r\n"
"    H1 and H2 can also be made by underlining the preceding text:\r\n"
"    Heading 1\r\n"
"    =========\r\n"
"    Heading 2\r\n"
"    ---------\r\n"
"\r\n"
"    Headings receive link anchors; two or more also show an Outline control.\r\n"
"\r\n"
"PARAGRAPHS AND LINE BREAKS\r\n"
"    Separate paragraphs with a blank line.\r\n"
"    End a line with two spaces or a backslash for a hard line break.\r\n"
"\r\n"
"EMPHASIS AND INLINE CODE\r\n"
"    **bold** or __bold__\r\n"
"    *italic* or _italic_\r\n"
"    ***bold italic***\r\n"
"    ~~strikethrough~~\r\n"
"    `inline code`\r\n"
"    Escape Markdown punctuation with a backslash: \\*literal asterisks\\*\r\n"
"\r\n"
"LINKS, IMAGES, AUTOLINKS\r\n"
"    [link text](https://example.com)\r\n"
"    [link with title](https://example.com \"Optional title\")\r\n"
"    ![alt text](images/photo.png)\r\n"
"    [relative link](docs/guide.md)\r\n"
"    <https://example.com>\r\n"
"    https://example.com (bare URLs are linked automatically)\r\n"
"    Relative links and images resolve from the Markdown file's folder.\r\n"
"\r\n"
"    Reference-style links:\r\n"
"    [link text][id]\r\n"
"    [id]: https://example.com\r\n"
"\r\n"
"LISTS\r\n"
"    - bullet item\r\n"
"    - another item\r\n"
"      - nested item\r\n"
"\r\n"
"    1. first\r\n"
"    2. second\r\n"
"       1. nested\r\n"
"\r\n"
"    Ordered lists may start at any number. Indent items to nest them.\r\n"
"    Task lists:\r\n"
"    - [ ] an unchecked task\r\n"
"    - [x] a checked task\r\n"
"\r\n"
"BLOCKQUOTES AND ALERTS\r\n"
"    > a quoted line\r\n"
"\r\n"
"    > [!NOTE]\r\n"
"    > Useful information for the reader.\r\n"
"\r\n"
"    Alert types: NOTE, TIP, IMPORTANT, WARNING, CAUTION\r\n"
"\r\n"
"CODE BLOCKS\r\n"
"    Indent with 4 spaces, or fence with triple backticks:\r\n"
"    ```\r\n"
"    code goes here\r\n"
"    ```\r\n"
"\r\n"
"    Add a language for syntax highlighting, such as ```c, ```python,\r\n"
"    ```javascript, ```powershell, or ```yaml.\r\n"
"\r\n"
"MERMAID DIAGRAMS\r\n"
"    Fence diagram source with the mermaid language:\r\n"
"    ```mermaid\r\n"
"    graph TD\r\n"
"      A --> B\r\n"
"    ```\r\n"
"    Use the preview toolbar or Ctrl+mouse wheel to zoom; drag to pan.\r\n"
"\r\n"
"MATHS\r\n"
"    Inline: Euler's identity is $e^{i\\pi} + 1 = 0$.\r\n"
"    Display: $$\\int_0^1 x^2 \\, dx$$\r\n"
"    Fenced display maths:\r\n"
"    ```math\r\n"
"    \\frac{-b \\pm \\sqrt{b^2 - 4ac}}{2a}\r\n"
"    ```\r\n"
"\r\n"
"TABLES\r\n"
"    | Left | Centre | Right |\r\n"
"    | :--- | :----: | ----: |\r\n"
"    | A    | B      | C     |\r\n"
"    Colons in the separator row set column alignment.\r\n"
"\r\n"
"FOOTNOTES\r\n"
"    A sentence with a note.[^1]\r\n"
"\r\n"
"    [^1]: The footnote text.\r\n"
"\r\n"
"YAML FRONT MATTER\r\n"
"    A metadata block must start on the document's first line. It is omitted\r\n"
"    from the preview and may close with either --- or ...\r\n"
"    ---\r\n"
"    title: Example\r\n"
"    tags: [notes, demo]\r\n"
"    ---\r\n"
"\r\n"
"HORIZONTAL RULES\r\n"
"    ---\r\n"
"    ***\r\n"
"    ___\r\n"
"\r\n"
"PREVIEW SAFETY\r\n"
"    Raw HTML is omitted. Unsafe javascript: and data: URLs are not linked.\r\n";

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
            apply_help_theme();
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
            if ((HWND)lParam == g_hHelpEdit && g_helpBkBrush) {
                HDC hdcEdit = (HDC)wParam;
                SetBkColor(hdcEdit, g_previewDarkMode
                    ? RGB(0x0D, 0x11, 0x17)
                    : RGB(0xFF, 0xFF, 0xFF));
                SetTextColor(hdcEdit, g_previewDarkMode
                    ? RGB(0xE6, 0xED, 0xF3)
                    : RGB(0x1F, 0x23, 0x28));
                return (LRESULT)g_helpBkBrush;
            }
            break;
        case WM_ERASEBKGND:
            if (g_helpBkBrush) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                FillRect((HDC)wParam, &rc, g_helpBkBrush);
                return 1;
            }
            break;
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
            if (g_helpBkBrush) {
                DeleteObject(g_helpBkBrush);
                g_helpBkBrush = NULL;
            }
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
    AppendMenuA(hFile, MF_STRING, IDM_FILE_EXPORT_HTML, "Export Rendered &HTML...");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_CLOSE, "&Close Tab\tCtrl+W");
    AppendMenuA(hFile, MF_STRING | (g_closedDocumentCount ? 0 : MF_GRAYED),
                IDM_FILE_REOPEN_CLOSED, "&Reopen Closed Tab\tCtrl+Shift+T");
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
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_GOTOLINE, "&Go to Line...\tCtrl+G");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hEditMenu, "&Edit");

    HMENU hFormat = CreatePopupMenu();
    AppendMenuA(hFormat, MF_STRING, IDM_FMT_FOOTNOTE, "Insert &Footnote");
    AppendMenuA(hFormat, MF_STRING, IDM_FMT_HRULE, "Insert &Horizontal Rule");
    AppendMenuA(hFormat, MF_STRING, IDM_FMT_ALERT, "Insert &Alert");
    AppendMenuA(hFormat, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hFormat, MF_STRING, IDM_FORMAT_FONT, "&Font...");
    AppendMenuA(hMenuBar, MF_POPUP, (UINT_PTR)hFormat, "F&ormat");

    HMENU hView = CreatePopupMenu();
    AppendMenuA(hView, MF_STRING | (g_previewVisible ? MF_CHECKED : MF_UNCHECKED), IDM_VIEW_TOGGLEPREVIEW, "Show &Preview\tCtrl+Shift+V");
    AppendMenuA(hView, MF_STRING | (g_editorVisible ? MF_CHECKED : MF_UNCHECKED), IDM_VIEW_TOGGLEEDITOR, "Show &Editor\tCtrl+Shift+E");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hView, MF_STRING, IDM_VIEW_NEXT_TAB, "&Next Tab\tCtrl+Tab");
    AppendMenuA(hView, MF_STRING, IDM_VIEW_PREVIOUS_TAB, "&Previous Tab\tCtrl+Shift+Tab");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    HMENU hTheme = CreatePopupMenu();
    AppendMenuA(hTheme, MF_STRING, IDM_VIEW_THEME_GITHUB_LIGHT, "GitHub &Light");
    AppendMenuA(hTheme, MF_STRING, IDM_VIEW_THEME_GITHUB_DARK, "GitHub &Dark");
    CheckMenuRadioItem(hTheme, IDM_VIEW_THEME_GITHUB_LIGHT, IDM_VIEW_THEME_GITHUB_DARK,
                       g_previewDarkMode ? IDM_VIEW_THEME_GITHUB_DARK : IDM_VIEW_THEME_GITHUB_LIGHT,
                       MF_BYCOMMAND);
    AppendMenuA(hView, MF_POPUP, (UINT_PTR)hTheme, "&Theme");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hView, MF_STRING | (g_previewOnLeft ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_PREVIEW_ON_LEFT, "Preview on &Left");
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

static int g_dragging = 0;

static void set_webview_bounds(RECT wvRc, BOOL visible) {
    if (!g_webviewController) return;
    ICoreWebView2Controller_put_Bounds(g_webviewController, wvRc);
    ICoreWebView2Controller_put_IsVisible(g_webviewController, visible);
}

/* ---- formatting toolbar: a strip of owner-drawn BUTTON children of the
   main window, reserved space in layout() above the editor/preview panes.
   Icons are drawn on the fly with GDI (lines/shapes/text), not loaded from
   image resources, so they redraw crisply at any DPI and recolor for free
   when the theme changes. A NULL draw function marks a separator (a small
   gap, no button). ---- */
typedef void (*ToolbarIconPaintFn)(HDC hdc, const RECT *rc, COLORREF fg);

static void toolbar_icon_bold(HDC hdc, const RECT *rc, COLORREF fg) {
    HFONT font = CreateFontA(-(rc->bottom - rc->top) * 6 / 10, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Georgia");
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, "B", -1, (RECT *)rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    DeleteObject(font);
}

static void toolbar_icon_italic(HDC hdc, const RECT *rc, COLORREF fg) {
    HFONT font = CreateFontA(-(rc->bottom - rc->top) * 6 / 10, 0, 0, 0, FW_NORMAL, TRUE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Georgia");
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, "I", -1, (RECT *)rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    DeleteObject(font);
}

static void toolbar_icon_strikethrough(HDC hdc, const RECT *rc, COLORREF fg) {
    HFONT font = CreateFontA(-(rc->bottom - rc->top) * 6 / 10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    RECT textRc = *rc;
    DrawTextA(hdc, "S", -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_CALCRECT);
    OffsetRect(&textRc, ((rc->right - rc->left) - (textRc.right - textRc.left)) / 2,
               ((rc->bottom - rc->top) - (textRc.bottom - textRc.top)) / 2);
    DrawTextA(hdc, "S", -1, (RECT *)rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    int midY = (textRc.top + textRc.bottom) / 2;
    MoveToEx(hdc, textRc.left, midY, NULL);
    LineTo(hdc, textRc.right, midY);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    SelectObject(hdc, old);
    DeleteObject(font);
}

static void toolbar_icon_code(HDC hdc, const RECT *rc, COLORREF fg) {
    HFONT font = CreateFontA(-(rc->bottom - rc->top) * 5 / 10, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, "</>", -1, (RECT *)rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    DeleteObject(font);
}

static void toolbar_icon_heading(HDC hdc, const RECT *rc, COLORREF fg, const char *label) {
    HFONT font = CreateFontA(-(rc->bottom - rc->top) * 55 / 100, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, label, -1, (RECT *)rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    DeleteObject(font);
}
static void toolbar_icon_h1(HDC hdc, const RECT *rc, COLORREF fg) { toolbar_icon_heading(hdc, rc, fg, "H1"); }
static void toolbar_icon_h2(HDC hdc, const RECT *rc, COLORREF fg) { toolbar_icon_heading(hdc, rc, fg, "H2"); }
static void toolbar_icon_h3(HDC hdc, const RECT *rc, COLORREF fg) { toolbar_icon_heading(hdc, rc, fg, "H3"); }

static void toolbar_icon_quote(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    int barX = rc->left + w * 3 / 10;
    HPEN pen = CreatePen(PS_SOLID, (h + 15) / 16 > 1 ? (h + 15) / 16 : 2, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, barX, rc->top + h * 2 / 10, NULL);
    LineTo(hdc, barX, rc->bottom - h * 2 / 10);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    int lineX0 = barX + w / 8, lineX1 = rc->right - w * 2 / 10;
    HPEN thin = CreatePen(PS_SOLID, 1, fg);
    oldPen = (HPEN)SelectObject(hdc, thin);
    for (int i = 0; i < 3; i++) {
        int y = rc->top + h * 3 / 10 + i * (h * 4 / 10) / 2;
        MoveToEx(hdc, lineX0, y, NULL);
        LineTo(hdc, lineX1, y);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(thin);
}

static void toolbar_icon_bullet_list(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    HBRUSH brush = CreateSolidBrush(fg);
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, brush);
    int dotR = h / 10 > 1 ? h / 10 : 1;
    int lineX0 = rc->left + w * 4 / 10, lineX1 = rc->right - w * 2 / 10;
    for (int i = 0; i < 3; i++) {
        int y = rc->top + h * 22 / 100 + i * (h * 28 / 100);
        Ellipse(hdc, rc->left + w * 2 / 10 - dotR, y - dotR, rc->left + w * 2 / 10 + dotR, y + dotR);
        MoveToEx(hdc, lineX0, y, NULL);
        LineTo(hdc, lineX1, y);
    }
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

static void toolbar_icon_number_list(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    HFONT font = CreateFontA(-h * 32 / 100, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    int lineX0 = rc->left + w * 4 / 10, lineX1 = rc->right - w * 2 / 10;
    const char *digits[3] = { "1", "2", "3" };
    for (int i = 0; i < 3; i++) {
        int y = rc->top + h * 22 / 100 + i * (h * 28 / 100);
        RECT digitRc = { rc->left, y - h / 6, rc->left + w * 3 / 10, y + h / 6 };
        DrawTextA(hdc, digits[i], -1, &digitRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        MoveToEx(hdc, lineX0, y, NULL);
        LineTo(hdc, lineX1, y);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
    SelectObject(hdc, old);
    DeleteObject(font);
}

static void toolbar_icon_task_list(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    int boxSize = h * 4 / 10;
    RECT box = { rc->left + w * 15 / 100, rc->top + (h - boxSize) / 2,
                 rc->left + w * 15 / 100 + boxSize, rc->top + (h - boxSize) / 2 + boxSize };
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, box.left, box.top, box.right, box.bottom);
    MoveToEx(hdc, box.left + boxSize / 6, box.top + boxSize / 2, NULL);
    LineTo(hdc, box.left + boxSize * 2 / 5, box.bottom - boxSize / 8);
    LineTo(hdc, box.right - boxSize / 8, box.top + boxSize / 6);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);

    HPEN thin = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen2 = (HPEN)SelectObject(hdc, thin);
    int lineX0 = box.right + w / 10, lineX1 = rc->right - w * 15 / 100;
    int y = (box.top + box.bottom) / 2;
    MoveToEx(hdc, lineX0, y, NULL);
    LineTo(hdc, lineX1, y);
    SelectObject(hdc, oldPen2);
    DeleteObject(thin);
}

static void toolbar_icon_link(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    int linkW = w * 45 / 100, linkH = h * 45 / 100;
    HPEN pen = CreatePen(PS_SOLID, (h + 11) / 12 > 1 ? (h + 11) / 12 : 2, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

    RECT r1 = { rc->left + w * 12 / 100, rc->top + h * 12 / 100,
                rc->left + w * 12 / 100 + linkW, rc->top + h * 12 / 100 + linkH };
    RoundRect(hdc, r1.left, r1.top, r1.right, r1.bottom, linkH / 2, linkH / 2);

    RECT r2 = { rc->right - w * 12 / 100 - linkW, rc->bottom - h * 12 / 100 - linkH,
                rc->right - w * 12 / 100, rc->bottom - h * 12 / 100 };
    RoundRect(hdc, r2.left, r2.top, r2.right, r2.bottom, linkH / 2, linkH / 2);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

static void toolbar_icon_image(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    RECT frame = { rc->left + w * 12 / 100, rc->top + h * 18 / 100, rc->right - w * 12 / 100, rc->bottom - h * 18 / 100 };
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, frame.left, frame.top, frame.right, frame.bottom);

    HBRUSH sunBrush = CreateSolidBrush(fg);
    HBRUSH oldB2 = (HBRUSH)SelectObject(hdc, sunBrush);
    int sunR = (frame.bottom - frame.top) / 6;
    int sunCx = frame.left + (frame.right - frame.left) * 3 / 10, sunCy = frame.top + (frame.bottom - frame.top) * 3 / 10;
    Ellipse(hdc, sunCx - sunR, sunCy - sunR, sunCx + sunR, sunCy + sunR);
    SelectObject(hdc, oldB2);
    DeleteObject(sunBrush);

    POINT mountain[4] = {
        { frame.left + 1, frame.bottom - 1 },
        { frame.left + (frame.right - frame.left) * 4 / 10, frame.top + (frame.bottom - frame.top) * 4 / 10 },
        { frame.left + (frame.right - frame.left) * 7 / 10, frame.bottom - (frame.bottom - frame.top) * 35 / 100 },
        { frame.right - 1, frame.bottom - 1 },
    };
    HBRUSH mountainBrush = CreateSolidBrush(fg);
    HBRUSH oldB3 = (HBRUSH)SelectObject(hdc, mountainBrush);
    Polygon(hdc, mountain, 4);
    SelectObject(hdc, oldB3);
    DeleteObject(mountainBrush);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

static void toolbar_icon_table(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    RECT grid = { rc->left + w * 12 / 100, rc->top + h * 15 / 100, rc->right - w * 12 / 100, rc->bottom - h * 15 / 100 };
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, grid.left, grid.top, grid.right, grid.bottom);
    int colW = (grid.right - grid.left) / 3;
    for (int i = 1; i < 3; i++) {
        MoveToEx(hdc, grid.left + colW * i, grid.top, NULL);
        LineTo(hdc, grid.left + colW * i, grid.bottom);
    }
    int rowH = (grid.bottom - grid.top) / 2;
    MoveToEx(hdc, grid.left, grid.top + rowH, NULL);
    LineTo(hdc, grid.right, grid.top + rowH);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

/* A bordered box (the code fence) with a couple of short interior strokes
   standing in for lines of code - distinct from toolbar_icon_code's inline
   "</>" glyph, which has no border. */
static void toolbar_icon_codeblock(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    RECT box = { rc->left + w * 8 / 100, rc->top + h * 12 / 100, rc->right - w * 8 / 100, rc->bottom - h * 12 / 100 };
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, box.left, box.top, box.right, box.bottom);

    int lineH = (box.bottom - box.top) / 4;
    int inset = (box.right - box.left) * 15 / 100;
    MoveToEx(hdc, box.left + inset, box.top + lineH, NULL);
    LineTo(hdc, box.right - inset * 2, box.top + lineH);
    MoveToEx(hdc, box.left + inset, box.top + lineH * 2, NULL);
    LineTo(hdc, box.right - inset, box.top + lineH * 2);
    MoveToEx(hdc, box.left + inset, box.top + lineH * 3, NULL);
    LineTo(hdc, box.right - inset * 3, box.top + lineH * 3);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

/* A bordered box split into two panes, the right one filled - standing in
   for the editor/preview split with the preview pane highlighted. Used for
   the toolbar's preview-toggle button, mirroring the View menu's "Show
   Preview" command (IDM_VIEW_TOGGLEPREVIEW). */
static void toolbar_icon_togglepreview(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    RECT box = { rc->left + w * 8 / 100, rc->top + h * 12 / 100, rc->right - w * 8 / 100, rc->bottom - h * 12 / 100 };
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, box.left, box.top, box.right, box.bottom);

    int midX = box.left + (box.right - box.left) / 2;
    MoveToEx(hdc, midX, box.top, NULL);
    LineTo(hdc, midX, box.bottom);

    RECT rightPane = { midX + 1, box.top + 1, box.right, box.bottom - 1 };
    HBRUSH fillBrush = CreateSolidBrush(fg);
    FillRect(hdc, &rightPane, fillBrush);
    DeleteObject(fillBrush);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

/* Two opposing arrows represent exchanging the left and right panes. */
static void toolbar_icon_swappanels(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    int left = rc->left + w * 18 / 100;
    int right = rc->right - w * 18 / 100;
    int topY = rc->top + h * 35 / 100;
    int bottomY = rc->top + h * 65 / 100;
    int arrow = h * 14 / 100;

    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);

    MoveToEx(hdc, left, topY, NULL);
    LineTo(hdc, right, topY);
    MoveToEx(hdc, right, topY, NULL);
    LineTo(hdc, right - arrow, topY - arrow);
    MoveToEx(hdc, right, topY, NULL);
    LineTo(hdc, right - arrow, topY + arrow);

    MoveToEx(hdc, right, bottomY, NULL);
    LineTo(hdc, left, bottomY);
    MoveToEx(hdc, left, bottomY, NULL);
    LineTo(hdc, left + arrow, bottomY - arrow);
    MoveToEx(hdc, left, bottomY, NULL);
    LineTo(hdc, left + arrow, bottomY + arrow);

    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

static void toolbar_icon_text(HDC hdc, const RECT *rc, COLORREF fg, const char *label, int size) {
    HFONT font = CreateFontA(-size, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, "Segoe UI");
    HFONT old = (HFONT)SelectObject(hdc, font);
    RECT textRc = *rc;
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextA(hdc, label, -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
    DeleteObject(font);
}

static void toolbar_icon_footnote(HDC hdc, const RECT *rc, COLORREF fg) {
    toolbar_icon_text(hdc, rc, fg, "fn", 14);
}

static void toolbar_icon_hrule(HDC hdc, const RECT *rc, COLORREF fg) {
    HPEN pen = CreatePen(PS_SOLID, 2, fg);
    HPEN old = (HPEN)SelectObject(hdc, pen);
    int y = (rc->top + rc->bottom) / 2;
    MoveToEx(hdc, rc->left + 3, y, NULL);
    LineTo(hdc, rc->right - 3, y);
    SelectObject(hdc, old);
    DeleteObject(pen);
}

static void toolbar_icon_alert(HDC hdc, const RECT *rc, COLORREF fg) {
    toolbar_icon_text(hdc, rc, fg, "!", 18);
}

struct ToolbarButtonDef {
    WORD id;
    ToolbarIconPaintFn paint; /* NULL marks a separator (a small gap, no button) */
    const char *tip;
};
static const struct ToolbarButtonDef TOOLBAR_BUTTONS[] = {
    { IDM_FMT_BOLD, toolbar_icon_bold, "Bold" },
    { IDM_FMT_ITALIC, toolbar_icon_italic, "Italic" },
    { IDM_FMT_STRIKETHROUGH, toolbar_icon_strikethrough, "Strikethrough" },
    { IDM_FMT_CODE, toolbar_icon_code, "Inline code" },
    { 0, NULL, NULL },
    { IDM_FMT_H1, toolbar_icon_h1, "Heading 1" },
    { IDM_FMT_H2, toolbar_icon_h2, "Heading 2" },
    { IDM_FMT_H3, toolbar_icon_h3, "Heading 3" },
    { IDM_FMT_QUOTE, toolbar_icon_quote, "Blockquote" },
    { 0, NULL, NULL },
    { IDM_FMT_BULLETLIST, toolbar_icon_bullet_list, "Bullet list" },
    { IDM_FMT_NUMBERLIST, toolbar_icon_number_list, "Numbered list" },
    { IDM_FMT_TASKLIST, toolbar_icon_task_list, "Task list" },
    { 0, NULL, NULL },
    { IDM_FMT_LINK, toolbar_icon_link, "Insert link" },
    { IDM_FMT_IMAGE, toolbar_icon_image, "Insert image" },
    { IDM_FMT_TABLE, toolbar_icon_table, "Insert table" },
    { IDM_FMT_CODEBLOCK, toolbar_icon_codeblock, "Insert code block" },
    { IDM_FMT_FOOTNOTE, toolbar_icon_footnote, "Insert footnote" },
    { IDM_FMT_HRULE, toolbar_icon_hrule, "Insert horizontal rule" },
    { IDM_FMT_ALERT, toolbar_icon_alert, "Insert alert" },
    { 0, NULL, NULL },
    { IDM_VIEW_TOGGLEPREVIEW, toolbar_icon_togglepreview, "Toggle preview" },
    { IDM_VIEW_PREVIEW_ON_LEFT, toolbar_icon_swappanels, "Swap panel sides" },
};
#define TOOLBAR_BUTTON_COUNT (int)(sizeof(TOOLBAR_BUTTONS) / sizeof(TOOLBAR_BUTTONS[0]))
#define TOOLBAR_BUTTON_SIZE (TOOLBAR_HEIGHT - 6)
static HWND g_hToolbarButtons[TOOLBAR_BUTTON_COUNT];

static void create_toolbar(HWND hwnd) {
    for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++) {
        if (!TOOLBAR_BUTTONS[i].paint) continue;
        WCHAR *tip = to_wide(TOOLBAR_BUTTONS[i].tip);
        g_hToolbarButtons[i] = CreateWindowExW(0, L"BUTTON", tip ? tip : L"",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)TOOLBAR_BUTTONS[i].id, GetModuleHandleA(NULL), NULL);
        /* The window text isn't displayed (owner-drawn), but is kept as the
           accessible name / tooltip-eligible text for the button. */
        free(tip);
    }
}

/* Paint one owner-drawn toolbar button: a flat/pressed frame around the
   button's icon, drawn via the icon's paint function. Called from
   WM_DRAWITEM in WndProc. Windows reports ODS_SELECTED automatically for
   owner-draw buttons while the mouse is held down over them; true hover
   highlighting would need per-button subclassing to track WM_MOUSEMOVE/
   WM_MOUSELEAVE, which isn't worth the complexity for this toolbar. */
static void draw_toolbar_button(const DRAWITEMSTRUCT *dis) {
    int index = -1;
    for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++) {
        if (g_hToolbarButtons[i] == dis->hwndItem) { index = i; break; }
    }
    if (index < 0 || !TOOLBAR_BUTTONS[index].paint) return;

    COLORREF bg = g_previewDarkMode ? RGB(0x0D, 0x11, 0x17) : RGB(0xFF, 0xFF, 0xFF);
    COLORREF fg = g_previewDarkMode ? RGB(0xE6, 0xED, 0xF3) : RGB(0x1F, 0x23, 0x28);
    COLORREF pressedBg = g_previewDarkMode ? RGB(0x30, 0x36, 0x3D) : RGB(0xE1, 0xE7, 0xEC);

    int pressed = (dis->itemState & ODS_SELECTED) != 0;
    COLORREF fillColor = pressed ? pressedBg : bg;

    HBRUSH fillBrush = CreateSolidBrush(fillColor);
    FillRect(dis->hDC, &dis->rcItem, fillBrush);
    DeleteObject(fillBrush);

    if (dis->itemState & ODS_FOCUS) {
        HPEN focusPen = CreatePen(PS_SOLID, 1, fg);
        HPEN oldPen = (HPEN)SelectObject(dis->hDC, focusPen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
        Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
        SelectObject(dis->hDC, oldPen);
        SelectObject(dis->hDC, oldBrush);
        DeleteObject(focusPen);
    }

    RECT iconRc = dis->rcItem;
    InflateRect(&iconRc, -2, -2);
    TOOLBAR_BUTTONS[index].paint(dis->hDC, &iconRc, fg);
}

/* Lay the toolbar buttons out left-to-right along the reserved top strip,
   with a wider gap standing in for a separator between groups. All buttons
   are the same size, sized to the toolbar's height. */
static int layout_toolbar(int width, int top) {
    int x = 4, y = top + 3;
    for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++) {
        if (!TOOLBAR_BUTTONS[i].paint) {
            if (x > 4) x += 10;
            continue;
        }
        HWND hBtn = g_hToolbarButtons[i];
        if (!hBtn) continue;
        if (x > 4 && x + TOOLBAR_BUTTON_SIZE > width - 4) {
            x = 4;
            y += TOOLBAR_BUTTON_SIZE + 4;
        }
        MoveWindow(hBtn, x, y, TOOLBAR_BUTTON_SIZE, TOOLBAR_BUTTON_SIZE, TRUE);
        x += TOOLBAR_BUTTON_SIZE + 4;
    }
    return y + TOOLBAR_BUTTON_SIZE + 3;
}

static void layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;

    /* The formatting toolbar edits Markdown source, so it's only useful (and
       only shown) while the editor pane is visible. */
    MoveWindow(g_hTabs, 0, 0, w, TAB_HEIGHT, TRUE);
    int y = TAB_HEIGHT;
    for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++) {
        if (g_hToolbarButtons[i]) ShowWindow(g_hToolbarButtons[i], g_editorVisible ? SW_SHOW : SW_HIDE);
    }
    if (g_editorVisible) y = layout_toolbar(w, y);
    h -= y;
    if (h < 0) h = 0;

    if (g_editorVisible && !g_previewVisible) {
        MoveWindow(g_hEdit, 0, y, w, h, TRUE);
        ShowWindow(g_hEdit, SW_SHOW);
        ShowWindow(g_hPreview, SW_HIDE);
        RECT zero = { 0, 0, 0, 0 };
        set_webview_bounds(zero, FALSE);
        return;
    }

    if (!g_editorVisible && g_previewVisible) {
        ShowWindow(g_hEdit, SW_HIDE);
        ShowWindow(g_hPreview, SW_SHOW);
        MoveWindow(g_hPreview, 0, y, w, h, TRUE);
        /* WebView2 controller bounds are relative to g_hPreview's own client
           origin (which MoveWindow already positioned), not the main window. */
        RECT wvRc = { 0, 0, w, h };
        set_webview_bounds(wvRc, TRUE);
        return;
    }

    /* both visible (the !g_editorVisible && !g_previewVisible case is prevented by the toggle handlers) */
    if (g_splitterPos < 0)
        g_splitterPos = MulDiv(w - SPLITTER_WIDTH, g_splitterRatio, 1000);
    if (g_splitterPos < 100) g_splitterPos = 100;
    if (g_splitterPos > w - 100) g_splitterPos = (w - 100 > 100) ? w - 100 : 100;

    ShowWindow(g_hEdit, SW_SHOW);
    ShowWindow(g_hPreview, SW_SHOW);
    int rightWidth = w - g_splitterPos - SPLITTER_WIDTH;
    RECT wvRc;
    if (g_previewOnLeft) {
        MoveWindow(g_hPreview, 0, y, g_splitterPos, h, TRUE);
        MoveWindow(g_hEdit, g_splitterPos + SPLITTER_WIDTH, y, rightWidth, h, TRUE);
        wvRc = (RECT){ 0, 0, g_splitterPos, h };
    } else {
        MoveWindow(g_hEdit, 0, y, g_splitterPos, h, TRUE);
        MoveWindow(g_hPreview, g_splitterPos + SPLITTER_WIDTH, y, rightWidth, h, TRUE);
        wvRc = (RECT){ 0, 0, rightWidth, h };
    }
    set_webview_bounds(wvRc, TRUE);
}

static void update_view_menu(HWND hwnd) {
    HMENU hMenuBar = GetMenu(hwnd);
    HMENU hView = GetSubMenu(hMenuBar, 3); /* File, Edit, Format, View */
    CheckMenuItem(hView, IDM_VIEW_TOGGLEPREVIEW, MF_BYCOMMAND | (g_previewVisible ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_TOGGLEEDITOR, MF_BYCOMMAND | (g_editorVisible ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_PREVIEW_ON_LEFT, MF_BYCOMMAND | (g_previewOnLeft ? MF_CHECKED : MF_UNCHECKED));
    HMENU hTheme = GetSubMenu(hView, 6);
    if (hTheme) {
        CheckMenuRadioItem(hTheme, IDM_VIEW_THEME_GITHUB_LIGHT, IDM_VIEW_THEME_GITHUB_DARK,
                           g_previewDarkMode ? IDM_VIEW_THEME_GITHUB_DARK : IDM_VIEW_THEME_GITHUB_LIGHT,
                           MF_BYCOMMAND);
    }
}

static void toggle_preview(HWND hwnd) {
    if (g_previewVisible && !g_editorVisible) return; /* would leave nothing visible */
    g_previewVisible = !g_previewVisible;
    update_view_menu(hwnd);
    layout(hwnd);
    save_layout_settings();
}

static void toggle_editor(HWND hwnd) {
    if (g_editorVisible && !g_previewVisible) return; /* would leave nothing visible */
    g_editorVisible = !g_editorVisible;
    update_view_menu(hwnd);
    layout(hwnd);
    save_layout_settings();
}

static void toggle_panel_sides(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (g_splitterPos >= 0 && rc.right > SPLITTER_WIDTH) {
        g_splitterPos = rc.right - SPLITTER_WIDTH - g_splitterPos;
    }
    g_previewOnLeft = !g_previewOnLeft;
    save_layout_settings();
    update_view_menu(hwnd);
    layout(hwnd);
}

static int splitter_contains_x(int x) {
    return g_previewVisible && g_editorVisible &&
           x >= g_splitterPos && x <= g_splitterPos + SPLITTER_WIDTH;
}

static void select_theme(HWND hwnd, int darkMode) {
    g_previewDarkMode = darkMode;
    save_theme_settings();
    apply_preview_theme();
    apply_editor_theme(g_hEdit);
    apply_preview_host_theme();
    apply_help_theme();
    apply_main_theme(hwnd);
    for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++) {
        if (g_hToolbarButtons[i]) InvalidateRect(g_hToolbarButtons[i], NULL, TRUE);
    }
    if (g_hTabs) InvalidateRect(g_hTabs, NULL, TRUE);
    refresh_menu(hwnd);
}

static void handle_main_command(HWND hwnd, WORD cmd) {
    if (cmd >= IDM_FILE_RECENT_0 && cmd < IDM_FILE_RECENT_0 + MAX_RECENT_FILES) {
        do_open_recent(cmd - IDM_FILE_RECENT_0);
        return;
    }
    if (cmd >= IDM_FMT_BOLD && cmd <= IDM_FMT_ALERT) {
        toolbar_command(cmd);
        return;
    }

    switch (cmd) {
        case IDM_FILE_NEW: do_new(); break;
        case IDM_FILE_OPEN: do_open(); break;
        case IDM_FILE_SAVE: do_save(); break;
        case IDM_FILE_SAVEAS: do_saveas(); break;
        case IDM_FILE_EXPORT_HTML: do_export_html(); break;
        case IDM_FILE_CLOSE: do_close_document(); break;
        case IDM_FILE_REOPEN_CLOSED: do_reopen_closed_tab(); break;
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
        case IDM_EDIT_GOTOLINE: show_goto_line_dialog(); break;
        case IDM_EDIT_FINDNEXT:
            if (g_findWhat[0]) {
                char *findUtf8 = to_utf8(g_findWhat);
                if (findUtf8) find_in_edit(findUtf8, (g_fr.Flags & FR_MATCHCASE) ? 1 : 0, 1);
                free(findUtf8);
            } else show_find_dialog(0);
            break;
        case IDM_FORMAT_FONT: do_choose_font(); break;
        case IDM_VIEW_TOGGLEPREVIEW: toggle_preview(hwnd); break;
        case IDM_VIEW_TOGGLEEDITOR: toggle_editor(hwnd); break;
        case IDM_VIEW_PREVIEW_ON_LEFT: toggle_panel_sides(hwnd); break;
        case IDM_VIEW_NEXT_TAB:
            if (g_documentCount > 1) switch_document((g_activeDocument + 1) % g_documentCount);
            break;
        case IDM_VIEW_PREVIOUS_TAB:
            if (g_documentCount > 1) switch_document((g_activeDocument + g_documentCount - 1) % g_documentCount);
            break;
        case IDM_VIEW_THEME_GITHUB_LIGHT:
        case IDM_VIEW_THEME_GITHUB_DARK:
            select_theme(hwnd, cmd == IDM_VIEW_THEME_GITHUB_DARK);
            break;
        case IDM_HELP_REFERENCE: show_help_reference(); break;
    }
}

static RECT tab_close_rectangle(const RECT *tabRect) {
    RECT close = *tabRect;
    close.left = close.right - 23;
    close.right -= 5;
    close.top += 5;
    close.bottom -= 5;
    return close;
}

static void draw_tab_item(const DRAWITEMSTRUCT *dis) {
    int index = (int)dis->itemID;
    if (index < 0 || index >= g_documentCount) return;
    int selected = TabCtrl_GetCurSel(g_hTabs) == index;
    COLORREF bg = g_previewDarkMode
        ? (selected ? RGB(0x0D, 0x11, 0x17) : RGB(0x16, 0x1B, 0x22))
        : (selected ? RGB(0xFF, 0xFF, 0xFF) : RGB(0xF0, 0xF3, 0xF6));
    COLORREF fg = g_previewDarkMode
        ? (selected ? RGB(0xF0, 0xF6, 0xFC) : RGB(0xB1, 0xBA, 0xC4))
        : RGB(0x1F, 0x23, 0x28);
    COLORREF border = g_previewDarkMode ? RGB(0x30, 0x36, 0x3D) : RGB(0xD0, 0xD7, 0xDE);
    HBRUSH brush = CreateSolidBrush(bg);
    FillRect(dis->hDC, &dis->rcItem, brush);
    DeleteObject(brush);
    HPEN borderPen = CreatePen(PS_SOLID, 1, border);
    HPEN oldPen = (HPEN)SelectObject(dis->hDC, borderPen);
    MoveToEx(dis->hDC, dis->rcItem.right - 1, dis->rcItem.top + 2, NULL);
    LineTo(dis->hDC, dis->rcItem.right - 1, dis->rcItem.bottom - 2);

    RECT textRect = dis->rcItem;
    textRect.left += 9;
    textRect.right -= 33;
    WCHAR label[MAX_PATH + 32];
    format_document_label(&g_documents[index], label, sizeof(label) / sizeof(label[0]));
    HFONT font = (HFONT)SendMessageW(g_hTabs, WM_GETFONT, 0, 0);
    HFONT oldFont = font ? (HFONT)SelectObject(dis->hDC, font) : NULL;
    SetTextColor(dis->hDC, fg);
    SetBkMode(dis->hDC, TRANSPARENT);
    DrawTextW(dis->hDC, label, -1, &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (oldFont) SelectObject(dis->hDC, oldFont);

    RECT close = tab_close_rectangle(&dis->rcItem);
    int pad = 4;
    HPEN closePen = CreatePen(PS_SOLID, 1, fg);
    SelectObject(dis->hDC, closePen);
    MoveToEx(dis->hDC, close.left + pad, close.top + pad, NULL);
    LineTo(dis->hDC, close.right - pad, close.bottom - pad);
    MoveToEx(dis->hDC, close.right - pad, close.top + pad, NULL);
    LineTo(dis->hDC, close.left + pad, close.bottom - pad);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(closePen);
    DeleteObject(borderPen);

    if (g_documents[index].externalChanged) {
        COLORREF changedColor = RGB(0xD2, 0x99, 0x22);
        HBRUSH changed = CreateSolidBrush(changedColor);
        HPEN changedPen = CreatePen(PS_SOLID, 1, changedColor);
        HBRUSH oldBrush = (HBRUSH)SelectObject(dis->hDC, changed);
        HPEN changedOldPen = (HPEN)SelectObject(dis->hDC, changedPen);
        Ellipse(dis->hDC, close.left - 9, (close.top + close.bottom) / 2 - 3,
                close.left - 3, (close.top + close.bottom) / 2 + 3);
        SelectObject(dis->hDC, changedOldPen);
        SelectObject(dis->hDC, oldBrush);
        DeleteObject(changedPen);
        DeleteObject(changed);
    }
}

static LRESULT CALLBACK TabSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR subclassId, DWORD_PTR refData) {
    (void)subclassId; (void)refData;
    if (msg == WM_ERASEBKGND && g_previewDarkMode) {
        RECT client;
        GetClientRect(hwnd, &client);
        HBRUSH brush = CreateSolidBrush(RGB(0x01, 0x04, 0x09));
        FillRect((HDC)wParam, &client, brush);
        DeleteObject(brush);
        return 1;
    } else if (msg == WM_PAINT && g_previewDarkMode) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        HBRUSH brush = CreateSolidBrush(RGB(0x01, 0x04, 0x09));
        FillRect(dc, &client, brush);
        DeleteObject(brush);
        int selected = TabCtrl_GetCurSel(hwnd);
        for (int i = 0; i < g_documentCount; i++) {
            RECT itemRect;
            if (!TabCtrl_GetItemRect(hwnd, i, &itemRect)) continue;
            DRAWITEMSTRUCT dis = {0};
            dis.CtlType = ODT_TAB;
            dis.CtlID = ID_TABS;
            dis.itemID = (UINT)i;
            dis.itemAction = ODA_DRAWENTIRE;
            dis.itemState = i == selected ? ODS_SELECTED : 0;
            dis.hwndItem = hwnd;
            dis.hDC = dc;
            dis.rcItem = itemRect;
            draw_tab_item(&dis);
        }
        EndPaint(hwnd, &ps);
        return 0;
    } else if (msg == WM_LBUTTONUP || msg == WM_MBUTTONUP) {
        TCHITTESTINFO hit = {0};
        hit.pt.x = GET_X_LPARAM(lParam);
        hit.pt.y = GET_Y_LPARAM(lParam);
        int index = TabCtrl_HitTest(hwnd, &hit);
        if (index >= 0) {
            RECT tabRect;
            TabCtrl_GetItemRect(hwnd, index, &tabRect);
            RECT close = tab_close_rectangle(&tabRect);
            POINT point = hit.pt;
            if (msg == WM_MBUTTONUP || PtInRect(&close, point)) {
                PostMessageW(g_hMainWnd, WM_APP_CLOSE_TAB, (WPARAM)index, 0);
                return 0;
            }
        }
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, TabSubclassProc, 1);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void initialize_main_window(HWND hwnd) {
    g_hMainWnd = hwnd;

    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    g_fontMono = CreateFontA(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");
    ReleaseDC(hwnd, hdc);

    SetMenu(hwnd, build_menu());
    create_toolbar(hwnd);

    g_hTabs = CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER | TCS_TOOLTIPS |
        TCS_FIXEDWIDTH | TCS_FORCELABELLEFT | TCS_RAGGEDRIGHT | TCS_OWNERDRAWFIXED,
        0, 0, 0, 0, hwnd, (HMENU)ID_TABS, GetModuleHandleA(NULL), NULL);
    /* A compact fixed width keeps more documents accessible without making
       short filenames consume excessive space. The tooltip below supplies
       the full path whenever the visible label is clipped. */
    SendMessageW(g_hTabs, TCM_SETITEMSIZE, 0,
        MAKELPARAM(MulDiv(120, dpi, 96), MulDiv(22, dpi, 96)));
    HWND tabTooltips = TabCtrl_GetToolTips(g_hTabs);
    if (tabTooltips) {
        SendMessageW(tabTooltips, CCM_SETUNICODEFORMAT, TRUE, 0);
        SendMessageW(tabTooltips, TTM_SETMAXTIPWIDTH, 0, 700);
    }
    SetWindowSubclass(g_hTabs, TabSubclassProc, 1, 0);

    g_hEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "Scintilla", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL,
        0, 0, 0, 0, hwnd, (HMENU)ID_EDIT, GetModuleHandleA(NULL), NULL);
    configure_scintilla(g_hEdit);
    apply_editor_font(g_fontFaceName, g_fontPointSize);

    g_documentCount = 1;
    g_activeDocument = 0;
    memset(&g_documents[0], 0, sizeof(g_documents[0]));
    g_documents[0].sciDocument = (sptr_t)SendMessageA(g_hEdit, SCI_GETDOCPOINTER, 0, 0);
    SendMessageA(g_hEdit, SCI_ADDREFDOCUMENT, 0, (LPARAM)g_documents[0].sciDocument);
    g_documents[0].untitledNumber = g_nextUntitledNumber++;
    TCITEMW firstTab = {0};
    firstTab.mask = TCIF_TEXT;
    firstTab.pszText = L"";
    SendMessageW(g_hTabs, TCM_INSERTITEMW, 0, (LPARAM)&firstTab);
    update_tab_label(0);

    SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    /* Establish a clean initial document state before the window is shown;
       New/Open perform the same reset after replacing text. */
    set_dirty(0);
    update_preview();

    apply_preview_host_theme();
    apply_main_theme(hwnd);
    g_hPreview = CreateWindowExA(WS_EX_CLIENTEDGE, "STATIC", "",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd, (HMENU)ID_PREVIEW, GetModuleHandleA(NULL), NULL);

    init_webview();
    SetTimer(hwnd, IDT_FILE_WATCH, 1500, NULL);
    layout(hwnd);
}

static void handle_editor_notification(HWND hwnd, NMHDR *notification) {
    if (notification->hwndFrom != g_hEdit) return;

    struct SCNotification *scn = (struct SCNotification *)notification;
    if (notification->code == SCN_UPDATEUI) {
        if (scn->updated & SC_UPDATE_V_SCROLL) {
            if (g_ignoreNextEditorScroll) g_ignoreNextEditorScroll = 0;
            else SetTimer(hwnd, IDT_SCROLL_SYNC, 35, NULL);
        }
        return;
    }
    if (notification->code != SCN_MODIFIED) return;
    if (!(scn->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) return;

    if (!g_switchingDocument) set_dirty(1);
    SetTimer(hwnd, IDT_PREVIEW_UPDATE, 150, NULL);
    if (scn->linesAdded != 0) update_linenumber_margin_width(g_hEdit);
}

static void destroy_main_window(HWND hwnd) {
    save_layout_settings();
    KillTimer(hwnd, IDT_PREVIEW_UPDATE);
    KillTimer(hwnd, IDT_FILE_WATCH);
    KillTimer(hwnd, IDT_SCROLL_SYNC);
    if (g_webview && g_webMessageRegistered) {
        ICoreWebView2_remove_WebMessageReceived(g_webview, g_webMessageToken);
        g_webMessageRegistered = 0;
    }
    if (g_webviewController) ICoreWebView2Controller_Close(g_webviewController);
    if (g_webview3) { ICoreWebView2_3_Release(g_webview3); g_webview3 = NULL; }
    if (g_webview) { ICoreWebView2_Release(g_webview); g_webview = NULL; }
    if (g_webviewController) { ICoreWebView2Controller_Release(g_webviewController); g_webviewController = NULL; }
    free(g_pendingHtmlBody);
    g_pendingHtmlBody = NULL;
    free(g_exportPath);
    g_exportPath = NULL;
    if (g_previewBkBrush) { DeleteObject(g_previewBkBrush); g_previewBkBrush = NULL; }
    if (g_helpBkBrush) { DeleteObject(g_helpBkBrush); g_helpBkBrush = NULL; }
    if (g_mainBkBrush) { DeleteObject(g_mainBkBrush); g_mainBkBrush = NULL; }
    if (g_fontMono) { DeleteObject(g_fontMono); g_fontMono = NULL; }
    for (int i = 0; i < g_recentFileCount; i++) free(g_recentFiles[i]);
    for (int i = 0; i < g_documentCount; i++) free(g_documents[i].path);
    for (int i = 0; i < g_closedDocumentCount; i++) free(g_closedDocuments[i].path);
    PostQuitMessage(0);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_msgFindReplace) {
        handle_findmsg((FINDREPLACEW *)lParam);
        return 0;
    }
    switch (msg) {
        case WM_CREATE:
            initialize_main_window(hwnd);
            return 0;
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
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_BUTTON) {
                draw_toolbar_button(dis);
                return TRUE;
            }
            if (dis->CtlType == ODT_TAB && dis->hwndItem == g_hTabs) {
                draw_tab_item(dis);
                return TRUE;
            }
            break;
        }
        /* g_hEdit/g_hPreview/the toolbar buttons cover the rest of the client
           area themselves; this only ever paints the toolbar strip's margins
           and the splitter gap, so filling the whole client rect is fine -
           there's nothing else left for DefWindowProc's COLOR_WINDOW brush to
           paint that isn't about to be covered by a child window anyway. */
        case WM_ERASEBKGND:
            if (g_mainBkBrush) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                FillRect((HDC)wParam, &rc, g_mainBkBrush);
                return 1;
            }
            break;
        case WM_TIMER:
            if (wParam == IDT_PREVIEW_UPDATE) {
                update_preview();
                return 0;
            }
            if (wParam == IDT_FILE_WATCH) {
                check_external_file_change();
                return 0;
            }
            if (wParam == IDT_SCROLL_SYNC) {
                sync_preview_to_editor_scroll();
                return 0;
            }
            break;
        case WM_SIZE:
            layout(hwnd);
            return 0;
        case WM_SETCURSOR: {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (splitter_contains_x(pt.x)) {
                SetCursor(LoadCursorA(NULL, (LPCSTR)IDC_SIZEWE));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam);
            if (splitter_contains_x(x)) {
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
                save_layout_settings();
            }
            return 0;
        }
        case WM_DROPFILES:
            handle_dropfiles((HDROP)wParam);
            return 0;
        case WM_NOTIFY: {
            NMHDR *notification = (NMHDR *)lParam;
            if (notification->hwndFrom == g_hTabs && notification->code == TCN_SELCHANGE) {
                switch_document(TabCtrl_GetCurSel(g_hTabs));
            } else if ((notification->code == TTN_GETDISPINFOW ||
                        notification->code == TTN_GETDISPINFOA) && g_hTabs &&
                       notification->hwndFrom == TabCtrl_GetToolTips(g_hTabs)) {
                int index = (int)notification->idFrom;
                const WCHAR *wideText = (index >= 0 && index < g_documentCount &&
                    g_documents[index].path) ? g_documents[index].path : L"Unsaved document";
                if (notification->code == TTN_GETDISPINFOW) {
                    ((NMTTDISPINFOW *)notification)->lpszText = (WCHAR *)wideText;
                } else {
                    WideCharToMultiByte(CP_ACP, 0, wideText, -1, g_tabTooltipAnsi,
                        sizeof(g_tabTooltipAnsi), NULL, NULL);
                    ((NMTTDISPINFOA *)notification)->lpszText = g_tabTooltipAnsi;
                }
            } else {
                handle_editor_notification(hwnd, notification);
            }
            return 0;
        }
        case WM_COMMAND:
            handle_main_command(hwnd, LOWORD(wParam));
            return 0;
        case WM_APP_CLOSE_TAB:
            do_close_document_at((int)wParam);
            return 0;
        case WM_CLOSE:
            do_exit();
            return 0;
        case WM_QUERYENDSESSION:
            save_layout_settings();
            save_session_documents();
            return TRUE;
        case WM_DESTROY:
            destroy_main_window(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    INITCOMMONCONTROLSEX commonControls = { sizeof(commonControls), ICC_TAB_CLASSES };
    InitCommonControlsEx(&commonControls);

    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    WCHAR *cliFile = (argv && argc > 1) ? wcsdup_heap(argv[1]) : NULL;
    if (argv) LocalFree(argv);
    g_commandLinePreviewMode = cliFile != NULL;
    if (cliFile) {
        /* command-line launch to view a file: show just the rendered preview */
        g_previewVisible = 1;
        g_editorVisible = 0;
    } else {
        /* plain launch with no file: show just the editor */
        g_previewVisible = 0;
        g_editorVisible = 1;
    }

    HMODULE scintillaModule = LoadLibraryW(L"Scintilla.dll");
    if (!scintillaModule) {
        MessageBoxW(NULL, L"Could not load Scintilla.dll (expected next to markedit.exe).", L"MarkEdit", MB_OK | MB_ICONERROR);
        free(cliFile);
        return 1;
    }

    load_font_settings();
    load_theme_settings();
    load_layout_settings();
    if (g_commandLinePreviewMode) {
        /* File-association/command-line launches are preview-only for this
           session, without overwriting the user's normal panel visibility. */
        g_previewVisible = 1;
        g_editorVisible = 0;
    }
    load_recent_files();
    prune_missing_recent_files();

    HRESULT comResult = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    g_msgFindReplace = RegisterWindowMessageW(FINDMSGSTRINGW);

    HICON hIconBig = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    HICON hIconSmall = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"MarkEditWndClass";
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); /* IDC_ARROW */
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = hIconBig;
    wc.hIconSm = hIconSmall;
    RegisterClassExW(&wc);

    int windowX = g_haveSavedWindowRect ? g_savedWindowRect.left : CW_USEDEFAULT;
    int windowY = g_haveSavedWindowRect ? g_savedWindowRect.top : CW_USEDEFAULT;
    int windowWidth = g_haveSavedWindowRect
        ? g_savedWindowRect.right - g_savedWindowRect.left : 1100;
    int windowHeight = g_haveSavedWindowRect
        ? g_savedWindowRect.bottom - g_savedWindowRect.top : 750;
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, L"MarkEditWndClass", L"Untitled - MarkEdit",
        WS_OVERLAPPEDWINDOW,
        windowX, windowY, windowWidth, windowHeight,
        NULL, NULL, hInstance, NULL);

    if (hIconBig) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    if (hIconSmall) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);

    if (cliFile) load_file(cliFile);
    else restore_session_documents();
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
        { FVIRTKEY | FCONTROL, 'G', IDM_EDIT_GOTOLINE },
        { FVIRTKEY | FCONTROL, 'W', IDM_FILE_CLOSE },
        { FVIRTKEY | FCONTROL | FSHIFT, 'T', IDM_FILE_REOPEN_CLOSED },
        { FVIRTKEY | FCONTROL, VK_TAB, IDM_VIEW_NEXT_TAB },
        { FVIRTKEY | FCONTROL | FSHIFT, VK_TAB, IDM_VIEW_PREVIOUS_TAB },
        { FVIRTKEY, VK_F3, IDM_EDIT_FINDNEXT },
        { FVIRTKEY | FCONTROL | FSHIFT, 'V', IDM_VIEW_TOGGLEPREVIEW },
        { FVIRTKEY | FCONTROL | FSHIFT, 'E', IDM_VIEW_TOGGLEEDITOR },
    };
    HACCEL hAccel = CreateAcceleratorTableA(accels, sizeof(accels) / sizeof(accels[0]));

    ShowWindow(hwnd, g_startMaximized ? SW_SHOWMAXIMIZED : nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    int messageResult;
    while ((messageResult = GetMessage(&msg, NULL, 0, 0)) > 0) {
        if (g_hFindDlg && IsDialogMessage(g_hFindDlg, &msg)) continue;
        if (TranslateAcceleratorA(hwnd, hAccel, &msg)) continue;
        if (IsDialogMessage(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (hAccel) DestroyAcceleratorTable(hAccel);
    if (SUCCEEDED(comResult)) CoUninitialize();
    FreeLibrary(scintillaModule);
    return messageResult < 0 ? 1 : 0;
}
