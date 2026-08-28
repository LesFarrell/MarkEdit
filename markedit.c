/*
 * markedit - a tiny live-preview Markdown editor for Win32.
 * Left pane: plain-text edit control for Markdown source.
 * Right pane: WebView2-hosted HTML preview (supports Mermaid diagrams,
 * with native browser zoom/pan/scroll), re-rendered on every edit.
 *
 * Build with Zig on PATH by running build.bat (see premake5.lua for the
 * alternative MinGW-w64 GCC build).
 */

#define COBJMACROS

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <spellcheck.h>
#include <uxtheme.h>
#include <wincodec.h>
#include "json_formatter.h"
#include "markdown.h"
#include "path_utils.h"
#include "text_codec.h"

#include <WebView2.h>
#include "third_party/scintilla/include/Scintilla.h"
#include "third_party/lexilla/include/Lexilla.h"
#include "third_party/lexilla/include/SciLexer.h"

#include "markedit.h"

static HWND g_hEdit; /* Scintilla control */
static HWND g_hPreview; /* WebView2 host child window */
static HWND g_hTabs;
static HWND g_hStatus;
static HWND g_hOutline;
static HWND g_hMainWnd;
static WCHAR g_fontFaceName[LF_FACESIZE] = L"Consolas";
static int g_fontPointSize = 10;

static ICoreWebView2 *g_webview = NULL;
static ICoreWebView2Controller *g_webviewController = NULL;
static ICoreWebView2_3 *g_webview3 = NULL;
static ICoreWebView2ExecuteScriptCompletedHandler g_scriptHandler;
static int g_webviewReady = 0;
static int g_ignoreNextEditorScroll = 0;
static EventRegistrationToken g_webMessageToken;
static int g_webMessageRegistered = 0;
static HBRUSH g_previewBkBrush = NULL; /* matches the preview host's WM_CTLCOLORSTATIC background to the theme, so it doesn't flash white before WebView2 paints */
static HBRUSH g_mainBkBrush = NULL; /* fills the toolbar strip and splitter gap in the main window, matching the theme instead of the system COLOR_WINDOW default */
static HBRUSH g_menuBkBrush = NULL; /* owner-drawn top-level menu bar background */
static HFONT g_menuFont = NULL;
static int g_menuFontDpi = 0;
static char *g_pendingHtmlBody = NULL; /* set if update requested before webview ready */
static WCHAR *g_exportPath = NULL;
static WCHAR *g_pendingPdfPath = NULL;
static int g_pendingPrintAction = 0; /* 1 = print dialog, 2 = PDF export */
static int g_previewDarkMode = 0;
/* User-configurable Run/Debug C/C++ compiler backend; see load_compiler_settings()
   and find_native_compiler() further down. Auto keeps the existing GCC/Clang/
   Zig/MSVC priority search; the others pin Run/Debug to just one of them. */
typedef enum {
    COMPILER_BACKEND_AUTO = 0, COMPILER_BACKEND_GCC, COMPILER_BACKEND_CLANG,
    COMPILER_BACKEND_ZIG, COMPILER_BACKEND_MSVC
} CompilerBackendPreference;
static CompilerBackendPreference g_compilerBackend = COMPILER_BACKEND_AUTO;
static WCHAR *g_vcvarsallOverridePath = NULL; /* [Compiler] VcvarsallPath override; skips auto-detection */
static WCHAR *g_customCssPath = NULL;
static WCHAR *g_debugLocationPath = NULL;
static WCHAR *g_debugWorkingDirectory = NULL;
static WCHAR *g_lastDebugSource = NULL;
static HANDLE g_debugProcess = NULL;
static int g_debugLocationSeen = 0;
static int g_lastDebugLine = -1;
static int g_activeDebuggerKind = 0; /* 1 = pdb, 2 = GDB */
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
    int isReference; /* the Markdown reference, so Help reuses its tab */
} Document;

static Document g_documents[MAX_DOCUMENTS];
static int g_documentCount = 0;
static int g_activeDocument = 0;
static unsigned g_nextUntitledNumber = 1;
static int g_switchingDocument = 0;
static char g_tabTooltipAnsi[32768];
/* Set on every edit and every dirty-state change; cleared once
   write_recovery_snapshot() has brought the on-disk snapshot back in sync.
   Lets the 5s autosave timer skip its work entirely while idle instead of
   rewriting unchanged documents every tick. */
static int g_recoverySnapshotStale = 0;

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
static int g_focusMode = 0;
/* Markdown is prose: wrapping long paragraphs is the useful default. */
static int g_wordWrap = 0;
static int g_showWhitespace = 0;
static int g_indentGuides = 0;
static int g_spellCheckEnabled = 1;
static int g_outlineVisible = 0;
static int g_editorZoom = 0; /* Scintilla SCI_SETZOOM units: +1/-1 per level, 0 is 100%. */
static WCHAR g_spellLanguage[LOCALE_NAME_MAX_LENGTH] = L"";
static int g_outlineLines[1024];
static int g_outlineCount = 0;
static int g_tabDragIndex = -1;
static int g_tabDragged = 0;
static int g_activeLanguage = 0; /* Language of the active document; 0 is LANG_MARKDOWN. */
static ISpellChecker *g_spellChecker = NULL;

#define MAX_RECENT_FILES 10
static WCHAR *g_recentFiles[MAX_RECENT_FILES];
static int g_recentFileCount = 0;

static HWND g_hFindDlg = NULL;
static FINDREPLACEW g_fr;
static WCHAR g_findWhat[256] = L"";
static WCHAR g_replaceWith[256] = L"";
static DWORD g_findFlags = FR_DOWN;
static UINT g_msgFindReplace;

/* ---- simple growable text buffer, used to pull text out of the Scintilla control ---- */
static char *get_edit_text(HWND hEdit, int *outLen) {
    int len = (int)SendMessageA(hEdit, SCI_GETLENGTH, 0, 0);
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { if (outLen) *outLen = 0; return NULL; }
    SendMessageA(hEdit, SCI_GETTEXT, (WPARAM)(len + 1), (LPARAM)buf);
    if (outLen) *outLen = len;
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

/* Configure the Scintilla control as a source editor: a Lexilla lexer for
   syntax highlighting (a passive read-only colorizer - it never edits the
   buffer), real tab characters at a width of 4, CRLF line endings to match
   the rest of the app's file I/O, and only the change-notifications we
   actually act on. Smart Markdown input is handled explicitly by
   EditSubclassProc; Scintilla call tips remain disabled, and its autocomplete
   list is populated by show_word_completions() rather than by the lexer. */
#define MARGIN_LINENUMBERS 0
#define MARGIN_BOOKMARKS 1
#define MARGIN_FOLD 2
#define MARKER_BOOKMARK 1
#define MARKER_BREAKPOINT 2
#define MARKER_DEBUG_LINE 3
#define FOLD_MARGIN_WIDTH 14
#define INDICATOR_SPELLING 20
static int g_lineNumberDigits = 0;

/* Widen/narrow the line-number margin to fit the current line count (so "9"
   and "9999" both have comfortably-sized gutters, and it grows as the
   document grows past another power of ten). */
static void update_linenumber_margin_width(HWND hEdit) {
    int lineCount = (int)SendMessageA(hEdit, SCI_GETLINECOUNT, 0, 0);
    int digitCount = 1;
    for (int n = lineCount; n >= 10; n /= 10) digitCount++;
    if (digitCount == g_lineNumberDigits) return;
    g_lineNumberDigits = digitCount;
    char digits[16];
    wsprintfA(digits, "_%d", lineCount); /* leading '_' gives a little breathing room */
    int width = (int)SendMessageA(hEdit, SCI_TEXTWIDTH, STYLE_LINENUMBER, (LPARAM)digits);
    SendMessageA(hEdit, SCI_SETMARGINWIDTHN, MARGIN_LINENUMBERS, width);
}

/* Apply GitHub's light/dark palette to the editor so it matches the preview.
   Called once at startup and whenever the selected theme changes. */

/* ---- source language selection, driven by the document file extension ---- */

typedef enum {
    LANG_MARKDOWN = 0, LANG_JSON, LANG_XML, LANG_HTML, LANG_YAML,
    LANG_CPP, LANG_PYTHON, LANG_BASH, LANG_POWERSHELL, LANG_LUA
} Language;

typedef struct LanguageInfo {
    Language id;
    const char *lexer;
    const WCHAR *extensions; /* semicolon-delimited, each dot-prefixed */
    const char *keywords;    /* primary keyword list, NULL when the lexer needs none */
    const char *fenceTag;    /* Markdown info-string; lexer names are not always valid ones */
    int folds;               /* the vendored Markdown lexer emits no fold levels at all */
} LanguageInfo;

static const LanguageInfo LANGUAGES[] = {
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

#define LANGUAGE_COUNT ((int)(sizeof(LANGUAGES) / sizeof(LANGUAGES[0])))

/* Case-insensitive match of `extension` against one semicolon-delimited list. */
static int extension_in_list(const WCHAR *extension, const WCHAR *list) {
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

/* Picks the language for a document path. Markdown is the fallback, so an
   unknown or missing extension keeps the original behaviour. */
static const LanguageInfo *language_for_path(const WCHAR *path) {
    if (!path) return &LANGUAGES[LANG_MARKDOWN];
    const WCHAR *extension = PathFindExtensionW(path);
    if (!extension || !*extension) return &LANGUAGES[LANG_MARKDOWN];

    for (int l = 0; l < LANGUAGE_COUNT; l++) {
        if (extension_in_list(extension, LANGUAGES[l].extensions)) return &LANGUAGES[l];
    }
    return &LANGUAGES[LANG_MARKDOWN];
}
static void apply_editor_theme(HWND hEdit) {
    /* Scintilla draws its own client area but leaves the scrollbars to the
       system, which paints them light unless the window opts into the dark
       explorer theme. */
    SetWindowTheme(hEdit, g_previewDarkMode ? L"DarkMode_Explorer" : L"Explorer", NULL);

    if (g_previewDarkMode) {
        SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_DEFAULT, RGB(0xE6, 0xED, 0xF3));
        SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_DEFAULT, RGB(0x0D, 0x11, 0x17));
        SendMessageA(hEdit, SCI_STYLECLEARALL, 0, 0);

        SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_LINENUMBER, RGB(0x7D, 0x85, 0x90));
        SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_LINENUMBER, RGB(0x16, 0x1B, 0x22));

        SendMessageA(hEdit, SCI_SETCARETLINEBACK, RGB(0x21, 0x26, 0x2D), 0);
        SendMessageA(hEdit, SCI_SETCARETFORE, RGB(0xE6, 0xED, 0xF3), 0);
        SendMessageA(hEdit, SCI_SETSELBACK, TRUE, RGB(0x26, 0x4F, 0x78));
        SendMessageA(hEdit, SCI_SETSELFORE, TRUE, RGB(0xFF, 0xFF, 0xFF));
    } else {
        SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_DEFAULT, RGB(0x1F, 0x23, 0x28));
        SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_DEFAULT, RGB(0xFF, 0xFF, 0xFF));
        SendMessageA(hEdit, SCI_STYLECLEARALL, 0, 0);

        SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_LINENUMBER, RGB(0x65, 0x6D, 0x76));
        SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_LINENUMBER, RGB(0xF6, 0xF8, 0xFA));

        SendMessageA(hEdit, SCI_SETCARETLINEBACK, RGB(0xE8, 0xEC, 0xF1), 0);
        SendMessageA(hEdit, SCI_SETCARETFORE, RGB(0x1F, 0x23, 0x28), 0);
        SendMessageA(hEdit, SCI_SETSELBACK, TRUE, RGB(0xB6, 0xD7, 0xFF));
        SendMessageA(hEdit, SCI_SETSELFORE, TRUE, RGB(0x1F, 0x23, 0x28));
    }

    /* These follow SCI_STYLECLEARALL above, which resets every style. The
       gutter colours match STYLE_LINENUMBER so the margins read as one strip. */
    COLORREF faint = g_previewDarkMode ? RGB(0x6E, 0x76, 0x81) : RGB(0xAF, 0xB8, 0xC1);
    COLORREF gutter = g_previewDarkMode ? RGB(0x16, 0x1B, 0x22) : RGB(0xF6, 0xF8, 0xFA);
    COLORREF text = g_previewDarkMode ? RGB(0xE6, 0xED, 0xF3) : RGB(0x1F, 0x23, 0x28);

    SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_INDENTGUIDE, (LPARAM)faint);
    SendMessageA(hEdit, SCI_SETWHITESPACEFORE, TRUE, (LPARAM)faint);
    SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_BRACELIGHT, (LPARAM)text);
    SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_BRACELIGHT,
                 g_previewDarkMode ? RGB(0x3F, 0x46, 0x4F) : RGB(0xCF, 0xD8, 0xE3));
    SendMessageA(hEdit, SCI_STYLESETBOLD, STYLE_BRACELIGHT, TRUE);
    SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_BRACEBAD,
                 g_previewDarkMode ? RGB(0xFF, 0x7B, 0x72) : RGB(0xCF, 0x22, 0x2E));
    SendMessageA(hEdit, SCI_STYLESETBOLD, STYLE_BRACEBAD, TRUE);

    SendMessageA(hEdit, SCI_SETFOLDMARGINCOLOUR, TRUE, (LPARAM)gutter);
    SendMessageA(hEdit, SCI_SETFOLDMARGINHICOLOUR, TRUE, (LPARAM)gutter);
    for (int marker = SC_MARKNUM_FOLDEREND; marker <= SC_MARKNUM_FOLDEROPEN; marker++) {
        SendMessageA(hEdit, SCI_MARKERSETFORE, (WPARAM)marker, (LPARAM)gutter);
        SendMessageA(hEdit, SCI_MARKERSETBACK, (WPARAM)marker, (LPARAM)faint);
    }
    SendMessageA(hEdit, SCI_MARKERSETFORE, MARKER_BOOKMARK,
                 (LPARAM)(g_previewDarkMode ? RGB(0x1F, 0x6F, 0xEB) : RGB(0x09, 0x69, 0xDA)));
    SendMessageA(hEdit, SCI_MARKERSETBACK, MARKER_BOOKMARK,
                 (LPARAM)(g_previewDarkMode ? RGB(0x38, 0x8B, 0xFD) : RGB(0x21, 0x8B, 0xFF)));
    SendMessageA(hEdit, SCI_MARKERSETFORE, MARKER_BREAKPOINT, RGB(0xFF, 0xFF, 0xFF));
    SendMessageA(hEdit, SCI_MARKERSETBACK, MARKER_BREAKPOINT,
                 (LPARAM)(g_previewDarkMode ? RGB(0xF8, 0x51, 0x49) : RGB(0xCF, 0x22, 0x2E)));
    SendMessageA(hEdit, SCI_MARKERSETBACK, MARKER_DEBUG_LINE,
                 (LPARAM)(g_previewDarkMode ? RGB(0x9E, 0x6A, 0x03) : RGB(0xFF, 0xDF, 0x5D)));
    SendMessageA(hEdit, SCI_MARKERSETALPHA, MARKER_DEBUG_LINE, 90);

    SendMessageA(hEdit, SCI_INDICSETSTYLE, INDICATOR_SPELLING, INDIC_SQUIGGLE);
    SendMessageA(hEdit, SCI_INDICSETFORE, INDICATOR_SPELLING,
                 (LPARAM)(g_previewDarkMode ? RGB(0xFF, 0x7B, 0x72) : RGB(0xCF, 0x22, 0x2E)));
    SendMessageA(hEdit, SCI_INDICSETUNDER, INDICATOR_SPELLING, TRUE);

    apply_language_styles(hEdit);
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

/* The GitHub syntax palette, shared by every non-Markdown lexer so the editor
   keeps one look across languages. */
typedef struct SyntaxPalette {
    COLORREF comment, keyword, string, number, function, variable, tag, attribute, operatorColour;
} SyntaxPalette;

static SyntaxPalette syntax_palette(void) {
    SyntaxPalette p;
    if (g_previewDarkMode) {
        p.comment = RGB(0x8B, 0x94, 0x9E);
        p.keyword = RGB(0xFF, 0x7B, 0x72);
        p.string = RGB(0xA5, 0xD6, 0xFF);
        p.number = RGB(0x79, 0xC0, 0xFF);
        p.function = RGB(0xD2, 0xA8, 0xFF);
        p.variable = RGB(0xFF, 0xA6, 0x57);
        p.tag = RGB(0x7E, 0xE7, 0x87);
        p.attribute = RGB(0x79, 0xC0, 0xFF);
        p.operatorColour = RGB(0xE6, 0xED, 0xF3);
    } else {
        p.comment = RGB(0x6E, 0x77, 0x81);
        p.keyword = RGB(0xCF, 0x22, 0x2E);
        p.string = RGB(0x0A, 0x30, 0x69);
        p.number = RGB(0x09, 0x69, 0xDA);
        p.function = RGB(0x82, 0x50, 0xDF);
        p.variable = RGB(0x95, 0x38, 0x00);
        p.tag = RGB(0x11, 0x63, 0x29);
        p.attribute = RGB(0x09, 0x69, 0xDA);
        p.operatorColour = RGB(0x1F, 0x23, 0x28);
    }
    return p;
}

static void style_range(HWND hEdit, const int *styles, int count, COLORREF colour) {
    for (int i = 0; i < count; i++)
        SendMessageA(hEdit, SCI_STYLESETFORE, (WPARAM)styles[i], (LPARAM)colour);
}

static void apply_json_styles(HWND hEdit, const SyntaxPalette *p) {
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

static void apply_markup_styles(HWND hEdit, const SyntaxPalette *p) {
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

static void apply_yaml_styles(HWND hEdit, const SyntaxPalette *p) {
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_YAML_IDENTIFIER, (LPARAM)p->attribute);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_YAML_KEYWORD, (LPARAM)p->keyword);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_YAML_NUMBER, (LPARAM)p->number);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_YAML_COMMENT, (LPARAM)p->comment);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_YAML_REFERENCE, (LPARAM)p->variable);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_YAML_DOCUMENT, (LPARAM)p->comment);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_YAML_OPERATOR, (LPARAM)p->operatorColour);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_YAML_ERROR, (LPARAM)p->keyword);
}

static void apply_cpp_styles(HWND hEdit, const SyntaxPalette *p) {
    const int comments[] = { SCE_C_COMMENT, SCE_C_COMMENTLINE, SCE_C_COMMENTDOC, SCE_C_COMMENTLINEDOC };
    style_range(hEdit, comments, 4, p->comment);
    const int strings[] = { SCE_C_STRING, SCE_C_CHARACTER, SCE_C_VERBATIM, SCE_C_STRINGEOL };
    style_range(hEdit, strings, 4, p->string);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_C_WORD, (LPARAM)p->keyword);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_C_WORD2, (LPARAM)p->function);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_C_NUMBER, (LPARAM)p->number);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_C_PREPROCESSOR, (LPARAM)p->variable);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_C_OPERATOR, (LPARAM)p->operatorColour);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_C_REGEX, (LPARAM)p->string);
}

static void apply_python_styles(HWND hEdit, const SyntaxPalette *p) {
    const int comments[] = { SCE_P_COMMENTLINE, SCE_P_COMMENTBLOCK };
    style_range(hEdit, comments, 2, p->comment);
    const int strings[] = { SCE_P_STRING, SCE_P_CHARACTER, SCE_P_TRIPLE, SCE_P_TRIPLEDOUBLE, SCE_P_FSTRING };
    style_range(hEdit, strings, 5, p->string);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_P_WORD, (LPARAM)p->keyword);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_P_NUMBER, (LPARAM)p->number);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_P_DEFNAME, (LPARAM)p->function);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_P_CLASSNAME, (LPARAM)p->function);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_P_DECORATOR, (LPARAM)p->variable);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_P_OPERATOR, (LPARAM)p->operatorColour);
}

static void apply_bash_styles(HWND hEdit, const SyntaxPalette *p) {
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

static void apply_powershell_styles(HWND hEdit, const SyntaxPalette *p) {
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
    style_range(hEdit, functions, 3, p->function);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_POWERSHELL_OPERATOR, (LPARAM)p->operatorColour);
}

static void apply_lua_styles(HWND hEdit, const SyntaxPalette *p) {
    const int comments[] = { SCE_LUA_COMMENT, SCE_LUA_COMMENTLINE, SCE_LUA_COMMENTDOC };
    style_range(hEdit, comments, 3, p->comment);
    const int strings[] = { SCE_LUA_STRING, SCE_LUA_CHARACTER, SCE_LUA_LITERALSTRING, SCE_LUA_STRINGEOL };
    style_range(hEdit, strings, 4, p->string);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_LUA_WORD, (LPARAM)p->keyword);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_LUA_NUMBER, (LPARAM)p->number);
    const int functions[] = { SCE_LUA_WORD2, SCE_LUA_WORD3 };
    style_range(hEdit, functions, 2, p->function);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_LUA_IDENTIFIER, (LPARAM)p->variable);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_LUA_PREPROCESSOR, (LPARAM)p->variable);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_LUA_OPERATOR, (LPARAM)p->operatorColour);
    SendMessageA(hEdit, SCI_STYLESETFORE, SCE_LUA_LABEL, (LPARAM)p->function);
}

/* Applies the current theme colours for whichever language is active. */
static void apply_language_styles(HWND hEdit) {
    if (g_activeLanguage == LANG_MARKDOWN) {
        apply_markdown_lexer_styles(hEdit);
        return;
    }

    SyntaxPalette palette = syntax_palette();
    switch (g_activeLanguage) {
        case LANG_JSON: apply_json_styles(hEdit, &palette); break;
        case LANG_XML:
        case LANG_HTML: apply_markup_styles(hEdit, &palette); break;
        case LANG_YAML: apply_yaml_styles(hEdit, &palette); break;
        case LANG_CPP: apply_cpp_styles(hEdit, &palette); break;
        case LANG_PYTHON: apply_python_styles(hEdit, &palette); break;
        case LANG_BASH: apply_bash_styles(hEdit, &palette); break;
        case LANG_POWERSHELL: apply_powershell_styles(hEdit, &palette); break;
        case LANG_LUA: apply_lua_styles(hEdit, &palette); break;
        default: break;
    }
}

/* Switches the editor to the lexer for `path`, reapplying the theme so the
   new styles get colours. Safe to call when the language is unchanged. */
static void apply_document_language(HWND hEdit, const WCHAR *path) {
    const LanguageInfo *language = language_for_path(path);
    g_activeLanguage = language->id;

    void *lexer = CreateLexer(language->lexer);
    if (lexer) SendMessageA(hEdit, SCI_SETILEXER, 0, (LPARAM)lexer);
    if (language->keywords)
        SendMessageA(hEdit, SCI_SETKEYWORDS, 0, (LPARAM)language->keywords);

    /* fold.html is separate from fold: without it the XML/HTML lexer reports no
       fold points at all. The margin is hidden for languages that cannot fold,
       rather than shown as a dead strip. */
    SendMessageA(hEdit, SCI_SETPROPERTY, (WPARAM)"fold", (LPARAM)(language->folds ? "1" : "0"));
    SendMessageA(hEdit, SCI_SETPROPERTY, (WPARAM)"fold.compact", (LPARAM)"0");
    SendMessageA(hEdit, SCI_SETPROPERTY, (WPARAM)"fold.html", (LPARAM)"1");
    SendMessageA(hEdit, SCI_SETMARGINWIDTHN, MARGIN_FOLD,
                 language->folds ? FOLD_MARGIN_WIDTH : 0);

    apply_editor_theme(hEdit);
    SendMessageA(hEdit, SCI_COLOURISE, 0, (LPARAM)-1);
    schedule_spell_check();
}

/* ---- Native Windows spell checking for Markdown prose. -----------------
   Scintilla stores UTF-8 byte positions while ISpellChecker reports UTF-16
   offsets, so each reported range is converted before applying an indicator.
   Lexer styles keep code, links, rules, and Markdown punctuation unmarked. */

static const GUID MARKEDIT_CLSID_SPELL_CHECKER_FACTORY =
    {0x7ab36653, 0x1796, 0x484b, {0xbd, 0xfa, 0xe7, 0x4f, 0x1d, 0xb7, 0xc1, 0xdc}};
static const GUID MARKEDIT_IID_SPELL_CHECKER_FACTORY =
    {0x8e018a9d, 0x2415, 0x4677, {0xbf, 0x08, 0x79, 0x4e, 0xa6, 0x1f, 0x94, 0xbb}};

static int create_spell_checker_for_language(const WCHAR *requested, int reportErrors) {
    ISpellCheckerFactory *factory = NULL;
    HRESULT hr = CoCreateInstance(&MARKEDIT_CLSID_SPELL_CHECKER_FACTORY, NULL,
        CLSCTX_INPROC_SERVER, &MARKEDIT_IID_SPELL_CHECKER_FACTORY, (void **)&factory);
    if (FAILED(hr) || !factory) return 0;

    WCHAR language[LOCALE_NAME_MAX_LENGTH] = L"";
    if (requested && requested[0]) {
        wcsncpy(language, requested, LOCALE_NAME_MAX_LENGTH - 1);
        language[LOCALE_NAME_MAX_LENGTH - 1] = 0;
    } else if (!GetUserDefaultLocaleName(language, LOCALE_NAME_MAX_LENGTH))
        wcscpy(language, L"en-GB");

    WINBOOL supported = FALSE;
    ISpellCheckerFactory_IsSupported(factory, language, &supported);
    if (!supported) {
        IEnumString *languages = NULL;
        if (SUCCEEDED(ISpellCheckerFactory_get_SupportedLanguages(factory, &languages)) && languages) {
            LPOLESTR first = NULL;
            ULONG fetched = 0;
            if (IEnumString_Next(languages, 1, &first, &fetched) == S_OK && first) {
                wcsncpy(language, first, LOCALE_NAME_MAX_LENGTH - 1);
                language[LOCALE_NAME_MAX_LENGTH - 1] = 0;
                CoTaskMemFree(first);
                supported = TRUE;
            }
            IEnumString_Release(languages);
        }
    }

    ISpellChecker *checker = NULL;
    if (supported && SUCCEEDED(ISpellCheckerFactory_CreateSpellChecker(
            factory, language, &checker)) && checker) {
        if (g_spellChecker) ISpellChecker_Release(g_spellChecker);
        g_spellChecker = checker;
        wcsncpy(g_spellLanguage, language, LOCALE_NAME_MAX_LENGTH - 1);
        g_spellLanguage[LOCALE_NAME_MAX_LENGTH - 1] = 0;
    }
    ISpellCheckerFactory_Release(factory);
    if (!checker && reportErrors)
        MessageBoxW(g_hMainWnd, L"The selected Windows spelling dictionary is unavailable.",
                    L"Spell Check", MB_OK | MB_ICONWARNING);
    return checker != NULL;
}

static void init_spell_checker(void) {
    create_spell_checker_for_language(g_spellLanguage, 0);
}

static void choose_spell_language(void) {
    ISpellCheckerFactory *factory = NULL;
    if (FAILED(CoCreateInstance(&MARKEDIT_CLSID_SPELL_CHECKER_FACTORY, NULL,
                                CLSCTX_INPROC_SERVER, &MARKEDIT_IID_SPELL_CHECKER_FACTORY,
                                (void **)&factory)) || !factory) return;
    IEnumString *languages = NULL;
    WCHAR **labels = NULL;
    int count = 0, capacity = 0;
    if (SUCCEEDED(ISpellCheckerFactory_get_SupportedLanguages(factory, &languages)) && languages) {
        for (;;) {
            LPOLESTR language = NULL; ULONG fetched = 0;
            if (IEnumString_Next(languages, 1, &language, &fetched) != S_OK || !language) break;
            if (count >= capacity) {
                int next = capacity ? capacity * 2 : 16;
                WCHAR **grown = (WCHAR **)realloc(labels, (size_t)next * sizeof(WCHAR *));
                if (!grown) { CoTaskMemFree(language); break; }
                labels = grown; capacity = next;
            }
            WCHAR displayName[LOCALE_NAME_MAX_LENGTH] = L"";
            GetLocaleInfoEx(language, LOCALE_SLOCALIZEDDISPLAYNAME, displayName,
                            LOCALE_NAME_MAX_LENGTH);
            size_t labelCapacity = wcslen(language) + wcslen(displayName) + 8;
            labels[count] = (WCHAR *)malloc(labelCapacity * sizeof(WCHAR));
            if (labels[count]) {
                if (displayName[0]) swprintf(labels[count], labelCapacity, L"%ls — %ls", displayName, language);
                else wcscpy(labels[count], language);
                count++;
            }
            CoTaskMemFree(language);
        }
        IEnumString_Release(languages);
    }
    ISpellCheckerFactory_Release(factory);
    if (!count) {
        MessageBoxW(g_hMainWnd, L"Windows has no installed spelling dictionaries.",
                    L"Spell Check", MB_OK | MB_ICONINFORMATION);
        free(labels); return;
    }
    int selected = show_choice_dialog(L"Spell Check Language",
                                      L"Choose an installed Windows dictionary:", labels, count);
    if (selected >= 0 && selected < count) {
        WCHAR locale[LOCALE_NAME_MAX_LENGTH];
        WCHAR *dash = wcsrchr(labels[selected], L'—');
        const WCHAR *source = dash ? dash + 1 : labels[selected];
        while (*source == L' ') source++;
        wcsncpy(locale, source, LOCALE_NAME_MAX_LENGTH - 1);
        locale[LOCALE_NAME_MAX_LENGTH - 1] = 0;
        if (create_spell_checker_for_language(locale, 1)) {
            WCHAR *settings = get_settings_path();
            if (settings) {
                WritePrivateProfileStringW(L"SpellCheck", L"Language", g_spellLanguage, settings);
                free(settings);
            }
            g_spellCheckEnabled = 1;
            schedule_spell_check();
            refresh_menu(g_hMainWnd);
        }
    }
    for (int i = 0; i < count; i++) free(labels[i]);
    free(labels);
}

static int markdown_style_is_prose(int style) {
    return style == SCE_MARKDOWN_DEFAULT ||
           style == SCE_MARKDOWN_STRONG1 || style == SCE_MARKDOWN_STRONG2 ||
           style == SCE_MARKDOWN_EM1 || style == SCE_MARKDOWN_EM2 ||
           (style >= SCE_MARKDOWN_HEADER1 && style <= SCE_MARKDOWN_HEADER6) ||
           style == SCE_MARKDOWN_BLOCKQUOTE || style == SCE_MARKDOWN_STRIKEOUT;
}

static void schedule_spell_check(void) {
    if (!g_hMainWnd) return;
    SetTimer(g_hMainWnd, IDT_SPELL_CHECK, 350, NULL);
}

static void perform_spell_check(void) {
    if (!g_hEdit) return;
    if (g_hMainWnd) KillTimer(g_hMainWnd, IDT_SPELL_CHECK);

    int textLength = (int)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    SendMessageA(g_hEdit, SCI_SETINDICATORCURRENT, INDICATOR_SPELLING, 0);
    SendMessageA(g_hEdit, SCI_INDICATORCLEARRANGE, 0, (LPARAM)textLength);
    if (!g_spellCheckEnabled || !g_spellChecker ||
        g_activeLanguage != LANG_MARKDOWN || textLength <= 0) return;

    /* Ensure the Markdown styles used to exclude code and links are current. */
    SendMessageA(g_hEdit, SCI_COLOURISE, 0, (LPARAM)-1);
    int ignoredLength = 0;
    char *utf8 = get_edit_text(g_hEdit, &ignoredLength);
    if (!utf8) return;

    int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8, textLength, NULL, 0);
    if (wideLength <= 0) {
        free(utf8);
        return;
    }
    WCHAR *wide = (WCHAR *)calloc((size_t)wideLength + 1, sizeof(WCHAR));
    if (!wide) {
        free(utf8);
        return;
    }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, textLength, wide, wideLength);

    IEnumSpellingError *errors = NULL;
    if (SUCCEEDED(ISpellChecker_Check(g_spellChecker, wide, &errors)) && errors) {
        for (;;) {
            ISpellingError *error = NULL;
            if (IEnumSpellingError_Next(errors, &error) != S_OK || !error) break;
            ULONG wideStart = 0, wideSpan = 0;
            CORRECTIVE_ACTION action = CORRECTIVE_ACTION_NONE;
            ISpellingError_get_StartIndex(error, &wideStart);
            ISpellingError_get_Length(error, &wideSpan);
            ISpellingError_get_CorrectiveAction(error, &action);
            if (action != CORRECTIVE_ACTION_NONE && wideSpan > 0 &&
                wideStart <= (ULONG)wideLength && wideSpan <= (ULONG)wideLength - wideStart) {
                int byteStart = WideCharToMultiByte(CP_UTF8, 0, wide, (int)wideStart,
                    NULL, 0, NULL, NULL);
                int byteSpan = WideCharToMultiByte(CP_UTF8, 0, wide + wideStart,
                    (int)wideSpan, NULL, 0, NULL, NULL);
                if (byteSpan > 0 && byteStart >= 0 && byteStart + byteSpan <= textLength &&
                    markdown_style_is_prose((int)SendMessageA(
                        g_hEdit, SCI_GETSTYLEAT, (WPARAM)byteStart, 0))) {
                    SendMessageA(g_hEdit, SCI_SETINDICATORVALUE, 1, 0);
                    SendMessageA(g_hEdit, SCI_INDICATORFILLRANGE,
                                 (WPARAM)byteStart, (LPARAM)byteSpan);
                }
            }
            ISpellingError_Release(error);
        }
        IEnumSpellingError_Release(errors);
    }
    free(wide);
    free(utf8);
}

/* Keeps the WebView2 controller's own background in step with the theme. It
   shows through before the shell page paints: at startup, while resizing, and
   for the frame between a theme switch and the page restyling itself. */
static void apply_webview_background_colour(void) {
    if (!g_webviewController) return;
    ICoreWebView2Controller2 *ctrl2 = NULL;
    ICoreWebView2Controller_QueryInterface(g_webviewController, &IID_ICoreWebView2Controller2,
                                           (void **)&ctrl2);
    if (!ctrl2) return;
    COREWEBVIEW2_COLOR bg = g_previewDarkMode
        ? (COREWEBVIEW2_COLOR){255, 0x0D, 0x11, 0x17}
        : (COREWEBVIEW2_COLOR){255, 0xFF, 0xFF, 0xFF};
    ICoreWebView2Controller2_put_DefaultBackgroundColor(ctrl2, bg);
    ICoreWebView2Controller2_Release(ctrl2);
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

/* Popup menus need an explicit uxtheme opt-in to follow MarkEdit's theme.
   The visible menu bar is owner-drawn below because Windows does not repaint
   that non-client area when this preference changes at runtime. These private
   ordinal exports are available from Windows 10 1903 onward. */
typedef enum { UXTHEME_APPMODE_FORCEDARK = 2, UXTHEME_APPMODE_FORCELIGHT = 3 } UxthemeAppMode;
typedef UxthemeAppMode (WINAPI *SetPreferredAppModeFn)(UxthemeAppMode appMode);
typedef BOOL (WINAPI *AllowDarkModeForWindowFn)(HWND hwnd, BOOL allow);
typedef void (WINAPI *FlushMenuThemesFn)(void);

static SetPreferredAppModeFn g_setPreferredAppMode = NULL;
static AllowDarkModeForWindowFn g_allowDarkModeForWindow = NULL;
static FlushMenuThemesFn g_flushMenuThemes = NULL;

static void init_native_menu_theme_support(void) {
    HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
    if (!uxtheme) return;
    FARPROC procedure = GetProcAddress(uxtheme, MAKEINTRESOURCEA(133));
    memcpy(&g_allowDarkModeForWindow, &procedure, sizeof(g_allowDarkModeForWindow));
    procedure = GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
    memcpy(&g_setPreferredAppMode, &procedure, sizeof(g_setPreferredAppMode));
    procedure = GetProcAddress(uxtheme, MAKEINTRESOURCEA(136));
    memcpy(&g_flushMenuThemes, &procedure, sizeof(g_flushMenuThemes));
}

/* Switches the menu bar and its dropdowns between light and dark chrome.
   FORCEDARK/FORCELIGHT are intentional: the app has its own theme selector,
   so the selected menu colour must not depend on the Windows system theme.
   The per-window opt-in is what lets the non-client menu bar follow that
   process preference. Safe on older Windows releases where the private
   uxtheme exports are absent; those releases retain their system menu. */
static void apply_menu_theme(HWND hwnd, int dark) {
    if (g_setPreferredAppMode)
        g_setPreferredAppMode(dark ? UXTHEME_APPMODE_FORCEDARK : UXTHEME_APPMODE_FORCELIGHT);
    if (hwnd && g_allowDarkModeForWindow)
        g_allowDarkModeForWindow(hwnd, dark ? TRUE : FALSE);
    if (g_flushMenuThemes)
        g_flushMenuThemes();
    if (hwnd) {
        if (g_menuBkBrush) DeleteObject(g_menuBkBrush);
        g_menuBkBrush = CreateSolidBrush(dark
            ? RGB(0x0D, 0x11, 0x17) : RGB(0xFF, 0xFF, 0xFF));
        SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
        HMENU menu = GetMenu(hwnd);
        if (menu && g_menuBkBrush) {
            MENUINFO info = {0};
            info.cbSize = sizeof(info);
            info.fMask = MIM_BACKGROUND;
            info.hbrBack = g_menuBkBrush;
            SetMenuInfo(menu, &info);
        }
        SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
        DrawMenuBar(hwnd);
    }
}

/* Wrapped lines continue with a hanging indent so a wrapped list item or
   blockquote stays visually attached to its marker. */
static void apply_word_wrap(HWND hEdit) {
    SendMessageA(hEdit, SCI_SETWRAPMODE, g_wordWrap ? SC_WRAP_WORD : SC_WRAP_NONE, 0);
    if (!g_wordWrap) return;
    SendMessageA(hEdit, SCI_SETWRAPINDENTMODE, SC_WRAPINDENT_SAME, 0);
    SendMessageA(hEdit, SCI_SETWRAPVISUALFLAGS, SC_WRAPVISUALFLAG_END, 0);
}

static void apply_show_whitespace(HWND hEdit) {
    SendMessageA(hEdit, SCI_SETVIEWWS, g_showWhitespace ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE, 0);
}

static void apply_indent_guides(HWND hEdit) {
    SendMessageA(hEdit, SCI_SETINDENTATIONGUIDES, g_indentGuides ? SC_IV_LOOKBOTH : SC_IV_NONE, 0);
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
    apply_word_wrap(hEdit);
    apply_show_whitespace(hEdit);
    apply_indent_guides(hEdit);
    SendMessageA(hEdit, SCI_SETZOOM, (WPARAM)g_editorZoom, 0);

    /* Additional carets edit in step with the main one; the smart-input
       handlers in EditSubclassProc still act on the main caret alone. */
    SendMessageA(hEdit, SCI_SETMULTIPLESELECTION, TRUE, 0);
    SendMessageA(hEdit, SCI_SETADDITIONALSELECTIONTYPING, TRUE, 0);
    SendMessageA(hEdit, SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH, 0);

    /* Line numbers: a dedicated margin, sized to fit the current line count. */
    SendMessageA(hEdit, SCI_SETMARGINTYPEN, MARGIN_LINENUMBERS, SC_MARGIN_NUMBER);
    update_linenumber_margin_width(hEdit);

    /* Bookmark and fold margins. Marker definitions are per-view, so they are
       set once here and survive the SCI_SETDOCPOINTER swaps done by tabs. */
    SendMessageA(hEdit, SCI_SETMARGINTYPEN, MARGIN_BOOKMARKS, SC_MARGIN_SYMBOL);
    SendMessageA(hEdit, SCI_SETMARGINMASKN, MARGIN_BOOKMARKS,
                 (LPARAM)((1 << MARKER_BOOKMARK) | (1 << MARKER_BREAKPOINT)));
    SendMessageA(hEdit, SCI_SETMARGINWIDTHN, MARGIN_BOOKMARKS, 14);
    SendMessageA(hEdit, SCI_SETMARGINSENSITIVEN, MARGIN_BOOKMARKS, TRUE);
    SendMessageA(hEdit, SCI_MARKERDEFINE, MARKER_BOOKMARK, SC_MARK_BOOKMARK);
    SendMessageA(hEdit, SCI_MARKERDEFINE, MARKER_BREAKPOINT, SC_MARK_CIRCLE);
    SendMessageA(hEdit, SCI_MARKERDEFINE, MARKER_DEBUG_LINE, SC_MARK_BACKGROUND);

    SendMessageA(hEdit, SCI_SETMARGINTYPEN, MARGIN_FOLD, SC_MARGIN_SYMBOL);
    SendMessageA(hEdit, SCI_SETMARGINMASKN, MARGIN_FOLD, (LPARAM)SC_MASK_FOLDERS);
    SendMessageA(hEdit, SCI_SETMARGINSENSITIVEN, MARGIN_FOLD, TRUE);
    SendMessageA(hEdit, SCI_MARKERDEFINE, SC_MARKNUM_FOLDER, SC_MARK_BOXPLUS);
    SendMessageA(hEdit, SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN, SC_MARK_BOXMINUS);
    SendMessageA(hEdit, SCI_MARKERDEFINE, SC_MARKNUM_FOLDERSUB, SC_MARK_VLINE);
    SendMessageA(hEdit, SCI_MARKERDEFINE, SC_MARKNUM_FOLDERTAIL, SC_MARK_LCORNER);
    SendMessageA(hEdit, SCI_MARKERDEFINE, SC_MARKNUM_FOLDEREND, SC_MARK_BOXPLUSCONNECTED);
    SendMessageA(hEdit, SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUSCONNECTED);
    SendMessageA(hEdit, SCI_MARKERDEFINE, SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER);
    SendMessageA(hEdit, SCI_SETFOLDFLAGS, SC_FOLDFLAG_LINEAFTER_CONTRACTED, 0);
    SendMessageA(hEdit, SCI_SETAUTOMATICFOLD, SC_AUTOMATICFOLD_CLICK | SC_AUTOMATICFOLD_CHANGE, 0);

    /* Syntax-highlight the source using one of Lexilla's bundled lexers
       (vendored trimmed to the languages listed in LANGUAGES - see
       third_party/lexilla). ILexer5* ownership passes to Scintilla once
       handed to SCI_SETILEXER. apply_document_language() picks the lexer
       from the file extension; new documents start out as Markdown.

       Explicit text colors, rather than relying on Scintilla's own defaults,
       so the caret-line highlight below is guaranteed to contrast with them.
       apply_editor_theme() sets the actual color values (light or dark),
       including the lexer's per-construct colors. */
    apply_document_language(hEdit, g_currentFile);

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
    g_lineNumberDigits = 0; /* font metrics changed even if the line count did not */
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
    DWORD cap = 512, n = 0;
    WCHAR *cssPath = NULL;
    do {
        free(cssPath);
        cssPath = (WCHAR *)calloc(cap, sizeof(WCHAR));
        if (!cssPath) break;
        n = GetPrivateProfileStringW(L"Preview", L"CustomCSS", L"", cssPath, cap, path);
        if (n < cap - 1) break;
        cap *= 2;
    } while (cap <= 32768);
    if (cssPath && cssPath[0]) g_customCssPath = cssPath;
    else free(cssPath);
    free(path);
}

static void save_theme_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WritePrivateProfileStringW(L"Preview", L"Theme",
                               g_previewDarkMode ? L"github-dark" : L"github-light", path);
    WritePrivateProfileStringW(L"Preview", L"DarkMode", NULL, path);
    WritePrivateProfileStringW(L"Preview", L"CustomCSS",
                               g_customCssPath ? g_customCssPath : L"", path);
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
    g_focusMode = read_profile_int(L"View", L"FocusMode", 0, path) != 0;
    g_wordWrap = read_profile_int(L"View", L"WordWrap", g_wordWrap, path) != 0;
    g_spellCheckEnabled = read_profile_int(L"View", L"SpellCheck", g_spellCheckEnabled, path) != 0;
    g_showWhitespace = read_profile_int(L"View", L"ShowWhitespace", g_showWhitespace, path) != 0;
    g_indentGuides = read_profile_int(L"View", L"IndentGuides", g_indentGuides, path) != 0;
    g_outlineVisible = read_profile_int(L"View", L"OutlineVisible", g_outlineVisible, path) != 0;
    g_editorZoom = read_profile_int(L"View", L"EditorZoom", g_editorZoom, path);
    if (g_editorZoom < -10) g_editorZoom = -10;
    if (g_editorZoom > 20) g_editorZoom = 20;
    GetPrivateProfileStringW(L"SpellCheck", L"Language", L"en-GB", g_spellLanguage,
                             LOCALE_NAME_MAX_LENGTH, path);
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
    write_profile_int(L"View", L"FocusMode", g_focusMode, path);
    write_profile_int(L"View", L"WordWrap", g_wordWrap, path);
    write_profile_int(L"View", L"SpellCheck", g_spellCheckEnabled, path);
    write_profile_int(L"View", L"ShowWhitespace", g_showWhitespace, path);
    write_profile_int(L"View", L"IndentGuides", g_indentGuides, path);
    write_profile_int(L"View", L"OutlineVisible", g_outlineVisible, path);
    write_profile_int(L"View", L"EditorZoom", g_editorZoom, path);
    WritePrivateProfileStringW(L"SpellCheck", L"Language", g_spellLanguage, path);
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

static void load_find_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    GetPrivateProfileStringW(L"Find", L"FindWhat", L"", g_findWhat,
                             sizeof(g_findWhat) / sizeof(g_findWhat[0]), path);
    GetPrivateProfileStringW(L"Find", L"ReplaceWith", L"", g_replaceWith,
                             sizeof(g_replaceWith) / sizeof(g_replaceWith[0]), path);
    g_findFlags = read_profile_int(L"Find", L"MatchCase", 0, path) ? FR_MATCHCASE : 0;
    if (read_profile_int(L"Find", L"SearchDown", 1, path)) g_findFlags |= FR_DOWN;
    free(path);
}

static void save_find_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WritePrivateProfileStringW(L"Find", L"FindWhat", g_findWhat, path);
    WritePrivateProfileStringW(L"Find", L"ReplaceWith", g_replaceWith, path);
    write_profile_int(L"Find", L"MatchCase", (g_findFlags & FR_MATCHCASE) != 0, path);
    write_profile_int(L"Find", L"SearchDown", (g_findFlags & FR_DOWN) != 0, path);
    free(path);
}

/* [Compiler] is hand-edited in markedit.ini rather than exposed through a
   menu: Backend pins Run/Debug C/C++ to one compiler instead of the default
   Auto priority search (GCC, Clang, Zig, then MSVC as a Run-only fallback),
   and VcvarsallPath points at a specific vcvarsall.bat when MSVC isn't
   installed in one of the standard Program Files locations find_vcvarsall()
   checks. */
static void load_compiler_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WCHAR value[16];
    GetPrivateProfileStringW(L"Compiler", L"Backend", L"Auto", value, 16, path);
    if (_wcsicmp(value, L"GCC") == 0) g_compilerBackend = COMPILER_BACKEND_GCC;
    else if (_wcsicmp(value, L"Clang") == 0) g_compilerBackend = COMPILER_BACKEND_CLANG;
    else if (_wcsicmp(value, L"Zig") == 0) g_compilerBackend = COMPILER_BACKEND_ZIG;
    else if (_wcsicmp(value, L"MSVC") == 0) g_compilerBackend = COMPILER_BACKEND_MSVC;
    else g_compilerBackend = COMPILER_BACKEND_AUTO;

    DWORD cap = 512, n = 0;
    WCHAR *vcvarsall = NULL;
    do {
        free(vcvarsall);
        vcvarsall = (WCHAR *)calloc(cap, sizeof(WCHAR));
        if (!vcvarsall) break;
        n = GetPrivateProfileStringW(L"Compiler", L"VcvarsallPath", L"", vcvarsall, cap, path);
        if (n < cap - 1) break;
        cap *= 2;
    } while (cap <= 32768);
    if (vcvarsall && vcvarsall[0]) g_vcvarsallOverridePath = vcvarsall;
    else free(vcvarsall);
    free(path);
}

/* ---- autosave recovery -------------------------------------------------
   Recovery snapshots live in LocalAppData rather than beside the executable,
   so running the portable distribution never dirties its own folder. Only
   modified documents are written; normal saves and a clean exit remove them. */

static WCHAR *get_recovery_directory(void) {
    DWORD needed = GetEnvironmentVariableW(L"LOCALAPPDATA", NULL, 0);
    if (!needed) return NULL;
    WCHAR *local = (WCHAR *)malloc((size_t)needed * sizeof(WCHAR));
    if (!local || !GetEnvironmentVariableW(L"LOCALAPPDATA", local, needed)) {
        free(local);
        return NULL;
    }
    WCHAR *app = path_join(local, L"MarkEdit");
    free(local);
    if (!app) return NULL;
    CreateDirectoryW(app, NULL);
    WCHAR *recovery = path_join(app, L"Recovery");
    free(app);
    if (recovery) CreateDirectoryW(recovery, NULL);
    return recovery;
}

static char *get_document_text(int index, int *outLength) {
    if (!g_hEdit || index < 0 || index >= g_documentCount) return NULL;
    sptr_t current = (sptr_t)SendMessageA(g_hEdit, SCI_GETDOCPOINTER, 0, 0);
    int oldSwitching = g_switchingDocument;
    g_switchingDocument = 1;
    if (current != g_documents[index].sciDocument)
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_documents[index].sciDocument);
    char *text = get_edit_text(g_hEdit, outLength);
    if (current != g_documents[index].sciDocument)
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)current);
    g_switchingDocument = oldSwitching;
    return text;
}

static int write_bytes_file(const WCHAR *path, const char *bytes, size_t length) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    size_t total = 0;
    int ok = 1;
    while (total < length) {
        DWORD chunk = (DWORD)((length - total) > 1024 * 1024 ? 1024 * 1024 : length - total);
        DWORD written = 0;
        if (!WriteFile(file, bytes + total, chunk, &written, NULL) || written != chunk) {
            ok = 0;
            break;
        }
        total += written;
    }
    if (!CloseHandle(file)) ok = 0;
    return ok;
}

static char *read_bytes_file(const WCHAR *path, int *outLength) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > INT_MAX) {
        CloseHandle(file);
        return NULL;
    }
    int length = (int)size.QuadPart;
    char *text = (char *)malloc((size_t)length + 1);
    DWORD read = 0;
    int ok = text && ReadFile(file, text, (DWORD)length, &read, NULL) && read == (DWORD)length;
    CloseHandle(file);
    if (!ok) { free(text); return NULL; }
    text[length] = 0;
    if (outLength) *outLength = length;
    return text;
}

static void clear_recovery_snapshot(void) {
    WCHAR *directory = get_recovery_directory();
    if (!directory) return;
    WCHAR *manifest = path_join(directory, L"manifest.ini");
    if (manifest) { DeleteFileW(manifest); free(manifest); }
    for (int i = 0; i < MAX_DOCUMENTS; i++) {
        WCHAR name[32];
        swprintf(name, sizeof(name) / sizeof(name[0]), L"document-%d.txt", i);
        WCHAR *path = path_join(directory, name);
        if (path) { DeleteFileW(path); free(path); }
    }
    free(directory);
}

static void write_recovery_snapshot(void) {
    if (!g_hEdit || g_commandLinePreviewMode) return;
    if (!g_recoverySnapshotStale) return;
    g_recoverySnapshotStale = 0;
    int dirtyCount = 0;
    for (int i = 0; i < g_documentCount; i++) if (g_documents[i].dirty) dirtyCount++;
    if (!dirtyCount) {
        clear_recovery_snapshot();
        update_status_position();
        return;
    }

    WCHAR *directory = get_recovery_directory();
    WCHAR *manifest = directory ? path_join(directory, L"manifest.ini") : NULL;
    if (!directory || !manifest) { free(directory); free(manifest); return; }
    DeleteFileW(manifest);
    int writtenCount = 0;
    for (int i = 0; i < g_documentCount; i++) {
        Document *doc = &g_documents[i];
        if (!doc->dirty) continue;
        int length = 0;
        char *text = get_document_text(i, &length);
        WCHAR fileName[32], key[32], value[32];
        swprintf(fileName, sizeof(fileName) / sizeof(fileName[0]), L"document-%d.txt", writtenCount);
        WCHAR *contentPath = path_join(directory, fileName);
        if (!text || !contentPath || !write_bytes_file(contentPath, text, (size_t)length)) {
            free(text); free(contentPath); continue;
        }
        swprintf(key, sizeof(key) / sizeof(key[0]), L"File%d", writtenCount);
        WritePrivateProfileStringW(L"Recovery", key, fileName, manifest);
        swprintf(key, sizeof(key) / sizeof(key[0]), L"Path%d", writtenCount);
        WritePrivateProfileStringW(L"Recovery", key, doc->path ? doc->path : L"", manifest);
        swprintf(key, sizeof(key) / sizeof(key[0]), L"Untitled%d", writtenCount);
        swprintf(value, sizeof(value) / sizeof(value[0]), L"%u", doc->untitledNumber);
        WritePrivateProfileStringW(L"Recovery", key, value, manifest);
        swprintf(key, sizeof(key) / sizeof(key[0]), L"Bom%d", writtenCount);
        WritePrivateProfileStringW(L"Recovery", key, doc->hasBom ? L"1" : L"0", manifest);
        writtenCount++;
        free(text);
        free(contentPath);
    }
    WCHAR count[32];
    swprintf(count, sizeof(count) / sizeof(count[0]), L"%d", writtenCount);
    WritePrivateProfileStringW(L"Recovery", L"Count", count, manifest);
    free(manifest);
    free(directory);
    update_status_bar();
}

static int restore_recovery_snapshot(void) {
    WCHAR *directory = get_recovery_directory();
    WCHAR *manifest = directory ? path_join(directory, L"manifest.ini") : NULL;
    if (!directory || !manifest) { free(directory); free(manifest); return 0; }
    int count = GetPrivateProfileIntW(L"Recovery", L"Count", 0, manifest);
    if (count <= 0 || count > MAX_DOCUMENTS) {
        free(directory); free(manifest); return 0;
    }
    int answer = MessageBoxW(g_hMainWnd,
        L"MarkEdit found autosaved documents from an interrupted session. Recover them?",
        L"Recover Documents", MB_YESNO | MB_ICONQUESTION);
    if (answer != IDYES) {
        free(directory); free(manifest);
        clear_recovery_snapshot();
        return 0;
    }

    int restored = 0;
    for (int i = 0; i < count; i++) {
        WCHAR key[32], fileName[64], originalPath[32768];
        swprintf(key, sizeof(key) / sizeof(key[0]), L"File%d", i);
        GetPrivateProfileStringW(L"Recovery", key, L"", fileName,
                                 sizeof(fileName) / sizeof(fileName[0]), manifest);
        if (!fileName[0]) continue;
        WCHAR *contentPath = path_join(directory, fileName);
        int length = 0;
        char *text = contentPath ? read_bytes_file(contentPath, &length) : NULL;
        free(contentPath);
        if (!text) continue;
        if (restored > 0 && !create_document_tab()) { free(text); break; }
        if (restored == 0 && !active_document_is_reusable() && !create_document_tab()) {
            free(text); break;
        }
        g_switchingDocument = 1;
        set_edit_text(g_hEdit, text);
        SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
        g_switchingDocument = 0;
        free(text);
        swprintf(key, sizeof(key) / sizeof(key[0]), L"Path%d", i);
        GetPrivateProfileStringW(L"Recovery", key, L"", originalPath,
                                 sizeof(originalPath) / sizeof(originalPath[0]), manifest);
        replace_wstring(&g_currentFile, originalPath[0] ? originalPath : NULL);
        swprintf(key, sizeof(key) / sizeof(key[0]), L"Untitled%d", i);
        g_documents[g_activeDocument].untitledNumber =
            (unsigned)GetPrivateProfileIntW(L"Recovery", key, 0, manifest);
        swprintf(key, sizeof(key) / sizeof(key[0]), L"Bom%d", i);
        g_hasBom = GetPrivateProfileIntW(L"Recovery", key, 0, manifest) != 0;
        apply_document_language(g_hEdit, g_currentFile);
        set_dirty(1);
        restored++;
    }
    free(directory);
    free(manifest);
    if (restored) {
        update_document_mapping();
        update_preview();
        write_recovery_snapshot();
    }
    return restored > 0;
}

/* The status bar deliberately computes from Scintilla's UTF-8 document model:
   character counts are Unicode-aware while columns remain editor columns. */
/* Ln/Col, encoding, and dirty-status fields only - all O(1) Scintilla
   queries, safe to call on every caret move or scroll tick. Word/character
   counts need a full-document scan, so they're left to update_status_bar()
   and only refreshed when the text can actually have changed. */
/* Backing storage for the status bar's four segments (0: Ln/Col, 1: Words/
   Characters, 2: encoding, 3: save state). The strip is fully repainted by
   StatusSubclassProc below instead of through SB_SETTEXT + owner-draw:
   comctl32 draws a light 3D bevel between parts by itself regardless of
   SBT_OWNERDRAW/SBT_NOBORDERS, which reads as bright seams against a dark
   theme. Parts must own their text for as long as the control might repaint
   them, so a transient stack buffer would otherwise be read back as a
   dangling pointer once these functions return. */
static WCHAR g_statusPartText[4][128];

static void update_status_position(void) {
    if (!g_hStatus || !g_hEdit || g_documentCount <= 0) return;
    int position = (int)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    int line = (int)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)position, 0) + 1;
    int column = (int)SendMessageA(g_hEdit, SCI_GETCOLUMN, (WPARAM)position, 0) + 1;
    swprintf(g_statusPartText[0], 128, L"Ln %d, Col %d", line, column);
    int eol = (int)SendMessageA(g_hEdit, SCI_GETEOLMODE, 0, 0);
    const WCHAR *eolName = eol == SC_EOL_LF ? L"LF" : eol == SC_EOL_CR ? L"CR" : L"CRLF";
    swprintf(g_statusPartText[2], 128, L"UTF-8%ls   %ls", g_hasBom ? L" BOM" : L"", eolName);
    swprintf(g_statusPartText[3], 128, L"%ls", g_dirty ? L"Modified · recovery enabled" : L"Saved");
    InvalidateRect(g_hStatus, NULL, FALSE);
}

static void update_status_bar(void) {
    update_status_position();
    if (!g_hStatus || !g_hEdit || g_documentCount <= 0) return;
    int length = (int)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    int characters = (int)SendMessageA(g_hEdit, SCI_COUNTCHARACTERS, 0, (LPARAM)length);
    int ignored = 0, words = 0, inWord = 0;
    char *text = get_edit_text(g_hEdit, &ignored);
    if (text) {
        for (int i = 0; i < length; i++) {
            unsigned char ch = (unsigned char)text[i];
            int wordChar = ch >= 0x80 || isalnum(ch) || ch == '_' || ch == '\'';
            if (wordChar && !inWord) words++;
            inWord = wordChar;
        }
        free(text);
    }
    swprintf(g_statusPartText[1], 128, L"Words %d   Characters %d", words, characters);
    InvalidateRect(g_hStatus, NULL, FALSE);
}

/* Paints the whole status bar strip itself in the app's own GitHub palette:
   comctl32's default rendering has no opt-in for a custom light/dark palette
   the way the editor/menu/toolbar do, and (unlike those) drawing only the
   owner-drawn parts still leaves the control's own bevel between them, which
   shows up as light seams. SB_SETPARTS (via layout()) still establishes the
   four part rectangles this reads with SB_GETRECT; nothing else about the
   control's own painting is used. */
static LRESULT CALLBACK StatusSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR subclassId, DWORD_PTR refData) {
    (void)subclassId; (void)refData;
    if (msg == WM_ERASEBKGND) {
        return 1; /* WM_PAINT fills the whole strip itself. */
    } else if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        COLORREF background = g_previewDarkMode ? RGB(0x0D, 0x11, 0x17) : RGB(0xFF, 0xFF, 0xFF);
        COLORREF foreground = g_previewDarkMode ? RGB(0xE6, 0xED, 0xF3) : RGB(0x1F, 0x23, 0x28);
        HBRUSH brush = CreateSolidBrush(background);
        FillRect(dc, &client, brush);
        DeleteObject(brush);
        SetTextColor(dc, foreground);
        SetBkMode(dc, TRANSPARENT);
        for (int i = 0; i < 4; i++) {
            RECT partRect;
            if (!SendMessageW(hwnd, SB_GETRECT, (WPARAM)i, (LPARAM)&partRect)) continue;
            partRect.left += 4;
            partRect.right -= 4;
            DrawTextW(dc, g_statusPartText[i], -1, &partRect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        EndPaint(hwnd, &ps);
        return 0;
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, StatusSubclassProc, 1);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
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

/* Read a reasonably-sized UTF-8 text file. Custom preview stylesheets are
   deliberately injected as text instead of linked directly, which keeps the
   preview's restrictive CSP intact and also makes exported HTML self-contained. */
static char *read_utf8_text_file(const WCHAR *path, size_t maximumSize) {
    if (!path) return _strdup("");
    WCHAR *ioPath = extended_path(path);
    HANDLE file = CreateFileW(ioPath ? ioPath : path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    free(ioPath);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        (unsigned long long)size.QuadPart > maximumSize) {
        CloseHandle(file);
        return NULL;
    }
    size_t length = (size_t)size.QuadPart;
    char *buffer = (char *)malloc(length + 1);
    size_t total = 0;
    while (buffer && total < length) {
        DWORD chunk = (DWORD)((length - total) > 1024 * 1024 ? 1024 * 1024 : length - total);
        DWORD got = 0;
        if (!ReadFile(file, buffer + total, chunk, &got, NULL) || got == 0) break;
        total += got;
    }
    CloseHandle(file);
    if (!buffer || total != length) { free(buffer); return NULL; }
    buffer[length] = 0;
    size_t offset = has_utf8_bom(buffer, (int)length) ? 3 : 0;
    if (length > offset && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            buffer + offset, (int)(length - offset), NULL, 0) <= 0) {
        free(buffer);
        return NULL;
    }
    if (offset) memmove(buffer, buffer + offset, length - offset + 1);
    return buffer;
}

/* WebView2 accepts UTF-16 scripts. Centralizing the conversion keeps every
   fire-and-forget call on the same allocation/error path and lets LTO inline
   it into the small fixed-script callers. */
static HRESULT execute_webview_script(const char *script,
                                      ICoreWebView2ExecuteScriptCompletedHandler *handler) {
    if (!g_webview || !script) return E_HANDLE;
    WCHAR *wide = to_wide(script);
    if (!wide) return E_OUTOFMEMORY;
    HRESULT result = ICoreWebView2_ExecuteScript(g_webview, wide, handler);
    free(wide);
    return result;
}

static void apply_custom_preview_css(int reportErrors) {
    if (!g_webviewReady || !g_webview) return;
    char *css = g_customCssPath ? read_utf8_text_file(g_customCssPath, 4 * 1024 * 1024) : _strdup("");
    if (!css) {
        if (reportErrors)
            MessageBoxW(g_hMainWnd,
                L"The custom stylesheet could not be read. It must be a UTF-8 CSS file smaller than 4 MB.",
                L"MarkEdit", MB_OK | MB_ICONERROR);
        css = _strdup("");
    }
    if (!css) return;
    char *escaped = json_escape(css);
    free(css);
    if (!escaped) return;
    const char *prefix = "(()=>{let s=document.getElementById('markedit-custom-css');"
        "if(!s){s=document.createElement('style');s.id='markedit-custom-css';document.head.appendChild(s)}"
        "s.textContent='";
    const char *suffix = "';})()";
    size_t length = strlen(prefix) + strlen(escaped) + strlen(suffix);
    char *script = (char *)malloc(length + 1);
    if (script) {
        memcpy(script, prefix, strlen(prefix));
        memcpy(script + strlen(prefix), escaped, strlen(escaped));
        memcpy(script + strlen(prefix) + strlen(escaped), suffix, strlen(suffix) + 1);
        execute_webview_script(script, &g_scriptHandler);
        free(script);
    }
    free(escaped);
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
        ok = write_bytes_file(ioPath ? ioPath : g_exportPath, utf8, strlen(utf8));
        free(ioPath);
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

static HRESULT STDMETHODCALLTYPE PdfHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out, &IID_ICoreWebView2PrintToPdfCompletedHandler);
}

static HRESULT STDMETHODCALLTYPE PdfDone_Invoke(ICoreWebView2PrintToPdfCompletedHandler *this_,
                                                 HRESULT errorCode, WINBOOL result) {
    (void)this_;
    if (FAILED(errorCode) || !result)
        MessageBoxW(g_hMainWnd, L"Could not export the rendered preview to PDF.",
                    L"MarkEdit", MB_OK | MB_ICONERROR);
    else
        MessageBoxW(g_hMainWnd, L"PDF export completed.", L"MarkEdit",
                    MB_OK | MB_ICONINFORMATION);
    free(g_pendingPdfPath);
    g_pendingPdfPath = NULL;
    g_pendingPrintAction = 0;
    return S_OK;
}

static ICoreWebView2PrintToPdfCompletedHandlerVtbl g_pdfVtbl = {
    (void*)PdfHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)PdfDone_Invoke
};
static ICoreWebView2PrintToPdfCompletedHandler g_pdfHandler = { &g_pdfVtbl };

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
    apply_custom_preview_css(0);
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
            int hi = hex_digit_value(p[1]);
            int lo = hex_digit_value(p[2]);
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

/* Launches a terminal rooted in the current file's folder (the app's own
   folder for an unsaved document). Windows Terminal is tried first, since
   it's the modern default on Windows 11; cmd.exe is the universal fallback
   for machines that don't have it installed. */
static void open_terminal_here(void) {
    WCHAR *directory = g_currentFile ? path_directory(g_currentFile) : NULL;
    if (!directory) {
        WCHAR *module = get_module_path();
        directory = module ? path_directory(module) : NULL;
        free(module);
    }
    if (!directory) return;
    HINSTANCE result = ShellExecuteW(g_hMainWnd, L"open", L"wt.exe", NULL, directory, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32)
        ShellExecuteW(g_hMainWnd, L"open", L"cmd.exe", NULL, directory, SW_SHOWNORMAL);
    free(directory);
}

/* Opens Explorer with the current file pre-selected. Unlike Open Terminal
   Here, there's no reasonable fallback for an unsaved document - Explorer
   can't select a file that doesn't exist on disk yet. */
static void reveal_in_explorer(void) {
    if (!g_currentFile) {
        MessageBoxW(g_hMainWnd, L"Save the document first to show it in Explorer.",
                    L"Show in Explorer", MB_OK | MB_ICONINFORMATION);
        return;
    }
    size_t capacity = wcslen(g_currentFile) + 16;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!parameters) return;
    swprintf(parameters, capacity, L"/select,\"%ls\"", g_currentFile);
    ShellExecuteW(g_hMainWnd, L"open", L"explorer.exe", parameters, NULL, SW_SHOWNORMAL);
    free(parameters);
}

static void apply_preview_theme(void) {
    if (!g_webviewReady || !g_webview) return;
    char script[64];
    wsprintfA(script, "window.setTheme('%s');", g_previewDarkMode ? "dark" : "light");
    execute_webview_script(script, &g_scriptHandler);
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
    execute_webview_script(script, &g_scriptHandler);
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

    apply_webview_background_colour();

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
        /* The preview is a document view, not a browser: let the app's own
           accelerators through instead of WebView2's F11 fullscreen, Ctrl+P,
           refresh and friends. Absent on WebView2 Runtimes before 1.0.864.35. */
        ICoreWebView2Settings3 *settings3 = NULL;
        if (SUCCEEDED(ICoreWebView2Settings_QueryInterface(settings, &IID_ICoreWebView2Settings3,
                                                           (void **)&settings3)) && settings3) {
            ICoreWebView2Settings3_put_AreBrowserAcceleratorKeysEnabled(settings3, FALSE);
            ICoreWebView2Settings3_Release(settings3);
        }
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
    /* Loading/restoring/switching tabs should not parse Markdown while the
       preview is hidden. Showing the pane calls update_preview(), and export
       sets g_exportPath first to force a current off-screen render. */
    if (!g_previewVisible && !g_exportPath) return;
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
    if (g_hMainWnd && g_editorVisible && g_previewVisible)
        SetTimer(g_hMainWnd, IDT_SCROLL_SYNC, 40, NULL);
}

static void sync_preview_to_editor_scroll(void) {
    if (g_hMainWnd) KillTimer(g_hMainWnd, IDT_SCROLL_SYNC);
    if (!g_webviewReady || !g_webview || !g_editorVisible || !g_previewVisible) return;
    int line = (int)SendMessageA(g_hEdit, SCI_GETFIRSTVISIBLELINE, 0, 0) + 1;
    char script[96];
    snprintf(script, sizeof(script),
             "if(window.scrollToSourceLine)window.scrollToSourceLine(%d);", line);
    execute_webview_script(script, &g_scriptHandler);
}

/* ================= rest of the app (menus, file I/O, find/replace, font) ================= */

static const WCHAR *document_short_name(const Document *doc) {
    return doc->path ? PathFindFileNameW(doc->path) : NULL;
}

static void format_document_label(const Document *doc, WCHAR *label, size_t capacity) {
    const WCHAR *shortName = document_short_name(doc);
    if (doc->isReference && !shortName) {
        swprintf(label, capacity, L"Markdown Reference%ls", doc->dirty ? L" *" : L"");
    } else if (shortName && shortName[0]) {
        swprintf(label, capacity, L"%ls%ls", shortName, doc->dirty ? L" *" : L"");
    } else if (doc->untitledNumber == 0) {
        swprintf(label, capacity, L"Untitled.md%ls", doc->dirty ? L" *" : L"");
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
    apply_document_language(g_hEdit, g_documents[index].path);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)g_documents[index].anchor, (LPARAM)g_documents[index].caret);
    SendMessageA(g_hEdit, SCI_SETFIRSTVISIBLELINE, (WPARAM)g_documents[index].firstVisibleLine, 0);
    SendMessageA(g_hEdit, SCI_SETXOFFSET, (WPARAM)g_documents[index].xOffset, 0);
    update_linenumber_margin_width(g_hEdit);
    g_switchingDocument = 0;
    if (g_hTabs) TabCtrl_SetCurSel(g_hTabs, index);
    update_document_mapping();
    update_preview();
    update_title();
    update_status_bar();
    update_outline();
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
    const Document *active = &g_documents[g_activeDocument];
    const WCHAR *name = g_currentFile ? g_currentFile
        : active->isReference ? L"Markdown Reference"
        : (active->untitledNumber == 0 ? L"Untitled.md" : L"Untitled");
    size_t cap = wcslen(name) + 32;
    WCHAR *title = (WCHAR *)malloc(cap * sizeof(WCHAR));
    if (!title) return;
    swprintf(title, cap, L"%ls%ls - MarkEdit", name, g_dirty ? L"*" : L"");
    SetWindowTextW(g_hMainWnd, title);
    free(title);
}

static void set_dirty(int dirty) {
    g_dirty = dirty;
    g_recoverySnapshotStale = 1;
    update_tab_label(g_activeDocument);
    update_title();
    update_status_position();
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
        if (has_utf8_bom(buf, (int)length)) {
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
        apply_document_language(g_hEdit, g_currentFile);
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
    ofn.lpstrFilter = L"Markdown Files (*.md;*.markdown)\0*.md;*.markdown\0Python Files (*.py;*.pyw)\0*.py;*.pyw\0C/C++ Files (*.c;*.cc;*.cpp;*.cxx)\0*.c;*.cc;*.cpp;*.cxx\0Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 5; /* "All Files (*.*)" - the dialog otherwise defaults to entry 1 */
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

static int is_image_file(const WCHAR *path) {
    const WCHAR *extension = PathFindExtensionW(path);
    return extension && (_wcsicmp(extension, L".png") == 0 ||
        _wcsicmp(extension, L".jpg") == 0 || _wcsicmp(extension, L".jpeg") == 0 ||
        _wcsicmp(extension, L".gif") == 0 || _wcsicmp(extension, L".bmp") == 0 ||
        _wcsicmp(extension, L".webp") == 0 || _wcsicmp(extension, L".svg") == 0);
}

/* Create a collision-safe destination in an "images" folder next to the
   current Markdown file. The returned relative path always uses URL-style
   slashes so it can be inserted directly into Markdown. */
static int create_image_destination(const WCHAR *preferredName, const WCHAR *defaultExtension,
                                    WCHAR **destinationOut, WCHAR **relativeOut) {
    *destinationOut = NULL;
    *relativeOut = NULL;
    if (!g_currentFile) {
        do_save();
        if (!g_currentFile) return 0;
    }
    WCHAR *documentDirectory = path_directory(g_currentFile);
    WCHAR *imagesDirectory = documentDirectory ? path_join(documentDirectory, L"images") : NULL;
    free(documentDirectory);
    if (!imagesDirectory) return 0;
    WCHAR *ioDirectory = extended_path(imagesDirectory);
    BOOL made = CreateDirectoryW(ioDirectory ? ioDirectory : imagesDirectory, NULL);
    DWORD directoryError = made ? ERROR_SUCCESS : GetLastError();
    free(ioDirectory);
    if (!made && directoryError != ERROR_ALREADY_EXISTS) {
        free(imagesDirectory);
        MessageBoxW(g_hMainWnd, L"Could not create the document's images folder.",
                    L"MarkEdit", MB_OK | MB_ICONERROR);
        return 0;
    }

    const WCHAR *leaf = preferredName && preferredName[0] ? PathFindFileNameW(preferredName) : L"image";
    size_t leafLength = wcslen(leaf);
    WCHAR *safe = (WCHAR *)malloc((leafLength + 16) * sizeof(WCHAR));
    if (!safe) { free(imagesDirectory); return 0; }
    size_t o = 0;
    for (size_t i = 0; i < leafLength; i++) {
        WCHAR c = leaf[i];
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
        safe[o++] = c;
    }
    safe[o] = 0;
    WCHAR *extension = PathFindExtensionW(safe);
    if (!extension || !extension[0]) {
        wcscat(safe, defaultExtension ? defaultExtension : L".png");
        extension = PathFindExtensionW(safe);
    }
    size_t extensionOffset = (size_t)(extension - safe);
    WCHAR *extensionCopy = wcsdup_heap(extension);
    safe[extensionOffset] = 0;
    if (!safe[0]) wcscpy(safe, L"image");

    for (unsigned suffix = 0; suffix < 10000; suffix++) {
        size_t nameCap = wcslen(safe) + (extensionCopy ? wcslen(extensionCopy) : 0) + 32;
        WCHAR *name = (WCHAR *)malloc(nameCap * sizeof(WCHAR));
        if (!name) break;
        if (suffix == 0) swprintf(name, nameCap, L"%ls%ls", safe, extensionCopy ? extensionCopy : L"");
        else swprintf(name, nameCap, L"%ls-%u%ls", safe, suffix, extensionCopy ? extensionCopy : L"");
        WCHAR *candidate = path_join(imagesDirectory, name);
        WCHAR *ioCandidate = candidate ? extended_path(candidate) : NULL;
        DWORD attrs = candidate ? GetFileAttributesW(ioCandidate ? ioCandidate : candidate) : INVALID_FILE_ATTRIBUTES;
        free(ioCandidate);
        if (candidate && attrs == INVALID_FILE_ATTRIBUTES) {
            size_t relativeCap = wcslen(name) + 8;
            WCHAR *relative = (WCHAR *)malloc(relativeCap * sizeof(WCHAR));
            if (relative) swprintf(relative, relativeCap, L"images/%ls", name);
            if (relative) {
                *destinationOut = candidate;
                *relativeOut = relative;
                free(name);
                free(extensionCopy);
                free(safe);
                free(imagesDirectory);
                return 1;
            }
            free(candidate);
        }
        free(candidate);
        free(name);
    }
    free(extensionCopy);
    free(safe);
    free(imagesDirectory);
    return 0;
}

static int insert_markdown_image(const WCHAR *relativePath, const WCHAR *altText) {
    char *relative = to_utf8(relativePath);
    char *alt = to_utf8(altText && altText[0] ? altText : L"image");
    if (!relative || !alt) { free(relative); free(alt); return 0; }
    size_t escapedCap = strlen(alt) * 2 + 1;
    char *escapedAlt = (char *)malloc(escapedCap);
    if (!escapedAlt) { free(relative); free(alt); return 0; }
    size_t o = 0;
    for (size_t i = 0; alt[i]; i++) {
        if (alt[i] == '\\' || alt[i] == '[' || alt[i] == ']') escapedAlt[o++] = '\\';
        escapedAlt[o++] = alt[i];
    }
    escapedAlt[o] = 0;
    size_t cap = o + strlen(relative) + 16;
    char *markdown = (char *)malloc(cap);
    int inserted = markdown != NULL;
    if (markdown) {
        snprintf(markdown, cap, "![%s](<%s>)", escapedAlt, relative);
        SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)markdown);
        SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
    }
    free(markdown);
    free(escapedAlt);
    free(relative);
    free(alt);
    return inserted;
}

static int import_image_file(const WCHAR *source) {
    WCHAR *destination = NULL, *relative = NULL;
    if (!create_image_destination(source, PathFindExtensionW(source), &destination, &relative)) return 0;
    WCHAR *ioSource = extended_path(source);
    WCHAR *ioDestination = extended_path(destination);
    BOOL copied = CopyFileW(ioSource ? ioSource : source,
                            ioDestination ? ioDestination : destination, TRUE);
    free(ioSource);
    free(ioDestination);
    if (!copied) {
        MessageBoxW(g_hMainWnd, L"Could not copy the dropped image into the document's images folder.",
                    L"MarkEdit", MB_OK | MB_ICONERROR);
        free(destination);
        free(relative);
        return 0;
    }
    WCHAR *alt = wcsdup_heap(PathFindFileNameW(source));
    if (alt) PathRemoveExtensionW(alt);
    int inserted = insert_markdown_image(relative, alt ? alt : L"image");
    free(alt);
    free(destination);
    free(relative);
    return inserted;
}

static int save_hbitmap_as_png(HBITMAP bitmap, const WCHAR *path) {
    IWICImagingFactory *factory = NULL;
    IWICBitmap *source = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *properties = NULL;
    UINT width = 0, height = 0;
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory, (void **)&factory);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateBitmapFromHBITMAP(
        factory, bitmap, NULL, WICBitmapIgnoreAlpha, &source);
    if (SUCCEEDED(hr)) hr = IWICBitmap_GetSize(source, &width, &height);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(factory, &stream);
    WCHAR *ioPath = extended_path(path);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(stream, ioPath ? ioPath : path, GENERIC_WRITE);
    free(ioPath);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &properties);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(frame, properties);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(frame, width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &format);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WriteSource(frame, (IWICBitmapSource *)source, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(encoder);
    if (properties) IPropertyBag2_Release(properties);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IWICStream_Release(stream);
    if (source) IWICBitmap_Release(source);
    if (factory) IWICImagingFactory_Release(factory);
    return SUCCEEDED(hr);
}

static int paste_image_from_clipboard(void) {
    if (!IsClipboardFormatAvailable(CF_BITMAP)) return 0;
    /* Rich sources (browsers, Word, Excel) offer text alongside a rendered
       bitmap. Text is what the user means to paste into Markdown source. */
    if (IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT)) return 0;
    if (!g_currentFile) {
        do_save();
        if (!g_currentFile) return 1;
    }
    if (!OpenClipboard(g_hMainWnd)) return 1;
    HBITMAP bitmap = (HBITMAP)GetClipboardData(CF_BITMAP);
    if (!bitmap) { CloseClipboard(); return 1; }
    SYSTEMTIME now;
    GetLocalTime(&now);
    WCHAR name[64];
    swprintf(name, sizeof(name) / sizeof(name[0]), L"image-%04u%02u%02u-%02u%02u%02u.png",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    WCHAR *destination = NULL, *relative = NULL;
    int made = create_image_destination(name, L".png", &destination, &relative);
    int saved = made && save_hbitmap_as_png(bitmap, destination);
    CloseClipboard();
    if (!saved) {
        MessageBoxW(g_hMainWnd, L"Could not save the clipboard image.",
                    L"MarkEdit", MB_OK | MB_ICONERROR);
        free(destination);
        free(relative);
        return 1;
    }
    insert_markdown_image(relative, L"image");
    free(destination);
    free(relative);
    return 1;
}

static void handle_dropfiles(HDROP hDrop) {
    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
    int insertedImage = 0;
    int undoActive = 0;
    for (UINT i = 0; i < count; i++) {
        UINT cap = DragQueryFileW(hDrop, i, NULL, 0) + 1;
        WCHAR *path = (WCHAR *)malloc((size_t)cap * sizeof(WCHAR));
        if (path && DragQueryFileW(hDrop, i, path, cap)) {
            if (g_editorVisible && is_image_file(path)) {
                if (!undoActive) {
                    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
                    undoActive = 1;
                }
                if (insertedImage) SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)editor_eol_sequence());
                if (import_image_file(path)) insertedImage = 1;
            } else {
                if (undoActive) {
                    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
                    undoActive = 0;
                }
                open_file_in_tab(path);
                insertedImage = 0;
            }
        }
        free(path);
    }
    if (undoActive) SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
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
    apply_document_language(g_hEdit, g_currentFile);
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
    ofn.lpstrFilter = L"Markdown Files (*.md)\0*.md\0Python Files (*.py;*.pyw)\0*.py;*.pyw\0C/C++ Files (*.c;*.cc;*.cpp;*.cxx)\0*.c;*.cc;*.cpp;*.cxx\0Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
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
    g_exportPath = path;
    update_preview();
    HRESULT hr = execute_webview_script(script, &g_exportScriptHandler);
    if (FAILED(hr)) {
        free(g_exportPath);
        g_exportPath = NULL;
        MessageBoxW(g_hMainWnd, L"Could not start the HTML export.", L"MarkEdit", MB_OK | MB_ICONERROR);
    }
    free(script); free(baseUtf8); free(titleUtf8); free(baseEscaped); free(titleEscaped);
}

static void prepare_rendered_print(int exportPdf) {
    if (!g_webview || !g_webviewReady) {
        MessageBoxW(g_hMainWnd, L"The rendered preview is not ready yet.", L"MarkEdit",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g_pendingPrintAction) {
        MessageBoxW(g_hMainWnd, L"A print or PDF operation is already in progress.", L"MarkEdit",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (exportPdf) {
        DWORD capacity = 32768;
        WCHAR *path = (WCHAR *)calloc(capacity, sizeof(WCHAR));
        if (!path) return;
        if (g_currentFile) {
            wcsncpy(path, g_currentFile, capacity - 1);
            PathRenameExtensionW(path, L".pdf");
        } else wcscpy(path, L"Untitled.pdf");
        OPENFILENAMEW dialog = {0};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = g_hMainWnd;
        dialog.lpstrFilter = L"PDF Documents (*.pdf)\0*.pdf\0All Files (*.*)\0*.*\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = capacity;
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        dialog.lpstrDefExt = L"pdf";
        if (!GetSaveFileNameW(&dialog)) { free(path); return; }
        g_pendingPdfPath = path;
        g_pendingPrintAction = 2;
    } else {
        g_pendingPrintAction = 1;
    }
    if (!g_previewVisible) {
        g_previewVisible = 1;
        update_view_menu(g_hMainWnd);
        layout(g_hMainWnd);
    }
    update_preview();
    SetTimer(g_hMainWnd, IDT_PRINT_PREVIEW, 300, NULL);
}

static void run_pending_print(void) {
    KillTimer(g_hMainWnd, IDT_PRINT_PREVIEW);
    if (!g_webview || !g_pendingPrintAction) return;
    if (g_pendingPrintAction == 1) {
        ICoreWebView2_16 *printWebView = NULL;
        if (SUCCEEDED(ICoreWebView2_QueryInterface(g_webview, &IID_ICoreWebView2_16,
                                                    (void **)&printWebView)) && printWebView) {
            HRESULT result = ICoreWebView2_16_ShowPrintUI(printWebView,
                COREWEBVIEW2_PRINT_DIALOG_KIND_SYSTEM);
            ICoreWebView2_16_Release(printWebView);
            if (FAILED(result))
                MessageBoxW(g_hMainWnd, L"Could not open the print dialog.", L"MarkEdit",
                            MB_OK | MB_ICONERROR);
        } else {
            MessageBoxW(g_hMainWnd, L"This WebView2 Runtime does not support printing.",
                        L"MarkEdit", MB_OK | MB_ICONERROR);
        }
        g_pendingPrintAction = 0;
    } else if (g_pendingPrintAction == 2 && g_pendingPdfPath) {
        ICoreWebView2_7 *pdfWebView = NULL;
        if (SUCCEEDED(ICoreWebView2_QueryInterface(g_webview, &IID_ICoreWebView2_7,
                                                   (void **)&pdfWebView)) && pdfWebView) {
            HRESULT result = ICoreWebView2_7_PrintToPdf(pdfWebView, g_pendingPdfPath, NULL,
                                                        &g_pdfHandler);
            ICoreWebView2_7_Release(pdfWebView);
            if (SUCCEEDED(result)) return;
        }
        MessageBoxW(g_hMainWnd, L"This WebView2 Runtime could not start PDF export.",
                    L"MarkEdit", MB_OK | MB_ICONERROR);
        free(g_pendingPdfPath);
        g_pendingPdfPath = NULL;
        g_pendingPrintAction = 0;
    }
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
        /* Closing the final tab leaves one canonical, reusable blank document.
           Numbered names are reserved for additional unsaved tabs. */
        g_documents[0].untitledNumber = 0;
        g_switchingDocument = 1;
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)newDoc);
        configure_scintilla(g_hEdit);
        apply_editor_font(g_fontFaceName, g_fontPointSize);
        SendMessageA(g_hEdit, SCI_CLEARALL, 0, 0);
        SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
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
    clear_recovery_snapshot();
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

/* Reusable searchable picker used by Quick Open, folder search, the command
   palette and spelling-language selection. Item data keeps the
   original index while the visible list is filtered. */
typedef struct ChoiceDialogState {
    WCHAR **labels;
    int count;
    int result;
    const WCHAR *prompt;
    HWND edit;
    HWND list;
} ChoiceDialogState;

static void refill_choice_list(ChoiceDialogState *state) {
    WCHAR filter[256];
    GetWindowTextW(state->edit, filter, sizeof(filter) / sizeof(filter[0]));
    SendMessageW(state->list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < state->count; i++) {
        if (filter[0] && !StrStrIW(state->labels[i], filter)) continue;
        int row = (int)SendMessageW(state->list, LB_ADDSTRING, 0, (LPARAM)state->labels[i]);
        if (row >= 0) SendMessageW(state->list, LB_SETITEMDATA, (WPARAM)row, (LPARAM)i);
    }
    if (SendMessageW(state->list, LB_GETCOUNT, 0, 0) > 0)
        SendMessageW(state->list, LB_SETCURSEL, 0, 0);
}

static LRESULT CALLBACK ChoiceWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ChoiceDialogState *state = (ChoiceDialogState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        state = (ChoiceDialogState *)((CREATESTRUCTW *)lParam)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
        CreateWindowExW(0, L"STATIC", state->prompt, WS_CHILD | WS_VISIBLE,
            12, 10, 556, 20, hwnd, NULL, GetModuleHandleW(NULL), NULL);
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            12, 32, 556, 25, hwnd, (HMENU)10, GetModuleHandleW(NULL), NULL);
        state->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            12, 64, 556, 286, hwnd, (HMENU)11, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Open", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            404, 360, 78, 26, hwnd, (HMENU)IDOK, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            490, 360, 78, 26, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(NULL), NULL);
        refill_choice_list(state);
        SetFocus(state->edit);
        return 0;
    }
    if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wParam) == 10 && HIWORD(wParam) == EN_CHANGE) {
                refill_choice_list(state);
                return 0;
            }
            if (LOWORD(wParam) == IDOK ||
                (LOWORD(wParam) == 11 && HIWORD(wParam) == LBN_DBLCLK)) {
                int row = (int)SendMessageW(state->list, LB_GETCURSEL, 0, 0);
                if (row != LB_ERR)
                    state->result = (int)SendMessageW(state->list, LB_GETITEMDATA, (WPARAM)row, 0);
                if (state->result >= 0) DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
            break;
        case WM_SIZE: {
            int width = LOWORD(lParam), height = HIWORD(lParam);
            MoveWindow(state->edit, 12, 32, width - 24, 25, TRUE);
            MoveWindow(state->list, 12, 64, width - 24, height - 108, TRUE);
            return 0;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static int show_choice_dialog(const WCHAR *title, const WCHAR *prompt,
                              WCHAR **labels, int count) {
    if (!labels || count <= 0) return -1;
    static int registered = 0;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = ChoiceWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MarkEditChoiceDialog";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return -1;
        registered = 1;
    }
    ChoiceDialogState state = { labels, count, -1, prompt, NULL, NULL };
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MarkEditChoiceDialog", title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 440, g_hMainWnd, NULL,
        GetModuleHandleW(NULL), &state);
    if (!dialog) return -1;
    RECT owner, rc;
    GetWindowRect(g_hMainWnd, &owner); GetWindowRect(dialog, &rc);
    SetWindowPos(dialog, HWND_TOP,
        owner.left + ((owner.right - owner.left) - (rc.right - rc.left)) / 2,
        owner.top + ((owner.bottom - owner.top) - (rc.bottom - rc.top)) / 2,
        0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    EnableWindow(g_hMainWnd, FALSE);
    MSG message = {0}; int loopResult = -1;
    while (IsWindow(dialog) && (loopResult = GetMessageW(&message, NULL, 0, 0)) > 0) {
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    EnableWindow(g_hMainWnd, TRUE);
    SetForegroundWindow(g_hMainWnd);
    if (loopResult == 0) PostQuitMessage((int)message.wParam);
    return state.result;
}

typedef struct TextInputState {
    const WCHAR *prompt;
    const WCHAR *initial;
    WCHAR *output;
    int outputCapacity;
    int accepted;
    HWND edit;
} TextInputState;

static LRESULT CALLBACK TextInputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TextInputState *state = (TextInputState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        state = (TextInputState *)((CREATESTRUCTW *)lParam)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
        CreateWindowExW(0, L"STATIC", state->prompt, WS_CHILD | WS_VISIBLE,
            12, 14, 396, 20, hwnd, NULL, GetModuleHandleW(NULL), NULL);
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", state->initial,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            12, 38, 396, 25, hwnd, (HMENU)10, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            244, 76, 78, 26, hwnd, (HMENU)IDOK, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            330, 76, 78, 26, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(NULL), NULL);
        SendMessageW(state->edit, EM_SETSEL, 0, -1);
        SetFocus(state->edit);
        return 0;
    }
    if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);
    if (msg == WM_COMMAND && LOWORD(wParam) == IDOK) {
        GetWindowTextW(state->edit, state->output, state->outputCapacity);
        state->accepted = 1;
        DestroyWindow(hwnd);
        return 0;
    }
    if ((msg == WM_COMMAND && LOWORD(wParam) == IDCANCEL) || msg == WM_CLOSE) {
        DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static int show_text_input(const WCHAR *title, const WCHAR *prompt,
                           const WCHAR *initial, WCHAR *output, int outputCapacity) {
    static int registered = 0;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = TextInputWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MarkEditTextInput";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 0;
        registered = 1;
    }
    TextInputState state = { prompt, initial ? initial : L"", output, outputCapacity, 0, NULL };
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MarkEditTextInput", title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 440, 145, g_hMainWnd, NULL,
        GetModuleHandleW(NULL), &state);
    if (!dialog) return 0;
    RECT owner, rc;
    GetWindowRect(g_hMainWnd, &owner); GetWindowRect(dialog, &rc);
    SetWindowPos(dialog, HWND_TOP,
        owner.left + ((owner.right - owner.left) - (rc.right - rc.left)) / 2,
        owner.top + ((owner.bottom - owner.top) - (rc.bottom - rc.top)) / 2,
        0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    EnableWindow(g_hMainWnd, FALSE);
    MSG message = {0}; int loopResult = -1;
    while (IsWindow(dialog) && (loopResult = GetMessageW(&message, NULL, 0, 0)) > 0) {
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    EnableWindow(g_hMainWnd, TRUE);
    SetForegroundWindow(g_hMainWnd);
    if (loopResult == 0) PostQuitMessage((int)message.wParam);
    return state.accepted;
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
    g_fr.Flags = g_findFlags;

    if (replace) {
        g_hFindDlg = ReplaceTextW(&g_fr);
    } else {
        g_hFindDlg = FindTextW(&g_fr);
    }
}

static void handle_findmsg(FINDREPLACEW *fr) {
    /* Every message carries the current checkbox/radio state, including the
       one sent when the dialog closes, so capture it before acting. */
    g_findFlags = fr->Flags & (FR_MATCHCASE | FR_DOWN);
    save_find_settings();

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

static void do_choose_custom_css(void) {
    DWORD cap = 32768;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    if (g_customCssPath) wcsncpy(path, g_customCssPath, cap - 1);
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"CSS Stylesheets (*.css)\0*.css\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrDefExt = L"css";
    if (GetOpenFileNameW(&ofn)) {
        char *css = read_utf8_text_file(path, 4 * 1024 * 1024);
        if (!css) {
            MessageBoxW(g_hMainWnd,
                L"The stylesheet must be a readable UTF-8 CSS file smaller than 4 MB.",
                L"MarkEdit", MB_OK | MB_ICONERROR);
        } else {
            free(css);
            WCHAR *full = absolute_path(path);
            replace_wstring(&g_customCssPath, full ? full : path);
            free(full);
            save_theme_settings();
            apply_custom_preview_css(1);
            refresh_menu(g_hMainWnd);
        }
    }
    free(path);
}

static void clear_custom_css(void) {
    free(g_customCssPath);
    g_customCssPath = NULL;
    save_theme_settings();
    apply_custom_preview_css(0);
    refresh_menu(g_hMainWnd);
}

/* ---- Help > Markdown Reference: a plain-text syntax cheat sheet. ---- */

/* The Markdown reference is itself Markdown: opening it in a tab renders it
   through the normal preview pipeline, so it demonstrates every construct it
   documents. Kept read-only-ish by being untitled - saving prompts for a path. */
static const char *HELP_REFERENCE_MARKDOWN =
"# MarkEdit Markdown Reference\r\n"
"\r\n"
"CommonMark with GitHub-flavoured extensions and MarkEdit preview features.\r\n"
"Edit this document freely to experiment - it is unsaved, so nothing is\r\n"
"overwritten.\r\n"
"\r\n"
"## Headings\r\n"
"\r\n"
"```\r\n"
"# Heading 1\r\n"
"## Heading 2\r\n"
"### Heading 3\r\n"
"###### Heading 6\r\n"
"```\r\n"
"\r\n"
"H1 and H2 can also be made by underlining the preceding text:\r\n"
"\r\n"
"```\r\n"
"Heading 1\r\n"
"=========\r\n"
"Heading 2\r\n"
"---------\r\n"
"```\r\n"
"\r\n"
"Headings receive link anchors; two or more also show an Outline control.\r\n"
"\r\n"
"## Paragraphs and line breaks\r\n"
"\r\n"
"Separate paragraphs with a blank line.\r\n"
"End a line with two spaces or a backslash for a hard line break.\r\n"
"\r\n"
"## Emphasis and inline code\r\n"
"\r\n"
"| Markdown | Result |\r\n"
"| :--- | :--- |\r\n"
"| `**bold**` or `__bold__` | **bold** |\r\n"
"| `*italic*` or `_italic_` | *italic* |\r\n"
"| `***bold italic***` | ***bold italic*** |\r\n"
"| `~~strikethrough~~` | ~~strikethrough~~ |\r\n"
"| `` `inline code` `` | `inline code` |\r\n"
"\r\n"
"Escape Markdown punctuation with a backslash: \\*literal asterisks\\*\r\n"
"\r\n"
"## Links, images, autolinks\r\n"
"\r\n"
"```\r\n"
"[link text](https://example.com)\r\n"
"[link with title](https://example.com \"Optional title\")\r\n"
"![alt text](images/photo.png)\r\n"
"[relative link](docs/guide.md)\r\n"
"<https://example.com>\r\n"
"https://example.com (bare URLs are linked automatically)\r\n"
"```\r\n"
"\r\n"
"Relative links and images resolve from the Markdown file's folder.\r\n"
"Pasting an image, or dropping one onto the editor, copies it into an\r\n"
"`images` folder beside the document and inserts a relative link.\r\n"
"\r\n"
"Reference-style links:\r\n"
"\r\n"
"```\r\n"
"[link text][id]\r\n"
"\r\n"
"[id]: https://example.com\r\n"
"```\r\n"
"\r\n"
"## Lists\r\n"
"\r\n"
"- bullet item\r\n"
"- another item\r\n"
"  - nested item\r\n"
"\r\n"
"1. first\r\n"
"2. second\r\n"
"   1. nested\r\n"
"\r\n"
"Ordered lists may start at any number. Indent items to nest them.\r\n"
"Pressing Enter continues a list and Tab nests the current item.\r\n"
"\r\n"
"Task lists:\r\n"
"\r\n"
"- [ ] an unchecked task\r\n"
"- [x] a checked task\r\n"
"\r\n"
"## Blockquotes and alerts\r\n"
"\r\n"
"> a quoted line\r\n"
"\r\n"
"> [!NOTE]\r\n"
"> Useful information for the reader.\r\n"
"\r\n"
"> [!TIP]\r\n"
"> An optional suggestion.\r\n"
"\r\n"
"> [!IMPORTANT]\r\n"
"> Something the reader must not miss.\r\n"
"\r\n"
"> [!WARNING]\r\n"
"> A consequence the reader should be careful of.\r\n"
"\r\n"
"> [!CAUTION]\r\n"
"> A risk that needs care.\r\n"
"\r\n"
"## Code blocks\r\n"
"\r\n"
"Indent with 4 spaces, or fence with triple backticks. Add a language for\r\n"
"syntax highlighting, such as `c`, `python`, `javascript`, `powershell`,\r\n"
"or `yaml`.\r\n"
"\r\n"
"```c\r\n"
"int main(void) {\r\n"
"    return 0;\r\n"
"}\r\n"
"```\r\n"
"\r\n"
"## Mermaid diagrams\r\n"
"\r\n"
"Fence diagram source with the `mermaid` language. Use the preview toolbar\r\n"
"or Ctrl+mouse wheel to zoom; drag to pan.\r\n"
"\r\n"
"```mermaid\r\n"
"graph TD\r\n"
"  A[Write Markdown] --> B[Live preview]\r\n"
"  B --> C[Export HTML]\r\n"
"```\r\n"
"\r\n"
"## Maths\r\n"
"\r\n"
"Inline: Euler's identity is $e^{i\\pi} + 1 = 0$.\r\n"
"\r\n"
"Display: $$\\int_0^1 x^2 dx = \\frac{1}{3}$$\r\n"
"\r\n"
"Fenced display maths:\r\n"
"\r\n"
"```math\r\n"
"\\frac{-b \\pm \\sqrt{b^2 - 4ac}}{2a}\r\n"
"```\r\n"
"\r\n"
"## Tables\r\n"
"\r\n"
"Colons in the separator row set column alignment.\r\n"
"\r\n"
"| Left | Centre | Right |\r\n"
"| :--- | :----: | ----: |\r\n"
"| A | B | C |\r\n"
"| longer cell | centred | 42 |\r\n"
"\r\n"
"## Footnotes\r\n"
"\r\n"
"A sentence with a note.[^1]\r\n"
"\r\n"
"[^1]: The footnote text.\r\n"
"\r\n"
"## YAML front matter\r\n"
"\r\n"
"A metadata block must start on the document's first line. It is omitted\r\n"
"from the preview and may close with either `---` or `...`\r\n"
"\r\n"
"```\r\n"
"---\r\n"
"title: Example\r\n"
"tags: [notes, demo]\r\n"
"---\r\n"
"```\r\n"
"\r\n"
"## Horizontal rules\r\n"
"\r\n"
"Three or more `-`, `*`, or `_` on their own line:\r\n"
"\r\n"
"---\r\n"
"\r\n"
"## Preview safety\r\n"
"\r\n"
"Raw HTML is omitted. Unsafe `javascript:` and `data:` URLs are not linked.\r\n";

/* Opens the reference as an ordinary unsaved document, so the live preview
   renders it and every construct in it can be edited and experimented with. */
static void show_help_reference(void) {
    for (int i = 0; i < g_documentCount; i++) {
        if (g_documents[i].isReference) {
            switch_document(i);
            return;
        }
    }
    if (!create_document_tab()) return;
    g_documents[g_activeDocument].isReference = 1;
    g_switchingDocument = 1;
    set_edit_text(g_hEdit, HELP_REFERENCE_MARKDOWN);
    SendMessageA(g_hEdit, SCI_SETSEL, 0, 0);
    SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    g_switchingDocument = 0;
    if (!g_previewVisible) toggle_preview(g_hMainWnd);
    update_tab_label(g_activeDocument);
    update_title();
    update_preview();
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

/* Windows does not recolour the visible, non-client menu bar when an app
   switches its private dark-mode preference at runtime. Draw just those five
   top-level items ourselves; the dropdown contents remain native menus and
   are themed by apply_menu_theme(). */
static HFONT main_menu_font(void) {
    HDC dc = GetDC(g_hMainWnd);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(g_hMainWnd, dc);
    if (!g_menuFont || dpi != g_menuFontDpi) {
        if (g_menuFont) DeleteObject(g_menuFont);
        g_menuFont = CreateFontA(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
        g_menuFontDpi = dpi;
    }
    return g_menuFont;
}

static void measure_main_menu_item(MEASUREITEMSTRUCT *mis) {
    const char *label = (const char *)(UINT_PTR)mis->itemData;
    HDC dc = GetDC(g_hMainWnd);
    if (!dc || !label) {
        if (dc) ReleaseDC(g_hMainWnd, dc);
        return;
    }
    HFONT oldFont = (HFONT)SelectObject(dc, main_menu_font());
    RECT textRect = {0};
    DrawTextA(dc, label, -1, &textRect, DT_SINGLELINE | DT_CALCRECT);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    /* Win32 adds about 12 DPI-scaled pixels of its own to a menu-bar item's
       measured width. Offset part of that so the final rectangle retains
       the seven-pixel inset on each side that draw_main_menu_item uses. */
    int requestedWidth = textRect.right + MulDiv(2, dpi, 96);
    mis->itemWidth = (UINT)(requestedWidth > 1 ? requestedWidth : 1);
    mis->itemHeight = (UINT)(textRect.bottom + MulDiv(8, dpi, 96));
    SelectObject(dc, oldFont);
    ReleaseDC(g_hMainWnd, dc);
}

static void draw_main_menu_item(const DRAWITEMSTRUCT *dis) {
    const char *label = (const char *)(UINT_PTR)dis->itemData;
    if (!label) return;

    int active = (dis->itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
    COLORREF background = g_previewDarkMode
        ? (active ? RGB(0x21, 0x26, 0x2D) : RGB(0x0D, 0x11, 0x17))
        : (active ? RGB(0xE8, 0xEC, 0xF1) : RGB(0xFF, 0xFF, 0xFF));
    COLORREF foreground = (dis->itemState & (ODS_DISABLED | ODS_GRAYED))
        ? (g_previewDarkMode ? RGB(0x8B, 0x94, 0x9E) : RGB(0x65, 0x6D, 0x76))
        : (g_previewDarkMode ? RGB(0xE6, 0xED, 0xF3) : RGB(0x1F, 0x23, 0x28));

    HBRUSH brush = CreateSolidBrush(background);
    FillRect(dis->hDC, &dis->rcItem, brush);
    DeleteObject(brush);

    int oldBkMode = SetBkMode(dis->hDC, TRANSPARENT);
    COLORREF oldText = SetTextColor(dis->hDC, foreground);
    HFONT oldFont = (HFONT)SelectObject(dis->hDC, main_menu_font());
    RECT textRect = dis->rcItem;
    int dpi = GetDeviceCaps(dis->hDC, LOGPIXELSY);
    textRect.left += MulDiv(7, dpi, 96);
    UINT flags = DT_SINGLELINE | DT_LEFT | DT_VCENTER;
    if (dis->itemState & ODS_NOACCEL) flags |= DT_HIDEPREFIX;
    DrawTextA(dis->hDC, label, -1, &textRect, flags);
    SelectObject(dis->hDC, oldFont);
    SetTextColor(dis->hDC, oldText);
    SetBkMode(dis->hDC, oldBkMode);
}

static HMENU build_menu(void) {
    HMENU hMenuBar = CreateMenu();

    HMENU hFile = CreatePopupMenu();
    AppendMenuA(hFile, MF_STRING, IDM_FILE_NEW, "&New\tCtrl+N");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_OPEN, "&Open...\tCtrl+O");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_QUICK_OPEN, "&Quick Open in Folder...\tCtrl+K");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_FIND_FOLDER, "Find in F&older...");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_SAVE, "&Save\tCtrl+S");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_SAVEAS, "Save &As...\tCtrl+Shift+S");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_EXPORT_HTML, "Export Rendered &HTML...");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_EXPORT_PDF, "Export Rendered &PDF...");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_PRINT, "&Print Rendered Preview...\tCtrl+P");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_OPEN_TERMINAL, "Open &Terminal Here");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_SHOW_EXPLORER, "Show in E&xplorer");
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
    AppendMenuA(hMenuBar, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)hFile, "&File");

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
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_DUPLICATE_LINE, "&Duplicate Line\tCtrl+D");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_MOVE_LINE_UP, "Move Line &Up\tAlt+Up");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_MOVE_LINE_DOWN, "Move Line &Down\tAlt+Down");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_DELETE_LINE, "De&lete Line\tCtrl+Shift+K");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_JOIN_LINES, "&Join Lines\tCtrl+J");
    AppendMenuA(hEditMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_TOGGLE_COMMENT, "Toggle Co&mment\tCtrl+/");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_TRIM_WHITESPACE, "Trim Trailing &Whitespace");
    AppendMenuA(hEditMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_SELECT_NEXT, "Select Next &Occurrence\tCtrl+Shift+D");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_SELECT_ALL_OCCURRENCES, "Select All O&ccurrences\tCtrl+Shift+L");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_MATCH_BRACE, "Jump to Matching &Brace\tCtrl+]");
    AppendMenuA(hEditMenu, MF_SEPARATOR, 0, NULL);
    HMENU hBookmarks = CreatePopupMenu();
    AppendMenuA(hBookmarks, MF_STRING, IDM_EDIT_BOOKMARK_TOGGLE, "Toggle &Bookmark\tCtrl+F2");
    AppendMenuA(hBookmarks, MF_STRING, IDM_EDIT_BOOKMARK_NEXT, "&Next Bookmark\tF2");
    AppendMenuA(hBookmarks, MF_STRING, IDM_EDIT_BOOKMARK_PREV, "&Previous Bookmark\tShift+F2");
    AppendMenuA(hBookmarks, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hBookmarks, MF_STRING, IDM_EDIT_BOOKMARK_CLEAR, "&Clear All Bookmarks");
    AppendMenuA(hEditMenu, MF_POPUP, (UINT_PTR)hBookmarks, "Boo&kmarks");
    HMENU hConvert = CreatePopupMenu();
    AppendMenuA(hConvert, MF_STRING, IDM_EDIT_UPPERCASE, "&UPPERCASE");
    AppendMenuA(hConvert, MF_STRING, IDM_EDIT_LOWERCASE, "&lowercase");
    AppendMenuA(hConvert, MF_STRING, IDM_EDIT_TITLECASE, "&Title Case");
    AppendMenuA(hConvert, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hConvert, MF_STRING, IDM_EDIT_SORT_ASC, "Sort Lines &Ascending");
    AppendMenuA(hConvert, MF_STRING, IDM_EDIT_SORT_DESC, "Sort Lines Descend&ing");
    AppendMenuA(hConvert, MF_STRING, IDM_EDIT_SORT_DEDUPE, "Remove &Duplicate Lines");
    AppendMenuA(hConvert, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hConvert, MF_STRING, IDM_EDIT_TABS_TO_SPACES, "Tabs to &Spaces");
    AppendMenuA(hConvert, MF_STRING, IDM_EDIT_SPACES_TO_TABS, "Spaces to Ta&bs");
    AppendMenuA(hConvert, MF_SEPARATOR, 0, NULL);
    HMENU hLineEndings = CreatePopupMenu();
    AppendMenuA(hLineEndings, MF_STRING, IDM_EDIT_EOL_CRLF, "&CRLF (Windows)");
    AppendMenuA(hLineEndings, MF_STRING, IDM_EDIT_EOL_LF, "&LF (Unix/macOS)");
    AppendMenuA(hLineEndings, MF_STRING, IDM_EDIT_EOL_CR, "C&R (Classic Mac)");
    AppendMenuA(hConvert, MF_POPUP, (UINT_PTR)hLineEndings, "Line &Endings");
    AppendMenuA(hEditMenu, MF_POPUP, (UINT_PTR)hConvert, "Con&vert");
    AppendMenuA(hEditMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_FIND, "&Find...\tCtrl+F");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_FINDNEXT, "Find &Next\tF3");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_REPLACE, "&Replace...\tCtrl+H");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_GOTOLINE, "&Go to Line...\tCtrl+G");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_COMMAND_PALETTE, "Command &Palette...\tCtrl+Shift+P");
    AppendMenuA(hMenuBar, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)hEditMenu, "&Edit");

    HMENU hMarkdown = CreatePopupMenu();
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_BOLD, "&Bold");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_ITALIC, "&Italic");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_STRIKETHROUGH, "Strikethrou&gh");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_CODE, "Inline &Code");
    AppendMenuA(hMarkdown, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_H1, "Heading &1");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_H2, "Heading &2");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_H3, "Heading &3");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_QUOTE, "Block&quote");
    AppendMenuA(hMarkdown, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_BULLETLIST, "B&ullet List");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_NUMBERLIST, "&Numbered List");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_TASKLIST, "Tas&k List");
    AppendMenuA(hMarkdown, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_LINK, "&Link");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_IMAGE, "I&mage");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_TABLE, "&Table");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_TABLE_EDITOR, "Table &Editor...");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_CODEBLOCK, "Code Bl&ock");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_FOOTNOTE, "&Footnote");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_HRULE, "&Horizontal Rule");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_ALERT, "&Alert");

    HMENU hJson = CreatePopupMenu();
    AppendMenuA(hJson, MF_STRING, IDM_FMT_JSON_PRETTY, "&Format JSON");
    AppendMenuA(hJson, MF_STRING, IDM_FMT_JSON_MINIFY, "&Minify JSON");

    HMENU hFormat = CreatePopupMenu();
    AppendMenuA(hFormat, MF_POPUP, (UINT_PTR)hMarkdown, "&Markdown");
    AppendMenuA(hFormat, MF_POPUP, (UINT_PTR)hJson, "&JSON");
    AppendMenuA(hFormat, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hFormat, MF_STRING, IDM_FORMAT_FONT, "&Font...");
    AppendMenuA(hMenuBar, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)hFormat, "F&ormat");

    HMENU hRun = CreatePopupMenu();
    AppendMenuA(hRun, MF_STRING, IDM_RUN_FILE, "&Run File\tCtrl+F5");
    AppendMenuA(hRun, MF_STRING, IDM_DEBUG_FILE, "&Debug File\tF5");
    AppendMenuA(hRun, MF_STRING, IDM_DEBUG_CONTINUE, "&Continue\tF6");
    AppendMenuA(hRun, MF_STRING, IDM_DEBUG_STEP_OVER, "Step &Over\tF10");
    AppendMenuA(hRun, MF_STRING, IDM_DEBUG_STEP_INTO, "Step &Into\tShift+F11");
    AppendMenuA(hRun, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hRun, MF_STRING, IDM_DEBUG_BREAKPOINT, "Toggle &Breakpoint\tF9");
    AppendMenuA(hRun, MF_STRING, IDM_DEBUG_CLEAR_BREAKPOINTS, "&Clear All Breakpoints");
    AppendMenuA(hMenuBar, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)hRun, "&Run");

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
    AppendMenuA(hView, MF_STRING, IDM_VIEW_CUSTOM_CSS, "Custom Preview &CSS...");
    AppendMenuA(hView, MF_STRING | (g_customCssPath ? 0 : MF_GRAYED),
                IDM_VIEW_CLEAR_CUSTOM_CSS, "C&lear Custom Preview CSS");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hView, MF_STRING | (g_previewOnLeft ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_PREVIEW_ON_LEFT, "Preview on &Left");
    AppendMenuA(hView, MF_STRING | (g_focusMode ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_FOCUS_MODE, "&Focus Mode\tF11");
    AppendMenuA(hView, MF_STRING | (g_wordWrap ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_WORD_WRAP, "&Word Wrap\tAlt+Z");
    AppendMenuA(hView, MF_STRING | (g_spellCheckEnabled ? MF_CHECKED : MF_UNCHECKED) |
                (g_spellChecker ? 0 : MF_GRAYED), IDM_VIEW_SPELL_CHECK, "&Spell Check");
    AppendMenuA(hView, MF_STRING, IDM_VIEW_SPELL_LANGUAGE, "Spell Check &Language...");
    AppendMenuA(hView, MF_STRING | (g_outlineVisible ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_OUTLINE, "Document &Outline");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hView, MF_STRING | (g_showWhitespace ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_SHOW_WHITESPACE, "Show W&hitespace");
    AppendMenuA(hView, MF_STRING | (g_indentGuides ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_INDENT_GUIDES, "&Indent Guides");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hView, MF_STRING, IDM_VIEW_FOLD_TOGGLE, "Toggle Fo&ld");
    AppendMenuA(hView, MF_STRING, IDM_VIEW_FOLD_ALL, "Fold &All");
    AppendMenuA(hView, MF_STRING, IDM_VIEW_UNFOLD_ALL, "&Unfold All");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hView, MF_STRING, IDM_VIEW_ZOOM_IN, "Zoom &In\tCtrl+=");
    AppendMenuA(hView, MF_STRING, IDM_VIEW_ZOOM_OUT, "Zoom &Out\tCtrl+-");
    AppendMenuA(hView, MF_STRING, IDM_VIEW_ZOOM_RESET, "&Reset Zoom\tCtrl+0");
    AppendMenuA(hMenuBar, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)hView, "&View");

    HMENU hHelp = CreatePopupMenu();
    AppendMenuA(hHelp, MF_STRING, IDM_HELP_REFERENCE, "&Markdown Reference");
    AppendMenuA(hMenuBar, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)hHelp, "&Help");

    if (g_menuBkBrush) {
        MENUINFO info = {0};
        info.cbSize = sizeof(info);
        info.fMask = MIM_BACKGROUND;
        info.hbrBack = g_menuBkBrush;
        SetMenuInfo(hMenuBar, &info);
    }

    return hMenuBar;
}

/* Rebuild and install the menu bar (used after the recent-files list changes,
   since Win32 menus aren't backed by a live model - the HMENU has to be
   replaced whenever its contents differ). */
static void refresh_menu(HWND hwnd) {
    HMENU oldMenu = GetMenu(hwnd);
    if (g_focusMode) {
        if (oldMenu) {
            SetMenu(hwnd, NULL);
            DestroyMenu(oldMenu);
        }
        DrawMenuBar(hwnd);
        return;
    }
    SetMenu(hwnd, build_menu());
    if (oldMenu) DestroyMenu(oldMenu);
    DrawMenuBar(hwnd);
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

/* A terminal window frame with a ">_" prompt inside. */
static void toolbar_icon_terminal(HDC hdc, const RECT *rc, COLORREF fg) {
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    RECT box = { rc->left + w * 8 / 100, rc->top + h * 12 / 100, rc->right - w * 8 / 100, rc->bottom - h * 12 / 100 };
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, box.left, box.top, box.right, box.bottom);

    int boxW = box.right - box.left, boxH = box.bottom - box.top;
    int promptX = box.left + boxW * 20 / 100;
    int promptTopY = box.top + boxH * 30 / 100;
    int promptMidY = box.top + boxH * 50 / 100;
    int promptBottomY = box.top + boxH * 70 / 100;
    int chevronDx = boxW * 16 / 100;

    MoveToEx(hdc, promptX, promptTopY, NULL);
    LineTo(hdc, promptX + chevronDx, promptMidY);
    LineTo(hdc, promptX, promptBottomY);

    int underscoreLeft = promptX + chevronDx + boxW * 6 / 100;
    int underscoreRight = box.right - boxW * 20 / 100;
    MoveToEx(hdc, underscoreLeft, promptBottomY, NULL);
    LineTo(hdc, underscoreRight, promptBottomY);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
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
    { 0, NULL, NULL },
    { IDM_FILE_OPEN_TERMINAL, toolbar_icon_terminal, "Open terminal here" },
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

    int statusHeight = 22;
    if (g_hStatus) {
        MoveWindow(g_hStatus, 0, h - statusHeight, w, statusHeight, TRUE);
        int parts[4] = { MulDiv(150, w > 0 ? GetDpiForWindow(hwnd) : 96, 96),
                         MulDiv(345, w > 0 ? GetDpiForWindow(hwnd) : 96, 96),
                         MulDiv(510, w > 0 ? GetDpiForWindow(hwnd) : 96, 96), -1 };
        SendMessageW(g_hStatus, SB_SETPARTS, 4, (LPARAM)parts);
        /* StatusSubclassProc paints from these part rectangles via
           SB_GETRECT, so a resize needs a repaint to match the new sizes -
           including once at startup, right after the parts are first set. */
        update_status_bar();
        h -= statusHeight;
    }

    /* The formatting toolbar edits Markdown source, so it's only useful (and
       only shown) while the editor pane is visible. */
    ShowWindow(g_hTabs, g_focusMode ? SW_HIDE : SW_SHOW);
    if (!g_focusMode) MoveWindow(g_hTabs, 0, 0, w, TAB_HEIGHT, TRUE);
    int y = g_focusMode ? 0 : TAB_HEIGHT;
    for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++) {
        if (g_hToolbarButtons[i])
            ShowWindow(g_hToolbarButtons[i], g_editorVisible && !g_focusMode ? SW_SHOW : SW_HIDE);
    }
    if (g_editorVisible && !g_focusMode) y = layout_toolbar(w, y);
    h -= y;
    if (h < 0) h = 0;

    int paneWidth = w;
    if (g_hOutline && g_outlineVisible && g_editorVisible && !g_focusMode) {
        int outlineWidth = MulDiv(230, GetDpiForWindow(hwnd), 96);
        if (outlineWidth > w / 3) outlineWidth = w / 3;
        paneWidth = w - outlineWidth;
        MoveWindow(g_hOutline, paneWidth, y, outlineWidth, h, TRUE);
        ShowWindow(g_hOutline, SW_SHOW);
    } else if (g_hOutline) {
        ShowWindow(g_hOutline, SW_HIDE);
    }

    if (g_editorVisible && !g_previewVisible) {
        MoveWindow(g_hEdit, 0, y, paneWidth, h, TRUE);
        ShowWindow(g_hEdit, SW_SHOW);
        ShowWindow(g_hPreview, SW_HIDE);
        RECT zero = { 0, 0, 0, 0 };
        set_webview_bounds(zero, FALSE);
        return;
    }

    if (!g_editorVisible && g_previewVisible) {
        ShowWindow(g_hEdit, SW_HIDE);
        ShowWindow(g_hPreview, SW_SHOW);
        MoveWindow(g_hPreview, 0, y, paneWidth, h, TRUE);
        /* WebView2 controller bounds are relative to g_hPreview's own client
           origin (which MoveWindow already positioned), not the main window. */
        RECT wvRc = { 0, 0, paneWidth, h };
        set_webview_bounds(wvRc, TRUE);
        return;
    }

    /* both visible (the !g_editorVisible && !g_previewVisible case is prevented by the toggle handlers) */
    if (g_splitterPos < 0)
        g_splitterPos = MulDiv(paneWidth - SPLITTER_WIDTH, g_splitterRatio, 1000);
    if (g_splitterPos < 100) g_splitterPos = 100;
    if (g_splitterPos > paneWidth - 100)
        g_splitterPos = (paneWidth - 100 > 100) ? paneWidth - 100 : 100;

    ShowWindow(g_hEdit, SW_SHOW);
    ShowWindow(g_hPreview, SW_SHOW);
    int rightWidth = paneWidth - g_splitterPos - SPLITTER_WIDTH;
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
    if (!hMenuBar) return;
    HMENU hView = GetSubMenu(hMenuBar, 3); /* File, Edit, Format, View */
    CheckMenuItem(hView, IDM_VIEW_TOGGLEPREVIEW, MF_BYCOMMAND | (g_previewVisible ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_TOGGLEEDITOR, MF_BYCOMMAND | (g_editorVisible ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_PREVIEW_ON_LEFT, MF_BYCOMMAND | (g_previewOnLeft ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_WORD_WRAP, MF_BYCOMMAND | (g_wordWrap ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_SPELL_CHECK, MF_BYCOMMAND |
                  (g_spellCheckEnabled ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_SHOW_WHITESPACE, MF_BYCOMMAND | (g_showWhitespace ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_INDENT_GUIDES, MF_BYCOMMAND | (g_indentGuides ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_OUTLINE, MF_BYCOMMAND | (g_outlineVisible ? MF_CHECKED : MF_UNCHECKED));
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
    if (g_previewVisible) update_preview();
    else KillTimer(hwnd, IDT_PREVIEW_UPDATE);
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

static void toggle_focus_mode(HWND hwnd) {
    g_focusMode = !g_focusMode;
    refresh_menu(hwnd);
    layout(hwnd);
    save_layout_settings();
    /* Focus mode hides the menu, so F11 is the only way back out. Keep it on
       the editor where the accelerator is reachable - WebView2 claims F11 for
       its own fullscreen whenever the preview has focus. */
    if (g_editorVisible) SetFocus(g_hEdit);
}

static void toggle_word_wrap(HWND hwnd) {
    g_wordWrap = !g_wordWrap;
    apply_word_wrap(g_hEdit);
    /* Wrapping makes the horizontal extent meaningless; unwrapping has to
       re-measure it, which tracking does as lines come back into view. */
    if (!g_wordWrap) SendMessageA(g_hEdit, SCI_SETSCROLLWIDTH, 1, 0);
    update_view_menu(hwnd);
    save_layout_settings();
}

static void toggle_spell_check(HWND hwnd) {
    if (!g_spellChecker) return;
    g_spellCheckEnabled = !g_spellCheckEnabled;
    if (g_spellCheckEnabled) schedule_spell_check();
    else perform_spell_check(); /* clears existing indicators */
    update_view_menu(hwnd);
    save_layout_settings();
}

static void toggle_show_whitespace(HWND hwnd) {
    g_showWhitespace = !g_showWhitespace;
    apply_show_whitespace(g_hEdit);
    update_view_menu(hwnd);
    save_layout_settings();
}

static void toggle_indent_guides(HWND hwnd) {
    g_indentGuides = !g_indentGuides;
    apply_indent_guides(g_hEdit);
    update_view_menu(hwnd);
    save_layout_settings();
}

/* Scintilla's own zoom range is unbounded in either direction; clamp to a
   range that stays legible (-10) and useful (+20, roughly 3x) at the
   default 10pt-ish editor font. */
static void adjust_editor_zoom(int delta) {
    int zoom = g_editorZoom + delta;
    if (zoom < -10) zoom = -10;
    if (zoom > 20) zoom = 20;
    g_editorZoom = zoom;
    SendMessageA(g_hEdit, SCI_SETZOOM, (WPARAM)g_editorZoom, 0);
    save_layout_settings();
}

static void reset_editor_zoom(void) {
    g_editorZoom = 0;
    SendMessageA(g_hEdit, SCI_SETZOOM, 0, 0);
    save_layout_settings();
}

static void fold_toggle_at_cursor(void) {
    int pos = (int)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    int line = (int)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)pos, 0);
    SendMessageA(g_hEdit, SCI_TOGGLEFOLD, (WPARAM)line, 0);
}

static void fold_all_lines(int expand) {
    SendMessageA(g_hEdit, SCI_FOLDALL,
                 (WPARAM)(expand ? SC_FOLDACTION_EXPAND : SC_FOLDACTION_CONTRACT), 0);
}

/* Sublime/VSCode-style Ctrl+D: selects the word under the caret, or extends
   the multi-selection to the next occurrence of the current selection. */
static void select_next_occurrence(void) {
    SendMessageA(g_hEdit, SCI_MULTIPLESELECTADDNEXT, 0, 0);
}

static int is_brace_char(char c) {
    return c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}';
}

/* The position of the brace adjacent to the caret - preferring the character
   just before it, so the match follows the caret the way editors expect -
   or -1 if the caret isn't next to one. */
static int brace_position_near_caret(void) {
    int pos = (int)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    if (pos > 0 && is_brace_char((char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)(pos - 1), 0)))
        return pos - 1;
    if (is_brace_char((char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)pos, 0)))
        return pos;
    return -1;
}

/* Keeps the matching-brace highlight (STYLE_BRACELIGHT/STYLE_BRACEBAD, set up
   in apply_editor_theme) in sync with the caret. Called on every SCN_UPDATEUI. */
static void update_brace_highlight(void) {
    int bracePos = brace_position_near_caret();
    if (bracePos < 0) {
        SendMessageA(g_hEdit, SCI_BRACEHIGHLIGHT, (WPARAM)-1, (LPARAM)-1);
        return;
    }
    int match = (int)SendMessageA(g_hEdit, SCI_BRACEMATCH, (WPARAM)bracePos, 0);
    if (match >= 0) SendMessageA(g_hEdit, SCI_BRACEHIGHLIGHT, (WPARAM)bracePos, (LPARAM)match);
    else SendMessageA(g_hEdit, SCI_BRACEBADLIGHT, (WPARAM)bracePos, 0);
}

static void jump_to_matching_brace(void) {
    int bracePos = brace_position_near_caret();
    if (bracePos < 0) return;
    int match = (int)SendMessageA(g_hEdit, SCI_BRACEMATCH, (WPARAM)bracePos, 0);
    if (match < 0) return;
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)match, (LPARAM)match);
    SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
}

/* Uppercases the first letter of each word in the selection and lowercases
   the rest. Non-ASCII bytes (including UTF-8 continuation bytes) are left
   untouched rather than mangled. */
static void apply_title_case(void) {
    int start = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int end = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    int length = end - start;
    if (length <= 0) return;
    char *text = (char *)malloc((size_t)length + 1);
    if (!text) return;
    SendMessageA(g_hEdit, SCI_GETSELTEXT, 0, (LPARAM)text);
    int atWordStart = 1;
    for (int i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (isalpha(c)) {
            text[i] = (char)(atWordStart ? toupper(c) : tolower(c));
            atWordStart = 0;
        } else {
            atWordStart = 1;
        }
    }
    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)start, (LPARAM)end);
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)text);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)start, (LPARAM)(start + length));
    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
    free(text);
}

static int compare_lines_asc(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int compare_lines_desc(const void *a, const void *b) {
    return strcmp(*(char *const *)b, *(char *const *)a);
}

/* mode: 0 = sort ascending, 1 = sort descending, 2 = remove duplicate lines
   (first occurrence kept, order otherwise preserved). Operates on the lines
   touched by the current selection, as one undo step. */
static void transform_selected_lines(int mode) {
    int firstLine, lastLine;
    toolbar_selected_line_range(&firstLine, &lastLine);
    if (lastLine <= firstLine) return;

    int rangeStart = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)firstLine, 0);
    int rangeEnd = (int)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)lastLine, 0);
    int totalLength = rangeEnd - rangeStart;
    char *buffer = (char *)malloc((size_t)totalLength + 1);
    if (!buffer) return;
    struct Sci_TextRange tr;
    tr.chrg.cpMin = rangeStart;
    tr.chrg.cpMax = rangeEnd;
    tr.lpstrText = buffer;
    SendMessageA(g_hEdit, SCI_GETTEXTRANGE, 0, (LPARAM)&tr);

    int lineCount = lastLine - firstLine + 1;
    char **lines = (char **)malloc((size_t)lineCount * sizeof(char *));
    if (!lines) { free(buffer); return; }
    int n = 0;
    char *cursor = buffer;
    for (int i = 0; i < lineCount; i++) {
        char *eol = strchr(cursor, '\n');
        size_t len = eol ? (size_t)(eol - cursor) : strlen(cursor);
        if (len > 0 && cursor[len - 1] == '\r') len--;
        char *copy = (char *)malloc(len + 1);
        if (!copy) {
            for (int j = 0; j < n; j++) free(lines[j]);
            free(lines);
            free(buffer);
            return;
        }
        memcpy(copy, cursor, len);
        copy[len] = 0;
        lines[n++] = copy;
        cursor = eol ? eol + 1 : cursor + strlen(cursor);
    }

    if (mode == 2) {
        int kept = 0;
        for (int i = 0; i < n; i++) {
            int dup = 0;
            for (int j = 0; j < kept; j++) {
                if (strcmp(lines[j], lines[i]) == 0) { dup = 1; break; }
            }
            if (dup) free(lines[i]);
            else lines[kept++] = lines[i];
        }
        n = kept;
    } else {
        qsort(lines, (size_t)n, sizeof(char *), mode == 1 ? compare_lines_desc : compare_lines_asc);
    }

    const char *eol = editor_eol_sequence();
    size_t outCap = 1;
    for (int i = 0; i < n; i++) outCap += strlen(lines[i]) + strlen(eol);
    char *out = (char *)malloc(outCap);
    if (out) {
        out[0] = 0;
        for (int i = 0; i < n; i++) {
            strcat(out, lines[i]);
            if (i < n - 1) strcat(out, eol);
        }
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)rangeStart, (LPARAM)rangeEnd);
        SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)out);
        free(out);
    }
    for (int i = 0; i < n; i++) free(lines[i]);
    free(lines);
    free(buffer);
}

/* Rewrites each line's leading indentation between tabs and spaces, using the
   editor's configured tab width. Only leading whitespace is touched - tabs or
   spaces elsewhere on the line (inside a string, say) are left alone. */
static void convert_indentation(int toSpaces) {
    int tabWidth = (int)SendMessageA(g_hEdit, SCI_GETTABWIDTH, 0, 0);
    if (tabWidth <= 0) tabWidth = 4;
    int lineCount = (int)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
    for (int line = lineCount - 1; line >= 0; line--) {
        int lineStart = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
        int lineEnd = (int)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)line, 0);
        int indentEnd = lineStart;
        int column = 0;
        while (indentEnd < lineEnd) {
            char ch = (char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)indentEnd, 0);
            if (ch == ' ') { column++; indentEnd++; }
            else if (ch == '\t') { column += tabWidth - (column % tabWidth); indentEnd++; }
            else break;
        }
        if (indentEnd == lineStart) continue;
        int newLen;
        char *newIndent;
        if (toSpaces) {
            newLen = column;
            newIndent = (char *)malloc((size_t)newLen + 1);
            if (!newIndent) continue;
            memset(newIndent, ' ', (size_t)newLen);
        } else {
            int tabs = column / tabWidth;
            int spaces = column % tabWidth;
            newLen = tabs + spaces;
            newIndent = (char *)malloc((size_t)newLen + 1);
            if (!newIndent) continue;
            memset(newIndent, '\t', (size_t)tabs);
            memset(newIndent + tabs, ' ', (size_t)spaces);
        }
        newIndent[newLen] = 0;
        SendMessageA(g_hEdit, SCI_SETTARGETRANGE, (WPARAM)lineStart, (LPARAM)indentEnd);
        SendMessageA(g_hEdit, SCI_REPLACETARGET, (WPARAM)newLen, (LPARAM)newIndent);
        free(newIndent);
    }
    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
}

/* Rewrites every line ending to eolMode (SC_EOL_CRLF/CR/LF) and switches the
   document's own EOL mode to match, so newly typed newlines follow suit. */
static void convert_line_endings(int eolMode) {
    SendMessageA(g_hEdit, SCI_CONVERTEOLS, (WPARAM)eolMode, 0);
    SendMessageA(g_hEdit, SCI_SETEOLMODE, (WPARAM)eolMode, 0);
    update_status_position();
}

/* Sublime/VSCode-style Ctrl+Shift+L: every occurrence of the word under the
   caret (or of the current selection) becomes its own selection at once. */
static void select_all_occurrences(void) {
    SendMessageA(g_hEdit, SCI_MULTIPLESELECTADDEACH, 0, 0);
}

/* Bookmarks are Scintilla markers on MARKER_BOOKMARK, which live on the
   document buffer itself - each tab keeps its own set automatically as
   SCI_SETDOCPOINTER swaps between them. */
static void toggle_bookmark_at_line(int line) {
    int mask = (int)SendMessageA(g_hEdit, SCI_MARKERGET, (WPARAM)line, 0);
    if (mask & (1 << MARKER_BOOKMARK))
        SendMessageA(g_hEdit, SCI_MARKERDELETE, (WPARAM)line, (LPARAM)MARKER_BOOKMARK);
    else
        SendMessageA(g_hEdit, SCI_MARKERADD, (WPARAM)line, (LPARAM)MARKER_BOOKMARK);
}

static void toggle_bookmark_at_cursor(void) {
    int pos = (int)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    toggle_bookmark_at_line((int)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)pos, 0));
}

static void goto_bookmark(int forward) {
    int pos = (int)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    int line = (int)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)pos, 0);
    int found;
    if (forward) {
        found = (int)SendMessageA(g_hEdit, SCI_MARKERNEXT, (WPARAM)(line + 1), (LPARAM)(1 << MARKER_BOOKMARK));
        if (found < 0) found = (int)SendMessageA(g_hEdit, SCI_MARKERNEXT, 0, (LPARAM)(1 << MARKER_BOOKMARK));
    } else {
        found = (int)SendMessageA(g_hEdit, SCI_MARKERPREVIOUS, (WPARAM)(line - 1), (LPARAM)(1 << MARKER_BOOKMARK));
        if (found < 0) {
            int lastLine = (int)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0) - 1;
            found = (int)SendMessageA(g_hEdit, SCI_MARKERPREVIOUS, (WPARAM)lastLine, (LPARAM)(1 << MARKER_BOOKMARK));
        }
    }
    if (found < 0) return;
    int gotoPos = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)found, 0);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)gotoPos, (LPARAM)gotoPos);
    SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
}

static void clear_all_bookmarks(void) {
    SendMessageA(g_hEdit, SCI_MARKERDELETEALL, (WPARAM)MARKER_BOOKMARK, 0);
}

typedef enum DebugSourceKind {
    DEBUG_SOURCE_NONE = 0, DEBUG_SOURCE_PYTHON, DEBUG_SOURCE_C, DEBUG_SOURCE_CPP, DEBUG_SOURCE_LUA
} DebugSourceKind;

static DebugSourceKind debug_source_kind(const WCHAR *path) {
    const WCHAR *extension = path ? PathFindExtensionW(path) : NULL;
    if (!extension) return DEBUG_SOURCE_NONE;
    if (_wcsicmp(extension, L".py") == 0 || _wcsicmp(extension, L".pyw") == 0)
        return DEBUG_SOURCE_PYTHON;
    if (_wcsicmp(extension, L".c") == 0) return DEBUG_SOURCE_C;
    if (_wcsicmp(extension, L".cc") == 0 || _wcsicmp(extension, L".cpp") == 0 ||
        _wcsicmp(extension, L".cxx") == 0)
        return DEBUG_SOURCE_CPP;
    if (_wcsicmp(extension, L".lua") == 0) return DEBUG_SOURCE_LUA;
    return DEBUG_SOURCE_NONE;
}

/* Only Python/C/C++ have a debugger backend (pdb, GDB); Lua is Run-only, so
   it must not be treated as debuggable just because debug_source_kind()
   returns something other than DEBUG_SOURCE_NONE for it - that value also
   drives whether clicking the margin sets a breakpoint vs. a bookmark. */
static int is_debuggable_source(DebugSourceKind kind) {
    return kind == DEBUG_SOURCE_PYTHON || kind == DEBUG_SOURCE_C || kind == DEBUG_SOURCE_CPP;
}

/* Debugger breakpoints are separate from general-purpose bookmarks, but share
   the same narrow marker margin. Scintilla keeps them attached to their lines
   as text is inserted or removed. */
static void toggle_breakpoint_at_line(int line) {
    if (!is_debuggable_source(debug_source_kind(g_currentFile))) {
        MessageBoxW(g_hMainWnd, L"Breakpoints are available for Python, C, and C++ files.",
                    L"Debugging", MB_OK | MB_ICONINFORMATION);
        return;
    }
    int mask = (int)SendMessageA(g_hEdit, SCI_MARKERGET, (WPARAM)line, 0);
    if (mask & (1 << MARKER_BREAKPOINT))
        SendMessageA(g_hEdit, SCI_MARKERDELETE, (WPARAM)line, (LPARAM)MARKER_BREAKPOINT);
    else
        SendMessageA(g_hEdit, SCI_MARKERADD, (WPARAM)line, (LPARAM)MARKER_BREAKPOINT);
}

static void toggle_breakpoint_at_cursor(void) {
    int pos = (int)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    int line = (int)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)pos, 0);
    toggle_breakpoint_at_line(line);
}

static void clear_debug_breakpoints(void) {
    SendMessageA(g_hEdit, SCI_MARKERDELETEALL, (WPARAM)MARKER_BREAKPOINT, 0);
}

static int is_runnable_python_path(const WCHAR *path) {
    return debug_source_kind(path) == DEBUG_SOURCE_PYTHON;
}

static int is_existing_file(const WCHAR *path) {
    DWORD attributes = path ? GetFileAttributesW(path) : INVALID_FILE_ATTRIBUTES;
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/* The full path to Windows PowerShell when it's found under the system
   directory, or NULL to let ShellExecute resolve "powershell.exe" from PATH
   instead. Caller frees a non-NULL result. */
static WCHAR *locate_powershell(void) {
    WCHAR systemDirectory[MAX_PATH];
    if (!GetSystemDirectoryW(systemDirectory, MAX_PATH)) return NULL;
    WCHAR *candidate = path_join(systemDirectory, L"WindowsPowerShell\\v1.0\\powershell.exe");
    if (candidate && is_existing_file(candidate)) return candidate;
    free(candidate);
    return NULL;
}

/* Collects the 0-based line numbers of every breakpoint marker in the active
   document in one pass, so callers don't need a separate counting scan
   before walking the same markers again to emit debugger commands. Returns
   the count; *outLines is NULL when it's 0. Caller frees a non-NULL result. */
static int collect_breakpoint_lines(int **outLines) {
    int capacity = 0, count = 0;
    int *lines = NULL;
    int line = -1;
    while ((line = (int)SendMessageA(g_hEdit, SCI_MARKERNEXT, (WPARAM)(line + 1),
                                      (LPARAM)(1 << MARKER_BREAKPOINT))) >= 0) {
        if (count >= capacity) {
            int next = capacity ? capacity * 2 : 8;
            int *grown = (int *)realloc(lines, (size_t)next * sizeof(int));
            if (!grown) break;
            lines = grown;
            capacity = next;
        }
        lines[count++] = line;
    }
    *outLines = lines;
    return count;
}

static WCHAR *python_from_environment_root(const WCHAR *root) {
    if (!root || !root[0]) return NULL;
    WCHAR *candidate = path_join(root, L"Scripts\\python.exe");
    if (candidate && is_existing_file(candidate)) return candidate;
    free(candidate);
    return NULL;
}

static WCHAR *python_from_named_environment(const WCHAR *directory, const WCHAR *name) {
    WCHAR *root = path_join(directory, name);
    WCHAR *python = python_from_environment_root(root);
    free(root);
    return python;
}

static WCHAR *executable_on_search_path(const WCHAR *name) {
    DWORD capacity = SearchPathW(NULL, name, NULL, 0, NULL, NULL);
    if (!capacity) return NULL;
    WCHAR *path = (WCHAR *)malloc(((size_t)capacity + 1) * sizeof(WCHAR));
    if (!path) return NULL;
    DWORD length = SearchPathW(NULL, name, NULL, capacity + 1, path, NULL);
    if (!length || length > capacity) {
        free(path);
        return NULL;
    }
    return path;
}

/* Prefer an activated environment, then a project-local .venv/venv found by
   walking up from the script, and finally the standard launcher/interpreters
   on PATH. This lets common Python projects work without MarkEdit settings. */
static WCHAR *find_python_interpreter(const WCHAR *scriptPath) {
    DWORD environmentLength = GetEnvironmentVariableW(L"VIRTUAL_ENV", NULL, 0);
    if (environmentLength) {
        WCHAR *environment = (WCHAR *)malloc((size_t)environmentLength * sizeof(WCHAR));
        if (environment && GetEnvironmentVariableW(L"VIRTUAL_ENV", environment,
                                                    environmentLength)) {
            WCHAR *python = python_from_environment_root(environment);
            free(environment);
            if (python) return python;
        } else {
            free(environment);
        }
    }

    WCHAR *directory = path_directory(scriptPath);
    while (directory) {
        WCHAR *python = python_from_named_environment(directory, L".venv");
        if (!python) python = python_from_named_environment(directory, L"venv");
        if (python) {
            free(directory);
            return python;
        }
        WCHAR *parent = path_directory(directory);
        if (!parent || _wcsicmp(parent, directory) == 0) {
            free(parent);
            break;
        }
        free(directory);
        directory = parent;
    }
    free(directory);

    static const WCHAR *names[] = { L"py.exe", L"python.exe", L"python3.exe" };
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        WCHAR *python = executable_on_search_path(names[i]);
        if (python) return python;
    }
    return NULL;
}

/* A PowerShell single-quoted literal only treats another apostrophe specially;
   doubling it gives us safe arguments even for paths containing shell syntax. */
static WCHAR *powershell_literal(const WCHAR *value) {
    size_t length = wcslen(value);
    WCHAR *literal = (WCHAR *)malloc((length * 2 + 3) * sizeof(WCHAR));
    if (!literal) return NULL;
    size_t output = 0;
    literal[output++] = L'\'';
    for (size_t i = 0; i < length; i++) {
        if (value[i] == L'\'') literal[output++] = L'\'';
        literal[output++] = value[i];
    }
    literal[output++] = L'\'';
    literal[output] = 0;
    return literal;
}

static WCHAR *debug_temporary_path(const WCHAR *extension) {
    WCHAR temporaryDirectory[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, temporaryDirectory);
    if (!length || length >= MAX_PATH) return NULL;
    WCHAR name[96];
    swprintf(name, sizeof(name) / sizeof(name[0]), L"markedit-debug-%lu-%llu.%ls",
             GetCurrentProcessId(), (unsigned long long)GetTickCount64(), extension);
    return path_join(temporaryDirectory, name);
}

static void clear_debug_execution_markers(void) {
    if (!g_hEdit || g_documentCount <= 0) return;
    sptr_t active = (sptr_t)SendMessageA(g_hEdit, SCI_GETDOCPOINTER, 0, 0);
    int oldSwitching = g_switchingDocument;
    g_switchingDocument = 1;
    for (int i = 0; i < g_documentCount; i++) {
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_documents[i].sciDocument);
        SendMessageA(g_hEdit, SCI_MARKERDELETEALL, MARKER_DEBUG_LINE, 0);
    }
    SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)active);
    g_switchingDocument = oldSwitching;
}

static void stop_debug_location_tracking(void) {
    if (g_hMainWnd) KillTimer(g_hMainWnd, IDT_DEBUG_LOCATION);
    if (g_debugProcess) {
        CloseHandle(g_debugProcess);
        g_debugProcess = NULL;
    }
    if (g_debugLocationPath) DeleteFileW(g_debugLocationPath);
    free(g_debugLocationPath); g_debugLocationPath = NULL;
    free(g_debugWorkingDirectory); g_debugWorkingDirectory = NULL;
    free(g_lastDebugSource); g_lastDebugSource = NULL;
    g_debugLocationSeen = 0;
    g_lastDebugLine = -1;
    g_activeDebuggerKind = 0;
    clear_debug_execution_markers();
}

static void start_debug_location_tracking(const WCHAR *locationPath,
                                          const WCHAR *workingDirectory,
                                          int debuggerKind, HANDLE process) {
    stop_debug_location_tracking();
    g_debugLocationPath = wcsdup_heap(locationPath);
    g_debugWorkingDirectory = wcsdup_heap(workingDirectory);
    g_debugProcess = process;
    g_debugLocationSeen = 1; /* The launcher creates a `starting` sentinel. */
    g_activeDebuggerKind = debuggerKind;
    if (g_hMainWnd) SetTimer(g_hMainWnd, IDT_DEBUG_LOCATION, 150, NULL);
}

static WCHAR *debug_location_to_wide(const char *text) {
    WCHAR *wide = to_wide(text);
    if (wide) return wide;
    int length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
    if (length <= 0) return NULL;
    wide = (WCHAR *)malloc((size_t)length * sizeof(WCHAR));
    if (!wide || !MultiByteToWideChar(CP_ACP, 0, text, -1, wide, length)) {
        free(wide);
        return NULL;
    }
    return wide;
}

static int parse_debug_location(char *text, WCHAR **sourceOut, int *lineOut) {
    if (!text || !sourceOut || !lineOut) return 0;
    char *source = text;
    char *separator = strchr(text, '\n');
    char *lineText = NULL;
    if (separator) {
        *separator = 0;
        lineText = separator + 1;
        /* GDB's `frame` output always starts with the "#N" frame number;
           matching on " at " too would misidentify a pdb sentinel (a bare
           "path\nline") whose path happens to contain that substring. */
        if (text[0] == '#') {
            *separator = '\n';
            char *at = strstr(text, " at ");
            if (!at) return 0;
            source = at + 4;
            char *end = strchr(source, '\n');
            if (end) *end = 0;
            char *colon = strrchr(source, ':');
            if (!colon) return 0;
            *colon = 0;
            lineText = colon + 1;
        }
    }
    if (!separator || !lineText) return 0;
    char *endNumber = NULL;
    long line = strtol(lineText, &endNumber, 10);
    if (endNumber == lineText || line <= 0 || line > INT_MAX) return 0;
    size_t sourceLength = strlen(source);
    while (sourceLength && (source[sourceLength - 1] == '\r' || source[sourceLength - 1] == ' '))
        source[--sourceLength] = 0;
    WCHAR *wide = debug_location_to_wide(source);
    if (!wide) return 0;
    for (WCHAR *character = wide; *character; character++)
        if (*character == L'/') *character = L'\\';
    WCHAR *resolved = PathIsRelativeW(wide) && g_debugWorkingDirectory
        ? path_join(g_debugWorkingDirectory, wide) : wcsdup_heap(wide);
    free(wide);
    if (!resolved) return 0;
    WCHAR *absolute = absolute_path(resolved);
    free(resolved);
    *sourceOut = absolute;
    *lineOut = (int)line;
    return absolute != NULL;
}

static void poll_debug_location(void) {
    if (!g_debugLocationPath) return;
    if (g_debugProcess && WaitForSingleObject(g_debugProcess, 0) == WAIT_OBJECT_0) {
        stop_debug_location_tracking();
        return;
    }
    int length = 0;
    char *text = read_bytes_file(g_debugLocationPath, &length);
    if (!text) {
        /* A debugger may have the file briefly open while replacing its
           contents. Only a genuinely missing file means the session ended. */
        if (GetFileAttributesW(g_debugLocationPath) != INVALID_FILE_ATTRIBUTES) return;
        if (g_debugLocationSeen) stop_debug_location_tracking();
        return;
    }
    g_debugLocationSeen = 1;
    WCHAR *source = NULL;
    int line = 0;
    int parsed = parse_debug_location(text, &source, &line);
    free(text);
    if (!parsed) return;
    if (g_lastDebugSource && _wcsicmp(g_lastDebugSource, source) == 0 &&
        g_lastDebugLine == line) {
        free(source);
        return;
    }
    if (!is_existing_file(source) || !open_file_in_tab(source)) {
        free(source);
        return;
    }
    clear_debug_execution_markers();
    SendMessageA(g_hEdit, SCI_MARKERADD, (WPARAM)(line - 1), MARKER_DEBUG_LINE);
    SendMessageA(g_hEdit, SCI_ENSUREVISIBLE, (WPARAM)(line - 1), 0);
    int visibleLines = (int)SendMessageA(g_hEdit, SCI_LINESONSCREEN, 0, 0);
    SendMessageA(g_hEdit, SCI_SETFIRSTVISIBLELINE,
                 (WPARAM)(line > visibleLines / 2 ? line - visibleLines / 2 - 1 : 0), 0);
    replace_wstring(&g_lastDebugSource, source);
    g_lastDebugLine = line;
    free(source);
}

static int send_debug_console_command(const WCHAR *command) {
    if (!g_debugProcess || WaitForSingleObject(g_debugProcess, 0) != WAIT_TIMEOUT)
        return 0;
    DWORD processId = GetProcessId(g_debugProcess);
    if (!processId) return 0;
    if (!AttachConsole(processId)) {
        if (GetLastError() != ERROR_ACCESS_DENIED) return 0;
        FreeConsole();
        if (!AttachConsole(processId)) return 0;
    }
    HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
    int ok = 0;
    if (input != INVALID_HANDLE_VALUE) {
        size_t length = wcslen(command);
        INPUT_RECORD *records = (INPUT_RECORD *)calloc(length * 2 + 2, sizeof(INPUT_RECORD));
        if (records) {
            DWORD count = 0;
            for (size_t i = 0; i < length; i++) {
                records[count].EventType = KEY_EVENT;
                records[count].Event.KeyEvent.bKeyDown = TRUE;
                records[count].Event.KeyEvent.wRepeatCount = 1;
                records[count++].Event.KeyEvent.uChar.UnicodeChar = command[i];
                records[count] = records[count - 1];
                records[count++].Event.KeyEvent.bKeyDown = FALSE;
            }
            records[count].EventType = KEY_EVENT;
            records[count].Event.KeyEvent.bKeyDown = TRUE;
            records[count].Event.KeyEvent.wRepeatCount = 1;
            records[count].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
            records[count++].Event.KeyEvent.uChar.UnicodeChar = L'\r';
            records[count] = records[count - 1];
            records[count++].Event.KeyEvent.bKeyDown = FALSE;
            DWORD inserted = 0;
            ok = WriteConsoleInputW(input, records, count, &inserted) && inserted == count;
            free(records);
        }
        CloseHandle(input);
    }
    FreeConsole();
    return ok;
}

static void run_debugger_command(int action) {
    const WCHAR *command = NULL;
    if (g_activeDebuggerKind == 1)
        command = action == IDM_DEBUG_STEP_OVER ? L"nv" :
                  action == IDM_DEBUG_STEP_INTO ? L"sv" : L"cv";
    else if (g_activeDebuggerKind == 2)
        command = action == IDM_DEBUG_STEP_OVER ? L"next" :
                  action == IDM_DEBUG_STEP_INTO ? L"step" : L"continue";
    if (!command || !send_debug_console_command(command))
        MessageBoxW(g_hMainWnd, L"Start a debug session with F5 before using this command.",
                    L"Debugging", MB_OK | MB_ICONINFORMATION);
}

static WCHAR *python_double_quoted_literal(const WCHAR *value) {
    size_t length = wcslen(value);
    WCHAR *literal = (WCHAR *)malloc((length * 2 + 3) * sizeof(WCHAR));
    if (!literal) return NULL;
    size_t output = 0;
    literal[output++] = L'"';
    for (size_t i = 0; i < length; i++) {
        if (value[i] == L'\\' || value[i] == L'"') literal[output++] = L'\\';
        literal[output++] = value[i];
    }
    literal[output++] = L'"';
    literal[output] = 0;
    return literal;
}

static int write_pdb_driver(const WCHAR *path, const WCHAR *statePath, const WCHAR *scriptPath,
                            const int *breakpointLines, int breakpointCount) {
    WCHAR *stateLiteral = python_double_quoted_literal(statePath);
    WCHAR *scriptLiteral = python_double_quoted_literal(scriptPath);
    if (!stateLiteral || !scriptLiteral) {
        free(stateLiteral); free(scriptLiteral);
        return 0;
    }
    size_t capacity = wcslen(stateLiteral) + wcslen(scriptLiteral) +
                      (size_t)breakpointCount * 64 + 3072;
    WCHAR *driver = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!driver) {
        free(stateLiteral); free(scriptLiteral);
        return 0;
    }
    /* Two Python-3.13+ pdb quirks this driver has to work around:
       1. debugger._run(pdb._ScriptTarget(...)) (used below when available)
          clears the driver's own __main__ globals before running the target
          script, so a plain module-level lookup of STATE/pprint from inside
          interaction() - called back later, once the script is running -
          raises NameError. Capturing them as default-argument values bakes
          them into the function object at *definition* time instead, which
          survives the globals wipe.
       2. pdb._ScriptTarget resolves the script path through realpath-style
          normalization for the code object it compiles, so a breakpoint set
          against the path as given (which can be an 8.3 short name, e.g. for
          a file under a long-username or %TEMP%-style directory) silently
          never matches. Normalizing SCRIPT the same way before set_break()
          keeps both in agreement regardless of which path form was passed in. */
    int written = swprintf(driver, capacity,
        L"import os, pdb, pprint, sys\n"
        L"STATE = %ls\n"
        L"SCRIPT = %ls\n"
        L"SCRIPT = os.path.realpath(SCRIPT)\n"
        L"class MarkEditPdb(pdb.Pdb):\n"
        L"    def interaction(self, frame, traceback, _markedit_state=STATE, _markedit_pprint=pprint):\n"
        L"        try:\n"
        L"            with open(_markedit_state, 'w', encoding='utf-8') as stream:\n"
        L"                stream.write(frame.f_code.co_filename + '\\n' + str(frame.f_lineno))\n"
        L"        except OSError:\n"
        L"            pass\n"
        L"        print('\\nLocals:')\n"
        L"        _markedit_pprint.pprint({key: value for key, value in frame.f_locals.items() "
        L"if not key.startswith('__')})\n"
        L"        return super().interaction(frame, traceback)\n"
        L"debugger = MarkEditPdb()\n"
        L"debugger.rcLines.extend([\n"
        L"    'alias vars pp locals()',\n"
        L"    'alias globals pp globals()',\n"
        L"    'alias nv next',\n"
        L"    'alias sv step',\n"
        L"    'alias cv continue',\n"
        L"])\n",
        stateLiteral, scriptLiteral);
    free(stateLiteral); free(scriptLiteral);
    if (written < 0) {
        free(driver);
        return 0;
    }
    size_t used = (size_t)written;
    for (int i = 0; i < breakpointCount; i++) {
        written = swprintf(driver + used, capacity - used,
                           L"debugger.set_break(SCRIPT, %d)\n", breakpointLines[i] + 1);
        if (written < 0) { free(driver); return 0; }
        used += (size_t)written;
    }
    if (breakpointCount) {
        written = swprintf(driver + used, capacity - used,
                           L"debugger.rcLines.append('continue')\n");
        if (written < 0) { free(driver); return 0; }
        used += (size_t)written;
    }
    written = swprintf(driver + used, capacity - used,
        L"sys.argv = [SCRIPT]\n"
        L"if hasattr(pdb, '_ScriptTarget') and hasattr(debugger, '_run'):\n"
        L"    debugger._run(pdb._ScriptTarget(SCRIPT))\n"
        L"elif hasattr(debugger, '_runscript'):\n"
        L"    debugger._runscript(SCRIPT)\n"
        L"else:\n"
        L"    namespace = {'__name__': '__main__', '__file__': SCRIPT}\n"
        L"    debugger.run(compile(open(SCRIPT, 'rb').read(), SCRIPT, 'exec'), namespace, namespace)\n");
    if (written < 0) { free(driver); return 0; }
    char *utf8 = to_utf8(driver);
    free(driver);
    if (!utf8) return 0;
    int result = write_bytes_file(path, utf8, strlen(utf8));
    free(utf8);
    return result;
}

/* Shared cleanup for launch_python_file()'s several early-exit points. */
static void free_python_launch_buffers(WCHAR *directory, WCHAR *debugLocation, WCHAR *pdbDriver,
                                        WCHAR *quotedPython, WCHAR *quotedScript,
                                        WCHAR *quotedDebugLocation, WCHAR *quotedPdbDriver) {
    free(directory);
    free(debugLocation);
    free(pdbDriver);
    free(quotedPython);
    free(quotedScript);
    free(quotedDebugLocation);
    free(quotedPdbDriver);
}

static void launch_python_file(int debug) {
    /* launch_active_source_file() already saved g_currentFile before calling
       here, same as it does before launch_native_file(). */
    if (!is_runnable_python_path(g_currentFile)) {
        MessageBoxW(g_hMainWnd,
                    L"Save the active document as a .py or .pyw file first.",
                    debug ? L"Debug Python" : L"Run Python",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    WCHAR *python = find_python_interpreter(g_currentFile);
    if (!python) {
        MessageBoxW(g_hMainWnd,
                    L"No Python interpreter was found. Install Python 3, activate a virtual "
                    L"environment, or create .venv beside the project.",
                    debug ? L"Debug Python" : L"Run Python",
                    MB_OK | MB_ICONERROR);
        return;
    }
    WCHAR *directory = path_directory(g_currentFile);
    int usePython3Launcher = _wcsicmp(PathFindFileNameW(python), L"py.exe") == 0;
    WCHAR *debugLocation = debug ? debug_temporary_path(L"location") : NULL;
    WCHAR *pdbDriver = debug ? debug_temporary_path(L"py") : NULL;
    WCHAR *quotedPython = powershell_literal(python);
    WCHAR *quotedScript = powershell_literal(g_currentFile);
    WCHAR *quotedDebugLocation = debugLocation ? powershell_literal(debugLocation) : NULL;
    WCHAR *quotedPdbDriver = pdbDriver ? powershell_literal(pdbDriver) : NULL;
    free(python);
    if (!directory || !quotedPython || !quotedScript ||
        (debug && (!debugLocation || !pdbDriver || !quotedDebugLocation || !quotedPdbDriver))) {
        free_python_launch_buffers(directory, debugLocation, pdbDriver, quotedPython,
                                    quotedScript, quotedDebugLocation, quotedPdbDriver);
        return;
    }

    int *breakpointLines = NULL;
    int breakpointCount = debug ? collect_breakpoint_lines(&breakpointLines) : 0;
    if (debug && (!write_pdb_driver(pdbDriver, debugLocation, g_currentFile,
                                     breakpointLines, breakpointCount) ||
                  !write_bytes_file(debugLocation, "starting\n", 9))) {
        free(breakpointLines);
        DeleteFileW(pdbDriver);
        DeleteFileW(debugLocation);
        free_python_launch_buffers(directory, debugLocation, pdbDriver, quotedPython,
                                    quotedScript, quotedDebugLocation, quotedPdbDriver);
        return;
    }
    free(breakpointLines);
    size_t capacity = wcslen(quotedPython) + wcslen(quotedScript) +
                      (quotedDebugLocation ? wcslen(quotedDebugLocation) : 0) +
                      (quotedPdbDriver ? wcslen(quotedPdbDriver) : 0) + 1536;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!parameters) {
        if (debug) { DeleteFileW(pdbDriver); DeleteFileW(debugLocation); }
        free_python_launch_buffers(directory, debugLocation, pdbDriver, quotedPython,
                                    quotedScript, quotedDebugLocation, quotedPdbDriver);
        return;
    }

    int written;
    int filesHandedOff = 0;
    if (debug) {
        written = swprintf(parameters, capacity,
            L"-NoLogo -NoProfile -NoExit -Command \"Write-Host "
            L"'pdb: F10/n next, Shift+F11/s step, F6/c continue; vars, globals, b <line>, p <expr>, where, q'; "
            L"& %ls%ls '-c' 'import runpy,sys; "
            L"runpy.run_path(sys.argv[1],run_name=\"markedit_pdb_driver\")' %ls; "
            L"try { [System.IO.File]::Delete(%ls) } catch {}; "
            L"try { [System.IO.File]::Delete(%ls) } catch {}\"",
            quotedPython, usePython3Launcher ? L" -3" : L"", quotedPdbDriver,
            quotedPdbDriver, quotedDebugLocation);
    } else {
        written = swprintf(parameters, capacity,
            L"-NoLogo -NoProfile -NoExit -Command \"& %ls%ls %ls; Write-Host ''; "
            L"Write-Host ('Process exited with code ' + $LASTEXITCODE)\"",
            quotedPython, usePython3Launcher ? L" -3" : L"", quotedScript);
    }
    if (written < 0) goto python_launch_cleanup;

    WCHAR *powershell = locate_powershell();
    const WCHAR *executable = powershell ? powershell : L"powershell.exe";
    SHELLEXECUTEINFOW execute = {0};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.hwnd = g_hMainWnd;
    execute.lpVerb = L"open";
    execute.lpFile = executable;
    execute.lpParameters = parameters;
    execute.lpDirectory = directory;
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        MessageBoxW(g_hMainWnd, L"The Python console could not be started.",
                    debug ? L"Debug Python" : L"Run Python", MB_OK | MB_ICONERROR);
    } else {
        if (debug) {
            filesHandedOff = 1;
            start_debug_location_tracking(debugLocation, directory, 1, execute.hProcess);
        } else if (execute.hProcess) {
            CloseHandle(execute.hProcess);
        }
    }
    free(powershell);

python_launch_cleanup:
    if (debug && !filesHandedOff) {
        DeleteFileW(pdbDriver);
        DeleteFileW(debugLocation);
    }
    free(parameters);
    free_python_launch_buffers(directory, debugLocation, pdbDriver, quotedPython,
                                quotedScript, quotedDebugLocation, quotedPdbDriver);
}

/* Lua is Run-only: there's no bundled debugger the way GDB and pdb back
   C/C++ and Python (a standalone Lua debugger isn't something you can just
   find on PATH the way those are), so this only ever runs the script. */
static WCHAR *find_lua_interpreter(void) {
    static const WCHAR *names[] = { L"lua.exe", L"lua5.4.exe", L"lua54.exe",
                                     L"lua5.3.exe", L"lua53.exe", L"lua5.1.exe", L"luajit.exe" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        WCHAR *lua = executable_on_search_path(names[i]);
        if (lua) return lua;
    }
    return NULL;
}

static void launch_lua_file(void) {
    WCHAR *lua = find_lua_interpreter();
    if (!lua) {
        MessageBoxW(g_hMainWnd,
                    L"No Lua interpreter was found. Install Lua (or LuaJIT) and add it to PATH.",
                    L"Run Lua", MB_OK | MB_ICONERROR);
        return;
    }
    WCHAR *directory = path_directory(g_currentFile);
    WCHAR *quotedLua = powershell_literal(lua);
    WCHAR *quotedScript = powershell_literal(g_currentFile);
    free(lua);
    if (!directory || !quotedLua || !quotedScript) {
        free(directory); free(quotedLua); free(quotedScript);
        return;
    }

    size_t capacity = wcslen(quotedLua) + wcslen(quotedScript) + 256;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!parameters) {
        free(directory); free(quotedLua); free(quotedScript);
        return;
    }
    int written = swprintf(parameters, capacity,
        L"-NoLogo -NoProfile -NoExit -Command \"& %ls %ls; Write-Host ''; "
        L"Write-Host ('Process exited with code ' + $LASTEXITCODE)\"",
        quotedLua, quotedScript);
    if (written < 0) {
        free(parameters); free(directory); free(quotedLua); free(quotedScript);
        return;
    }

    WCHAR *powershell = locate_powershell();
    const WCHAR *executable = powershell ? powershell : L"powershell.exe";
    SHELLEXECUTEINFOW execute = {0};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.hwnd = g_hMainWnd;
    execute.lpVerb = L"open";
    execute.lpFile = executable;
    execute.lpParameters = parameters;
    execute.lpDirectory = directory;
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        MessageBoxW(g_hMainWnd, L"The Lua console could not be started.",
                    L"Run Lua", MB_OK | MB_ICONERROR);
    } else if (execute.hProcess) {
        CloseHandle(execute.hProcess);
    }
    free(powershell);
    free(parameters);
    free(directory);
    free(quotedLua);
    free(quotedScript);
}

typedef enum {
    NATIVE_COMPILER_GCC = 0, NATIVE_COMPILER_CLANG, NATIVE_COMPILER_ZIG, NATIVE_COMPILER_MSVC
} NativeCompilerKind;

/* MSVC has no single "cl.exe" to look up on PATH the way GCC/Clang/Zig do:
   its compiler only works once vcvarsall.bat has set up INCLUDE/LIB/PATH for
   a specific architecture, and it isn't installed on PATH by default. Rather
   than shelling out to vswhere.exe (adds a process-capture dependency for a
   value that rarely changes), check the standard install layout directly.
   Visual Studio itself always lands under the 32-bit Program Files directory
   even on 64-bit Windows; standalone Build Tools installs sometimes use the
   64-bit one, so both are checked. */
static WCHAR *find_vcvarsall(void) {
    if (g_vcvarsallOverridePath) return wcsdup_heap(g_vcvarsallOverridePath);
    static const WCHAR *years[] = { L"2022", L"2019" };
    static const WCHAR *editions[] = { L"Enterprise", L"Professional", L"Community", L"BuildTools" };
    static const WCHAR *envNames[] = { L"ProgramFiles(x86)", L"ProgramFiles" };
    for (int e = 0; e < 2; e++) {
        WCHAR programFiles[MAX_PATH];
        if (!GetEnvironmentVariableW(envNames[e], programFiles, MAX_PATH)) continue;
        for (int y = 0; y < (int)(sizeof(years) / sizeof(years[0])); y++) {
            for (int ed = 0; ed < (int)(sizeof(editions) / sizeof(editions[0])); ed++) {
                WCHAR path[MAX_PATH * 2];
                swprintf(path, MAX_PATH * 2,
                    L"%ls\\Microsoft Visual Studio\\%ls\\%ls\\VC\\Auxiliary\\Build\\vcvarsall.bat",
                    programFiles, years[y], editions[ed]);
                if (PathFileExistsW(path)) return wcsdup_heap(path);
            }
        }
    }
    return NULL;
}

/* g_compilerBackend (see load_compiler_settings()) either pins the search to
   one compiler or leaves it Auto, trying GCC, Clang, then Zig in that order
   and falling back to MSVC only for Run (never Debug: there's no GDB-
   readable debug info from cl.exe, so Debug requires a GCC- or Clang-style
   compiler same as before this setting existed). */
static WCHAR *find_native_compiler(DebugSourceKind kind, int debug, NativeCompilerKind *compilerKind) {
    static const WCHAR *cNames[]   = { L"gcc.exe", L"clang.exe", L"zig.exe" };
    static const WCHAR *cppNames[] = { L"g++.exe", L"clang++.exe", L"zig.exe" };
    static const NativeCompilerKind kinds[] = { NATIVE_COMPILER_GCC, NATIVE_COMPILER_CLANG, NATIVE_COMPILER_ZIG };
    const WCHAR **names = kind == DEBUG_SOURCE_C ? cNames : cppNames;

    if (compilerKind) *compilerKind = NATIVE_COMPILER_GCC;

    if (g_compilerBackend == COMPILER_BACKEND_MSVC) {
        if (debug) return NULL;
        WCHAR *vcvarsall = find_vcvarsall();
        if (vcvarsall && compilerKind) *compilerKind = NATIVE_COMPILER_MSVC;
        return vcvarsall;
    }

    for (int i = 0; i < 3; i++) {
        if (debug && kinds[i] == NATIVE_COMPILER_ZIG) continue;
        if (g_compilerBackend != COMPILER_BACKEND_AUTO) {
            CompilerBackendPreference want =
                kinds[i] == NATIVE_COMPILER_GCC ? COMPILER_BACKEND_GCC :
                kinds[i] == NATIVE_COMPILER_CLANG ? COMPILER_BACKEND_CLANG : COMPILER_BACKEND_ZIG;
            if (g_compilerBackend != want) continue;
        }
        WCHAR *compiler = executable_on_search_path(names[i]);
        if (compiler) {
            if (compilerKind) *compilerKind = kinds[i];
            return compiler;
        }
    }

    if (g_compilerBackend == COMPILER_BACKEND_AUTO && !debug) {
        WCHAR *vcvarsall = find_vcvarsall();
        if (vcvarsall) {
            if (compilerKind) *compilerKind = NATIVE_COMPILER_MSVC;
            return vcvarsall;
        }
    }
    return NULL;
}

static WCHAR *native_temporary_executable(void) {
    return debug_temporary_path(L"exe");
}

static WCHAR *native_temporary_gdb_commands(void) {
    return debug_temporary_path(L"gdb");
}

static WCHAR *gdb_source_path(const WCHAR *path) {
    WCHAR *copy = wcsdup_heap(path);
    if (!copy) return NULL;
    for (WCHAR *character = copy; *character; character++)
        if (*character == L'\\') *character = L'/';
    return copy;
}

/* Multi-line GDB commands such as `define hook-stop` must come from a command
   file. Supplying each line through a separate -ex leaves GDB waiting at its
   interactive definition prompt. The hook makes every next/step/continue stop
   show all arguments and in-scope local variables without another command. */
static int write_gdb_commands(const WCHAR *path, const WCHAR *sourcePath, const WCHAR *locationPath,
                              const int *breakpointLines, int breakpointCount) {
    size_t capacity = wcslen(locationPath) + 1024 +
                      (size_t)breakpointCount * (wcslen(sourcePath) + 64);
    WCHAR *commands = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!commands) return 0;
    int written = swprintf(commands, capacity,
        L"set pagination off\n"
        L"set breakpoint pending on\n"
        L"set print pretty on\n"
        L"set print object on\n"
        L"set logging file %ls\n"
        L"set logging overwrite on\n"
        L"set logging redirect on\n"
        L"define vars\n"
        L"  echo \\nArguments:\\n\n"
        L"  info args\n"
        L"  echo Locals:\\n\n"
        L"  info locals\n"
        L"end\n"
        L"define hook-stop\n"
        L"  set logging enabled on\n"
        L"  frame\n"
        L"  set logging enabled off\n"
        L"  vars\n"
        L"end\n", locationPath);
    if (written < 0) {
        free(commands);
        return 0;
    }
    size_t used = (size_t)written;
    for (int i = 0; i < breakpointCount; i++) {
        written = swprintf(commands + used, capacity - used,
                           L"break %ls:%d\n", sourcePath, breakpointLines[i] + 1);
        if (written < 0) {
            free(commands);
            return 0;
        }
        used += (size_t)written;
    }
    written = swprintf(commands + used, capacity - used,
                       breakpointCount ? L"run\n" : L"start\n");
    if (written < 0) {
        free(commands);
        return 0;
    }
    char *utf8 = to_utf8(commands);
    free(commands);
    if (!utf8) return 0;
    int result = write_bytes_file(path, utf8, strlen(utf8));
    free(utf8);
    return result;
}

/* MSVC needs its own launch path rather than reusing the PowerShell one
   below: cl.exe only works once vcvarsall.bat has set up INCLUDE/LIB/PATH
   for a target architecture, and a batch file's environment changes don't
   propagate out to a parent PowerShell session the way they would to a
   further command chained in the same cmd.exe process. So the whole
   build+run+cleanup chain runs as a single cmd.exe command instead. */
static void launch_msvc_file(DebugSourceKind kind, WCHAR *vcvarsallPath) {
    WCHAR *output = native_temporary_executable();
    WCHAR *directory = path_directory(g_currentFile);
    if (!output || !directory) {
        free(vcvarsallPath); free(output); free(directory);
        return;
    }

    const WCHAR *std = kind == DEBUG_SOURCE_C ? L"/std:c11" : L"/std:c++17";
    size_t capacity = wcslen(vcvarsallPath) + wcslen(output) * 3 + wcslen(g_currentFile) + 512;
    WCHAR *command = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!command) {
        free(vcvarsallPath); free(output); free(directory);
        return;
    }
    /* The leading/trailing quote wrapping the whole /k argument is cmd.exe's
       documented workaround for a command line that itself contains further
       quoted substrings (the VS install path, source, and output paths can
       all contain spaces): when the switch's remainder starts and ends with
       a quote character, cmd strips only that outer pair and leaves
       everything between - including each inner "..." - untouched, rather
       than mangling it with its ad-hoc first-quote/last-quote stripping
       rule. enabledelayedexpansion + !errorlevel! (rather than %errorlevel%)
       is needed because cmd expands %-variables once, when the whole /k
       line is parsed, before any of the chained commands actually run - so
       plain %errorlevel% would show a stale value from before the build. */
    int written = swprintf(command, capacity,
        L"/k \"setlocal enabledelayedexpansion && call \"%ls\" x64 >nul && "
        L"echo Building %ls... && "
        L"cl.exe /nologo %ls /O2 /EHsc /Fe:\"%ls\" \"%ls\" && "
        L"(\"%ls\" & echo. & echo Process exited with code !errorlevel!) & "
        L"del /f /q \"%ls\" >nul 2>nul\"",
        vcvarsallPath, kind == DEBUG_SOURCE_C ? L"C" : L"C++", std, output, g_currentFile,
        output, output);
    free(vcvarsallPath);
    if (written < 0) {
        free(command); free(output); free(directory);
        return;
    }

    SHELLEXECUTEINFOW execute = {0};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.hwnd = g_hMainWnd;
    execute.lpVerb = L"open";
    execute.lpFile = L"cmd.exe";
    execute.lpParameters = command;
    execute.lpDirectory = directory;
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        MessageBoxW(g_hMainWnd, L"The C/C++ build console could not be started.",
                    L"Run C/C++", MB_OK | MB_ICONERROR);
    } else if (execute.hProcess) {
        CloseHandle(execute.hProcess);
    }
    free(command);
    free(output);
    free(directory);
}

static void launch_native_file(DebugSourceKind kind, int debug) {
    NativeCompilerKind compilerKind = NATIVE_COMPILER_GCC;
    WCHAR *compiler = find_native_compiler(kind, debug, &compilerKind);
    if (compiler && compilerKind == NATIVE_COMPILER_MSVC) {
        launch_msvc_file(kind, compiler);
        return;
    }
    WCHAR *debugger = debug ? executable_on_search_path(L"gdb.exe") : NULL;
    if (!compiler) {
        MessageBoxW(g_hMainWnd,
                    debug
                        ? L"No GDB-compatible C/C++ compiler was found. Install GCC/G++ or Clang and add it to PATH."
                        : L"No C/C++ compiler was found. Install GCC, Clang, Zig, or Visual Studio "
                          L"(Desktop development with C++ workload) and add it to PATH.",
                    debug ? L"Debug C/C++" : L"Run C/C++", MB_OK | MB_ICONERROR);
        return;
    }
    if (debug && !debugger) {
        free(compiler);
        MessageBoxW(g_hMainWnd,
                    L"GDB was not found on PATH. Install a Windows GDB toolchain to step through C/C++ code.",
                    L"Debug C/C++", MB_OK | MB_ICONERROR);
        return;
    }

    WCHAR *output = native_temporary_executable();
    WCHAR *gdbCommands = debug ? native_temporary_gdb_commands() : NULL;
    WCHAR *debugLocation = debug ? debug_temporary_path(L"location") : NULL;
    WCHAR *directory = path_directory(g_currentFile);
    WCHAR *sourceForGdb = gdb_source_path(g_currentFile);
    WCHAR *locationForGdb = debugLocation ? gdb_source_path(debugLocation) : NULL;
    WCHAR *quotedCompiler = powershell_literal(compiler);
    WCHAR *quotedDebugger = debugger ? powershell_literal(debugger) : NULL;
    WCHAR *quotedSource = powershell_literal(g_currentFile);
    WCHAR *quotedOutput = output ? powershell_literal(output) : NULL;
    WCHAR *quotedGdbCommands = gdbCommands ? powershell_literal(gdbCommands) : NULL;
    WCHAR *quotedDebugLocation = debugLocation ? powershell_literal(debugLocation) : NULL;
    free(compiler);
    free(debugger);
    if (!output || (debug && (!gdbCommands || !debugLocation || !locationForGdb)) ||
        !directory || !sourceForGdb || !quotedCompiler ||
        (debug && (!quotedDebugger || !quotedGdbCommands || !quotedDebugLocation)) ||
        !quotedSource || !quotedOutput) {
        free(output); free(directory); free(sourceForGdb); free(quotedCompiler);
        free(gdbCommands); free(debugLocation); free(locationForGdb);
        free(quotedGdbCommands); free(quotedDebugLocation); free(quotedDebugger);
        free(quotedSource); free(quotedOutput);
        return;
    }

    int commandFileHandedOff = 0;
    int *breakpointLines = NULL;
    int breakpointCount = debug ? collect_breakpoint_lines(&breakpointLines) : 0;
    if (debug) {
        int wroteCommands = write_gdb_commands(gdbCommands, sourceForGdb, locationForGdb,
                                                breakpointLines, breakpointCount);
        free(breakpointLines);
        if (!wroteCommands || !write_bytes_file(debugLocation, "starting\n", 9))
            goto native_launch_cleanup;
    }
    size_t capacity = wcslen(quotedCompiler) + wcslen(quotedSource) +
                      wcslen(quotedOutput) + (quotedDebugger ? wcslen(quotedDebugger) : 0) +
                      (quotedGdbCommands ? wcslen(quotedGdbCommands) : 0) +
                      (quotedDebugLocation ? wcslen(quotedDebugLocation) : 0) + 2048;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!parameters) goto native_launch_cleanup;

    const WCHAR *compilerMode = compilerKind == NATIVE_COMPILER_ZIG
        ? (kind == DEBUG_SOURCE_C ? L" cc" : L" c++") : L"";
    const WCHAR *standard = kind == DEBUG_SOURCE_C ? L"-std=gnu11" : L"-std=gnu++17";
    const WCHAR *buildFlags = debug
        ? L"'-g' '-O0' '-fno-omit-frame-pointer'" : L"'-O2'";
    int written = swprintf(parameters, capacity,
        L"-NoLogo -NoProfile -NoExit -Command \"Write-Host 'Building %ls...'; "
        L"& %ls%ls '%ls' %ls '-o' %ls %ls; "
        L"if ($LASTEXITCODE -eq 0) { ",
        kind == DEBUG_SOURCE_C ? L"C" : L"C++", quotedCompiler, compilerMode,
        standard, buildFlags, quotedOutput, quotedSource);
    if (written < 0) goto native_parameters_cleanup;
    size_t used = (size_t)written;

    if (debug) {
        written = swprintf(parameters + used, capacity - used,
            L"Write-Host 'gdb: next/n, step/s, continue/c, vars, print <expr>, bt, watch <expr>, quit'; "
            L"Write-Host 'Arguments and locals are shown automatically whenever execution stops.'; "
            L"& %ls '-q' '-x' %ls %ls; ",
            quotedDebugger, quotedGdbCommands, quotedOutput);
        if (written < 0) goto native_parameters_cleanup;
        used += (size_t)written;
    } else {
        written = swprintf(parameters + used, capacity - used,
            L"& %ls; $markeditExit = $LASTEXITCODE; Write-Host ''; "
            L"Write-Host ('Process exited with code ' + $markeditExit); ", quotedOutput);
    }
    if (written < 0) goto native_parameters_cleanup;
    used += (size_t)written;
    /* Deleting the just-executed binary can transiently fail with "Access is
       denied" (Windows Defender's on-execute scan holds a brief lock on a
       freshly run .exe); a bare Delete() call would surface that as a scary
       PowerShell error right after the program's own output, even though the
       run itself succeeded. try/catch makes the best-effort cleanup silent. */
    written = swprintf(parameters + used, capacity - used,
        debug
            ? L"}; try { [System.IO.File]::Delete(%ls) } catch {}; "
              L"try { [System.IO.File]::Delete(%ls) } catch {}; "
              L"try { [System.IO.File]::Delete(%ls) } catch {}\""
            : L"}; try { [System.IO.File]::Delete(%ls) } catch {}\"",
        quotedOutput, quotedGdbCommands, quotedDebugLocation);
    if (written < 0) goto native_parameters_cleanup;

    WCHAR *powershell = locate_powershell();
    const WCHAR *shell = powershell ? powershell : L"powershell.exe";
    SHELLEXECUTEINFOW execute = {0};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.hwnd = g_hMainWnd;
    execute.lpVerb = L"open";
    execute.lpFile = shell;
    execute.lpParameters = parameters;
    execute.lpDirectory = directory;
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        MessageBoxW(g_hMainWnd, L"The C/C++ build console could not be started.",
                    debug ? L"Debug C/C++" : L"Run C/C++", MB_OK | MB_ICONERROR);
    } else {
        if (debug) {
            commandFileHandedOff = 1;
            start_debug_location_tracking(debugLocation, directory, 2, execute.hProcess);
        } else if (execute.hProcess) {
            CloseHandle(execute.hProcess);
        }
    }
    free(powershell);

native_parameters_cleanup:
    free(parameters);
native_launch_cleanup:
    if (!commandFileHandedOff) {
        if (gdbCommands) DeleteFileW(gdbCommands);
        if (debugLocation) DeleteFileW(debugLocation);
    }
    free(output);
    free(gdbCommands);
    free(debugLocation);
    free(locationForGdb);
    free(quotedGdbCommands);
    free(quotedDebugLocation);
    free(directory);
    free(sourceForGdb);
    free(quotedCompiler);
    free(quotedDebugger);
    free(quotedSource);
    free(quotedOutput);
}

static void launch_active_source_file(int debug) {
    if (!g_currentFile) {
        do_save();
        if (!g_currentFile) return;
    } else if (g_dirty && !save_to_path(g_currentFile)) {
        return;
    }
    DebugSourceKind kind = debug_source_kind(g_currentFile);
    if (kind == DEBUG_SOURCE_PYTHON) {
        launch_python_file(debug);
    } else if (kind == DEBUG_SOURCE_C || kind == DEBUG_SOURCE_CPP) {
        launch_native_file(kind, debug);
    } else if (kind == DEBUG_SOURCE_LUA) {
        if (debug) {
            MessageBoxW(g_hMainWnd, L"Lua files support Run only; there is no Lua debugger integration.",
                        L"Debug File", MB_OK | MB_ICONINFORMATION);
        } else {
            launch_lua_file();
        }
    } else {
        MessageBoxW(g_hMainWnd,
                    L"Run supports .py, .pyw, .c, .cc, .cpp, .cxx, and .lua files. "
                    L"Debug supports all of those except .lua.",
                    debug ? L"Debug File" : L"Run File", MB_OK | MB_ICONINFORMATION);
    }
}

/* Strip trailing spaces and tabs from every line, as one undo action. Lines
   are rewritten back to front so earlier line positions stay valid. */
static void trim_trailing_whitespace(void) {
    int lineCount = (int)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
    int caret = (int)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    int trimmed = 0;

    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
    for (int line = lineCount - 1; line >= 0; line--) {
        int lineStart = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
        int lineEnd = (int)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)line, 0);
        int cut = lineEnd;
        while (cut > lineStart) {
            char ch = (char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)(cut - 1), 0);
            if (ch != ' ' && ch != '\t') break;
            cut--;
        }
        if (cut == lineEnd) continue;

        SendMessageA(g_hEdit, SCI_SETTARGETRANGE, (WPARAM)cut, (LPARAM)lineEnd);
        SendMessageA(g_hEdit, SCI_REPLACETARGET, 0, (LPARAM)"");
        if (caret > lineEnd) caret -= lineEnd - cut;
        else if (caret > cut) caret = cut;
        trimmed = 1;
    }
    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);

    if (trimmed) SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)caret, (LPARAM)caret);
    SetFocus(g_hEdit);
}

/* True when `line` (length lineLen) is exactly an HTML comment delimiter,
   ignoring surrounding whitespace. */
static int line_is_comment_delimiter(const char *line, int lineLen, const char *delimiter) {
    int i = 0, end = lineLen;
    while (i < end && (line[i] == ' ' || line[i] == '\t')) i++;
    while (end > i && (line[end - 1] == ' ' || line[end - 1] == '\t')) end--;
    int delimiterLen = (int)strlen(delimiter);
    return end - i == delimiterLen && memcmp(line + i, delimiter, (size_t)delimiterLen) == 0;
}

/* Markdown has no line-comment syntax, so the selected line range is wrapped
   in an HTML comment on its own lines - and unwrapped when already exactly
   that. */
static void toggle_comment(void) {
    int firstLine, lastLine;
    toolbar_selected_line_range(&firstLine, &lastLine);

    int length;
    char *text = get_edit_text(g_hEdit, &length);
    if (!text) return;
    int blockStart = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)firstLine, 0);
    int blockEnd = (int)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)lastLine, 0);

    int openEnd = (int)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)firstLine, 0);
    int closeStart = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)lastLine, 0);
    if (lastLine - firstLine >= 1 &&
        line_is_comment_delimiter(text + blockStart, openEnd - blockStart, "<!--") &&
        line_is_comment_delimiter(text + closeStart, blockEnd - closeStart, "-->")) {
        int innerStart = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)(firstLine + 1), 0);
        /* lastLine == firstLine + 1 means the delimiters are on adjacent
           lines with nothing between them - the inner content is empty
           rather than computed from a line range (which would go negative). */
        int innerLength = lastLine - firstLine >= 2
            ? (int)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)(lastLine - 1), 0) - innerStart
            : 0;
        char *inner = (char *)malloc((size_t)innerLength + 1);
        if (inner) {
            memcpy(inner, text + innerStart, (size_t)innerLength);
            inner[innerLength] = 0;
            SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
            SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)blockStart, (LPARAM)blockEnd);
            SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)inner);
            SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)blockStart, (LPARAM)(blockStart + innerLength));
            SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
            free(inner);
        }
        free(text);
        SetFocus(g_hEdit);
        return;
    }

    const char *eol = editor_eol_sequence();
    size_t eolLen = strlen(eol);
    size_t bodyLen = (size_t)(blockEnd - blockStart);
    size_t openLen = 4 + eolLen;
    size_t wrappedLen = openLen + bodyLen + eolLen + 3;
    char *wrapped = (char *)malloc(wrappedLen + 1);
    if (wrapped) {
        memcpy(wrapped, "<!--", 4);
        memcpy(wrapped + 4, eol, eolLen);
        memcpy(wrapped + openLen, text + blockStart, bodyLen);
        memcpy(wrapped + openLen + bodyLen, eol, eolLen);
        memcpy(wrapped + openLen + bodyLen + eolLen, "-->", 3);
        wrapped[wrappedLen] = 0;
        SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)blockStart, (LPARAM)blockEnd);
        SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)wrapped);
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)blockStart, (LPARAM)(blockStart + (int)wrappedLen));
        SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
        free(wrapped);
    }
    free(text);
    SetFocus(g_hEdit);
}

/* True when the line begins a fenced code block delimiter (``` or ~~~,
   after up to three leading spaces as CommonMark allows). */
static int line_starts_fence(int line) {
    int lineStart = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
    int lineEnd = (int)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)line, 0);
    int i = lineStart;
    while (i < lineEnd && i - lineStart < 3 &&
           (char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)i, 0) == ' ') i++;
    char first = (char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)i, 0);
    if (first != '`' && first != '~') return 0;

    int run = 0;
    while (i + run < lineEnd &&
           (char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)(i + run), 0) == first) run++;
    return run >= 3;
}

/* ---- JSON reformatting for the fenced block at the caret ---- */

/* Finds the fenced block containing `line`, returning the source range between
   its delimiters. Returns 0 when the caret is not inside a fence. */
static int fence_body_range(int line, int *bodyStart, int *bodyEnd) {
    int lineCount = (int)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
    int openLine = -1, closeLine = -1;

    for (int scan = line; scan >= 0; scan--) {
        if (!line_starts_fence(scan)) continue;
        /* A fence delimiter on the caret's own line is the opener only if the
           caret sits on the opening one; scanning up finds that either way. */
        openLine = scan;
        break;
    }
    if (openLine < 0) return 0;

    for (int scan = openLine + 1; scan < lineCount; scan++) {
        if (line_starts_fence(scan)) { closeLine = scan; break; }
    }
    if (closeLine < 0 || line > closeLine) return 0;

    *bodyStart = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)(openLine + 1), 0);
    *bodyEnd = (int)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)(closeLine - 1), 0);
    return closeLine > openLine + 1;
}

/* Reformats the selection, or the fenced block at the caret when there is no
   selection. Leaves the document untouched if the text isn't valid JSON. */
static void format_json(int indent) {
    int start = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int end = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);

    if (start == end) {
        int caretLine = (int)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)start, 0);
        if (!fence_body_range(caretLine, &start, &end)) {
            MessageBoxW(g_hMainWnd,
                        L"Place the caret inside a fenced code block, or select the JSON to format.",
                        L"MarkEdit", MB_OK | MB_ICONINFORMATION);
            return;
        }
    }

    int length;
    char *text = get_edit_text(g_hEdit, &length);
    if (!text) return;

    char *formatted = json_reformat(text + start, end - start, editor_eol_sequence(), indent);
    free(text);
    if (!formatted) {
        MessageBoxW(g_hMainWnd, L"That text could not be read as JSON.",
                    L"MarkEdit", MB_OK | MB_ICONWARNING);
        return;
    }

    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)start, (LPARAM)end);
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)formatted);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)start, (LPARAM)(start + (int)strlen(formatted)));
    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);

    free(formatted);
    SetFocus(g_hEdit);
}

/* ---- document outline -------------------------------------------------- */

static int parse_atx_heading(const char *line, int length, int *textStart, int *textLength) {
    int i = 0;
    while (i < length && i < 3 && line[i] == ' ') i++;
    int hashes = 0;
    while (i + hashes < length && hashes < 6 && line[i + hashes] == '#') hashes++;
    if (!hashes || i + hashes >= length ||
        (line[i + hashes] != ' ' && line[i + hashes] != '\t')) return 0;
    int start = i + hashes;
    while (start < length && (line[start] == ' ' || line[start] == '\t')) start++;
    int end = length;
    while (end > start && (line[end - 1] == '\r' || line[end - 1] == '\n' ||
                            line[end - 1] == ' ' || line[end - 1] == '\t')) end--;
    int marker = end;
    while (marker > start && line[marker - 1] == '#') marker--;
    if (marker < end && marker > start && (line[marker - 1] == ' ' || line[marker - 1] == '\t')) {
        end = marker - 1;
        while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) end--;
    }
    *textStart = start;
    *textLength = end - start;
    return hashes;
}

static void update_outline(void) {
    if (!g_hOutline) return;
    SendMessageW(g_hOutline, LB_RESETCONTENT, 0, 0);
    g_outlineCount = 0;
    if (g_activeLanguage != LANG_MARKDOWN) return;
    int lines = (int)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
    for (int lineNumber = 0; lineNumber < lines && g_outlineCount < 1024; lineNumber++) {
        int linePosition = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)lineNumber, 0);
        int style = (int)SendMessageA(g_hEdit, SCI_GETSTYLEAT, (WPARAM)linePosition, 0);
        if (style < SCE_MARKDOWN_HEADER1 || style > SCE_MARKDOWN_HEADER6) continue;
        int length = (int)SendMessageA(g_hEdit, SCI_LINELENGTH, (WPARAM)lineNumber, 0);
        if (length <= 0 || length > 65535) continue;
        char *line = (char *)malloc((size_t)length + 1);
        if (!line) break;
        int got = (int)SendMessageA(g_hEdit, SCI_GETLINE, (WPARAM)lineNumber, (LPARAM)line);
        line[got >= 0 && got <= length ? got : length] = 0;
        int start = 0, textLength = 0;
        int level = parse_atx_heading(line, got, &start, &textLength);
        if (level && textLength > 0) {
            char *heading = (char *)malloc((size_t)textLength + 1);
            if (heading) {
                memcpy(heading, line + start, (size_t)textLength);
                heading[textLength] = 0;
                WCHAR *wide = to_wide(heading);
                if (wide) {
                    size_t capacity = wcslen(wide) + (size_t)level * 2 + 1;
                    WCHAR *label = (WCHAR *)malloc(capacity * sizeof(WCHAR));
                    if (label) {
                        int indent = (level - 1) * 2;
                        for (int i = 0; i < indent; i++) label[i] = L' ';
                        wcscpy(label + indent, wide);
                        SendMessageW(g_hOutline, LB_ADDSTRING, 0, (LPARAM)label);
                        g_outlineLines[g_outlineCount++] = lineNumber;
                        free(label);
                    }
                    free(wide);
                }
                free(heading);
            }
        }
        free(line);
    }
}

static void toggle_outline(HWND hwnd) {
    g_outlineVisible = !g_outlineVisible;
    update_outline();
    update_view_menu(hwnd);
    layout(hwnd);
    save_layout_settings();
}

static char *editor_line_text(int lineNumber, int *outLength) {
    int length = (int)SendMessageA(g_hEdit, SCI_LINELENGTH, (WPARAM)lineNumber, 0);
    if (length < 0 || length > 1024 * 1024) return NULL;
    char *line = (char *)malloc((size_t)length + 1);
    if (!line) return NULL;
    int got = (int)SendMessageA(g_hEdit, SCI_GETLINE, (WPARAM)lineNumber, (LPARAM)line);
    if (got < 0) got = 0;
    if (got > length) got = length;
    line[got] = 0;
    while (got > 0 && (line[got - 1] == '\r' || line[got - 1] == '\n')) line[--got] = 0;
    if (outLength) *outLength = got;
    return line;
}

static int table_row_cells(const char *line, char cells[][256], int maximum) {
    int count = 0;
    const char *cursor = line;
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (*cursor == '|') cursor++;
    while (*cursor && count < maximum) {
        const char *end = strchr(cursor, '|');
        if (!end) end = cursor + strlen(cursor);
        const char *start = cursor;
        while (start < end && (*start == ' ' || *start == '\t')) start++;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        size_t length = (size_t)(end - start);
        if (length > 255) length = 255;
        memcpy(cells[count], start, length);
        cells[count][length] = 0;
        count++;
        if (!*end) break;
        cursor = end + 1;
        if (!*cursor) break;
    }
    return count;
}

static int append_dynamic(char **buffer, size_t *length, size_t *capacity, const char *text) {
    size_t add = strlen(text);
    if (*length + add + 1 > *capacity) {
        size_t next = *capacity ? *capacity : 512;
        while (next < *length + add + 1) next *= 2;
        char *grown = (char *)realloc(*buffer, next);
        if (!grown) return 0;
        *buffer = grown; *capacity = next;
    }
    memcpy(*buffer + *length, text, add + 1);
    *length += add;
    return 1;
}

static void show_table_editor(void) {
    int caret = (int)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    int caretLine = (int)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)caret, 0);
    int lineCount = (int)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
    int first = caretLine, last = caretLine, existing = 0;
    int lineLength = 0;
    char *line = editor_line_text(caretLine, &lineLength);
    if (line && strchr(line, '|')) {
        while (first > 0) {
            char *previous = editor_line_text(first - 1, NULL);
            int belongs = previous && strchr(previous, '|');
            free(previous);
            if (!belongs) break;
            first--;
        }
        while (last + 1 < lineCount) {
            char *next = editor_line_text(last + 1, NULL);
            int belongs = next && strchr(next, '|');
            free(next);
            if (!belongs) break;
            last++;
        }
        if (last - first >= 1) existing = 1;
    }
    free(line);

    char cells[51][20][256] = {{{0}}};
    int existingRows = 0, columns = 3;
    if (existing) {
        char *header = editor_line_text(first, NULL);
        if (header) { columns = table_row_cells(header, cells[0], 20); free(header); }
        if (columns < 1) columns = 1;
        existingRows = 1;
        for (int sourceLine = first + 2; sourceLine <= last && existingRows < 51; sourceLine++) {
            char *row = editor_line_text(sourceLine, NULL);
            if (row) { table_row_cells(row, cells[existingRows], columns); free(row); }
            existingRows++;
        }
    } else {
        existingRows = 4; /* header plus three data rows */
    }

    WCHAR value[32], rowsText[32], columnsText[32];
    swprintf(value, sizeof(value) / sizeof(value[0]), L"%d", existingRows > 0 ? existingRows - 1 : 3);
    if (!show_text_input(L"Table Editor", L"Data rows (excluding the header):", value,
                         rowsText, sizeof(rowsText) / sizeof(rowsText[0]))) return;
    swprintf(value, sizeof(value) / sizeof(value[0]), L"%d", columns);
    if (!show_text_input(L"Table Editor", L"Columns:", value,
                         columnsText, sizeof(columnsText) / sizeof(columnsText[0]))) return;
    int dataRows = (int)wcstol(rowsText, NULL, 10);
    int requestedColumns = (int)wcstol(columnsText, NULL, 10);
    if (dataRows < 1 || dataRows > 50 || requestedColumns < 1 || requestedColumns > 20) {
        MessageBoxW(g_hMainWnd, L"Use 1–50 data rows and 1–20 columns.", L"Table Editor",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    columns = requestedColumns;
    const char *eol = editor_eol_sequence();
    char *table = NULL; size_t length = 0, capacity = 0;
    for (int row = 0; row < dataRows + 2; row++) {
        if (!append_dynamic(&table, &length, &capacity, "|")) break;
        for (int column = 0; column < columns; column++) {
            char fallback[64]; const char *cell = "";
            if (row == 0) {
                if (existing && column < 20 && cells[0][column][0]) cell = cells[0][column];
                else { snprintf(fallback, sizeof(fallback), "Header %d", column + 1); cell = fallback; }
            } else if (row == 1) cell = "---";
            else if (existing && row - 1 < existingRows && column < 20) cell = cells[row - 1][column];
            if (!append_dynamic(&table, &length, &capacity, " ") ||
                !append_dynamic(&table, &length, &capacity, cell) ||
                !append_dynamic(&table, &length, &capacity, " |")) break;
        }
        append_dynamic(&table, &length, &capacity, eol);
    }
    if (!table) return;
    int replaceStart, replaceEnd;
    if (existing) {
        replaceStart = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)first, 0);
        replaceEnd = last + 1 < lineCount
            ? (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)(last + 1), 0)
            : (int)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    } else {
        replaceStart = replaceEnd = caret;
        if (position_needs_leading_eol(caret)) {
            size_t eolLength = strlen(eol);
            char *withLeading = (char *)malloc(length + eolLength + 1);
            if (withLeading) {
                memcpy(withLeading, eol, eolLength);
                memcpy(withLeading + eolLength, table, length + 1);
                free(table); table = withLeading; length += eolLength;
            }
        }
    }
    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)replaceStart, 0);
    SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)replaceEnd, 0);
    SendMessageA(g_hEdit, SCI_REPLACETARGET, (WPARAM)length, (LPARAM)table);
    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
    free(table);
    SetFocus(g_hEdit);
}

/* ---- folder tools and command palette ---------------------------------- */

typedef struct WorkspaceFiles {
    WCHAR **paths;
    WCHAR **labels;
    int *lines;
    int count;
    int capacity;
    WCHAR *root;
} WorkspaceFiles;

static void free_workspace_files(WorkspaceFiles *files) {
    for (int i = 0; i < files->count; i++) {
        free(files->paths[i]);
        free(files->labels[i]);
    }
    free(files->paths); free(files->labels); free(files->lines); free(files->root);
    memset(files, 0, sizeof(*files));
}

static int add_workspace_item(WorkspaceFiles *files, const WCHAR *path,
                              const WCHAR *label, int line) {
    if (files->count >= files->capacity) {
        int next = files->capacity ? files->capacity * 2 : 128;
        WCHAR **paths = (WCHAR **)realloc(files->paths, (size_t)next * sizeof(WCHAR *));
        if (!paths) return 0;
        files->paths = paths;
        WCHAR **labels = (WCHAR **)realloc(files->labels, (size_t)next * sizeof(WCHAR *));
        if (!labels) return 0;
        files->labels = labels;
        int *lines = (int *)realloc(files->lines, (size_t)next * sizeof(int));
        if (!lines) return 0;
        files->lines = lines;
        files->capacity = next;
    }
    files->paths[files->count] = wcsdup_heap(path);
    files->labels[files->count] = wcsdup_heap(label);
    files->lines[files->count] = line;
    if (!files->paths[files->count] || !files->labels[files->count]) {
        free(files->paths[files->count]); free(files->labels[files->count]);
        return 0;
    }
    files->count++;
    return 1;
}

static WCHAR *choose_workspace_folder(void) {
    IFileOpenDialog *dialog = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IFileOpenDialog, (void **)&dialog)) || !dialog) return NULL;
    FILEOPENDIALOGOPTIONS options = 0;
    IFileOpenDialog_GetOptions(dialog, &options);
    IFileOpenDialog_SetOptions(dialog, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                               FOS_PATHMUSTEXIST);
    WCHAR *resultPath = NULL;
    if (SUCCEEDED(IFileOpenDialog_Show(dialog, g_hMainWnd))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(dialog, &item)) && item) {
            PWSTR selected = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &selected)) && selected) {
                resultPath = wcsdup_heap(selected);
                CoTaskMemFree(selected);
            }
            IShellItem_Release(item);
        }
    }
    IFileOpenDialog_Release(dialog);
    return resultPath;
}

static int workspace_file_extension(const WCHAR *name) {
    const WCHAR *extension = PathFindExtensionW(name);
    static const WCHAR *allowed[] = {
        L".md", L".markdown", L".mdown", L".mkd", L".txt", L".json", L".yaml", L".yml",
        L".html", L".htm", L".xml", L".c", L".h", L".cpp", L".hpp", L".js", L".ts",
        L".py", L".sh", L".bash", L".ps1"
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
        if (_wcsicmp(extension, allowed[i]) == 0) return 1;
    return 0;
}

static int skipped_workspace_directory(const WCHAR *name) {
    static const WCHAR *skipped[] = {
        L".git", L".svn", L".hg", L"node_modules", L"build", L"dist", L"Output",
        L"webview2_data", L".vs", L".vscode"
    };
    for (size_t i = 0; i < sizeof(skipped) / sizeof(skipped[0]); i++)
        if (_wcsicmp(name, skipped[i]) == 0) return 1;
    return 0;
}

static void enumerate_workspace_files(WorkspaceFiles *files, const WCHAR *directory, int depth) {
    if (depth > 24 || files->count >= 5000) return;
    WCHAR *pattern = path_join(directory, L"*");
    if (!pattern) return;
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(pattern, &data);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        WCHAR *full = path_join(directory, data.cFileName);
        if (!full) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                !skipped_workspace_directory(data.cFileName))
                enumerate_workspace_files(files, full, depth + 1);
        } else if (!(data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) &&
                   workspace_file_extension(data.cFileName)) {
            const WCHAR *relative = full;
            size_t rootLength = wcslen(files->root);
            if (_wcsnicmp(full, files->root, rootLength) == 0) {
                relative = full + rootLength;
                if (*relative == L'\\' || *relative == L'/') relative++;
            }
            add_workspace_item(files, full, relative, 0);
        }
        free(full);
    } while (files->count < 5000 && FindNextFileW(search, &data));
    FindClose(search);
}

static int collect_workspace_files(WorkspaceFiles *files) {
    memset(files, 0, sizeof(*files));
    files->root = choose_workspace_folder();
    if (!files->root) return 0;
    enumerate_workspace_files(files, files->root, 0);
    return 1;
}

static void do_quick_open(void) {
    WorkspaceFiles files;
    if (!collect_workspace_files(&files)) return;
    if (!files.count) {
        MessageBoxW(g_hMainWnd, L"No supported text files were found in that folder.",
                    L"Quick Open", MB_OK | MB_ICONINFORMATION);
        free_workspace_files(&files);
        return;
    }
    int selected = show_choice_dialog(L"Quick Open", L"Type part of a file name:",
                                      files.labels, files.count);
    if (selected >= 0 && selected < files.count) open_file_in_tab(files.paths[selected]);
    free_workspace_files(&files);
}

static void do_find_in_folder(void) {
    WorkspaceFiles files;
    if (!collect_workspace_files(&files)) return;
    WCHAR queryWide[256];
    if (!show_text_input(L"Find in Folder", L"Text to find (case-insensitive):", L"",
                         queryWide, sizeof(queryWide) / sizeof(queryWide[0])) || !queryWide[0]) {
        free_workspace_files(&files); return;
    }
    char *query = to_utf8(queryWide);
    if (!query) { free_workspace_files(&files); return; }
    WorkspaceFiles results = {0};
    results.root = wcsdup_heap(files.root);
    for (int i = 0; i < files.count && results.count < 2000; i++) {
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        if (!GetFileAttributesExW(files.paths[i], GetFileExInfoStandard, &attributes) ||
            attributes.nFileSizeHigh || attributes.nFileSizeLow > 4 * 1024 * 1024) continue;
        int length = 0;
        char *content = read_bytes_file(files.paths[i], &length);
        if (!content) continue;
        char *cursor = content;
        if (has_utf8_bom(cursor, length)) cursor += 3;
        int lineNumber = 1;
        while (*cursor && results.count < 2000) {
            char *end = strchr(cursor, '\n');
            if (end) *end = 0;
            if (StrStrIA(cursor, query)) {
                char snippet[181];
                size_t snippetLength = strlen(cursor);
                if (snippetLength > 180) snippetLength = 180;
                memcpy(snippet, cursor, snippetLength); snippet[snippetLength] = 0;
                while (snippetLength && (snippet[snippetLength - 1] == '\r' ||
                       snippet[snippetLength - 1] == '\n')) snippet[--snippetLength] = 0;
                WCHAR *wideSnippet = to_wide(snippet);
                size_t capacity = wcslen(files.labels[i]) + (wideSnippet ? wcslen(wideSnippet) : 0) + 48;
                WCHAR *label = (WCHAR *)malloc(capacity * sizeof(WCHAR));
                if (label) {
                    swprintf(label, capacity, L"%ls:%d — %ls", files.labels[i], lineNumber,
                             wideSnippet ? wideSnippet : L"");
                    add_workspace_item(&results, files.paths[i], label, lineNumber - 1);
                    free(label);
                }
                free(wideSnippet);
            }
            if (!end) break;
            *end = '\n'; cursor = end + 1; lineNumber++;
        }
        free(content);
    }
    free(query);
    free_workspace_files(&files);
    if (!results.count) {
        MessageBoxW(g_hMainWnd, L"No matches were found.", L"Find in Folder",
                    MB_OK | MB_ICONINFORMATION);
        free_workspace_files(&results); return;
    }
    int selected = show_choice_dialog(L"Find in Folder", L"Filter results, then open a match:",
                                      results.labels, results.count);
    if (selected >= 0 && selected < results.count && open_file_in_tab(results.paths[selected])) {
        SendMessageA(g_hEdit, SCI_GOTOLINE, (WPARAM)results.lines[selected], 0);
        SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
    }
    free_workspace_files(&results);
}

typedef struct PaletteCommand { const WCHAR *label; WORD command; } PaletteCommand;
static const PaletteCommand PALETTE_COMMANDS[] = {
    {L"File: New", IDM_FILE_NEW}, {L"File: Open", IDM_FILE_OPEN},
    {L"File: Quick Open in Folder", IDM_FILE_QUICK_OPEN},
    {L"File: Find in Folder", IDM_FILE_FIND_FOLDER}, {L"File: Save", IDM_FILE_SAVE},
    {L"File: Save As", IDM_FILE_SAVEAS}, {L"File: Export Rendered HTML", IDM_FILE_EXPORT_HTML},
    {L"File: Export Rendered PDF", IDM_FILE_EXPORT_PDF}, {L"File: Print Rendered Preview", IDM_FILE_PRINT},
    {L"File: Open Terminal Here", IDM_FILE_OPEN_TERMINAL}, {L"File: Show in Explorer", IDM_FILE_SHOW_EXPLORER},
    {L"File: Close Tab", IDM_FILE_CLOSE},
    {L"File: Reopen Closed Tab", IDM_FILE_REOPEN_CLOSED}, {L"Edit: Undo", IDM_EDIT_UNDO},
    {L"Edit: Redo", IDM_EDIT_REDO}, {L"Edit: Cut", IDM_EDIT_CUT}, {L"Edit: Copy", IDM_EDIT_COPY},
    {L"Edit: Paste", IDM_EDIT_PASTE}, {L"Edit: Delete", IDM_EDIT_DELETE},
    {L"Edit: Select All", IDM_EDIT_SELECTALL}, {L"Edit: Find", IDM_EDIT_FIND},
    {L"Edit: Find Next", IDM_EDIT_FINDNEXT}, {L"Edit: Replace", IDM_EDIT_REPLACE},
    {L"Edit: Go to Line", IDM_EDIT_GOTOLINE}, {L"Edit: Duplicate Line", IDM_EDIT_DUPLICATE_LINE},
    {L"Edit: Delete Line", IDM_EDIT_DELETE_LINE}, {L"Edit: Join Lines", IDM_EDIT_JOIN_LINES},
    {L"Edit: Move Line Up", IDM_EDIT_MOVE_LINE_UP}, {L"Edit: Move Line Down", IDM_EDIT_MOVE_LINE_DOWN},
    {L"Edit: Toggle Comment", IDM_EDIT_TOGGLE_COMMENT}, {L"Edit: Trim Trailing Whitespace", IDM_EDIT_TRIM_WHITESPACE},
    {L"Edit: Select Next Occurrence", IDM_EDIT_SELECT_NEXT},
    {L"Edit: Select All Occurrences", IDM_EDIT_SELECT_ALL_OCCURRENCES},
    {L"Edit: Jump to Matching Brace", IDM_EDIT_MATCH_BRACE},
    {L"Edit: Toggle Bookmark", IDM_EDIT_BOOKMARK_TOGGLE}, {L"Edit: Next Bookmark", IDM_EDIT_BOOKMARK_NEXT},
    {L"Edit: Previous Bookmark", IDM_EDIT_BOOKMARK_PREV}, {L"Edit: Clear Bookmarks", IDM_EDIT_BOOKMARK_CLEAR},
    {L"Edit: Convert to Uppercase", IDM_EDIT_UPPERCASE}, {L"Edit: Convert to Lowercase", IDM_EDIT_LOWERCASE},
    {L"Edit: Convert to Title Case", IDM_EDIT_TITLECASE}, {L"Edit: Sort Lines Ascending", IDM_EDIT_SORT_ASC},
    {L"Edit: Sort Lines Descending", IDM_EDIT_SORT_DESC}, {L"Edit: Remove Duplicate Lines", IDM_EDIT_SORT_DEDUPE},
    {L"Edit: Tabs to Spaces", IDM_EDIT_TABS_TO_SPACES}, {L"Edit: Spaces to Tabs", IDM_EDIT_SPACES_TO_TABS},
    {L"Edit: Convert Line Endings to CRLF", IDM_EDIT_EOL_CRLF},
    {L"Edit: Convert Line Endings to LF", IDM_EDIT_EOL_LF},
    {L"Edit: Convert Line Endings to CR", IDM_EDIT_EOL_CR},
    {L"Format: Bold", IDM_FMT_BOLD},
    {L"Format: Italic", IDM_FMT_ITALIC}, {L"Format: Strikethrough", IDM_FMT_STRIKETHROUGH},
    {L"Format: Inline Code", IDM_FMT_CODE}, {L"Format: Code Block", IDM_FMT_CODEBLOCK},
    {L"Format: Heading 1", IDM_FMT_H1}, {L"Format: Heading 2", IDM_FMT_H2},
    {L"Format: Heading 3", IDM_FMT_H3}, {L"Format: Blockquote", IDM_FMT_QUOTE},
    {L"Format: Bullet List", IDM_FMT_BULLETLIST}, {L"Format: Numbered List", IDM_FMT_NUMBERLIST},
    {L"Format: Task List", IDM_FMT_TASKLIST}, {L"Format: Link", IDM_FMT_LINK},
    {L"Format: Image", IDM_FMT_IMAGE}, {L"Format: Table", IDM_FMT_TABLE},
    {L"Format: Footnote", IDM_FMT_FOOTNOTE}, {L"Format: Horizontal Rule", IDM_FMT_HRULE},
    {L"Format: Alert", IDM_FMT_ALERT}, {L"Format: Table Editor", IDM_FMT_TABLE_EDITOR},
    {L"Format: Format JSON", IDM_FMT_JSON_PRETTY},
    {L"Format: Minify JSON", IDM_FMT_JSON_MINIFY}, {L"Format: Font", IDM_FORMAT_FONT},
    {L"Run: Run File", IDM_RUN_FILE}, {L"Run: Debug File", IDM_DEBUG_FILE},
    {L"Run: Continue", IDM_DEBUG_CONTINUE}, {L"Run: Step Over", IDM_DEBUG_STEP_OVER},
    {L"Run: Step Into", IDM_DEBUG_STEP_INTO},
    {L"Run: Toggle Breakpoint", IDM_DEBUG_BREAKPOINT},
    {L"Run: Clear All Breakpoints", IDM_DEBUG_CLEAR_BREAKPOINTS},
    {L"View: Toggle Preview", IDM_VIEW_TOGGLEPREVIEW}, {L"View: Toggle Editor", IDM_VIEW_TOGGLEEDITOR},
    {L"View: Toggle Outline", IDM_VIEW_OUTLINE}, {L"View: Focus Mode", IDM_VIEW_FOCUS_MODE},
    {L"View: Next Tab", IDM_VIEW_NEXT_TAB}, {L"View: Previous Tab", IDM_VIEW_PREVIOUS_TAB},
    {L"View: Preview on Left", IDM_VIEW_PREVIEW_ON_LEFT},
    {L"View: Word Wrap", IDM_VIEW_WORD_WRAP}, {L"View: Spell Check", IDM_VIEW_SPELL_CHECK},
    {L"View: Spell Check Language", IDM_VIEW_SPELL_LANGUAGE}, {L"View: Show Whitespace", IDM_VIEW_SHOW_WHITESPACE},
    {L"View: Indent Guides", IDM_VIEW_INDENT_GUIDES}, {L"View: Zoom In", IDM_VIEW_ZOOM_IN},
    {L"View: Zoom Out", IDM_VIEW_ZOOM_OUT}, {L"View: Reset Zoom", IDM_VIEW_ZOOM_RESET},
    {L"View: GitHub Light Theme", IDM_VIEW_THEME_GITHUB_LIGHT},
    {L"View: GitHub Dark Theme", IDM_VIEW_THEME_GITHUB_DARK}, {L"View: Custom Preview CSS", IDM_VIEW_CUSTOM_CSS},
    {L"View: Clear Custom Preview CSS", IDM_VIEW_CLEAR_CUSTOM_CSS}, {L"View: Toggle Fold", IDM_VIEW_FOLD_TOGGLE},
    {L"View: Fold All", IDM_VIEW_FOLD_ALL}, {L"View: Unfold All", IDM_VIEW_UNFOLD_ALL},
    {L"Help: Markdown Reference", IDM_HELP_REFERENCE}
};

static void show_command_palette(void) {
    int count = (int)(sizeof(PALETTE_COMMANDS) / sizeof(PALETTE_COMMANDS[0]));
    WCHAR **labels = (WCHAR **)malloc((size_t)count * sizeof(WCHAR *));
    if (!labels) return;
    for (int i = 0; i < count; i++) labels[i] = (WCHAR *)PALETTE_COMMANDS[i].label;
    int selected = show_choice_dialog(L"Command Palette", L"Type to find a command:", labels, count);
    free(labels);
    if (selected >= 0 && selected < count)
        PostMessageW(g_hMainWnd, WM_COMMAND, PALETTE_COMMANDS[selected].command, 0);
}

static int splitter_contains_x(int x) {
    return g_previewVisible && g_editorVisible &&
           x >= g_splitterPos && x <= g_splitterPos + SPLITTER_WIDTH;
}

static void select_theme(HWND hwnd, int darkMode) {
    g_previewDarkMode = darkMode;
    save_theme_settings();
    apply_menu_theme(hwnd, darkMode);
    apply_preview_theme();
    apply_webview_background_colour();
    apply_editor_theme(g_hEdit);
    apply_preview_host_theme();
    apply_main_theme(hwnd);
    for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++) {
        if (g_hToolbarButtons[i]) InvalidateRect(g_hToolbarButtons[i], NULL, TRUE);
    }
    if (g_hTabs) InvalidateRect(g_hTabs, NULL, TRUE);
    if (g_hStatus) InvalidateRect(g_hStatus, NULL, TRUE);
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
        case IDM_FILE_NEW: create_document_tab(); break;
        case IDM_FILE_OPEN: do_open(); break;
        case IDM_FILE_QUICK_OPEN: do_quick_open(); break;
        case IDM_FILE_FIND_FOLDER: do_find_in_folder(); break;
        case IDM_FILE_SAVE: do_save(); break;
        case IDM_FILE_SAVEAS: do_saveas(); break;
        case IDM_FILE_EXPORT_HTML: do_export_html(); break;
        case IDM_FILE_EXPORT_PDF: prepare_rendered_print(1); break;
        case IDM_FILE_PRINT: prepare_rendered_print(0); break;
        case IDM_FILE_OPEN_TERMINAL: open_terminal_here(); break;
        case IDM_FILE_SHOW_EXPLORER: reveal_in_explorer(); break;
        case IDM_FILE_CLOSE: do_close_document_at(g_activeDocument); break;
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
        case IDM_EDIT_PASTE:
            if (!paste_image_from_clipboard()) SendMessageA(g_hEdit, SCI_PASTE, 0, 0);
            break;
        case IDM_EDIT_DELETE: SendMessageA(g_hEdit, SCI_CLEAR, 0, 0); break;
        case IDM_EDIT_SELECTALL: SendMessageA(g_hEdit, SCI_SELECTALL, 0, 0); break;
        case IDM_EDIT_DUPLICATE_LINE: SendMessageA(g_hEdit, SCI_LINEDUPLICATE, 0, 0); break;
        case IDM_EDIT_MOVE_LINE_UP: SendMessageA(g_hEdit, SCI_MOVESELECTEDLINESUP, 0, 0); break;
        case IDM_EDIT_MOVE_LINE_DOWN: SendMessageA(g_hEdit, SCI_MOVESELECTEDLINESDOWN, 0, 0); break;
        case IDM_EDIT_DELETE_LINE: SendMessageA(g_hEdit, SCI_LINEDELETE, 0, 0); break;
        case IDM_EDIT_JOIN_LINES: SendMessageA(g_hEdit, SCI_LINESJOIN, 0, 0); break;
        case IDM_EDIT_TOGGLE_COMMENT: toggle_comment(); break;
        case IDM_EDIT_TRIM_WHITESPACE: trim_trailing_whitespace(); break;
        case IDM_EDIT_SELECT_NEXT: select_next_occurrence(); break;
        case IDM_EDIT_SELECT_ALL_OCCURRENCES: select_all_occurrences(); break;
        case IDM_EDIT_MATCH_BRACE: jump_to_matching_brace(); break;
        case IDM_EDIT_BOOKMARK_TOGGLE: toggle_bookmark_at_cursor(); break;
        case IDM_EDIT_BOOKMARK_NEXT: goto_bookmark(1); break;
        case IDM_EDIT_BOOKMARK_PREV: goto_bookmark(0); break;
        case IDM_EDIT_BOOKMARK_CLEAR: clear_all_bookmarks(); break;
        case IDM_EDIT_UPPERCASE: SendMessageA(g_hEdit, SCI_UPPERCASE, 0, 0); break;
        case IDM_EDIT_LOWERCASE: SendMessageA(g_hEdit, SCI_LOWERCASE, 0, 0); break;
        case IDM_EDIT_TITLECASE: apply_title_case(); break;
        case IDM_EDIT_SORT_ASC: transform_selected_lines(0); break;
        case IDM_EDIT_SORT_DESC: transform_selected_lines(1); break;
        case IDM_EDIT_SORT_DEDUPE: transform_selected_lines(2); break;
        case IDM_EDIT_TABS_TO_SPACES: convert_indentation(1); break;
        case IDM_EDIT_SPACES_TO_TABS: convert_indentation(0); break;
        case IDM_EDIT_EOL_CRLF: convert_line_endings(SC_EOL_CRLF); break;
        case IDM_EDIT_EOL_LF: convert_line_endings(SC_EOL_LF); break;
        case IDM_EDIT_EOL_CR: convert_line_endings(SC_EOL_CR); break;
        case IDM_EDIT_FIND: show_find_dialog(0); break;
        case IDM_EDIT_REPLACE: show_find_dialog(1); break;
        case IDM_EDIT_GOTOLINE: show_goto_line_dialog(); break;
        case IDM_EDIT_COMMAND_PALETTE: show_command_palette(); break;
        case IDM_EDIT_FINDNEXT:
            if (g_findWhat[0]) {
                char *findUtf8 = to_utf8(g_findWhat);
                if (findUtf8) find_in_edit(findUtf8, (g_fr.Flags & FR_MATCHCASE) ? 1 : 0, 1);
                free(findUtf8);
            } else show_find_dialog(0);
            break;
        case IDM_FORMAT_FONT: do_choose_font(); break;
        case IDM_FMT_JSON_PRETTY: format_json(1); break;
        case IDM_FMT_JSON_MINIFY: format_json(0); break;
        case IDM_FMT_TABLE_EDITOR: show_table_editor(); break;
        case IDM_RUN_FILE: launch_active_source_file(0); break;
        case IDM_DEBUG_FILE: launch_active_source_file(1); break;
        case IDM_DEBUG_CONTINUE: run_debugger_command(IDM_DEBUG_CONTINUE); break;
        case IDM_DEBUG_STEP_OVER: run_debugger_command(IDM_DEBUG_STEP_OVER); break;
        case IDM_DEBUG_STEP_INTO: run_debugger_command(IDM_DEBUG_STEP_INTO); break;
        case IDM_DEBUG_BREAKPOINT: toggle_breakpoint_at_cursor(); break;
        case IDM_DEBUG_CLEAR_BREAKPOINTS: clear_debug_breakpoints(); break;
        case IDM_VIEW_TOGGLEPREVIEW: toggle_preview(hwnd); break;
        case IDM_VIEW_TOGGLEEDITOR: toggle_editor(hwnd); break;
        case IDM_VIEW_PREVIEW_ON_LEFT: toggle_panel_sides(hwnd); break;
        case IDM_VIEW_CUSTOM_CSS: do_choose_custom_css(); break;
        case IDM_VIEW_CLEAR_CUSTOM_CSS: clear_custom_css(); break;
        case IDM_VIEW_FOCUS_MODE: toggle_focus_mode(hwnd); break;
        case IDM_VIEW_WORD_WRAP: toggle_word_wrap(hwnd); break;
        case IDM_VIEW_SPELL_CHECK: toggle_spell_check(hwnd); break;
        case IDM_VIEW_SPELL_LANGUAGE: choose_spell_language(); break;
        case IDM_VIEW_SHOW_WHITESPACE: toggle_show_whitespace(hwnd); break;
        case IDM_VIEW_INDENT_GUIDES: toggle_indent_guides(hwnd); break;
        case IDM_VIEW_FOLD_TOGGLE: fold_toggle_at_cursor(); break;
        case IDM_VIEW_FOLD_ALL: fold_all_lines(0); break;
        case IDM_VIEW_UNFOLD_ALL: fold_all_lines(1); break;
        case IDM_VIEW_OUTLINE: toggle_outline(hwnd); break;
        case IDM_VIEW_ZOOM_IN: adjust_editor_zoom(1); break;
        case IDM_VIEW_ZOOM_OUT: adjust_editor_zoom(-1); break;
        case IDM_VIEW_ZOOM_RESET: reset_editor_zoom(); break;
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

static int line_has_list_marker(const char *line, int length) {
    int i = 0;
    while (i < length && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i + 1 < length && (line[i] == '-' || line[i] == '*' || line[i] == '+') &&
        (line[i + 1] == ' ' || line[i + 1] == '\t')) return 1;
    int digits = i;
    while (i < length && line[i] >= '0' && line[i] <= '9') i++;
    return i > digits && i + 1 < length && (line[i] == '.' || line[i] == ')') &&
        (line[i + 1] == ' ' || line[i + 1] == '\t');
}

/* Preserve real tabs for indentation, but use spaces for alignment after
   ordinary text. A collapsed caret anywhere in a Markdown list item indents
   the entire item, which makes nesting work without first moving the caret. */
static int smart_tab(int reverse) {
    int start = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int end = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (start != end) return 0; /* retain Scintilla's block-selection behavior */

    int lineNumber = (int)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)start, 0);
    int lineStart = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)lineNumber, 0);
    int lineEnd = (int)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)lineNumber, 0);
    int lineLength = lineEnd - lineStart;
    char *line = (char *)malloc((size_t)lineLength + 1);
    if (!line) return 0;
    struct Sci_TextRange range = { { lineStart, lineEnd }, line };
    SendMessageA(g_hEdit, SCI_GETTEXTRANGE, 0, (LPARAM)&range);
    line[lineLength] = 0;

    int leadingLength = 0;
    while (leadingLength < lineLength &&
           (line[leadingLength] == ' ' || line[leadingLength] == '\t')) leadingLength++;
    int listItem = line_has_list_marker(line, lineLength);

    if (reverse && leadingLength > 0) {
        int remove = 0;
        int tabWidth = (int)SendMessageA(g_hEdit, SCI_GETTABWIDTH, 0, 0);
        if (tabWidth < 1) tabWidth = 4;
        if (line[0] == '\t') remove = 1;
        else while (remove < leadingLength && remove < tabWidth && line[remove] == ' ') remove++;
        SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)lineStart, 0);
        SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)(lineStart + remove), 0);
        SendMessageA(g_hEdit, SCI_REPLACETARGET, 0, (LPARAM)"");
        free(line);
        return 1;
    }

    if (!reverse && listItem) {
        SendMessageA(g_hEdit, SCI_INSERTTEXT, (WPARAM)lineStart, (LPARAM)"\t");
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)(start + 1), (LPARAM)(start + 1));
        free(line);
        return 1;
    }

    free(line);
    if (reverse || start <= lineStart + leadingLength) return 0;

    int tabWidth = (int)SendMessageA(g_hEdit, SCI_GETTABWIDTH, 0, 0);
    if (tabWidth < 1) tabWidth = 4;
    int column = (int)SendMessageA(g_hEdit, SCI_GETCOLUMN, (WPARAM)start, 0);
    int spaces = tabWidth - column % tabWidth;
    char padding[32];
    if (spaces >= (int)sizeof(padding)) spaces = (int)sizeof(padding) - 1;
    memset(padding, ' ', (size_t)spaces);
    padding[spaces] = 0;
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)padding);
    return 1;
}

static int smart_markdown_newline(void) {
    int selectionStart = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int selectionEnd = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (selectionStart != selectionEnd) return 0;
    int caret = selectionEnd;
    if (caret > 0 && (int)SendMessageA(g_hEdit, SCI_GETSTYLEAT, (WPARAM)(caret - 1), 0) == SCE_MARKDOWN_CODEBK)
        return 0;
    int line = (int)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)caret, 0);
    int lineStart = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
    int beforeLength = caret - lineStart;
    if (beforeLength <= 0) return 0;
    char *before = (char *)malloc((size_t)beforeLength + 1);
    if (!before) return 0;
    struct Sci_TextRange range;
    range.chrg.cpMin = lineStart;
    range.chrg.cpMax = caret;
    range.lpstrText = before;
    SendMessageA(g_hEdit, SCI_GETTEXTRANGE, 0, (LPARAM)&range);
    before[beforeLength] = 0;

    int i = 0;
    while (i < beforeLength && (before[i] == ' ' || before[i] == '\t')) i++;
    int indentEnd = i;
    while (i < beforeLength && before[i] == '>') {
        i++;
        if (i < beforeLength && before[i] == ' ') i++;
    }
    int quoteEnd = i;
    int markerStart = i, markerEnd = i, ordered = 0, nextNumber = 0;
    char punctuation = 0;
    if (i + 1 < beforeLength && (before[i] == '-' || before[i] == '*' || before[i] == '+') &&
        (before[i + 1] == ' ' || before[i + 1] == '\t')) {
        i += 2;
        markerEnd = i;
        if (i + 3 < beforeLength && before[i] == '[' &&
            (before[i + 1] == ' ' || before[i + 1] == 'x' || before[i + 1] == 'X') &&
            before[i + 2] == ']' && (before[i + 3] == ' ' || before[i + 3] == '\t')) {
            i += 4;
            markerEnd = i;
        }
    } else {
        int digits = i;
        unsigned long number = 0;
        while (i < beforeLength && before[i] >= '0' && before[i] <= '9') {
            if (number < 9999999) number = number * 10 + (unsigned)(before[i] - '0');
            i++;
        }
        if (i > digits && i + 1 < beforeLength && (before[i] == '.' || before[i] == ')') &&
            (before[i + 1] == ' ' || before[i + 1] == '\t')) {
            punctuation = before[i];
            i += 2;
            markerEnd = i;
            ordered = 1;
            nextNumber = (int)number + 1;
        } else {
            i = markerStart;
        }
    }
    int hasMarker = markerEnd > markerStart;
    int hasQuote = quoteEnd > indentEnd;
    if (!hasMarker && !hasQuote) { free(before); return 0; }
    int contentStart = hasMarker ? markerEnd : quoteEnd;
    int contentEmpty = 1;
    for (int p = contentStart; p < beforeLength; p++) {
        if (before[p] != ' ' && before[p] != '\t') { contentEmpty = 0; break; }
    }

    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
    if (contentEmpty) {
        /* An empty list item or quote terminates that construct. Nested quote
           prefixes remain when terminating a list nested inside a quote. */
        int keep = hasMarker ? quoteEnd : indentEnd;
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)(lineStart + keep), (LPARAM)caret);
        SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)"");
    } else {
        const char *eol = editor_eol_sequence();
        char orderedMarker[48] = "";
        const char *markerText = NULL;
        int prefixLength = contentStart;
        if (ordered) {
            snprintf(orderedMarker, sizeof(orderedMarker), "%d%c ", nextNumber, punctuation);
            markerText = orderedMarker;
            prefixLength = markerStart;
        }
        size_t insertLength = strlen(eol) + (size_t)prefixLength +
            (markerText ? strlen(markerText) : 0);
        char *insert = (char *)malloc(insertLength + 1);
        if (insert) {
            strcpy(insert, eol);
            memcpy(insert + strlen(eol), before, (size_t)prefixLength);
            size_t used = strlen(eol) + (size_t)prefixLength;
            if (markerText) {
                strcpy(insert + used, markerText);
            } else if (hasMarker && markerEnd - markerStart >= 5 &&
                       before[markerStart + 2] == '[') {
                /* Continue task lists unchecked, even when the previous item was checked. */
                memcpy(insert + strlen(eol) + markerStart, before + markerStart,
                       (size_t)(markerEnd - markerStart));
                insert[strlen(eol) + markerStart + 3] = ' ';
                insert[insertLength] = 0;
            } else {
                insert[insertLength] = 0;
            }
            SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)insert);
            free(insert);
        }
    }
    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
    free(before);
    return 1;
}

static int ends_with_word(const char *text, int length, const char *word) {
    int wordLength = (int)strlen(word);
    if (length < wordLength || memcmp(text + length - wordLength, word,
                                      (size_t)wordLength) != 0) return 0;
    return length == wordLength || !(isalnum((unsigned char)text[length - wordLength - 1]) ||
                                      text[length - wordLength - 1] == '_');
}

static int style_allows_indent(int position) {
    int style = (int)SendMessageA(g_hEdit, SCI_GETSTYLEAT, (WPARAM)position, 0);
    switch (g_activeLanguage) {
        case LANG_CPP:
            return !(style == SCE_C_COMMENT || style == SCE_C_COMMENTLINE ||
                     style == SCE_C_COMMENTDOC || style == SCE_C_COMMENTLINEDOC ||
                     style == SCE_C_STRING || style == SCE_C_CHARACTER ||
                     style == SCE_C_STRINGEOL || style == SCE_C_VERBATIM ||
                     style == SCE_C_REGEX || style == SCE_C_STRINGRAW ||
                     style == SCE_C_TRIPLEVERBATIM || style == SCE_C_HASHQUOTEDSTRING);
        case LANG_JSON:
            return !(style == SCE_JSON_STRING || style == SCE_JSON_STRINGEOL ||
                     style == SCE_JSON_LINECOMMENT || style == SCE_JSON_BLOCKCOMMENT);
        case LANG_PYTHON:
            return !(style == SCE_P_COMMENTLINE || style == SCE_P_COMMENTBLOCK ||
                     style == SCE_P_STRING || style == SCE_P_CHARACTER ||
                     style == SCE_P_TRIPLE || style == SCE_P_TRIPLEDOUBLE ||
                     style == SCE_P_STRINGEOL || style == SCE_P_FSTRING ||
                     style == SCE_P_FCHARACTER || style == SCE_P_FTRIPLE ||
                     style == SCE_P_FTRIPLEDOUBLE);
        case LANG_YAML:
            return style != SCE_YAML_COMMENT;
        case LANG_BASH:
            return !(style == SCE_SH_COMMENTLINE || style == SCE_SH_STRING ||
                     style == SCE_SH_CHARACTER || style == SCE_SH_HERE_DELIM ||
                     style == SCE_SH_HERE_Q);
        case LANG_POWERSHELL:
            return !(style == SCE_POWERSHELL_COMMENT ||
                     style == SCE_POWERSHELL_COMMENTSTREAM ||
                     style == SCE_POWERSHELL_COMMENTDOCKEYWORD ||
                     style == SCE_POWERSHELL_STRING ||
                     style == SCE_POWERSHELL_CHARACTER ||
                     style == SCE_POWERSHELL_HERE_STRING ||
                     style == SCE_POWERSHELL_HERE_CHARACTER);
        default:
            return 1;
    }
}

static int line_opens_indent(const char *before, int contentStart, int trimmedEnd,
                             int lineStart, char *closingOut) {
    if (closingOut) *closingOut = 0;
    if (trimmedEnd <= contentStart || before[contentStart] == '#') return 0;
    if (!style_allows_indent(lineStart + trimmedEnd - 1)) return 0;
    char last = before[trimmedEnd - 1];
    switch (g_activeLanguage) {
        case LANG_CPP:
        case LANG_POWERSHELL:
            if (last == '{') { if (closingOut) *closingOut = '}'; return 1; }
            break;
        case LANG_JSON:
            if (last == '{') { if (closingOut) *closingOut = '}'; return 1; }
            if (last == '[') { if (closingOut) *closingOut = ']'; return 1; }
            break;
        case LANG_PYTHON:
        case LANG_YAML:
            return last == ':';
        case LANG_BASH:
            if (last == '{') { if (closingOut) *closingOut = '}'; return 1; }
            return ends_with_word(before + contentStart, trimmedEnd - contentStart, "then") ||
                   ends_with_word(before + contentStart, trimmedEnd - contentStart, "do") ||
                   ends_with_word(before + contentStart, trimmedEnd - contentStart, "in");
        default:
            break;
    }
    return 0;
}

/* Carry the current line's leading whitespace onto the next line and add one
   real tab after language-specific block openers. When the caret is between a
   paired opener and closer, expand both onto properly aligned lines at once. */
static int smart_indented_newline(void) {
    int selectionStart = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int selectionEnd = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (selectionStart != selectionEnd) return 0;
    int caret = selectionEnd;
    int lineNumber = (int)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)caret, 0);
    int lineStart = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)lineNumber, 0);
    int lineEnd = (int)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)lineNumber, 0);
    int beforeLength = caret - lineStart;
    if (beforeLength < 0) return 0;
    char *before = (char *)malloc((size_t)beforeLength + 1);
    if (!before) return 0;
    struct Sci_TextRange range = { { lineStart, caret }, before };
    SendMessageA(g_hEdit, SCI_GETTEXTRANGE, 0, (LPARAM)&range);
    before[beforeLength] = 0;

    int indentLength = 0;
    while (indentLength < beforeLength &&
           (before[indentLength] == ' ' || before[indentLength] == '\t')) indentLength++;
    int trimmedEnd = beforeLength;
    while (trimmedEnd > indentLength &&
           (before[trimmedEnd - 1] == ' ' || before[trimmedEnd - 1] == '\t')) trimmedEnd--;
    char closing = 0;
    int addIndent = line_opens_indent(before, indentLength, trimmedEnd, lineStart, &closing);

    int after = caret;
    while (after < lineEnd) {
        char character = (char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)after, 0);
        if (character != ' ' && character != '\t') break;
        after++;
    }
    int pairedClose = addIndent && closing && after < lineEnd &&
        (char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)after, 0) == closing;
    if (pairedClose) {
        int rest = after + 1;
        while (rest < lineEnd) {
            char character = (char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)rest, 0);
            if (character != ' ' && character != '\t') { pairedClose = 0; break; }
            rest++;
        }
    }

    if (!indentLength && !addIndent) { free(before); return 0; }
    const char *eol = editor_eol_sequence();
    size_t eolLength = strlen(eol);
    size_t firstLength = eolLength + (size_t)indentLength + (addIndent ? 1 : 0);
    size_t insertLength = firstLength + (pairedClose ? eolLength + (size_t)indentLength : 0);
    char *insert = (char *)malloc(insertLength + 1);
    if (!insert) { free(before); return 0; }
    size_t used = 0;
    memcpy(insert + used, eol, eolLength); used += eolLength;
    memcpy(insert + used, before, (size_t)indentLength); used += (size_t)indentLength;
    if (addIndent) insert[used++] = '\t';
    if (pairedClose) {
        memcpy(insert + used, eol, eolLength); used += eolLength;
        memcpy(insert + used, before, (size_t)indentLength); used += (size_t)indentLength;
    }
    insert[used] = 0;
    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
    if (pairedClose && after > caret)
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)caret, (LPARAM)after);
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)insert);
    if (pairedClose)
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)(caret + (int)firstLength),
                     (LPARAM)(caret + (int)firstLength));
    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
    free(insert);
    free(before);
    return 1;
}

static int smart_newline(void) {
    if (g_activeLanguage == LANG_MARKDOWN && smart_markdown_newline()) return 1;
    return smart_indented_newline();
}

/* Typing a closing brace on an otherwise blank indented line brings it back
   one indentation level. Existing auto-paired closers are still skipped by
   smart_pair_character() instead of being replaced. */
static int smart_closing_indent(char typed) {
    int valid = typed == '}' && (g_activeLanguage == LANG_CPP ||
        g_activeLanguage == LANG_BASH || g_activeLanguage == LANG_POWERSHELL ||
        g_activeLanguage == LANG_JSON);
    if (typed == ']' && g_activeLanguage == LANG_JSON) valid = 1;
    if (!valid) return 0;
    int start = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int end = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (start != end || (char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)end, 0) == typed)
        return 0;
    int line = (int)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)start, 0);
    int lineStart = (int)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
    for (int position = lineStart; position < start; position++) {
        char character = (char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)position, 0);
        if (character != ' ' && character != '\t') return 0;
    }
    if (start == lineStart) return 0;
    int removeStart = start;
    if ((char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)(start - 1), 0) == '\t') {
        removeStart--;
    } else {
        int tabWidth = (int)SendMessageA(g_hEdit, SCI_GETTABWIDTH, 0, 0);
        if (tabWidth < 1) tabWidth = 4;
        while (removeStart > lineStart && start - removeStart < tabWidth &&
               (char)SendMessageA(g_hEdit, SCI_GETCHARAT,
                                  (WPARAM)(removeStart - 1), 0) == ' ') removeStart--;
    }
    char replacement[2] = { typed, 0 };
    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)removeStart, (LPARAM)start);
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)replacement);
    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
    return 1;
}

static int smart_pair_character(char typed) {
    char closing = typed == '(' ? ')' : typed == '[' ? ']' : typed == '{' ? '}' : typed == '`' ? '`' : 0;
    int start = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int end = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (typed == '`' && start == end) {
        if ((char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)end, 0) == '`') {
            SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)(end + 1), (LPARAM)(end + 1));
            return 1;
        }
        if (start >= 2 && (char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)(start - 1), 0) == '`' &&
            (char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)(start - 2), 0) == '`') return 0;
    }
    if (!closing) {
        if ((typed == ')' || typed == ']' || typed == '}') && start == end &&
            (char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)end, 0) == typed) {
            SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)(end + 1), (LPARAM)(end + 1));
            return 1;
        }
        return 0;
    }
    int length = end - start;
    char *selected = (char *)malloc((size_t)length + 1);
    if (!selected) return 0;
    if (length > 0) SendMessageA(g_hEdit, SCI_GETSELTEXT, 0, (LPARAM)selected);
    else selected[0] = 0;
    size_t cap = (size_t)length + 3;
    char *replacement = (char *)malloc(cap);
    if (!replacement) { free(selected); return 0; }
    replacement[0] = typed;
    if (length > 0) memcpy(replacement + 1, selected, (size_t)length);
    replacement[length + 1] = closing;
    replacement[length + 2] = 0;
    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)replacement);
    if (length > 0) SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)(start + 1), (LPARAM)(start + 1 + length));
    else SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)(start + 1), (LPARAM)(start + 1));
    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
    free(replacement);
    free(selected);
    return 1;
}

static int smart_pair_backspace(void) {
    int start = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    int end = (int)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (start != end || start <= 0) return 0;
    char before = (char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)(start - 1), 0);
    char after = (char)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)start, 0);
    if (!((before == '(' && after == ')') || (before == '[' && after == ']') ||
          (before == '{' && after == '}') || (before == '`' && after == '`'))) return 0;
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)(start - 1), (LPARAM)(start + 1));
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)"");
    return 1;
}

static int show_spelling_context_menu(HWND hwnd, LPARAM lParam) {
    if (!g_spellCheckEnabled || !g_spellChecker || g_activeLanguage != LANG_MARKDOWN)
        return 0;

    POINT screenPoint;
    int position;
    if ((int)GET_X_LPARAM(lParam) == -1 && (int)GET_Y_LPARAM(lParam) == -1) {
        position = (int)SendMessageA(hwnd, SCI_GETCURRENTPOS, 0, 0);
        if (!GetCaretPos(&screenPoint)) screenPoint = (POINT){0, 0};
        ClientToScreen(hwnd, &screenPoint);
    } else {
        screenPoint.x = GET_X_LPARAM(lParam);
        screenPoint.y = GET_Y_LPARAM(lParam);
        POINT clientPoint = screenPoint;
        ScreenToClient(hwnd, &clientPoint);
        position = (int)SendMessageA(hwnd, SCI_POSITIONFROMPOINTCLOSE,
                                     (WPARAM)clientPoint.x, (LPARAM)clientPoint.y);
    }
    if (position < 0) return 0;

    SendMessageA(hwnd, SCI_SETINDICATORCURRENT, INDICATOR_SPELLING, 0);
    if (!SendMessageA(hwnd, SCI_INDICATORVALUEAT, INDICATOR_SPELLING, position) && position > 0)
        position--;
    if (!SendMessageA(hwnd, SCI_INDICATORVALUEAT, INDICATOR_SPELLING, position)) return 0;

    int start = (int)SendMessageA(hwnd, SCI_INDICATORSTART, INDICATOR_SPELLING, position);
    int end = (int)SendMessageA(hwnd, SCI_INDICATOREND, INDICATOR_SPELLING, position);
    if (start < 0 || end <= start) return 0;
    int byteLength = end - start;
    char *utf8Word = (char *)malloc((size_t)byteLength + 1);
    if (!utf8Word) return 1;
    struct Sci_TextRange range = { { start, end }, utf8Word };
    SendMessageA(hwnd, SCI_GETTEXTRANGE, 0, (LPARAM)&range);
    utf8Word[byteLength] = 0;
    WCHAR *word = to_wide(utf8Word);
    free(utf8Word);
    if (!word) return 1;

    enum {
        SPELL_SUGGESTION_FIRST = 1, SPELL_SUGGESTION_LAST = 8,
        SPELL_IGNORE = 20, SPELL_ADD = 21,
        SPELL_UNDO = 30, SPELL_CUT = 31, SPELL_COPY = 32,
        SPELL_PASTE = 33, SPELL_SELECT_ALL = 34
    };
    WCHAR *suggestions[SPELL_SUGGESTION_LAST] = {0};
    int suggestionCount = 0;
    IEnumString *suggestionEnum = NULL;
    if (SUCCEEDED(ISpellChecker_Suggest(g_spellChecker, word, &suggestionEnum)) && suggestionEnum) {
        while (suggestionCount < SPELL_SUGGESTION_LAST) {
            LPOLESTR suggestion = NULL;
            ULONG fetched = 0;
            if (IEnumString_Next(suggestionEnum, 1, &suggestion, &fetched) != S_OK || !suggestion)
                break;
            suggestions[suggestionCount++] = suggestion;
        }
        IEnumString_Release(suggestionEnum);
    }

    HMENU menu = CreatePopupMenu();
    for (int i = 0; i < suggestionCount; i++)
        AppendMenuW(menu, MF_STRING, SPELL_SUGGESTION_FIRST + i, suggestions[i]);
    if (suggestionCount == 0)
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"(No suggestions)");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, SPELL_IGNORE, L"Ignore Word");
    AppendMenuW(menu, MF_STRING, SPELL_ADD, L"Add to Dictionary");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (SendMessageA(hwnd, SCI_CANUNDO, 0, 0) ? 0 : MF_GRAYED),
                SPELL_UNDO, L"Undo");
    AppendMenuW(menu, MF_STRING, SPELL_CUT, L"Cut");
    AppendMenuW(menu, MF_STRING, SPELL_COPY, L"Copy");
    AppendMenuW(menu, MF_STRING, SPELL_PASTE, L"Paste");
    AppendMenuW(menu, MF_STRING, SPELL_SELECT_ALL, L"Select All");

    UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                  screenPoint.x, screenPoint.y, 0, g_hMainWnd, NULL);
    DestroyMenu(menu);
    if (command >= SPELL_SUGGESTION_FIRST && command <= (UINT)suggestionCount) {
        char *replacement = to_utf8(suggestions[command - SPELL_SUGGESTION_FIRST]);
        if (replacement) {
            SendMessageA(hwnd, SCI_BEGINUNDOACTION, 0, 0);
            SendMessageA(hwnd, SCI_SETTARGETSTART, (WPARAM)start, 0);
            SendMessageA(hwnd, SCI_SETTARGETEND, (WPARAM)end, 0);
            int inserted = (int)SendMessageA(hwnd, SCI_REPLACETARGET,
                                              (WPARAM)-1, (LPARAM)replacement);
            SendMessageA(hwnd, SCI_SETSEL, (WPARAM)(start + inserted),
                         (LPARAM)(start + inserted));
            SendMessageA(hwnd, SCI_ENDUNDOACTION, 0, 0);
            free(replacement);
            schedule_spell_check();
        }
    } else if (command == SPELL_IGNORE) {
        ISpellChecker_Ignore(g_spellChecker, word);
        schedule_spell_check();
    } else if (command == SPELL_ADD) {
        ISpellChecker_Add(g_spellChecker, word);
        schedule_spell_check();
    } else if (command == SPELL_UNDO) SendMessageA(hwnd, SCI_UNDO, 0, 0);
    else if (command == SPELL_CUT) SendMessageA(hwnd, SCI_CUT, 0, 0);
    else if (command == SPELL_COPY) SendMessageA(hwnd, SCI_COPY, 0, 0);
    else if (command == SPELL_PASTE) SendMessageA(hwnd, SCI_PASTE, 0, 0);
    else if (command == SPELL_SELECT_ALL) SendMessageA(hwnd, SCI_SELECTALL, 0, 0);

    for (int i = 0; i < suggestionCount; i++) CoTaskMemFree(suggestions[i]);
    free(word);
    return 1;
}

static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR id, DWORD_PTR data) {
    (void)id; (void)data;
    if (msg == WM_CHAR) {
        if (wParam == '\t' && smart_tab((GetKeyState(VK_SHIFT) & 0x8000) != 0)) return 0;
        if (wParam == '\r' && smart_newline()) return 0;
        if (wParam == '\b' && smart_pair_backspace()) return 0;
        if (wParam < 128 && smart_closing_indent((char)wParam)) return 0;
        if (wParam < 128 && smart_pair_character((char)wParam)) return 0;
    } else if (msg == WM_CONTEXTMENU) {
        if (show_spelling_context_menu(hwnd, lParam)) return 0;
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, EditSubclassProc, 2);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void move_document_tab(int from, int to) {
    if (from < 0 || to < 0 || from >= g_documentCount || to >= g_documentCount || from == to) return;
    save_active_document_view();
    Document moving = g_documents[from];
    if (from < to) {
        for (int i = from; i < to; i++) g_documents[i] = g_documents[i + 1];
    } else {
        for (int i = from; i > to; i--) g_documents[i] = g_documents[i - 1];
    }
    g_documents[to] = moving;
    if (g_activeDocument == from) g_activeDocument = to;
    else if (from < g_activeDocument && to >= g_activeDocument) g_activeDocument--;
    else if (from > g_activeDocument && to <= g_activeDocument) g_activeDocument++;

    TabCtrl_DeleteItem(g_hTabs, from);
    TCITEMW item = {0};
    item.mask = TCIF_TEXT; item.pszText = L"";
    SendMessageW(g_hTabs, TCM_INSERTITEMW, (WPARAM)to, (LPARAM)&item);
    for (int i = 0; i < g_documentCount; i++) update_tab_label(i);
    TabCtrl_SetCurSel(g_hTabs, g_activeDocument);
    update_title();
}

static LRESULT CALLBACK TabSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR subclassId, DWORD_PTR refData) {
    (void)subclassId; (void)refData;
    if (msg == WM_ERASEBKGND) {
        return 1; /* WM_PAINT fills the whole strip from a back buffer */
    } else if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);

        /* Draw the strip and every tab into a back buffer: the tabs are
           owner-drawn one at a time, which visibly flickers on-screen. */
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HBITMAP oldBitmap = (HBITMAP)SelectObject(buffer, bitmap);

        /* The recessed strip behind the tabs, matching the editor's
           line-number gutter immediately below it. */
        HBRUSH brush = CreateSolidBrush(g_previewDarkMode
            ? RGB(0x16, 0x1B, 0x22) : RGB(0xF6, 0xF8, 0xFA));
        FillRect(buffer, &client, brush);
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
            dis.hDC = buffer;
            dis.rcItem = itemRect;
            draw_tab_item(&dis);
        }

        BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        EndPaint(hwnd, &ps);
        return 0;
    } else if (msg == WM_LBUTTONDOWN) {
        TCHITTESTINFO hit = {0};
        hit.pt.x = GET_X_LPARAM(lParam); hit.pt.y = GET_Y_LPARAM(lParam);
        g_tabDragIndex = TabCtrl_HitTest(hwnd, &hit);
        g_tabDragged = 0;
        if (g_tabDragIndex >= 0) SetCapture(hwnd);
    } else if (msg == WM_MOUSEMOVE && g_tabDragIndex >= 0 && (wParam & MK_LBUTTON)) {
        TCHITTESTINFO hit = {0};
        hit.pt.x = GET_X_LPARAM(lParam); hit.pt.y = GET_Y_LPARAM(lParam);
        int target = TabCtrl_HitTest(hwnd, &hit);
        if (target >= 0 && target != g_tabDragIndex) {
            move_document_tab(g_tabDragIndex, target);
            g_tabDragIndex = target;
            g_tabDragged = 1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    } else if (msg == WM_LBUTTONUP || msg == WM_MBUTTONUP) {
        if (msg == WM_LBUTTONUP && g_tabDragIndex >= 0) {
            ReleaseCapture();
            g_tabDragIndex = -1;
            if (g_tabDragged) { g_tabDragged = 0; return 0; }
        }
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

    /* The process preference is selected before window creation; complete the
       per-window opt-in before attaching the menu so its first paint matches. */
    apply_menu_theme(hwnd, g_previewDarkMode);

    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(hwnd, hdc);

    if (!g_focusMode) SetMenu(hwnd, build_menu());
    create_toolbar(hwnd);

    g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd, (HMENU)ID_STATUS, GetModuleHandleW(NULL), NULL);
    SetWindowSubclass(g_hStatus, StatusSubclassProc, 1, 0);

    g_hOutline = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        0, 0, 0, 0, hwnd, (HMENU)ID_OUTLINE, GetModuleHandleW(NULL), NULL);

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

    /* No WS_EX_CLIENTEDGE on either pane: its sunken border is drawn in fixed
       system greys, which reads as a light seam against the dark theme. */
    g_hEdit = CreateWindowExA(0, "Scintilla", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL,
        0, 0, 0, 0, hwnd, (HMENU)ID_EDIT, GetModuleHandleA(NULL), NULL);
    SetWindowSubclass(g_hEdit, EditSubclassProc, 2, 0);
    configure_scintilla(g_hEdit);
    apply_editor_font(g_fontFaceName, g_fontPointSize);

    g_documentCount = 1;
    g_activeDocument = 0;
    memset(&g_documents[0], 0, sizeof(g_documents[0]));
    g_documents[0].sciDocument = (sptr_t)SendMessageA(g_hEdit, SCI_GETDOCPOINTER, 0, 0);
    SendMessageA(g_hEdit, SCI_ADDREFDOCUMENT, 0, (LPARAM)g_documents[0].sciDocument);
    /* The sole blank startup tab is the unnumbered canonical document. */
    g_documents[0].untitledNumber = 0;
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
    g_hPreview = CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd, (HMENU)ID_PREVIEW, GetModuleHandleA(NULL), NULL);

    init_webview();
    SetTimer(hwnd, IDT_FILE_WATCH, 1500, NULL);
    SetTimer(hwnd, IDT_AUTOSAVE, 5000, NULL);
    update_status_bar();
    update_outline();
    layout(hwnd);
}

static void handle_editor_notification(HWND hwnd, NMHDR *notification) {
    if (notification->hwndFrom != g_hEdit) return;

    struct SCNotification *scn = (struct SCNotification *)notification;
    if (notification->code == SCN_UPDATEUI) {
        if ((scn->updated & SC_UPDATE_V_SCROLL) && g_editorVisible && g_previewVisible) {
            if (g_ignoreNextEditorScroll) g_ignoreNextEditorScroll = 0;
            else SetTimer(hwnd, IDT_SCROLL_SYNC, 35, NULL);
        }
        update_brace_highlight();
        update_status_position();
        return;
    }
    if (notification->code == SCN_MARGINCLICK) {
        if (scn->margin == MARGIN_BOOKMARKS) {
            int line = (int)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)scn->position, 0);
            /* Debuggable files default the margin to breakpoints, but Shift+Click
               always reaches a bookmark - otherwise mouse-driven bookmarking would
               be silently unavailable for any Python/C/C++ file. */
            if (!(scn->modifiers & SCMOD_SHIFT) && is_debuggable_source(debug_source_kind(g_currentFile)))
                toggle_breakpoint_at_line(line);
            else
                toggle_bookmark_at_line(line);
        }
        return;
    }
    if (notification->code != SCN_MODIFIED) return;
    if (!(scn->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) return;

    if (!g_switchingDocument && !g_dirty) set_dirty(1);
    if (!g_switchingDocument) g_recoverySnapshotStale = 1;
    if (g_previewVisible) SetTimer(hwnd, IDT_PREVIEW_UPDATE, 150, NULL);
    schedule_spell_check();
    update_status_bar();
    SetTimer(hwnd, IDT_OUTLINE_UPDATE, 500, NULL);
    if (scn->linesAdded != 0) update_linenumber_margin_width(g_hEdit);
}

static void destroy_main_window(HWND hwnd) {
    save_layout_settings();
    KillTimer(hwnd, IDT_PREVIEW_UPDATE);
    KillTimer(hwnd, IDT_FILE_WATCH);
    KillTimer(hwnd, IDT_SCROLL_SYNC);
    KillTimer(hwnd, IDT_SPELL_CHECK);
    KillTimer(hwnd, IDT_AUTOSAVE);
    KillTimer(hwnd, IDT_OUTLINE_UPDATE);
    KillTimer(hwnd, IDT_PRINT_PREVIEW);
    stop_debug_location_tracking();
    if (g_webview && g_webMessageRegistered) {
        ICoreWebView2_remove_WebMessageReceived(g_webview, g_webMessageToken);
        g_webMessageRegistered = 0;
    }
    if (g_webviewController) ICoreWebView2Controller_Close(g_webviewController);
    if (g_spellChecker) { ISpellChecker_Release(g_spellChecker); g_spellChecker = NULL; }
    if (g_webview3) { ICoreWebView2_3_Release(g_webview3); g_webview3 = NULL; }
    if (g_webview) { ICoreWebView2_Release(g_webview); g_webview = NULL; }
    if (g_webviewController) { ICoreWebView2Controller_Release(g_webviewController); g_webviewController = NULL; }
    free(g_pendingHtmlBody);
    g_pendingHtmlBody = NULL;
    free(g_exportPath);
    g_exportPath = NULL;
    free(g_pendingPdfPath);
    g_pendingPdfPath = NULL;
    free(g_customCssPath);
    g_customCssPath = NULL;
    if (g_previewBkBrush) { DeleteObject(g_previewBkBrush); g_previewBkBrush = NULL; }
    if (g_mainBkBrush) { DeleteObject(g_mainBkBrush); g_mainBkBrush = NULL; }
    if (g_menuBkBrush) { DeleteObject(g_menuBkBrush); g_menuBkBrush = NULL; }
    if (g_menuFont) { DeleteObject(g_menuFont); g_menuFont = NULL; }
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
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lParam;
            if (mis->CtlType == ODT_MENU && mis->itemData) {
                measure_main_menu_item(mis);
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_MENU && dis->itemData) {
                draw_main_menu_item(dis);
                return TRUE;
            }
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
           area themselves, so this only ever shows through on the toolbar
           strip's margins and the splitter gap. */
        case WM_ERASEBKGND: {
            /* The class has no background brush, so this must always paint. */
            if (!g_mainBkBrush)
                g_mainBkBrush = CreateSolidBrush(
                    g_previewDarkMode ? RGB(0x0D, 0x11, 0x17) : RGB(0xFF, 0xFF, 0xFF));
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wParam, &rc, g_mainBkBrush);
            return 1;
        }
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
            if (wParam == IDT_SPELL_CHECK) {
                perform_spell_check();
                return 0;
            }
            if (wParam == IDT_AUTOSAVE) {
                write_recovery_snapshot();
                return 0;
            }
            if (wParam == IDT_OUTLINE_UPDATE) {
                KillTimer(hwnd, IDT_OUTLINE_UPDATE);
                update_outline();
                return 0;
            }
            if (wParam == IDT_PRINT_PREVIEW) {
                run_pending_print();
                return 0;
            }
            if (wParam == IDT_DEBUG_LOCATION) {
                poll_debug_location();
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
            /* Mouse moves arrive far faster than the panes can be resized, so
               skip the relayout when the splitter has not actually moved. */
            if (g_dragging && GET_X_LPARAM(lParam) != g_splitterPos) {
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
            if ((HWND)lParam == g_hOutline && HIWORD(wParam) == LBN_SELCHANGE) {
                int selected = (int)SendMessageW(g_hOutline, LB_GETCURSEL, 0, 0);
                if (selected >= 0 && selected < g_outlineCount) {
                    SendMessageA(g_hEdit, SCI_GOTOLINE, (WPARAM)g_outlineLines[selected], 0);
                    SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
                    SetFocus(g_hEdit);
                }
                return 0;
            }
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
            write_recovery_snapshot();
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

    INITCOMMONCONTROLSEX commonControls = { sizeof(commonControls), ICC_TAB_CLASSES | ICC_BAR_CLASSES };
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
    init_native_menu_theme_support();
    apply_menu_theme(NULL, g_previewDarkMode);
    load_layout_settings();
    load_find_settings();
    load_compiler_settings();
    if (g_commandLinePreviewMode) {
        /* File-association/command-line launches are preview-only for this
           session, without overwriting the user's normal panel visibility. */
        g_previewVisible = 1;
        g_editorVisible = 0;
    }
    load_recent_files();
    prune_missing_recent_files();

    HRESULT comResult = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(comResult)) init_spell_checker();

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
    /* No class brush: WM_ERASEBKGND paints the themed background itself, and
       COLOR_WINDOW would flash white behind a dark-mode window before it runs. */
    wc.hbrBackground = NULL;
    wc.hIcon = hIconBig;
    wc.hIconSm = hIconSmall;
    RegisterClassExW(&wc);

    int windowX = g_haveSavedWindowRect ? g_savedWindowRect.left : CW_USEDEFAULT;
    int windowY = g_haveSavedWindowRect ? g_savedWindowRect.top : CW_USEDEFAULT;
    int windowWidth = g_haveSavedWindowRect
        ? g_savedWindowRect.right - g_savedWindowRect.left : 1100;
    int windowHeight = g_haveSavedWindowRect
        ? g_savedWindowRect.bottom - g_savedWindowRect.top : 750;
    /* WS_CLIPCHILDREN stops the themed parent background repainting underneath
       the editor and preview panes, which otherwise flashes while resizing. */
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, L"MarkEditWndClass", L"Untitled.md - MarkEdit",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        windowX, windowY, windowWidth, windowHeight,
        NULL, NULL, hInstance, NULL);

    if (hIconBig) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    if (hIconSmall) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);

    if (cliFile) load_file(cliFile);
    else if (!restore_recovery_snapshot()) restore_session_documents();
    free(cliFile);

    ACCEL accels[] = {
        { FVIRTKEY | FCONTROL, 'N', IDM_FILE_NEW },
        { FVIRTKEY | FCONTROL, 'O', IDM_FILE_OPEN },
        { FVIRTKEY | FCONTROL, 'K', IDM_FILE_QUICK_OPEN },
        { FVIRTKEY | FCONTROL | FSHIFT, 'P', IDM_EDIT_COMMAND_PALETTE },
        { FVIRTKEY | FCONTROL, 'S', IDM_FILE_SAVE },
        { FVIRTKEY | FCONTROL, 'P', IDM_FILE_PRINT },
        { FVIRTKEY | FCONTROL | FSHIFT, 'S', IDM_FILE_SAVEAS },
        { FVIRTKEY | FCONTROL, 'A', IDM_EDIT_SELECTALL },
        { FVIRTKEY | FCONTROL, 'V', IDM_EDIT_PASTE },
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
        { FVIRTKEY, VK_F11, IDM_VIEW_FOCUS_MODE },
        { FVIRTKEY | FALT, 'Z', IDM_VIEW_WORD_WRAP },
        { FVIRTKEY | FCONTROL, 'D', IDM_EDIT_DUPLICATE_LINE },
        { FVIRTKEY | FALT, VK_UP, IDM_EDIT_MOVE_LINE_UP },
        { FVIRTKEY | FALT, VK_DOWN, IDM_EDIT_MOVE_LINE_DOWN },
        { FVIRTKEY | FCONTROL | FSHIFT, 'K', IDM_EDIT_DELETE_LINE },
        { FVIRTKEY | FCONTROL, 'J', IDM_EDIT_JOIN_LINES },
        { FVIRTKEY | FCONTROL, VK_OEM_2, IDM_EDIT_TOGGLE_COMMENT },
        { FVIRTKEY | FCONTROL | FSHIFT, 'D', IDM_EDIT_SELECT_NEXT },
        { FVIRTKEY | FCONTROL | FSHIFT, 'L', IDM_EDIT_SELECT_ALL_OCCURRENCES },
        { FVIRTKEY | FCONTROL, VK_OEM_6, IDM_EDIT_MATCH_BRACE },
        { FVIRTKEY | FCONTROL, VK_F5, IDM_RUN_FILE },
        { FVIRTKEY, VK_F5, IDM_DEBUG_FILE },
        { FVIRTKEY, VK_F6, IDM_DEBUG_CONTINUE },
        { FVIRTKEY, VK_F10, IDM_DEBUG_STEP_OVER },
        { FVIRTKEY | FSHIFT, VK_F11, IDM_DEBUG_STEP_INTO },
        { FVIRTKEY, VK_F9, IDM_DEBUG_BREAKPOINT },
        { FVIRTKEY | FCONTROL, VK_F2, IDM_EDIT_BOOKMARK_TOGGLE },
        { FVIRTKEY, VK_F2, IDM_EDIT_BOOKMARK_NEXT },
        { FVIRTKEY | FSHIFT, VK_F2, IDM_EDIT_BOOKMARK_PREV },
        { FVIRTKEY | FCONTROL, VK_OEM_PLUS, IDM_VIEW_ZOOM_IN },
        { FVIRTKEY | FCONTROL, VK_ADD, IDM_VIEW_ZOOM_IN },
        { FVIRTKEY | FCONTROL, VK_OEM_MINUS, IDM_VIEW_ZOOM_OUT },
        { FVIRTKEY | FCONTROL, VK_SUBTRACT, IDM_VIEW_ZOOM_OUT },
        { FVIRTKEY | FCONTROL, '0', IDM_VIEW_ZOOM_RESET },
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
