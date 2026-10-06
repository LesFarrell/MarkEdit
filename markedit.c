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
#include <dlgs.h> /* cmb1: the Font common dialog's font-name combo control ID, see do_choose_font() */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <spellcheck.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <dwmapi.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
/* Windows builds before 20H1 (19041) shipped the same attribute under this
   ordinal instead; harmless to set on newer builds since only one of the two
   actually takes effect there. */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1
#define DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 19
#endif
#include "editor_commands.h"
#include "editor_language.h"
#include "preview_fallback.h"
#include "file_explorer.h"
#include "file_io.h"
#include "recovery.h"
#include "help_reference.h"
#include "json_formatter.h"
#include "markdown.h"
#include "path_utils.h"
#include "settings.h"
#include "recent_list.h"
#include "text_codec.h"
#include "workspace_text.h"
#include "theme.h"
#include "themed_scrollbar.h"
#include "toolbar_icons.h"

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
static HWND g_hMinimap; /* zoomed-out Scintilla view sharing g_hEdit's document */
static HWND g_hSplitEdit; /* fully-interactive second view sharing g_hEdit's document, stacked below it */
static HWND g_lastEditView; /* most recently active/updated editor pane */
static HWND g_hDebugPanel; /* integrated debug controls and variables, shown while a session is active */
static HWND g_hDebugStatus;
static HWND g_hDebugVariables;
static HWND g_hDebugContinue;
static HWND g_hDebugStepOver;
static HWND g_hDebugStepInto;
static HWND g_hDebugStepOut;
static HWND g_hDebugClose;
HWND g_hMainWnd;
/* The main window's accelerator table, forwarded into from the preview's
   AcceleratorKeyPressed handler - see AccelKey_Invoke() - since disabling
   WebView2's own browser accelerators (AreBrowserAcceleratorKeysEnabled)
   stops it acting on them but does not, by itself, deliver them to the
   host's message loop. Set once in WinMain. */
static HACCEL g_hAccel = NULL;
/* Font face/weight/point-size: see settings.h for MAX_FONT_FACE_NAME's
   LF_FACESIZE-related commentary. Weight/italic are captured separately
   from the face name in do_choose_font() - a family that packages each
   weight as a same-named style (e.g. "Google Sans Code Monospaced" with a
   "Medium" style) needs the weight applied separately or the choice is
   silently lost to Regular on every reload. */
WCHAR g_fontFaceName[MAX_FONT_FACE_NAME] = L"Consolas";
int g_fontPointSize = 10;
int g_fontWeight = FW_NORMAL;
int g_fontItalic = 0;

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
static int g_previewShowingRawHtml = 0; /* true once show_html_file_preview() has navigated away from the shell page */
static WCHAR *g_expectedHtmlPreviewUrl = NULL; /* one-shot allowance for show_html_file_preview()'s own navigation; see NavStarting_Invoke */
static WCHAR *g_exportPath = NULL;
static unsigned long g_exportRequest = 0;
static WCHAR *g_pendingPdfPath = NULL;
typedef enum PrintAction {
    PRINT_ACTION_NONE = 0,
    PRINT_ACTION_DIALOG,
    PRINT_ACTION_EXPORT_PDF
} PrintAction;

static PrintAction g_pendingPrintAction = PRINT_ACTION_NONE;
int g_previewDarkMode = 0; /* the rendered Markdown preview's own theme, independent of the editor/app chrome below */
int g_editorDarkMode = 0; /* the Scintilla editor's colours, plus all surrounding app chrome (menus, dialogs, tabs, toolbar, title bar) */
/* CompilerBackendPreference is declared in settings.h; see load_compiler_settings()
   and find_native_compiler() further down. Auto keeps the existing GCC/Clang/
   Zig/MSVC priority search; the others pin Run/Debug to just one of them. */
CompilerBackendPreference g_compilerBackend = COMPILER_BACKEND_AUTO;
WCHAR *g_vcvarsallOverridePath = NULL; /* [Compiler] VcvarsallPath override; skips auto-detection */
WCHAR *g_gccBinOverride = NULL;   /* [Compiler] GccPath: folder to search for gcc.exe/g++.exe instead of PATH */
WCHAR *g_clangBinOverride = NULL; /* [Compiler] ClangPath: folder to search for clang.exe/clang++.exe instead of PATH */
WCHAR *g_zigBinOverride = NULL;   /* [Compiler] ZigPath: folder to search for zig.exe instead of PATH */
WCHAR *g_luaBinOverride = NULL;   /* [Compiler] LuaPath: folder to search for a Lua interpreter instead of PATH */
WCHAR *g_gdbBinOverride = NULL;   /* [Compiler] GdbPath: optional folder overriding bundled/PATH GDB */
WCHAR *g_compilerExtraFlags = NULL; /* [Compiler] ExtraFlags appended to Run/Debug C/C++ builds; set via Settings > Compiler */
WCHAR *g_customCssPath = NULL;
static WCHAR *g_debugLocationPath = NULL;
static WCHAR *g_debugWorkingDirectory = NULL;
static WCHAR *g_debugCommandPath = NULL;
static WCHAR *g_debugOutputPath = NULL;
static WCHAR *g_lastDebugSource = NULL;
static HANDLE g_debugProcess = NULL;
static HANDLE g_debugJob = NULL; /* owns the hidden GDB process tree so it cannot outlive MarkEdit */
static int g_debugLocationSeen = 0;
static int g_lastDebugLine = -1;
static int g_activeDebuggerKind = 0; /* 1 = pdb, 2 = GDB */
static int g_debugPanelVisible = 0;
static char *g_lastDebugState = NULL;
static void apply_debug_panel_theme(void);
static void restore_persisted_breakpoints(const WCHAR *path);
static void save_breakpoints_for_active_document(const WCHAR *path);
static void save_all_document_breakpoints(void);
#define MAX_DOCUMENTS RECOVERY_MAX_DOCUMENTS
typedef struct Document {
    sptr_t sciDocument;
    WCHAR *path;
    int dirty;
    int hasBom; /* the file has a byte-order mark for its encoding (UTF-8 or UTF-16) */
    int encoding; /* the TextEncoding the file is stored in; the editor holds UTF-8 */
    int encodingForced; /* chosen with Reopen with Encoding, so reloads keep it */
    FILETIME fileTime;
    int hasFileTime;
    int externalChanged;
    int externalDeleted;
    Sci_Position caret;
    Sci_Position anchor;
    Sci_Position firstVisibleLine;
    int xOffset;
    unsigned untitledNumber;
    int closedIndex;
    int isReference; /* 0, or a ReferenceKind for a Help page so Help reuses its tab */
    int largeFile; /* using large-file mode, either from open or after growing past the threshold */
} Document;

/* The Help pages opened as unsaved Markdown tabs (see show_help_page()). */
typedef enum { REFERENCE_MARKDOWN = 1, REFERENCE_SHORTCUTS } ReferenceKind;

static const WCHAR *reference_title(int kind) {
    return kind == REFERENCE_SHORTCUTS ? L"Keyboard Shortcuts" : L"Markdown Reference";
}

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

/* Documents a [[wiki-link]] jump was made from, most recent last, so
   "Go Back" can return to wherever the last jump started - see
   open_wikilink_target() and go_wiki_back(). Only documents with a saved
   path can be recorded; jumping from an unsaved document simply isn't
   undoable this way, the same limitation the recent-files list has. */
#define MAX_WIKI_BACK 10
static WCHAR *g_wikiBackStack[MAX_WIKI_BACK];
static int g_wikiBackCount = 0;

/* Most editor operations naturally target the selected tab. These aliases
   keep that code direct while the storage itself remains per-document. */
#define g_currentFile (g_documents[g_activeDocument].path)
#define g_dirty       (g_documents[g_activeDocument].dirty)
#define g_hasBom      (g_documents[g_activeDocument].hasBom)
#define g_encoding    (g_documents[g_activeDocument].encoding)
#define g_largeFile   (g_documents[g_activeDocument].largeFile)
/* LARGE_FILE_FEATURES: a file of at least this many MiB opens in large-file
   mode - streamed straight into a style-less, 64-bit-position Scintilla
   document (see read_large_file_document()). A smaller ordinary document
   that grows past the threshold also enters the same reduced-feature mode
   without destructively replacing its buffer or undo history. Every
   full-document feature that would otherwise rescan it on each pause in typing is switched off: lexing
   and folding, word wrap, preview, outline, word count, word completion,
   brace matching, smart Enter/Tab, and recovery snapshots. Plain editing, find/replace, and
   save still work. Persisted as [View] LargeFileThresholdMB. */
int g_largeFileThresholdMB = 64;
int g_previewVisible = 0;
int g_editorVisible = 1;
int g_previewOnLeft = 0;
int g_splitterRatio = 500; /* left pane width as thousandths of the available split width */
int g_splitterPos = -1; /* pixel x of splitter, initialized from g_splitterRatio */
int g_splitEditorVisible = 0;
int g_splitEditorRatio = 500; /* main pane height as thousandths of the editor region's height */
static int g_splitEditorPos = -1; /* pixel y of the split-editor splitter, local to the editor region */
static RECT g_splitEditorSplitterRect = {0};

static HWND tracked_edit_view(void) {
    if (g_lastEditView == g_hSplitEdit && g_hSplitEdit && g_splitEditorVisible)
        return g_hSplitEdit;
    return g_hEdit;
}
static RECT g_splitterRect = {0}; /* the main editor/preview splitter's own bounds, for drawing its line */
static int g_splitEditorRegionY = 0; /* window-relative y the editor region starts at, refreshed every layout() */
static int g_draggingSplitEditor = 0;
int g_commandLinePreviewMode = 0;
RECT g_savedWindowRect;
int g_haveSavedWindowRect = 0;
int g_startMaximized = 0;
/* File named on the command line, if any; handed off from WinMain to the
   deferred startup init that runs after the window is first shown (see
   run_deferred_startup_init()), and freed there. */
static WCHAR *g_pendingCliFile = NULL;
static int g_comInitialized = 0;
int g_focusMode = 0;
/* Markdown is prose: wrapping long paragraphs is the useful default. */
int g_wordWrap = 0;
int g_showWhitespace = 0;
int g_indentGuides = 0;
int g_indentUseSpaces = 0;
int g_tabWidth = 4;
int g_spellCheckEnabled = 1;
int g_outlineVisible = 0;
int g_minimapVisible = 1;
static int g_minimapDragging = 0;
static int g_minimapViewTop = 0; /* client-relative y (minimap coords) of the visible-region box */
static int g_minimapViewHeight = 0;
int g_editorZoom = 0; /* Scintilla SCI_SETZOOM units: +1/-1 per level, 0 is 100%. */
WCHAR g_spellLanguage[LOCALE_NAME_MAX_LENGTH] = L"";
static Sci_Position g_outlineLines[1024];
static int g_outlineCount = 0;
static int g_tabDragIndex = -1;
static int g_tabDragged = 0;
static int g_activeLanguage = 0; /* Language of the active document; 0 is LANG_MARKDOWN. */
static ISpellChecker *g_spellChecker = NULL;

/* The Shell progress dialog owns its own responsive UI while the caller
   performs synchronous streaming work. This keeps Scintilla access on its
   UI thread while still giving multi-gigabyte opens/saves progress and a
   working Cancel button. */
typedef struct LargeOperationProgress {
    IProgressDialog *dialog;
    int cancelled;
} LargeOperationProgress;

static void large_operation_progress_begin(LargeOperationProgress *progress,
                                           const WCHAR *title, const WCHAR *line) {
    memset(progress, 0, sizeof(*progress));
    if (!g_comInitialized || FAILED(CoCreateInstance(&CLSID_ProgressDialog, NULL,
            CLSCTX_INPROC_SERVER, &IID_IProgressDialog, (void **)&progress->dialog)) ||
            !progress->dialog) return;
    progress->dialog->lpVtbl->SetTitle(progress->dialog, title);
    progress->dialog->lpVtbl->SetLine(progress->dialog, 1, line, FALSE, NULL);
    progress->dialog->lpVtbl->SetCancelMsg(progress->dialog,
        L"Cancelling after the current block...", NULL);
    if (FAILED(progress->dialog->lpVtbl->StartProgressDialog(progress->dialog,
            g_hMainWnd, NULL, PROGDLG_MODAL | PROGDLG_AUTOTIME | PROGDLG_NOMINIMIZE, NULL))) {
        IUnknown_Release((IUnknown *)progress->dialog);
        progress->dialog = NULL;
    }
}

static void large_operation_progress_set_line(LargeOperationProgress *progress,
                                              const WCHAR *line) {
    if (!progress->dialog) return;
    progress->dialog->lpVtbl->SetLine(progress->dialog, 1, line, FALSE, NULL);
    progress->dialog->lpVtbl->SetProgress64(progress->dialog, 0, 1);
}

static int large_operation_progress_update(LargeOperationProgress *progress,
                                           size_t completed, size_t total) {
    if (!progress || !progress->dialog) return 1;
    progress->dialog->lpVtbl->SetProgress64(progress->dialog,
        (ULONGLONG)completed, (ULONGLONG)(total ? total : 1));
    if (progress->dialog->lpVtbl->HasUserCancelled(progress->dialog)) {
        progress->cancelled = 1;
        return 0;
    }
    return 1;
}

static int large_operation_progress_callback(size_t completed, size_t total, void *context) {
    return large_operation_progress_update((LargeOperationProgress *)context, completed, total);
}

static void large_operation_progress_end(LargeOperationProgress *progress) {
    if (!progress || !progress->dialog) return;
    progress->dialog->lpVtbl->StopProgressDialog(progress->dialog);
    IUnknown_Release((IUnknown *)progress->dialog);
    progress->dialog = NULL;
}

#define MAX_RECENT_FILES 10
static WCHAR *g_recentFiles[MAX_RECENT_FILES];
static int g_recentFileCount = 0;

#define MAX_RECENT_FOLDERS 10
static WCHAR *g_recentFolders[MAX_RECENT_FOLDERS];
static int g_recentFolderCount = 0;
static WCHAR *g_currentWorkspaceFolder = NULL;

static HWND g_hFindDlg = NULL;
static FINDREPLACEW g_fr;
WCHAR g_findWhat[256] = L"";
WCHAR g_replaceWith[256] = L"";
DWORD g_findFlags = FR_DOWN;
static UINT g_msgFindReplace;

/* ---- simple growable text buffer, used to pull text out of the Scintilla control ---- */
static char *get_edit_text(HWND hEdit, Sci_Position *outLen) {
    Sci_Position len = (Sci_Position)SendMessageA(hEdit, SCI_GETLENGTH, 0, 0);
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

static int position_needs_leading_eol(Sci_Position position) {
    if (position <= 0) return 0;
    int previous = (int)SendMessageA(g_hEdit, SCI_GETCHARAT, (WPARAM)(position - 1), 0);
    return previous != '\r' && previous != '\n';
}

/* ================= formatting toolbar actions (operate on g_hEdit) ================= */

/* Wrap the current selection in `open` ... `close`, toggling it off if the
   selection is already wrapped in exactly that pair. With no selection,
   insert an empty pair and place the caret between them so typing continues
   inside it. Only the selection itself is read (not the whole document). */
static void toolbar_wrap_pair(const char *open, const char *close) {
    Sci_Position selStart = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    int openLen = (int)strlen(open), closeLen = (int)strlen(close);

    if (selEnd == selStart) {
        char pair[48];
        snprintf(pair, sizeof(pair), "%s%s", open, close);
        SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)pair);
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)(selStart + openLen), (LPARAM)(selStart + openLen));
        SetFocus(g_hEdit);
        return;
    }

    Sci_Position selLen = selEnd - selStart;
    /* Points into Scintilla's own buffer; only valid until the next edit,
       so everything needed from it is copied out before SCI_REPLACESEL. */
    const char *selected = (const char *)SendMessageA(g_hEdit, SCI_GETRANGEPOINTER,
                                                      (WPARAM)selStart, (LPARAM)selLen);
    if (!selected) return;
    int alreadyWrapped = selLen >= openLen + closeLen &&
        memcmp(selected, open, (size_t)openLen) == 0 &&
        memcmp(selected + selLen - closeLen, close, (size_t)closeLen) == 0;

    char *replacement;
    Sci_Position newSelStart, newSelLen;
    if (alreadyWrapped) {
        newSelStart = selStart;
        newSelLen = selLen - openLen - closeLen;
        replacement = (char *)malloc((size_t)newSelLen + 1);
        if (!replacement) return;
        memcpy(replacement, selected + openLen, (size_t)newSelLen);
        replacement[newSelLen] = 0;
    } else {
        newSelStart = selStart + openLen;
        newSelLen = selLen;
        replacement = (char *)malloc((size_t)selLen + (size_t)openLen + (size_t)closeLen + 1);
        if (!replacement) return;
        memcpy(replacement, open, (size_t)openLen);
        memcpy(replacement + openLen, selected, (size_t)selLen);
        memcpy(replacement + openLen + selLen, close, (size_t)closeLen);
        replacement[openLen + selLen + closeLen] = 0;
    }
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)selStart, (LPARAM)selEnd);
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)replacement);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)newSelStart, (LPARAM)(newSelStart + newSelLen));
    free(replacement);
    SetFocus(g_hEdit);
}

/* Symmetric Markdown marker, e.g. "**" for bold. */
static void toolbar_wrap_inline(const char *marker) {
    toolbar_wrap_pair(marker, marker);
}

/* HTML tag pair: "<tag>" ... "</tag>". */
static void toolbar_wrap_tag(const char *tag) {
    char open[24], close[24];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    toolbar_wrap_pair(open, close);
}

/* How many bytes of `line` (of length lineLen) are one of the recognized
   line-prefix markers this toolbar manages (heading, quote, bullet, numbered,
   task). Returns 0 if none match. Used so re-clicking a different button on
   an already-prefixed line replaces the old prefix instead of stacking. */
static int toolbar_prefix_length(const char *line, Sci_Position lineLen) {
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
    Sci_Position firstLine, lastLine;
    editor_selected_line_range(g_hEdit, &firstLine, &lastLine);

    Sci_Position len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;

    /* Build the replacement text for the whole line range in one pass, then
       replace it in a single SCI_REPLACESEL so this is one undo step. */
    size_t cap = (size_t)len + (size_t)(lastLine - firstLine + 1) * 16 + 1;
    char *out = (char *)malloc(cap);
    if (!out) { free(text); return; }
    size_t o = 0;
    int number = 1;

    Sci_Position rangeStart = (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)firstLine, 0);
    Sci_Position rangeEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)lastLine, 0);

    /* For the numbered-list case, toggle the whole range off only if every
       line in it is already numbered (so clicking again removes numbering). */
    int allNumbered = 1;
    if (numbered) {
        for (Sci_Position line = firstLine; line <= lastLine && allNumbered; line++) {
            Sci_Position lineStart = (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
            Sci_Position lineEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)line, 0);
            const char *lineText = text + lineStart;
            Sci_Position lineLen = lineEnd - lineStart;
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
    for (Sci_Position line = firstLine; line <= lastLine; line++) {
        Sci_Position lineStart = (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
        Sci_Position lineEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)line, 0);
        Sci_Position lineLen = lineEnd - lineStart;
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
    Sci_Position selStart = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    Sci_Position len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;

    int hasSelection = selEnd > selStart;
    const char *middle = hasSelection ? text + selStart : placeholder;
    Sci_Position middleLen = hasSelection ? (selEnd - selStart) : (int)strlen(placeholder);

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
            Sci_Position urlStart = selStart + (int)beforeLen + middleLen + (int)(urlPlaceholder - after);
            SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)urlStart, (LPARAM)(urlStart + 3));
        } else if (!hasSelection) {
            Sci_Position caret = selStart + (int)beforeLen + middleLen + (int)afterLen;
            SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)caret, (LPARAM)caret);
        }
        free(whole);
    }
    free(text);
    SetFocus(g_hEdit);
}

/* Insert an HTML link around the selection (or a placeholder), then select
   the literal "url" inside the href attribute so it can be typed immediately
   - same idea as toolbar_insert_template, but the placeholder sits ahead of
   the wrapped text here since HTML puts the attribute before the content. */
static void toolbar_insert_html_link(void) {
    static const char before[] = "<a href=\"url\">";
    static const char after[] = "</a>";
    static const char placeholder[] = "link text";

    Sci_Position selStart = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    Sci_Position len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;

    int hasSelection = selEnd > selStart;
    const char *middle = hasSelection ? text + selStart : placeholder;
    Sci_Position middleLen = hasSelection ? (selEnd - selStart) : (int)strlen(placeholder);

    size_t beforeLen = sizeof(before) - 1, afterLen = sizeof(after) - 1;
    char *whole = (char *)malloc(beforeLen + (size_t)middleLen + afterLen + 1);
    if (whole) {
        memcpy(whole, before, beforeLen);
        memcpy(whole + beforeLen, middle, (size_t)middleLen);
        memcpy(whole + beforeLen + (size_t)middleLen, after, afterLen);
        whole[beforeLen + (size_t)middleLen + afterLen] = 0;
        SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)whole);

        Sci_Position urlStart = selStart + (int)(strstr(before, "url") - before);
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)urlStart, (LPARAM)(urlStart + 3));
        free(whole);
    }
    free(text);
    SetFocus(g_hEdit);
}

/* Insert an HTML image, using the selection (or a placeholder) as the alt
   text, then select the literal "url" inside the src attribute - same
   pattern as toolbar_insert_html_link, but <img> is a void element so the
   selection becomes an attribute value instead of wrapped content. */
static void toolbar_insert_html_image(void) {
    static const char before[] = "<img src=\"url\" alt=\"";
    static const char after[] = "\">";
    static const char placeholder[] = "alt text";

    Sci_Position selStart = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    Sci_Position len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;

    int hasSelection = selEnd > selStart;
    const char *middle = hasSelection ? text + selStart : placeholder;
    Sci_Position middleLen = hasSelection ? (selEnd - selStart) : (int)strlen(placeholder);

    size_t beforeLen = sizeof(before) - 1, afterLen = sizeof(after) - 1;
    char *whole = (char *)malloc(beforeLen + (size_t)middleLen + afterLen + 1);
    if (whole) {
        memcpy(whole, before, beforeLen);
        memcpy(whole + beforeLen, middle, (size_t)middleLen);
        memcpy(whole + beforeLen + (size_t)middleLen, after, afterLen);
        whole[beforeLen + (size_t)middleLen + afterLen] = 0;
        SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)whole);

        Sci_Position urlStart = selStart + (int)(strstr(before, "url") - before);
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)urlStart, (LPARAM)(urlStart + 3));
        free(whole);
    }
    free(text);
    SetFocus(g_hEdit);
}

static void toolbar_insert_table(void) {
    const char *eol = editor_eol_sequence();
    Sci_Position selStart = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);

    const char *leading = position_needs_leading_eol(selStart) ? eol : "";
    char table[168];
    snprintf(table, sizeof(table),
        "%s| Column 1 | Column 2 | Column 3 |%s"
        "| --- | --- | --- |%s"
        "| Cell | Cell | Cell |%s", leading, eol, eol, eol);
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)table);
    SetFocus(g_hEdit);
}

/* Insert a skeleton HTML <table> (one header row, one data row), same
   leading-newline handling as toolbar_insert_table but not selection-aware -
   an HTML table's cells aren't a natural fit for wrapping arbitrary selected
   text the way the list helper below wraps selected lines. */
static void toolbar_insert_html_table(void) {
    const char *eol = editor_eol_sequence();
    Sci_Position selStart = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);

    const char *leading = position_needs_leading_eol(selStart) ? eol : "";
    char table[320];
    snprintf(table, sizeof(table),
        "%s<table>%s"
        "  <tr>%s"
        "    <th>Column 1</th>%s"
        "    <th>Column 2</th>%s"
        "    <th>Column 3</th>%s"
        "  </tr>%s"
        "  <tr>%s"
        "    <td>Cell</td>%s"
        "    <td>Cell</td>%s"
        "    <td>Cell</td>%s"
        "  </tr>%s"
        "</table>%s",
        leading, eol, eol, eol, eol, eol, eol, eol, eol, eol, eol, eol, eol);
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)table);
    SetFocus(g_hEdit);
}

/* Wrap each selected line in <li>...</li> and the whole block in <ul>...
   </ul> (or <ol>...</ol> when `ordered`) - mirrors toolbar_apply_line_prefix's
   line-range handling, but for HTML markup rather than a Markdown prefix.
   With no selection, a skeleton with one placeholder item is inserted at the
   caret instead - unlike the line-prefix case, there's no existing line
   content worth preserving here, so this must insert rather than replace
   the current line (which may hold unrelated text). */
static void toolbar_insert_html_list(int ordered) {
    const char *eol = editor_eol_sequence();
    const char *tag = ordered ? "ol" : "ul";

    Sci_Position selStart = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);

    if (selEnd == selStart) {
        static const char *placeholder = "List item";
        char skeleton[96];
        int prefixLen = snprintf(skeleton, sizeof(skeleton), "<%s>%s  <li>", tag, eol);
        snprintf(skeleton + prefixLen, sizeof(skeleton) - (size_t)prefixLen,
                 "%s</li>%s</%s>", placeholder, eol, tag);
        SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)skeleton);
        Sci_Position placeholderStart = selStart + prefixLen;
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)placeholderStart,
                     (LPARAM)(placeholderStart + (int)strlen(placeholder)));
        SetFocus(g_hEdit);
        return;
    }

    Sci_Position firstLine, lastLine;
    editor_selected_line_range(g_hEdit, &firstLine, &lastLine);

    Sci_Position len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;

    size_t cap = (size_t)len + (size_t)(lastLine - firstLine + 1) * 32 + 64;
    char *out = (char *)malloc(cap);
    if (!out) { free(text); return; }
    size_t o = (size_t)snprintf(out, cap, "<%s>%s", tag, eol);

    for (Sci_Position line = firstLine; line <= lastLine; line++) {
        Sci_Position lineStart = (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
        Sci_Position lineEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)line, 0);
        Sci_Position lineLen = lineEnd - lineStart;
        o += (size_t)snprintf(out + o, cap - o, "  <li>");
        memcpy(out + o, text + lineStart, (size_t)lineLen);
        o += (size_t)lineLen;
        o += (size_t)snprintf(out + o, cap - o, "</li>%s", eol);
    }
    o += (size_t)snprintf(out + o, cap - o, "</%s>", tag);
    out[o] = 0;

    Sci_Position rangeStart = (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)firstLine, 0);
    Sci_Position rangeEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)lastLine, 0);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)rangeStart, (LPARAM)rangeEnd);
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)out);
    free(out);
    free(text);
    SetFocus(g_hEdit);
}

/* Wrap the current selection in a fenced code block (``` on its own line
   before and after). With no selection, insert an empty fence pair and
   place the caret right after the opening "```" so a language hint (e.g.
   "c") can be typed immediately. */
static void toolbar_insert_codeblock(void) {
    Sci_Position selStart = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    const char *eol = editor_eol_sequence();
    size_t eolLen = strlen(eol);
    const char *leading = position_needs_leading_eol(selStart) ? eol : "";
    size_t leadingLen = strlen(leading);

    if (selEnd == selStart) {
        char empty[36];
        snprintf(empty, sizeof(empty), "%s```%s%s```%s", leading, eol, eol, eol);
        SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)empty);
        Sci_Position caret = selStart + (int)leadingLen + 3;
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)caret, (LPARAM)caret);
        SetFocus(g_hEdit);
        return;
    }

    Sci_Position len;
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
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)(selStart + (int)openLen), (LPARAM)(selStart + (Sci_Position)openLen + (Sci_Position)selLen));
        free(wrapped);
    }
    free(text);
    SetFocus(g_hEdit);
}

static void toolbar_insert_horizontal_rule(void) {
    Sci_Position position = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    const char *eol = editor_eol_sequence();
    const char *leading = position_needs_leading_eol(position) ? eol : "";
    char block[24];
    snprintf(block, sizeof(block), "%s---%s", leading, eol);
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)block);
    SetFocus(g_hEdit);
}

static void toolbar_insert_alert(void) {
    Sci_Position selStart = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    const char *eol = editor_eol_sequence();
    Sci_Position len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;

    const char *body = selEnd > selStart ? text + selStart : "Alert text";
    Sci_Position bodyLen = selEnd > selStart ? selEnd - selStart : 10;
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
    for (Sci_Position i = 0; i < bodyLen; i++) {
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
    Sci_Position len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;
    int next = 1;
    for (Sci_Position i = 0; i + 3 < len; i++) {
        if (text[i] == '[' && text[i + 1] == '^' && text[i + 2] >= '0' && text[i + 2] <= '9') {
            int value = 0;
            Sci_Position j = i + 2;
            while (j < len && text[j] >= '0' && text[j] <= '9') value = value * 10 + (text[j++] - '0');
            if (j < len && text[j] == ']' && value >= next) next = value + 1;
        }
    }

    Sci_Position selStart = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    const char *note = selEnd > selStart ? text + selStart : "Footnote text";
    Sci_Position noteLen = selEnd > selStart ? selEnd - selStart : 13;
    const char *eol = editor_eol_sequence();
    char reference[32], prefix[48];
    snprintf(reference, sizeof(reference), "[^%d]", next);
    snprintf(prefix, sizeof(prefix), "%s%s[^%d]: ", eol, eol, next);

    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)reference);
    Sci_Position newLength = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)newLength, (LPARAM)newLength);
    SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)prefix);
    Sci_Position noteStart = (Sci_Position)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
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

/* Sets the EOL mode new lines are typed with to whichever line ending the
   freshly loaded document uses first (CRLF when it has none). Read back from
   Scintilla rather than the loaded bytes, so read_large_file_document()'s
   streamed load - which never holds the whole text in one buffer - gets the
   same answer as set_edit_text(). */
static void apply_detected_eol_mode(HWND hEdit) {
    Sci_Position length = (Sci_Position)SendMessageA(hEdit, SCI_GETLENGTH, 0, 0);
    Sci_Position lineEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETLINEENDPOSITION, 0, 0);
    int eolMode = SC_EOL_CRLF;
    if (lineEnd < length) {
        char first = (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)lineEnd, 0);
        char next = lineEnd + 1 < length
            ? (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)(lineEnd + 1), 0) : 0;
        if (first == '\n') eolMode = SC_EOL_LF;
        else eolMode = next == '\n' ? SC_EOL_CRLF : SC_EOL_CR;
    }
    SendMessageA(hEdit, SCI_SETEOLMODE, (WPARAM)eolMode, 0);
}

/* Replace the Scintilla control's entire contents with `length` bytes of
   `text`. A whole-document target replace rather than SCI_SETTEXT, which
   stops at the first NUL - a UTF-16 file can legitimately contain U+0000.
   It's still a single undoable change; callers loading a fresh document
   (New/Open) should follow up with SCI_EMPTYUNDOBUFFER so the previous
   document's content isn't reachable via Undo. */
static void set_edit_text(HWND hEdit, const char *text, Sci_Position length) {
    SendMessageA(hEdit, SCI_TARGETWHOLEDOCUMENT, 0, 0);
    SendMessageA(hEdit, SCI_REPLACETARGET, (WPARAM)length, (LPARAM)text);
    SendMessageA(hEdit, SCI_SETEMPTYSELECTION, 0, 0);
    apply_detected_eol_mode(hEdit);
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
/* Fixed point size for the minimap's copy of the editor font - deliberately
   independent of the user's chosen editor font size (and its zoom level) so
   the overview stays small and legible as an overview rather than scaling
   up along with the main editor's text. In hundredths of a point (see
   SCI_STYLESETSIZEFRACTIONAL/SC_FONT_SIZE_MULTIPLIER) so it can go below the
   smallest whole point size. */
#define MINIMAP_FONT_SIZE_HUNDREDTHS 150
/* Negative extra ascent/descent (see SCI_SETEXTRAASCENT/DESCENT) packs the
   minimap's lines closer together than the font's natural leading - Scintilla
   clamps ascent to a 1px and descent to a 0px floor, so this can't collapse
   lines into each other. Passed as plain negative ints below; the WPARAM
   round-trip preserves the sign (Scintilla reads it back via
   static_cast<int>(wParam)). */
#define MINIMAP_EXTRA_ASCENT -2
#define MINIMAP_EXTRA_DESCENT -3
#define MARKER_BOOKMARK 1
#define MARKER_BREAKPOINT 2
#define MARKER_DEBUG_LINE 3
#define MARKER_DEBUG_ARROW 4
#define FOLD_MARGIN_WIDTH 14
#define INDICATOR_SPELLING 20
static int g_lineNumberDigits = 0;
static int g_splitLineNumberDigits = 0; /* separate cache for g_hSplitEdit - see update_linenumber_margin_width() */

/* Widen/narrow the line-number margin to fit the current line count (so "9"
   and "9999" both have comfortably-sized gutters, and it grows as the
   document grows past another power of ten). Cached per-view (g_hEdit and
   g_hSplitEdit each get their own digit count remembered) so two views of
   the same document - which therefore usually recompute the same digit
   count on the same edit - don't cause one view's cache hit to skip the
   other view's still-needed SCI_SETMARGINWIDTHN call. */
static void update_linenumber_margin_width(HWND hEdit) {
    int *cache = (hEdit == g_hSplitEdit) ? &g_splitLineNumberDigits : &g_lineNumberDigits;
    Sci_Position lineCount = (Sci_Position)SendMessageA(hEdit, SCI_GETLINECOUNT, 0, 0);
    int digitCount = 1;
    for (Sci_Position n = lineCount; n >= 10; n /= 10) digitCount++;
    if (digitCount == *cache) return;
    *cache = digitCount;
    char digits[32];
    snprintf(digits, sizeof(digits), "_%lld", (long long)lineCount); /* leading '_' gives a little breathing room */
    int width = (int)SendMessageA(hEdit, SCI_TEXTWIDTH, STYLE_LINENUMBER, (LPARAM)digits);
    SendMessageA(hEdit, SCI_SETMARGINWIDTHN, MARGIN_LINENUMBERS, width);
}

/* apply_editor_theme() bolds several per-construct styles (Markdown headings/
   strong text, and various lexers' keywords) to make them stand out in the
   full-size editor. At the minimap's tiny fixed size (MINIMAP_FONT_SIZE_HUNDREDTHS)
   bold weight just reads as thick, smudged blocks rather than legible text,
   so every style is forced back to regular weight there after the theme is
   applied. */
static void strip_minimap_bold(void) {
    if (!g_hMinimap) return;
    for (int style = 0; style <= STYLE_MAX; style++)
        SendMessageA(g_hMinimap, SCI_STYLESETBOLD, (WPARAM)style, FALSE);
}

/* Apply GitHub's light/dark palette to the editor so it matches the preview.
   Called once at startup and whenever the selected theme changes. */
static void apply_editor_theme(HWND hEdit) {
    /* Scintilla draws its own client area but leaves the scrollbars to the
       system, which can't paint the track in the editor's background colour -
       so they're replaced with themed ones (see themed_scrollbar.h). */
    themed_scrollbar_refresh(hEdit);

    if (g_editorDarkMode) {
        SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_DEFAULT, THEME_DARK_FG);
        SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_DEFAULT, THEME_DARK_BG);
        SendMessageA(hEdit, SCI_STYLECLEARALL, 0, 0);

        SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_LINENUMBER, RGB(0x7D, 0x85, 0x90));
        SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_LINENUMBER, THEME_DARK_BG);

        SendMessageA(hEdit, SCI_SETCARETLINEBACK, RGB(0x21, 0x26, 0x2D), 0);
        SendMessageA(hEdit, SCI_SETCARETFORE, THEME_DARK_FG, 0);
        SendMessageA(hEdit, SCI_SETSELBACK, TRUE, RGB(0x26, 0x4F, 0x78));
        SendMessageA(hEdit, SCI_SETSELFORE, TRUE, RGB(0xFF, 0xFF, 0xFF));

        /* The word-completion popup (see update_word_completions()) is drawn
           by Scintilla itself via its Element colour API rather than styles,
           and otherwise defaults to plain system list-box colors (always
           white/black) regardless of editor theme. Mirrors STYLE_DEFAULT and
           SETSEL* above. Element colours carry an alpha byte (bits 24-31,
           unlike SCI_STYLESETFORE/BACK's plain COLORREF), hence the
           0xFF000000 for fully opaque. Its scrollbar is a separate, system-
           drawn native listbox these colours don't reach - see
           theme_autocomplete_popup(), applied each time the popup is shown. */
        SendMessageA(hEdit, SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST, (LPARAM)(THEME_DARK_FG | 0xFF000000));
        SendMessageA(hEdit, SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST_BACK, (LPARAM)(THEME_DARK_BG | 0xFF000000));
        SendMessageA(hEdit, SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST_SELECTED, (LPARAM)(RGB(0xFF, 0xFF, 0xFF) | 0xFF000000));
        SendMessageA(hEdit, SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST_SELECTED_BACK, (LPARAM)(RGB(0x26, 0x4F, 0x78) | 0xFF000000));
    } else {
        SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_DEFAULT, THEME_LIGHT_FG);
        SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_DEFAULT, THEME_LIGHT_BG);
        SendMessageA(hEdit, SCI_STYLECLEARALL, 0, 0);

        SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_LINENUMBER, RGB(0x65, 0x6D, 0x76));
        SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_LINENUMBER, RGB(0xF6, 0xF8, 0xFA));

        SendMessageA(hEdit, SCI_SETCARETLINEBACK, RGB(0xE8, 0xEC, 0xF1), 0);
        SendMessageA(hEdit, SCI_SETCARETFORE, RGB(0x1F, 0x23, 0x28), 0);
        SendMessageA(hEdit, SCI_SETSELBACK, TRUE, RGB(0xB6, 0xD7, 0xFF));
        SendMessageA(hEdit, SCI_SETSELFORE, TRUE, RGB(0x1F, 0x23, 0x28));

        SendMessageA(hEdit, SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST, (LPARAM)(RGB(0x1F, 0x23, 0x28) | 0xFF000000));
        SendMessageA(hEdit, SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST_BACK, (LPARAM)(RGB(0xFF, 0xFF, 0xFF) | 0xFF000000));
        SendMessageA(hEdit, SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST_SELECTED, (LPARAM)(RGB(0x1F, 0x23, 0x28) | 0xFF000000));
        SendMessageA(hEdit, SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST_SELECTED_BACK, (LPARAM)(RGB(0xB6, 0xD7, 0xFF) | 0xFF000000));
    }

    /* These follow SCI_STYLECLEARALL above, which resets every style. The
       gutter colours match STYLE_LINENUMBER so the margins read as one strip. */
    COLORREF faint = g_editorDarkMode ? RGB(0x6E, 0x76, 0x81) : RGB(0xAF, 0xB8, 0xC1);
    COLORREF gutter = g_editorDarkMode ? THEME_DARK_BG : RGB(0xF6, 0xF8, 0xFA);
    COLORREF text = theme_fg_color();

    SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_INDENTGUIDE, (LPARAM)faint);
    SendMessageA(hEdit, SCI_SETWHITESPACEFORE, TRUE, (LPARAM)faint);
    SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_BRACELIGHT, (LPARAM)text);
    SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_BRACELIGHT,
                 g_editorDarkMode ? RGB(0x3F, 0x46, 0x4F) : RGB(0xCF, 0xD8, 0xE3));
    SendMessageA(hEdit, SCI_STYLESETBOLD, STYLE_BRACELIGHT, TRUE);
    SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_BRACEBAD,
                 g_editorDarkMode ? RGB(0xFF, 0x7B, 0x72) : RGB(0xCF, 0x22, 0x2E));
    SendMessageA(hEdit, SCI_STYLESETBOLD, STYLE_BRACEBAD, TRUE);

    SendMessageA(hEdit, SCI_SETFOLDMARGINCOLOUR, TRUE, (LPARAM)gutter);
    SendMessageA(hEdit, SCI_SETFOLDMARGINHICOLOUR, TRUE, (LPARAM)gutter);
    for (int marker = SC_MARKNUM_FOLDEREND; marker <= SC_MARKNUM_FOLDEROPEN; marker++) {
        SendMessageA(hEdit, SCI_MARKERSETFORE, (WPARAM)marker, (LPARAM)gutter);
        SendMessageA(hEdit, SCI_MARKERSETBACK, (WPARAM)marker, (LPARAM)faint);
    }
    SendMessageA(hEdit, SCI_MARKERSETFORE, MARKER_BOOKMARK,
                 (LPARAM)(g_editorDarkMode ? RGB(0x1F, 0x6F, 0xEB) : RGB(0x09, 0x69, 0xDA)));
    SendMessageA(hEdit, SCI_MARKERSETBACK, MARKER_BOOKMARK,
                 (LPARAM)(g_editorDarkMode ? RGB(0x38, 0x8B, 0xFD) : RGB(0x21, 0x8B, 0xFF)));
    SendMessageA(hEdit, SCI_MARKERSETFORE, MARKER_BREAKPOINT, RGB(0xFF, 0xFF, 0xFF));
    SendMessageA(hEdit, SCI_MARKERSETBACK, MARKER_BREAKPOINT,
                 (LPARAM)(g_editorDarkMode ? RGB(0xF8, 0x51, 0x49) : RGB(0xCF, 0x22, 0x2E)));
    SendMessageA(hEdit, SCI_MARKERSETBACK, MARKER_DEBUG_LINE,
                 (LPARAM)(g_editorDarkMode ? RGB(0x9E, 0x6A, 0x03) : RGB(0xFF, 0xDF, 0x5D)));
    SendMessageA(hEdit, SCI_MARKERSETALPHA, MARKER_DEBUG_LINE, 110);
    SendMessageA(hEdit, SCI_MARKERSETFORE, MARKER_DEBUG_ARROW,
                 (LPARAM)(g_editorDarkMode ? THEME_DARK_BG : RGB(0xFF, 0xFF, 0xFF)));
    SendMessageA(hEdit, SCI_MARKERSETBACK, MARKER_DEBUG_ARROW,
                 (LPARAM)(g_editorDarkMode ? RGB(0xD2, 0x99, 0x22) : RGB(0x9A, 0x67, 0x00)));

    SendMessageA(hEdit, SCI_INDICSETSTYLE, INDICATOR_SPELLING, INDIC_SQUIGGLE);
    SendMessageA(hEdit, SCI_INDICSETFORE, INDICATOR_SPELLING,
                 (LPARAM)(g_editorDarkMode ? RGB(0xFF, 0x7B, 0x72) : RGB(0xCF, 0x22, 0x2E)));
    SendMessageA(hEdit, SCI_INDICSETUNDER, INDICATOR_SPELLING, TRUE);

    apply_language_styles(hEdit, (Language)g_activeLanguage, g_editorDarkMode);
    InvalidateRect(hEdit, NULL, TRUE);
}

/* Switches the editor to the lexer for `path`, reapplying the theme so the
   new styles get colours. Safe to call when the language is unchanged. A
   large-file document is always plain text: its document has no style
   storage to lex into (see read_large_file_document()). */
static void apply_document_language(HWND hEdit, const WCHAR *path) {
    const LanguageInfo *language = g_largeFile ? &LANGUAGES[LANG_TEXT] : language_for_path(path);
    g_activeLanguage = language->id;

    void *lexer = CreateLexer(language->lexer);
    /* NULL explicitly clears the previous lexer when switching to plain text. */
    SendMessageA(hEdit, SCI_SETILEXER, 0, (LPARAM)lexer);
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
    apply_word_wrap(hEdit);
    if (!g_largeFile) SendMessageA(hEdit, SCI_COLOURISE, 0, (LPARAM)-1);

    /* Style bytes live in the shared Scintilla document, but the colours
       assigned to those style numbers belong to each view. Keep the minimap's
       palette in step when the main view changes language (tab switches,
       Open, Save As, and large-file mode all come through here). It does not
       need its own lexer or a second colourising pass. */
    if (hEdit == g_hEdit && g_hMinimap) {
        apply_editor_theme(g_hMinimap);
        strip_minimap_bold();
    }
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

    BOOL supported = FALSE;
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
    if (g_largeFile) {
        KillTimer(g_hMainWnd, IDT_SPELL_CHECK);
        return;
    }
    SetTimer(g_hMainWnd, IDT_SPELL_CHECK, 350, NULL);
}

/* ISpellChecker_Check() below is a single synchronous COM call over the
   *entire* document (there's no incremental/ranged spellcheck API), run
   right on the UI thread with no chunking or background thread. That's
   unnoticeable at normal typing volume, since each keystroke rechecks a
   document that's already close to this size - but pasting a large block
   makes the document balloon in one step, and the native spellchecker is
   slow per byte, so the very next check after a big paste can block the UI
   thread (and the whole app's message pump) for a long, visible stretch.
   Skipping the check above this size trades spelling squiggles in huge
   documents for not freezing on every large paste. */
#define SPELLCHECK_MAX_LENGTH (256 * 1024)

static void perform_spell_check(void) {
    if (!g_hEdit) return;
    if (g_hMainWnd) KillTimer(g_hMainWnd, IDT_SPELL_CHECK);
    if (g_largeFile) return;

    Sci_Position textLength = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    SendMessageA(g_hEdit, SCI_SETINDICATORCURRENT, INDICATOR_SPELLING, 0);
    SendMessageA(g_hEdit, SCI_INDICATORCLEARRANGE, 0, (LPARAM)textLength);
    if (!g_spellCheckEnabled || !g_spellChecker ||
        (g_activeLanguage != LANG_MARKDOWN && g_activeLanguage != LANG_TEXT) ||
        textLength <= 0 || textLength > SPELLCHECK_MAX_LENGTH) return;

    /* Ensure the Markdown styles used to exclude code and links are current. */
    SendMessageA(g_hEdit, SCI_COLOURISE, 0, (LPARAM)-1);
    Sci_Position ignoredLength = 0;
    char *utf8 = get_edit_text(g_hEdit, &ignoredLength);
    if (!utf8) return;

    int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8, (int)textLength, NULL, 0);
    if (wideLength <= 0) {
        free(utf8);
        return;
    }
    WCHAR *wide = (WCHAR *)calloc((size_t)wideLength + 1, sizeof(WCHAR));
    if (!wide) {
        free(utf8);
        return;
    }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, (int)textLength, wide, wideLength);

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
                /* Plain text has no lexer at all (see the LANG_TEXT entry in
                   LANGUAGES) to distinguish code/links from prose the way
                   Markdown's does, so every character in it is spellable. */
                int spellable = g_activeLanguage == LANG_TEXT || markdown_style_is_prose(
                    (int)SendMessageA(g_hEdit, SCI_GETSTYLEAT, (WPARAM)byteStart, 0));
                if (byteSpan > 0 && byteStart >= 0 && byteStart + byteSpan <= textLength &&
                    spellable) {
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
    COLORREF colour = preview_bg_color();
    COREWEBVIEW2_COLOR bg = {255, GetRValue(colour), GetGValue(colour), GetBValue(colour)};
    ICoreWebView2Controller2_put_DefaultBackgroundColor(ctrl2, bg);
    ICoreWebView2Controller2_Release(ctrl2);
}

/* (Re)creates g_previewBkBrush to match g_previewDarkMode, so the preview
   host STATIC control's WM_CTLCOLORSTATIC background doesn't flash white/
   light when starting (or switching to) dark mode, before WebView2 itself
   has painted anything. */
static void apply_preview_host_theme(void) {
    if (g_previewBkBrush) DeleteObject(g_previewBkBrush);
    g_previewBkBrush = CreateSolidBrush(preview_bg_color());
    if (g_hPreview) InvalidateRect(g_hPreview, NULL, TRUE);
}

/* (Re)creates g_mainBkBrush to match g_editorDarkMode, and repaints the
   parts of the main window's own background it's used for (the toolbar
   strip and the splitter gap - everywhere else is covered by child windows). */
static void apply_main_theme(HWND hwnd) {
    if (g_mainBkBrush) DeleteObject(g_mainBkBrush);
    g_mainBkBrush = CreateSolidBrush(theme_bg_color());
    if (hwnd) InvalidateRect(hwnd, NULL, TRUE);

    /* The outline's content is owner-drawn (see draw_themed_listbox_item());
       its scrollbar is themed separately (see themed_scrollbar.h). */
    if (g_hOutline) {
        themed_scrollbar_refresh(g_hOutline);
        InvalidateRect(g_hOutline, NULL, TRUE);
    }
    apply_debug_panel_theme();
    explorer_apply_theme();
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

/* DarkMode_Explorer themes the menu bar's fill and its dropdowns, but the
   1px separator the non-client area draws along the menu bar's bottom edge
   stays a light system colour regardless - it isn't part of what that theme
   (or the ODT_MENU owner-draw for the item labels) covers. Painted over
   directly with the same dark fill used elsewhere, right after Windows
   finishes its own non-client painting. Only needed in dark mode; light
   mode's own separator already matches the rest of the light chrome. */
static void erase_menu_bottom_seam(HWND hwnd) {
    if (!g_editorDarkMode) return;
    RECT windowRect;
    if (!GetWindowRect(hwnd, &windowRect)) return;
    POINT clientTopLeft = {0, 0};
    if (!ClientToScreen(hwnd, &clientTopLeft)) return;
    RECT clientRect;
    if (!GetClientRect(hwnd, &clientRect)) return;
    int lineX = clientTopLeft.x - windowRect.left;
    int lineY = clientTopLeft.y - windowRect.top - 1;
    int lineWidth = clientRect.right - clientRect.left;
    if (lineX < 0 || lineY < 0 || lineWidth <= 0) return;
    HDC hdc = GetWindowDC(hwnd);
    if (!hdc) return;
    RECT lineRect = {lineX, lineY, lineX + lineWidth, lineY + 1};
    fill_rect_color(hdc, &lineRect, THEME_DARK_BG);
    ReleaseDC(hwnd, hdc);
}

/* Dark/light title bar and border for any top-level window: the per-window
   uxtheme opt-in, the DWM caption attribute (which DWM ignores until the
   frame is told to repaint, hence the SWP_FRAMECHANGED nudge), and the
   scrollbar/explorer theme. Used for the main window by apply_menu_theme()
   below and on its own for the popup windows (Go to Line, the searchable
   picker, the plain text prompt), which have no menu bar. */
static void apply_popup_titlebar_theme(HWND hwnd, int dark) {
    if (!hwnd) return;
    if (g_allowDarkModeForWindow) g_allowDarkModeForWindow(hwnd, dark ? TRUE : FALSE);
    BOOL useDark = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                      &useDark, sizeof(useDark))))
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1,
                               &useDark, sizeof(useDark));
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

/* Switches the menu bar, its dropdowns, and the window's title bar/border
   between light and dark chrome. FORCEDARK/FORCELIGHT are intentional: the
   app has its own theme selector, so the selected menu colour must not
   depend on the Windows system theme. The per-window opt-in is what lets
   the non-client menu bar follow that process preference. Safe on older
   Windows releases where the private uxtheme exports (or DWMWA_USE_
   IMMERSIVE_DARK_MODE) are absent; those releases retain their system
   chrome. */
static void apply_menu_theme(HWND hwnd, int dark) {
    if (g_setPreferredAppMode)
        g_setPreferredAppMode(dark ? UXTHEME_APPMODE_FORCEDARK : UXTHEME_APPMODE_FORCELIGHT);
    if (hwnd && g_allowDarkModeForWindow)
        g_allowDarkModeForWindow(hwnd, dark ? TRUE : FALSE);
    if (g_flushMenuThemes)
        g_flushMenuThemes();
    if (hwnd) {
        apply_popup_titlebar_theme(hwnd, dark);

        if (g_menuBkBrush) DeleteObject(g_menuBkBrush);
        g_menuBkBrush = CreateSolidBrush(dark ? THEME_DARK_BG : THEME_LIGHT_BG);
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
        erase_menu_bottom_seam(hwnd);
    }
}

/* Fills one splitter gap's rect with the border colour so it reads as a
   draggable divider instead of just more of the pane background on either
   side of it - shared by every splitter's WM_ERASEBKGND painting (main,
   explorer, split-editor). A zeroed/empty rect (the splitter's own "not
   currently shown" state) is a no-op. */
static void paint_splitter_gap(HDC hdc, const RECT *rect) {
    if (rect->right <= rect->left || rect->bottom <= rect->top) return;
    fill_rect_color(hdc, rect, theme_border_color());
}

/* One shared background brush for whichever of these popup dialogs
   (show_goto_line_dialog()/show_choice_dialog()/show_text_input()/the
   Find & Replace review list/the Settings dialog) are currently open. Most
   of them run their own modal message loop and never nest, but the Settings
   dialog's Compiler category and its spell-check language picker (Choose...)
   can open one of these ON TOP of another already-open one (Settings itself
   opening the Choice dialog) - g_popupThemeDepth counts how many are
   currently open so the brush is only actually torn down once the last one
   closes, rather than being pulled out from under a still-open outer dialog
   the moment a nested one exits. popup_dialog_theme_created() itself stays a
   plain "make sure the themed brush exists right now" call with no bearing
   on that count, since refresh_settings_dialog_theme() also calls it to
   recolor an already-open dialog's brush without opening a new one. */
static HBRUSH g_popupBkBrush = NULL;
static int g_popupThemeDepth = 0;

static void popup_dialog_theme_created(HWND hwnd) {
    apply_popup_titlebar_theme(hwnd, g_editorDarkMode);
    if (g_popupBkBrush) DeleteObject(g_popupBkBrush);
    g_popupBkBrush = CreateSolidBrush(theme_bg_color());
}

/* Call once per popup window actually opened (WM_CREATE), paired with
   popup_dialog_theme_destroyed() at that same window's WM_NCDESTROY. */
static void popup_dialog_theme_opened(HWND hwnd) {
    g_popupThemeDepth++;
    popup_dialog_theme_created(hwnd);
}

static void popup_dialog_theme_destroyed(void) {
    if (g_popupThemeDepth > 0) g_popupThemeDepth--;
    if (g_popupThemeDepth == 0 && g_popupBkBrush) {
        DeleteObject(g_popupBkBrush);
        g_popupBkBrush = NULL;
    }
}

/* Defined further down (with the outline panel's owner-draw, which they're
   shared with); forward-declared here since ChoiceWndProc, above the
   outline code in the file, needs them for its own result listbox. */
static void measure_themed_listbox_item(HWND listbox, MEASUREITEMSTRUCT *mis);
static void draw_themed_listbox_item(HWND listbox, const DRAWITEMSTRUCT *dis);

/* Shared WM_ERASEBKGND/WM_CTLCOLORSTATIC/WM_CTLCOLORBTN/WM_CTLCOLOREDIT/
   WM_CTLCOLORLISTBOX handling for the popup dialogs below - factored out since they all need
   identical theming for their STATIC labels, EDIT fields, and checkbox/radio
   buttons. Returns the brush/TRUE the caller should return from its WndProc,
   or 0 if msg wasn't handled. */
static LRESULT popup_dialog_theme_message(HWND hwnd, UINT msg, WPARAM wParam, LRESULT *handled) {
    *handled = 1;
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, g_popupBkBrush ? g_popupBkBrush : (HBRUSH)(COLOR_BTNFACE + 1));
        return 1;
    }
    /* Checkboxes send WM_CTLCOLORSTATIC for their label; radio buttons send
       WM_CTLCOLORBTN instead - both need the same themed text colour, or the
       radio labels stay stuck at the system default (near-black) in dark mode. */
    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN) {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, theme_fg_color());
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)(g_popupBkBrush ? g_popupBkBrush : (HBRUSH)(COLOR_BTNFACE + 1));
    }
    /* Owner-drawn listboxes still ask for WM_CTLCOLORLISTBOX to paint
       whatever their items don't cover - the blank area below the last row,
       or the whole box when there are only a few rows - so without it that
       space stays the default white regardless of theme. */
    if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, theme_fg_color());
        SetBkColor(hdc, theme_bg_color());
        return (LRESULT)(g_popupBkBrush ? g_popupBkBrush : (HBRUSH)(COLOR_BTNFACE + 1));
    }
    *handled = 0;
    return 0;
}

/* Owner-drawn OK/Cancel-style push button matching the popup dialogs'
   theme - standard BUTTON controls are visual-styles-drawn regardless of
   WM_CTLCOLORBTN, so getting a themed fill/border needs BS_OWNERDRAW. */
static void draw_popup_button(const DRAWITEMSTRUCT *dis) {
    int pressed = (dis->itemState & ODS_SELECTED) != 0;
    int isDefault = (GetWindowLongW(dis->hwndItem, GWL_STYLE) & BS_DEFPUSHBUTTON) != 0;
    COLORREF fill = g_editorDarkMode
        ? (pressed ? THEME_DARK_BORDER : THEME_DARK_BG)
        : (pressed ? RGB(0xE1, 0xE7, 0xEC) : RGB(0xF6, 0xF8, 0xFA));
    fill_rect_color(dis->hDC, &dis->rcItem, fill);

    HPEN pen = CreatePen(PS_SOLID, isDefault ? 2 : 1, isDefault ? theme_accent_color() : theme_border_color());
    HPEN oldPen = (HPEN)SelectObject(dis->hDC, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
    Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
    SelectObject(dis->hDC, oldBrush);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(pen);

    WCHAR text[128];
    GetWindowTextW(dis->hwndItem, text, 128);
    SetTextColor(dis->hDC, theme_fg_color());
    SetBkMode(dis->hDC, TRANSPARENT);
    RECT textRect = dis->rcItem;
    DrawTextW(dis->hDC, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* Wrapped lines continue with a hanging indent so a wrapped list item or
   blockquote stays visually attached to its marker. Wrap mode belongs to
   the view, not the document, so this is reapplied from
   apply_document_language() whenever the active document changes:
   wrapping a large-file document would lay out every line of it up front. */
static void apply_word_wrap(HWND hEdit) {
    int wrap = g_wordWrap && !g_largeFile;
    SendMessageA(hEdit, SCI_SETWRAPMODE, wrap ? SC_WRAP_WORD : SC_WRAP_NONE, 0);
    if (!wrap) return;
    SendMessageA(hEdit, SCI_SETWRAPINDENTMODE, SC_WRAPINDENT_SAME, 0);
    SendMessageA(hEdit, SCI_SETWRAPVISUALFLAGS, SC_WRAPVISUALFLAG_END, 0);
}

static void apply_show_whitespace(HWND hEdit) {
    SendMessageA(hEdit, SCI_SETVIEWWS, g_showWhitespace ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE, 0);
}

static void apply_indent_guides(HWND hEdit) {
    SendMessageA(hEdit, SCI_SETINDENTATIONGUIDES, g_indentGuides ? SC_IV_LOOKBOTH : SC_IV_NONE, 0);
}

static void apply_indentation(HWND hEdit) {
    SendMessageA(hEdit, SCI_SETUSETABS, g_indentUseSpaces ? FALSE : TRUE, 0);
    SendMessageA(hEdit, SCI_SETTABWIDTH, g_tabWidth, 0);
}

static void configure_scintilla(HWND hEdit) {
    themed_scrollbar_attach(hEdit, THEMED_SCROLL_SCINTILLA);
    SendMessageA(hEdit, SCI_SETCODEPAGE, SC_CP_UTF8, 0);
    SendMessageA(hEdit, SCI_SETEOLMODE, SC_EOL_CRLF, 0);
    apply_indentation(hEdit);
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
                 (LPARAM)((1 << MARKER_BOOKMARK) | (1 << MARKER_BREAKPOINT) |
                          (1 << MARKER_DEBUG_ARROW)));
    SendMessageA(hEdit, SCI_SETMARGINWIDTHN, MARGIN_BOOKMARKS, 14);
    SendMessageA(hEdit, SCI_SETMARGINSENSITIVEN, MARGIN_BOOKMARKS, TRUE);
    SendMessageA(hEdit, SCI_MARKERDEFINE, MARKER_BOOKMARK, SC_MARK_BOOKMARK);
    SendMessageA(hEdit, SCI_MARKERDEFINE, MARKER_BREAKPOINT, SC_MARK_CIRCLE);
    SendMessageA(hEdit, SCI_MARKERDEFINE, MARKER_DEBUG_LINE, SC_MARK_BACKGROUND);
    SendMessageA(hEdit, SCI_MARKERDEFINE, MARKER_DEBUG_ARROW, SC_MARK_SHORTARROW);

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

    /* Word completion (see update_word_completions()): matching is
       case-insensitive, the list we build is already sorted so Scintilla
       doesn't need to re-sort it, and the popup auto-hides once nothing
       typed so far matches any entry. */
    SendMessageA(hEdit, SCI_AUTOCSETIGNORECASE, TRUE, 0);
    SendMessageA(hEdit, SCI_AUTOCSETAUTOHIDE, TRUE, 0);
    SendMessageA(hEdit, SCI_AUTOCSETORDER, SC_ORDER_PRESORTED, 0);
    SendMessageA(hEdit, SCI_AUTOCSETMAXHEIGHT, 8, 0);
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
    SendMessageA(g_hEdit, SCI_STYLESETWEIGHT, STYLE_DEFAULT, g_fontWeight);
    SendMessageA(g_hEdit, SCI_STYLESETITALIC, STYLE_DEFAULT, g_fontItalic);
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

    /* The minimap is a second view onto the same document, so it keeps its
       own copy of every style and needs the same font/theme applied - but at
       a fixed small size (see MINIMAP_FONT_SIZE_HUNDREDTHS) rather than the
       editor's own point size, so it doesn't grow along with it. Zoom stays
       at 0: sizing is done via STYLESETSIZEFRACTIONAL alone. */
    if (g_hMinimap) {
        SendMessageA(g_hMinimap, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)faceUtf8);
        SendMessageA(g_hMinimap, SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, MINIMAP_FONT_SIZE_HUNDREDTHS);
        SendMessageA(g_hMinimap, SCI_STYLECLEARALL, 0, 0);
        apply_editor_theme(g_hMinimap);
        strip_minimap_bold();
        SendMessageA(g_hMinimap, SCI_SETZOOM, 0, 0);
        update_minimap_viewport();
    }

    /* The split pane is a second fully-interactive view onto the same
       document (unlike the minimap), so it gets the real point size, not a
       fixed fraction of it. */
    if (g_hSplitEdit) {
        SendMessageA(g_hSplitEdit, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)faceUtf8);
        SendMessageA(g_hSplitEdit, SCI_STYLESETSIZE, STYLE_DEFAULT, pointSize);
        SendMessageA(g_hSplitEdit, SCI_STYLESETWEIGHT, STYLE_DEFAULT, g_fontWeight);
        SendMessageA(g_hSplitEdit, SCI_STYLESETITALIC, STYLE_DEFAULT, g_fontItalic);
        SendMessageA(g_hSplitEdit, SCI_STYLECLEARALL, 0, 0);
        apply_editor_theme(g_hSplitEdit);
        SendMessageA(g_hSplitEdit, SCI_STYLESETFONT, STYLE_LINENUMBER, (LPARAM)faceUtf8);
        SendMessageA(g_hSplitEdit, SCI_STYLESETSIZE, STYLE_LINENUMBER, pointSize);
        g_splitLineNumberDigits = 0; /* font metrics changed even if the line count did not */
        update_linenumber_margin_width(g_hSplitEdit);
    }
    free(faceUtf8);
}

/* ---- autosave recovery -------------------------------------------------
   Recovery snapshots live in LocalAppData rather than beside the executable,
   so running the portable distribution never dirties its own folder. Only
   modified documents are written; normal saves and a clean exit remove them. */

/* %LOCALAPPDATA%\MarkEdit\<name>, created if needed: per-user, machine-local
   state that has to be writable wherever markedit.exe itself lives -
   including a Program Files install a standard user can't write to. */
static WCHAR *get_local_app_directory(const WCHAR *name) {
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
    WCHAR *directory = path_join(app, name);
    free(app);
    if (directory) CreateDirectoryW(directory, NULL);
    return directory;
}

static WCHAR *get_recovery_directory(void) {
    return get_local_app_directory(L"Recovery");
}

static char *get_document_text(int index, Sci_Position *outLength) {
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

static int clear_recovery_snapshot(void) {
    WCHAR *directory = get_recovery_directory();
    int ok = directory && recovery_clear(directory);
    free(directory);
    return ok;
}

static char *read_recovery_editor_text(void *context, int index, size_t *outLength) {
    (void)context;
    Sci_Position length = 0;
    char *text = get_document_text(index, &length);
    *outLength = (size_t)length;
    return text;
}

static void write_recovery_snapshot(void) {
    if (!g_hEdit || g_commandLinePreviewMode || !g_recoverySnapshotStale) return;
    RecoveryDocument documents[RECOVERY_MAX_DOCUMENTS];
    int count = 0;
    for (int i = 0; i < g_documentCount; i++) {
        Document *doc = &g_documents[i];
        if (!doc->dirty || doc->largeFile) continue;
        documents[count++] = (RecoveryDocument){ doc->path, doc->untitledNumber,
            doc->hasBom, (TextEncoding)doc->encoding, i };
    }
    WCHAR *directory = get_recovery_directory();
    if (directory && recovery_write(directory, documents, count, read_recovery_editor_text, NULL))
        g_recoverySnapshotStale = 0;
    free(directory);
    update_status_bar();
}

static int restore_recovery_snapshot(void) {
    WCHAR *directory = get_recovery_directory();
    if (!directory) return 0;
    recovery_cleanup(directory);
    int count = recovery_count(directory);
    if (count <= 0 || count > MAX_DOCUMENTS) {
        free(directory); return 0;
    }
    int answer = MessageBoxW(g_hMainWnd,
        L"MarkEdit found autosaved documents from an interrupted session. Recover them?",
        L"Recover Documents", MB_YESNO | MB_ICONQUESTION);
    if (answer != IDYES) {
        free(directory);
        clear_recovery_snapshot();
        return 0;
    }

    int restored = 0;
    for (int i = 0; i < count; i++) {
        RecoveryEntry entry;
        if (!recovery_read(directory, i, &entry)) continue;
        char *text = entry.text;
        int length = entry.length;
        if (restored > 0 && !create_document_tab()) { recovery_entry_free(&entry); break; }
        if (restored == 0 && !active_document_is_reusable() && !create_document_tab()) {
            recovery_entry_free(&entry); break;
        }
        g_switchingDocument = 1;
        set_edit_text(g_hEdit, text, length);
        SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
        g_switchingDocument = 0;
        replace_wstring(&g_currentFile, entry.path[0] ? entry.path : NULL);
        g_documents[g_activeDocument].untitledNumber = entry.untitledNumber;
        g_hasBom = entry.hasBom;
        g_encoding = entry.encoding;
        recovery_entry_free(&entry);
        apply_document_language(g_hEdit, g_currentFile);
        if (g_hSplitEdit) apply_document_language(g_hSplitEdit, g_currentFile);
        /* Matches load_file_as(): a recovered tab keeps the breakpoints that
           were persisted for the file it was snapshotted from. */
        restore_persisted_breakpoints(g_currentFile);
        set_dirty(1);
        restored++;
    }
    free(directory);

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
    HWND view = tracked_edit_view();
    Sci_Position position = (Sci_Position)SendMessageA(view, SCI_GETCURRENTPOS, 0, 0);
    Sci_Position line = (Sci_Position)SendMessageA(view, SCI_LINEFROMPOSITION, (WPARAM)position, 0) + 1;
    Sci_Position column = (Sci_Position)SendMessageA(view, SCI_GETCOLUMN, (WPARAM)position, 0) + 1;
    swprintf(g_statusPartText[0], 128, L"Ln %lld, Col %lld", (long long)line, (long long)column);
    int eol = (int)SendMessageA(view, SCI_GETEOLMODE, 0, 0);
    const WCHAR *eolName = eol == SC_EOL_LF ? L"LF" : eol == SC_EOL_CR ? L"CR" : L"CRLF";
    WCHAR encodingName[40];
    switch (g_encoding) {
        case TEXT_ENCODING_UTF16LE: wcscpy(encodingName, L"UTF-16 LE"); break;
        case TEXT_ENCODING_UTF16BE: wcscpy(encodingName, L"UTF-16 BE"); break;
        case TEXT_ENCODING_ANSI:
            swprintf(encodingName, 40, L"ANSI (CP %u)", text_ansi_code_page());
            break;
        default: wcscpy(encodingName, L"UTF-8"); break;
    }
    swprintf(g_statusPartText[2], 128, L"%ls%ls   %ls", encodingName, g_hasBom ? L" BOM" : L"", eolName);
    swprintf(g_statusPartText[3], 128, L"%ls", !g_dirty ? L"Saved"
             : g_largeFile ? L"Modified" : L"Modified · recovery enabled");
    InvalidateRect(g_hStatus, NULL, FALSE);
}

static void update_status_bar(void) {
    update_status_position();
    if (!g_hStatus || !g_hEdit || g_documentCount <= 0) return;
    if (g_largeFile) {
        double mib = (double)(Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0) / (1024.0 * 1024.0);
        swprintf(g_statusPartText[1], 128, L"Large file mode   %.1f MB", mib);
        InvalidateRect(g_hStatus, NULL, FALSE);
        return;
    }
    Sci_Position length = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    Sci_Position characters = (Sci_Position)SendMessageA(g_hEdit, SCI_COUNTCHARACTERS, 0, (LPARAM)length);
    Sci_Position ignored = 0;
    int words = 0, inWord = 0;
    char *text = get_edit_text(g_hEdit, &ignored);
    if (text) {
        for (Sci_Position i = 0; i < length; i++) {
            unsigned char ch = (unsigned char)text[i];
            int wordChar = ch >= 0x80 || isalnum(ch) || ch == '_' || ch == '\'';
            if (wordChar && !inWord) words++;
            inWord = wordChar;
        }
        free(text);
    }
    swprintf(g_statusPartText[1], 128, L"Words %d   Characters %lld", words, (long long)characters);
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
        fill_rect_color(dc, &client, theme_bg_color());
        SetTextColor(dc, theme_fg_color());
        SetBkMode(dc, TRANSPARENT);
        for (int i = 0; i < 4; i++) {
            RECT partRect = {0};
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

/* ---- recently opened files and workspace folders, most-recent-first,
   stored in the same INI. Both lists share one implementation; only the
   backing array, its cap, and the INI section/key names differ. ---- */
static const RecentList g_recentFileList = {g_recentFiles, &g_recentFileCount, MAX_RECENT_FILES,
                                            L"RecentFiles", L"File"};
static const RecentList g_recentFolderList = {g_recentFolders, &g_recentFolderCount, MAX_RECENT_FOLDERS,
                                              L"RecentFolders", L"Folder"};

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
            recent_list_remove(&g_recentFileList, g_recentFiles[i]);
        } else {
            i++;
        }
    }
}

static void load_workspace_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WCHAR *value = read_profile_string(L"Workspace", L"CurrentFolder", path);
    if (value) replace_wstring(&g_currentWorkspaceFolder, value);
    free(value);
    free(path);
}

static void save_workspace_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WritePrivateProfileStringW(L"Workspace", L"CurrentFolder",
                               g_currentWorkspaceFolder ? g_currentWorkspaceFolder : L"", path);
    free(path);
}



/* ================= WebView2 host page ================= */

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
    if (!g_webviewReady || !g_webview || g_previewShowingRawHtml) return;
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

static HRESULT STDMETHODCALLTYPE AccelKeyHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out,
        &IID_ICoreWebView2AcceleratorKeyPressedEventHandler);
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
    if (!g_exportPath) return S_OK;
    KillTimer(g_hMainWnd, IDT_HTML_EXPORT);
    WCHAR *html = SUCCEEDED(errorCode) ? decode_json_string(result) : NULL;
    char *utf8 = html ? to_utf8(html) : NULL;
    int ok = 0;
    if (utf8 && *utf8 && g_exportPath) {
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
/* ExecuteScript reports only whether the asynchronous export was started.
   The resolved HTML arrives through WebMessage_Invoke. */
static HRESULT STDMETHODCALLTYPE ExportStarted_Invoke(ICoreWebView2ExecuteScriptCompletedHandler *this_,
                                                       HRESULT errorCode, LPCWSTR result) {
    if (FAILED(errorCode) || !result || wcscmp(result, L"true") != 0)
        return ExportDone_Invoke(this_, E_FAIL, NULL);
    return S_OK;
}
static ICoreWebView2ExecuteScriptCompletedHandlerVtbl g_exportScriptVtbl = {
    (void*)ScriptHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)ExportStarted_Invoke
};
static ICoreWebView2ExecuteScriptCompletedHandler g_exportScriptHandler = { &g_exportScriptVtbl };

static HRESULT STDMETHODCALLTYPE PdfHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out, &IID_ICoreWebView2PrintToPdfCompletedHandler);
}

static HRESULT STDMETHODCALLTYPE PdfDone_Invoke(ICoreWebView2PrintToPdfCompletedHandler *this_,
                                                 HRESULT errorCode, BOOL result) {
    (void)this_;
    if (FAILED(errorCode) || !result)
        MessageBoxW(g_hMainWnd, L"Could not export the rendered preview to PDF.",
                    L"MarkEdit", MB_OK | MB_ICONERROR);
    else
        MessageBoxW(g_hMainWnd, L"PDF export completed.", L"MarkEdit",
                    MB_OK | MB_ICONINFORMATION);
    free(g_pendingPdfPath);
    g_pendingPdfPath = NULL;
    g_pendingPrintAction = PRINT_ACTION_NONE;
    return S_OK;
}

static ICoreWebView2PrintToPdfCompletedHandlerVtbl g_pdfVtbl = {
    (void*)PdfHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)PdfDone_Invoke
};
static ICoreWebView2PrintToPdfCompletedHandler g_pdfHandler = { &g_pdfVtbl };

static void open_wikilink_target(const WCHAR *encoded);

static HRESULT STDMETHODCALLTYPE WebMessage_Invoke(ICoreWebView2WebMessageReceivedEventHandler *this_,
                                                     ICoreWebView2 *sender,
                                                     ICoreWebView2WebMessageReceivedEventArgs *args) {
    (void)this_; (void)sender;
    LPWSTR message = NULL;
    if (FAILED(ICoreWebView2WebMessageReceivedEventArgs_TryGetWebMessageAsString(args, &message)) ||
        !message) return S_OK;
    WCHAR exportPrefix[64];
    swprintf(exportPrefix, 64, L"export-html:%lu:", g_exportRequest);
    if (g_exportPath && wcsncmp(message, exportPrefix, wcslen(exportPrefix)) == 0) {
        LPWSTR source = NULL;
        HRESULT sourceResult = ICoreWebView2WebMessageReceivedEventArgs_get_Source(args, &source);
        /* Only our preview shell may complete a pending filesystem write. */
        if (SUCCEEDED(sourceResult) && source &&
            ((wcsncmp(source, L"https://markedit.local/preview_shell.html", 39) == 0 &&
              (source[39] == 0 || source[39] == L'?' || source[39] == L'#')) ||
             (!g_webview3 && wcscmp(source, L"about:blank") == 0)))
            ExportDone_Invoke(NULL, S_OK, message + wcslen(exportPrefix));
        CoTaskMemFree(source);
        CoTaskMemFree(message);
        return S_OK;
    }
    /* Scroll sync only makes sense with both panes on screen; wiki-link
       navigation should work regardless of which panes are visible. */
    if (g_editorVisible && g_previewVisible && wcsncmp(message, L"scroll:", 7) == 0) {
        WCHAR *end = NULL;
        long line = wcstol(message + 7, &end, 10);
        Sci_Position lineCount = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
        if (end && *end == 0 && line >= 1 && line <= lineCount) {
            g_ignoreNextEditorScroll = 1;
            KillTimer(g_hMainWnd, IDT_SCROLL_SYNC);
            SendMessageA(tracked_edit_view(), SCI_SETFIRSTVISIBLELINE, (WPARAM)(line - 1), 0);
        }
    } else if (wcsncmp(message, L"wikilink:", 9) == 0) {
        open_wikilink_target(message + 9);
    }
    CoTaskMemFree(message);
    return S_OK;
}
static ICoreWebView2WebMessageReceivedEventHandlerVtbl g_webMessageVtbl = {
    (void*)WebMessageHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)WebMessage_Invoke
};
static ICoreWebView2WebMessageReceivedEventHandler g_webMessageHandler = { &g_webMessageVtbl };

/* AcceleratorKeyPressed handler: disabling AreBrowserAcceleratorKeysEnabled
   (see CtrlCreated_Invoke) stops WebView2 acting on its own accelerators,
   but the preview runs in a separate process with its own input queue, so
   the key never otherwise reaches this window's accelerator table - e.g.
   Ctrl+S, or Alt+Left for "Go Back" after following a wiki-link, would
   silently do nothing while focus is in the preview. Replaying the event as
   a real WM_(SYS)KEYDOWN through TranslateAcceleratorA against the same
   table WinMain's message loop uses puts every accelerator back to work
   from the preview, and is a no-op (falls through unhandled) for any key
   that isn't actually one of them. */
static HRESULT STDMETHODCALLTYPE AccelKey_Invoke(ICoreWebView2AcceleratorKeyPressedEventHandler *this_,
                                                   ICoreWebView2Controller *sender,
                                                   ICoreWebView2AcceleratorKeyPressedEventArgs *args) {
    (void)this_; (void)sender;
    if (!g_hAccel || !g_hMainWnd) return S_OK;
    COREWEBVIEW2_KEY_EVENT_KIND kind;
    if (FAILED(ICoreWebView2AcceleratorKeyPressedEventArgs_get_KeyEventKind(args, &kind))) return S_OK;
    if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
        return S_OK;
    UINT32 virtualKey = 0;
    INT keyLParam = 0;
    ICoreWebView2AcceleratorKeyPressedEventArgs_get_VirtualKey(args, &virtualKey);
    ICoreWebView2AcceleratorKeyPressedEventArgs_get_KeyEventLParam(args, &keyLParam);
    MSG msg = {0};
    msg.hwnd = g_hMainWnd;
    msg.message = (kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) ? WM_SYSKEYDOWN : WM_KEYDOWN;
    msg.wParam = (WPARAM)virtualKey;
    msg.lParam = (LPARAM)keyLParam;
    if (TranslateAcceleratorA(g_hMainWnd, g_hAccel, &msg))
        ICoreWebView2AcceleratorKeyPressedEventArgs_put_Handled(args, TRUE);
    return S_OK;
}
static ICoreWebView2AcceleratorKeyPressedEventHandlerVtbl g_accelKeyVtbl = {
    (void*)AccelKeyHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)AccelKey_Invoke
};
static ICoreWebView2AcceleratorKeyPressedEventHandler g_accelKeyHandler = { &g_accelKeyVtbl };

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

/* Percent-decodes [encoded, end) - which may contain UTF-16 surrogate pairs
   as well as %XX-escaped UTF-8 bytes, exactly as URIs from the preview's
   WebView2 content arrive - into a plain UTF-16 string. Shared by every
   caller that turns a link or postMessage payload from the preview back
   into a native string (document-relative links, wiki-link targets). */
static WCHAR *percent_decode_wide(const WCHAR *encoded, const WCHAR *end) {
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
    WCHAR *decoded = (WCHAR *)malloc(((size_t)wideLen + 1) * sizeof(WCHAR));
    if (!decoded) { free(bytes); return NULL; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, (int)o, decoded, wideLen);
    decoded[wideLen] = 0;
    free(bytes);
    return decoded;
}

static WCHAR *decode_document_url(const WCHAR *uri) {
    static const WCHAR prefix[] = L"https://document.local/";
    if (_wcsnicmp(uri, prefix, wcslen(prefix)) != 0) return NULL;
    const WCHAR *encoded = uri + wcslen(prefix);
    const WCHAR *end = encoded + wcscspn(encoded, L"?#");
    WCHAR *relative = percent_decode_wide(encoded, end);
    if (!relative) return NULL;
    for (WCHAR *p = relative; *p; p++) if (*p == L'/') *p = L'\\';
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

static int open_file_in_tab(const WCHAR *path);

/* Records the current document as a "Go Back" target, called right before a
   wiki-link jump actually navigates away from it. Bounded and FIFO like
   g_closedDocuments, and silently a no-op for an unsaved document - see the
   g_wikiBackStack comment. */
static void push_wiki_back(void) {
    if (!g_currentFile) return;
    if (g_wikiBackCount == MAX_WIKI_BACK) {
        free(g_wikiBackStack[0]);
        for (int i = 0; i + 1 < g_wikiBackCount; i++) g_wikiBackStack[i] = g_wikiBackStack[i + 1];
        g_wikiBackCount--;
    }
    g_wikiBackStack[g_wikiBackCount++] = wcsdup_heap(g_currentFile);
}

/* Returns to wherever the most recent wiki-link jump was made from. */
static void go_wiki_back(void) {
    if (g_wikiBackCount <= 0) return;
    WCHAR *path = g_wikiBackStack[--g_wikiBackCount];
    open_file_in_tab(path);
    free(path);
    refresh_menu(g_hMainWnd);
}

/* Resolves a [[wiki-link]] target clicked in the preview (see the
   "wikilink:" case in WebMessage_Invoke) to a note file and opens it. The
   target names a note by title, resolved against the current document's
   directory exactly like a regular document-relative Markdown link - a
   ".md" extension is assumed unless the author already gave one. If the
   note doesn't exist yet, offers to create it rather than silently failing,
   since that's the point of a wiki-link over a plain reference. */
static void open_wikilink_target(const WCHAR *encoded) {
    WCHAR *name = percent_decode_wide(encoded, encoded + wcslen(encoded));
    if (!name) return;
    for (WCHAR *p = name; *p; p++) if (*p == L'/') *p = L'\\';
    if (!*name || path_escapes_base(name)) { free(name); return; }

    WCHAR *directory = g_currentFile ? path_directory(g_currentFile) : NULL;
    if (!directory) {
        WCHAR *module = get_module_path();
        directory = module ? path_directory(module) : NULL;
        free(module);
    }
    if (!directory) { free(name); return; }

    WCHAR *combined = NULL;
    const WCHAR *target = name;
    if (!*PathFindExtensionW(name)) {
        combined = (WCHAR *)malloc((wcslen(name) + 4) * sizeof(WCHAR));
        if (combined) {
            wcscpy(combined, name);
            wcscat(combined, L".md");
            target = combined;
        }
    }
    WCHAR *full = path_join(directory, target);
    free(directory);
    free(combined);
    free(name);
    if (!full) return;

    DWORD attrs = GetFileAttributesW(full);
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        push_wiki_back();
        open_file_in_tab(full);
        refresh_menu(g_hMainWnd);
    } else {
        WCHAR prompt[MAX_PATH + 64];
        wsprintfW(prompt, L"\"%s\" doesn't exist yet. Create it?", PathFindFileNameW(full));
        if (MessageBoxW(g_hMainWnd, prompt, L"MarkEdit", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            HANDLE file = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (file == INVALID_HANDLE_VALUE) {
                MessageBoxW(g_hMainWnd, L"Could not create the note.", L"MarkEdit", MB_OK | MB_ICONERROR);
            } else {
                CloseHandle(file);
                push_wiki_back();
                open_file_in_tab(full);
                refresh_menu(g_hMainWnd);
            }
        }
    }
    free(full);
}

/* NavigationStarting handler: links clicked inside the preview (Markdown links,
   images-as-links, etc.) should open in the user's default browser instead of
   navigating the embedded WebView away from the preview shell. Our own shell
   page load is allowed through - either the virtual-host URL
   (https://markedit.local/preview_shell.html) or, on older WebView2 Runtimes
   lacking ICoreWebView2_3, the "about:..." page produced by NavigateToString -
   and so is show_html_file_preview()'s own navigation to the HTML file being
   previewed (g_expectedHtmlPreviewUrl, consumed here so it only ever allows
   that one navigation through: a click on a link inside the previewed page
   itself, even to that exact same URL, still gets redirected out below,
   same as every other link in the preview). Anything else is cancelled here
   and handed to ShellExecuteW instead. */
static HRESULT STDMETHODCALLTYPE NavStarting_Invoke(ICoreWebView2NavigationStartingEventHandler *this_, ICoreWebView2 *sender, ICoreWebView2NavigationStartingEventArgs *args) {
    (void)this_; (void)sender;
    LPWSTR uri = NULL;
    if (SUCCEEDED(ICoreWebView2NavigationStartingEventArgs_get_Uri(args, &uri)) && uri) {
        /* Prefix match (not exact-equality) so the "?theme=dark|light" query
           string passed on the initial navigation still counts as our shell
           page, rather than being treated as an external link. */
        static const WCHAR shellPrefix[] = L"https://markedit.local/preview_shell.html";
        size_t shellLen = wcslen(shellPrefix);
        int isExpectedHtmlPreview = g_expectedHtmlPreviewUrl && _wcsicmp(uri, g_expectedHtmlPreviewUrl) == 0;
        int isShellPage = (_wcsnicmp(uri, shellPrefix, shellLen) == 0 &&
                           (uri[shellLen] == 0 || uri[shellLen] == L'?' || uri[shellLen] == L'#')) ||
                           (!g_webviewReady && _wcsicmp(uri, L"about:blank") == 0) ||
                           isExpectedHtmlPreview;
        if (isExpectedHtmlPreview) replace_wstring(&g_expectedHtmlPreviewUrl, NULL);
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

/* Shared with the file explorer panel's "Reveal in Explorer" context-menu
   command (passed to explorer_init() as a callback, since file_explorer.c
   can't call a static markedit.c function by name). */
static void reveal_path_in_explorer(const WCHAR *path) {
    size_t capacity = wcslen(path) + 16;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!parameters) return;
    swprintf(parameters, capacity, L"/select,\"%ls\"", path);
    ShellExecuteW(g_hMainWnd, L"open", L"explorer.exe", parameters, NULL, SW_SHOWNORMAL);
    free(parameters);
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
    reveal_path_in_explorer(g_currentFile);
}

static void apply_preview_theme(void) {
    if (!g_webviewReady || !g_webview || g_previewShowingRawHtml) return;
    char script[64];
    wsprintfA(script, "window.setTheme('%s');", g_previewDarkMode ? "dark" : "light");
    execute_webview_script(script, &g_scriptHandler);
}

static void push_html_to_webview(const char *htmlFragment) {
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

/* Navigates the preview pane to the app's own shell page - the one
   push_html_to_webview()'s setContent() call (Markdown, and the "preview
   not available" message) needs loaded. Used both for the very first load
   (CtrlCreated_Invoke) and to return from show_html_file_preview()'s raw
   HTML file preview. g_webviewReady is cleared first: it means "the loaded
   page is our scriptable shell", which stops being true the moment this
   navigation starts, not just once NavDone_Invoke's completion notice
   arrives - without that, a push_html_to_webview() call made right after
   this one would see the still-stale-true flag and try to script the old
   (about to be replaced) page immediately instead of queuing into
   g_pendingHtmlBody for NavDone_Invoke to flush once the shell has actually
   (re)loaded. */
static void navigate_to_shell(void) {
    if (!g_webview) return;
    g_previewShowingRawHtml = 0;
    g_webviewReady = 0;
    if (g_webview3) {
        /* Pass the starting theme via query string so the page can apply it
           immediately on load, instead of defaulting to light and flipping
           to dark only once apply_preview_theme() runs after navigation
           completes (which caused a visible light-then-dark flash). */
        const WCHAR *shellUrl = g_previewDarkMode
            ? L"https://markedit.local/preview_shell.html?theme=dark"
            : L"https://markedit.local/preview_shell.html?theme=light";
        ICoreWebView2_Navigate(g_webview, shellUrl);
    } else {
        WCHAR *wHtml = to_wide(preview_fallback_html);
        if (wHtml) ICoreWebView2_NavigateToString(g_webview, wHtml);
        free(wHtml);
    }
}

/* Renders an HTML document by navigating the preview pane directly to it,
   instead of markdown_to_html()/push_html_to_webview()'s fragment-into-shell
   injection - this runs the page's own <script> tags and resolves its own
   relative <link>/<script>/<img> references exactly like a real browser,
   via the same https://document.local/ virtual host mapping
   (update_document_mapping()) the shell already uses for Markdown's own
   relative images. Only ever reached when g_currentFile is set:
   language_for_path() only assigns LANG_HTML to a document with a real,
   already-saved path, so this always reflects the file's last-saved
   content, not unsaved edits - save_to_path() calls update_preview() again
   after every successful save to keep it current. See NavStarting_Invoke
   for how this navigation avoids being redirected to the system browser
   like every other link in the preview is. */
static void show_html_file_preview(void) {
    if (!g_webview || !g_webview3 || !g_currentFile) {
        push_html_to_webview("<p>HTML preview needs a newer WebView2 Runtime.</p>");
        return;
    }
    const WCHAR *name = PathFindFileNameW(g_currentFile);
    size_t capacity = wcslen(name) + 32;
    WCHAR *url = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!url) return;
    swprintf(url, capacity, L"https://document.local/%ls", name);
    replace_wstring(&g_expectedHtmlPreviewUrl, url);
    g_previewShowingRawHtml = 1;
    g_webviewReady = 0;
    ICoreWebView2_Navigate(g_webview, url);
    free(url);
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
    EventRegistrationToken accelKeyToken;
    ICoreWebView2Controller_add_AcceleratorKeyPressed(ctrl, &g_accelKeyHandler, &accelKeyToken);

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
    }
    navigate_to_shell();

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
    /* The browser profile lives with the other per-user state rather than
       beside markedit.exe: an installed copy in Program Files can't write
       there, which made creating the WebView2 environment fail outright. */
    WCHAR *userDataDir = get_local_app_directory(L"WebView2");
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

/* Above this size a source-code preview skips syntax highlighting:
   highlight.js reruns over the whole block on every refresh, so a big file
   would stall the preview after each pause in typing. */
#define PREVIEW_HIGHLIGHT_MAX_LENGTH (512 * 1024)

/* Wraps a non-Markdown document as a single fenced code block tagged with
   `tag`, so it previews as highlighted source through the same Markdown
   pipeline (highlight.js picks the block up by its language class). The
   fence is one backtick longer than the longest run of backticks in the
   text - CommonMark fences can be any length from three - so nothing in
   the source can close it early. */
static char *source_as_markdown(const char *text, size_t length, const char *tag) {
    size_t longest = 0, run = 0;
    for (size_t i = 0; i < length; i++) {
        run = text[i] == '`' ? run + 1 : 0;
        if (run > longest) longest = run;
    }
    size_t fence = longest >= 3 ? longest + 1 : 3;
    size_t tagLength = strlen(tag);
    char *markdown = (char *)malloc(fence * 2 + tagLength + length + 4);
    if (!markdown) return NULL;
    size_t o = 0;
    memset(markdown + o, '`', fence); o += fence;
    memcpy(markdown + o, tag, tagLength); o += tagLength;
    markdown[o++] = '\n';
    memcpy(markdown + o, text, length); o += length;
    if (length && text[length - 1] != '\n') markdown[o++] = '\n';
    memset(markdown + o, '`', fence); o += fence;
    markdown[o++] = '\n';
    markdown[o] = 0;
    return markdown;
}

static void update_preview(void) {
    if (g_hMainWnd) KillTimer(g_hMainWnd, IDT_PREVIEW_UPDATE);
    /* Loading/restoring/switching tabs should not parse Markdown while the
       preview is hidden. Showing the pane calls update_preview(), and export
       sets g_exportPath first to force a current off-screen render. */
    if (!g_previewVisible && !g_exportPath) return;
    /* HTML gets a real browser-style render (see show_html_file_preview());
       export bypasses it, since that navigation isn't a render the export can
       capture, and gets the highlighted-source render other languages use. */
    if (g_activeLanguage == LANG_HTML && !g_exportPath) {
        show_html_file_preview();
        return;
    }
    /* Returning here from a raw HTML preview: push_html_to_webview() below
       needs the shell page loaded to run its setContent(), not whatever
       HTML file was last navigated to. */
    if (g_previewShowingRawHtml) navigate_to_shell();
    /* Checked before the language: a large-file document is always plain
       text (see apply_document_language()), which would otherwise get the
       message below as though it were simply the wrong kind of file. */
    if (g_largeFile) {
        push_html_to_webview("<p>Preview is turned off for large files.</p>");
        return;
    }
    Sci_Position len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;
    /* Running markdown_to_html() straight over a JSON/C/Python/etc. document
       would misread ordinary source lines (a leading '#' comment, say) as
       Markdown syntax, so every other language previews as its own source,
       syntax-highlighted. An untitled tab has no type yet - its editor
       stays plain text (see language_for_path()) - and previews as
       Markdown, as a saved file of an unknown type does. */
    if (g_activeLanguage != LANG_MARKDOWN && !(g_activeLanguage == LANG_TEXT && !g_currentFile)) {
        const char *tag = len > PREVIEW_HIGHLIGHT_MAX_LENGTH ? "" : LANGUAGES[g_activeLanguage].fenceTag;
        char *source = source_as_markdown(text, (size_t)len, tag);
        free(text);
        if (!source) return;
        text = source;
        len = (Sci_Position)strlen(source);
    }

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
    HWND view = tracked_edit_view();
    Sci_Position line = (Sci_Position)SendMessageA(view, SCI_GETFIRSTVISIBLELINE, 0, 0) + 1;
    char script[96];
    snprintf(script, sizeof(script),
             "if(window.scrollToSourceLine)window.scrollToSourceLine(%lld);", (long long)line);
    execute_webview_script(script, &g_scriptHandler);
}

/* ================= rest of the app (menus, file I/O, find/replace, font) ================= */

static const WCHAR *document_short_name(const Document *doc) {
    return doc->path ? PathFindFileNameW(doc->path) : NULL;
}

static void format_document_label(const Document *doc, WCHAR *label, size_t capacity) {
    const WCHAR *shortName = document_short_name(doc);
    if (doc->isReference && !shortName) {
        swprintf(label, capacity, L"%ls%ls", reference_title(doc->isReference), doc->dirty ? L" *" : L"");
    } else if (shortName && shortName[0]) {
        swprintf(label, capacity, L"%ls%ls", shortName, doc->dirty ? L" *" : L"");
    } else if (doc->untitledNumber == 0) {
        swprintf(label, capacity, L"Untitled.txt%ls", doc->dirty ? L" *" : L"");
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
    doc->caret = (Sci_Position)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    doc->anchor = (Sci_Position)SendMessageA(g_hEdit, SCI_GETANCHOR, 0, 0);
    doc->firstVisibleLine = (Sci_Position)SendMessageA(g_hEdit, SCI_GETFIRSTVISIBLELINE, 0, 0);
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
    doc->externalDeleted = 0;
    if (g_hTabs) InvalidateRect(g_hTabs, NULL, FALSE);
    free(ioPath);
}

/* Swaps the shared document pointer for the main editor, its minimap, and
   its split pane (if any), keeping all of them in sync with whichever tab is
   active. Not used for the transient peeks in get_document_text() and
   clear_debug_execution_markers(), which restore the original pointer
   before returning and would otherwise flash the minimap/split pane at an
   unrelated document. */
static void set_active_editor_document(sptr_t doc) {
    SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)doc);
    if (g_hMinimap) SendMessageA(g_hMinimap, SCI_SETDOCPOINTER, 0, (LPARAM)doc);
    if (g_hSplitEdit) SendMessageA(g_hSplitEdit, SCI_SETDOCPOINTER, 0, (LPARAM)doc);
}

static void switch_document(int index) {
    if (!g_hEdit || index < 0 || index >= g_documentCount || index == g_activeDocument) return;
    save_active_document_view();
    g_switchingDocument = 1;
    g_activeDocument = index;
    set_active_editor_document(g_documents[index].sciDocument);
    apply_editor_font(g_fontFaceName, g_fontPointSize);
    apply_document_language(g_hEdit, g_documents[index].path);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)g_documents[index].anchor, (LPARAM)g_documents[index].caret);
    SendMessageA(g_hEdit, SCI_SETFIRSTVISIBLELINE, (WPARAM)g_documents[index].firstVisibleLine, 0);
    SendMessageA(g_hEdit, SCI_SETXOFFSET, (WPARAM)g_documents[index].xOffset, 0);
    update_linenumber_margin_width(g_hEdit);
    if (g_hSplitEdit) {
        apply_document_language(g_hSplitEdit, g_documents[index].path);
        if (g_splitEditorVisible) {
            /* Starts wherever the main pane starts; the user can then scroll
               it independently - the split pane doesn't persist its own
               per-document view state (see set_active_editor_document()). */
            SendMessageA(g_hSplitEdit, SCI_SETSEL, (WPARAM)g_documents[index].anchor, (LPARAM)g_documents[index].caret);
            SendMessageA(g_hSplitEdit, SCI_SETFIRSTVISIBLELINE, (WPARAM)g_documents[index].firstVisibleLine, 0);
            SendMessageA(g_hSplitEdit, SCI_SETXOFFSET, (WPARAM)g_documents[index].xOffset, 0);
            update_linenumber_margin_width(g_hSplitEdit);
        }
    }
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
    set_active_editor_document(sciDoc);
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
        : active->isReference ? reference_title(active->isReference)
        : (active->untitledNumber == 0 ? L"Untitled.txt" : L"Untitled");
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

/* Swaps the active tab onto a different Scintilla document and releases the
   one it had. A document's storage options are fixed when it's created, so
   load_file() needs this whenever a file's large-file mode differs from the
   one the tab's current document was made for - and on every large-file
   load, since the loader always builds a new document. Configured the same
   way create_document_tab() sets up a new tab's document. */
static void replace_active_scintilla_document(sptr_t sciDoc, int largeFile) {
    sptr_t old = g_documents[g_activeDocument].sciDocument;
    int oldSwitching = g_switchingDocument;
    g_switchingDocument = 1;
    g_documents[g_activeDocument].sciDocument = sciDoc;
    g_largeFile = largeFile;
    set_active_editor_document(sciDoc);
    configure_scintilla(g_hEdit);
    apply_editor_font(g_fontFaceName, g_fontPointSize);
    SendMessageA(g_hEdit, SCI_RELEASEDOCUMENT, 0, (LPARAM)old);
    g_switchingDocument = oldSwitching;
}

/* A preview-only layout cannot show a large document because its rendered
   preview is deliberately disabled. Reveal the editor automatically; for a
   command-line/file-association launch, replace the preview pane outright so
   the initial window contains the document rather than only an explanation. */
static void ensure_large_file_editor_visible(void) {
    if (!g_largeFile || g_editorVisible) return;
    g_editorVisible = 1;
    if (g_commandLinePreviewMode) g_previewVisible = 0;
    update_view_menu(g_hMainWnd);
    layout(g_hMainWnd);
    InvalidateRect(g_hMainWnd, NULL, TRUE);
}

/* Scintilla's ILoader (ILoader.h) is a C++ interface. It has no virtual
   destructor, so its vtable is exactly these three slots in declaration
   order and can be called from C the same way the COM interfaces are. */
typedef struct SciLoader SciLoader;
typedef struct SciLoaderVtbl {
    int (SCI_METHOD *Release)(SciLoader *self);
    int (SCI_METHOD *AddData)(SciLoader *self, const char *data, Sci_Position length);
    void *(SCI_METHOD *ConvertToDocument)(SciLoader *self);
} SciLoaderVtbl;
struct SciLoader { const SciLoaderVtbl *lpVtbl; };

#define LARGE_FILE_READ_CHUNK (4 * 1024 * 1024)

/* Streams `file` into a new large-file document (see LARGE_FILE_FEATURES)
   through SCI_CREATELOADER, so the text is only ever held once - by
   Scintilla - rather than in a read buffer and then copied into the
   document by SCI_SETTEXT. The document stores no styles (halving its
   memory; it's never lexed) and uses 64-bit positions, which is what lets
   files past 2 GB open at all. Any encoding other than UTF-8 is converted
   a chunk at a time on the way in. `forcedEncoding` is a TextEncoding, or
   -1 to detect it: from the first chunk, then - for a file that looks like
   UTF-8 - by validating the rest as it streams, starting over as ANSI if a
   later chunk turns out not to be. Returns 0 on failure, with *outOfMemory
   set when Scintilla couldn't allocate the text. */
static sptr_t read_large_file_document(HANDLE file, size_t length, int forcedEncoding,
                                       int *outEncoding, int *outHasBom, int *outOfMemory,
                                       LargeOperationProgress *progress) {
    *outOfMemory = 0;
    char *chunk = (char *)malloc(LARGE_FILE_READ_CHUNK);
    char *decoded = (char *)malloc(text_decode_capacity(LARGE_FILE_READ_CHUNK));
    if (!chunk || !decoded) {
        free(chunk); free(decoded);
        *outOfMemory = 1;
        return 0;
    }
    sptr_t result = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        LARGE_INTEGER start = {0};
        if (attempt && !SetFilePointerEx(file, start, NULL, FILE_BEGIN)) break;
        if (!large_operation_progress_update(progress, 0, length)) break;
        SciLoader *loader = (SciLoader *)SendMessageA(g_hEdit, SCI_CREATELOADER, (WPARAM)length,
            (LPARAM)(SC_DOCUMENTOPTION_STYLES_NONE | SC_DOCUMENTOPTION_TEXT_LARGE));
        if (!loader) { *outOfMemory = 1; break; }
        TextCodecState codec = { TEXT_ENCODING_UTF8, {0}, 0 };
        Utf8Validator validator;
        utf8_validator_init(&validator);
        int validating = 0, ok = 1, restart = 0;
        *outHasBom = 0;
        size_t total = 0;
        while (ok && total < length) {
            DWORD want = (DWORD)((length - total) > LARGE_FILE_READ_CHUNK ? LARGE_FILE_READ_CHUNK : (length - total));
            DWORD got = 0;
            if (!ReadFile(file, chunk, want, &got, NULL) || got == 0) { ok = 0; break; }
            const char *data = chunk;
            size_t dataLength = got;
            if (total == 0) {
                size_t bom = 0;
                if (forcedEncoding >= 0) {
                    codec.encoding = (TextEncoding)forcedEncoding;
                    bom = text_bom_length(chunk, got, codec.encoding);
                } else {
                    codec.encoding = detect_text_encoding(chunk, got, got == length, &bom);
                    /* A first chunk that looks like UTF-8 only says so much. */
                    validating = codec.encoding == TEXT_ENCODING_UTF8 && !bom;
                }
                *outHasBom = bom > 0;
                data += bom;
                dataLength -= bom;
            }
            total += got;
            if (!large_operation_progress_update(progress, total, length)) { ok = 0; break; }
            if (validating && !utf8_validator_feed(&validator, data, dataLength)) { restart = 1; break; }
            if (codec.encoding != TEXT_ENCODING_UTF8) {
                size_t produced = 0;
                if (!text_decode(&codec, data, dataLength, total == length, decoded,
                                 text_decode_capacity(LARGE_FILE_READ_CHUNK), &produced)) { ok = 0; break; }
                data = decoded;
                dataLength = produced;
            }
            int status = dataLength ? loader->lpVtbl->AddData(loader, data, (Sci_Position)dataLength) : SC_STATUS_OK;
            if (status != SC_STATUS_OK) {
                *outOfMemory = status == SC_STATUS_BADALLOC;
                ok = 0;
            }
        }
        if (ok && !restart && validating && !utf8_validator_finish(&validator)) restart = 1;
        if (!ok || restart) {
            loader->lpVtbl->Release(loader);
            if (!ok) break;
            forcedEncoding = TEXT_ENCODING_ANSI;
            continue;
        }
        *outEncoding = codec.encoding;
        result = (sptr_t)loader->lpVtbl->ConvertToDocument(loader);
        break;
    }
    free(chunk);
    free(decoded);
    return result;
}

/* `forcedEncoding` is a TextEncoding to read the file as (File > Reopen
   with Encoding), or -1 to detect it. */
static int load_file_as(const WCHAR *path, int forcedEncoding) {
    WCHAR *displayPath = absolute_path(path);
    WCHAR *ioPath = extended_path(path);
    HANDLE hFile = ioPath ? CreateFileW(ioPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL)
        : INVALID_HANDLE_VALUE;
    free(ioPath);
    if (hFile == INVALID_HANDLE_VALUE) {
        MessageBoxW(g_hMainWnd, L"Could not open file.", L"MarkEdit", MB_OK | MB_ICONERROR);
        recent_list_remove(&g_recentFileList, path);
        refresh_menu(g_hMainWnd);
        free(displayPath);
        return 0;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart < 0) {
        CloseHandle(hFile);
        MessageBoxW(g_hMainWnd, L"Could not read file.", L"MarkEdit", MB_OK | MB_ICONERROR);
        free(displayPath);
        return 0;
    }
    size_t length = (size_t)size.QuadPart;
    /* Past INT_MAX a file has to open in large-file mode whatever the
       threshold is set to: only a large-file document has 64-bit positions. */
    int largeFile = size.QuadPart >= (LONGLONG)g_largeFileThresholdMB * 1024 * 1024 ||
                    size.QuadPart > INT_MAX;
    char *buf = NULL;
    size_t total = 0;
    int ok = 0, loaded = 0, outOfMemory = 0, tooLarge = 0, cancelled = 0;
    char *decoded = NULL;
    if (largeFile) {
        /* SCI_CREATELOADER resets g_hEdit's per-view line state as a side
           effect (Editor.cxx's CreateLoader). A successful load swaps in the
           new document, which rebuilds it; a failed one re-attaches the
           tab's own document below to do the same, so the view is saved
           first to put the caret and scroll position back. */
        save_active_document_view();
        HCURSOR oldCursor = SetCursor(LoadCursorA(NULL, IDC_WAIT));
        int hasBom = 0, encoding = TEXT_ENCODING_UTF8;
        LargeOperationProgress progress;
        large_operation_progress_begin(&progress, L"Opening large file", L"Loading text...");
        sptr_t sciDoc = read_large_file_document(hFile, length, forcedEncoding, &encoding,
                                                 &hasBom, &outOfMemory, &progress);
        cancelled = progress.cancelled;
        large_operation_progress_end(&progress);
        SetCursor(oldCursor);
        if (!sciDoc) {
            const Document *doc = &g_documents[g_activeDocument];
            g_switchingDocument = 1;
            set_active_editor_document(doc->sciDocument);
            SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)doc->anchor, (LPARAM)doc->caret);
            SendMessageA(g_hEdit, SCI_SETFIRSTVISIBLELINE, (WPARAM)doc->firstVisibleLine, 0);
            SendMessageA(g_hEdit, SCI_SETXOFFSET, (WPARAM)doc->xOffset, 0);
            g_switchingDocument = 0;
        } else {
            g_switchingDocument = 1;
            replace_active_scintilla_document(sciDoc, 1);
            apply_detected_eol_mode(g_hEdit);
            /* A loader-built document starts with undo collection off. */
            SendMessageA(g_hEdit, SCI_SETUNDOCOLLECTION, TRUE, 0);
            SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
            SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
            g_switchingDocument = 0;
            g_hasBom = hasBom;
            g_encoding = encoding;
            g_documents[g_activeDocument].encodingForced = forcedEncoding >= 0;
            /* Swapping in a loader-built document raises no SCN_MODIFIED,
               which is what refreshes these after an ordinary load. */
            update_linenumber_margin_width(g_hEdit);
            if (g_hSplitEdit) update_linenumber_margin_width(g_hSplitEdit);
            update_status_bar();
            loaded = 1;
            /* No convert_indentation() here, unlike the ordinary path
               below: rewriting every line of a large file on load is the
               kind of whole-document pass this mode exists to avoid. */
        }
    } else {
        buf = (char *)malloc(length + 1);
        if (!buf) outOfMemory = 1;
    }
    while (buf && total < length) {
        DWORD chunk = (DWORD)((length - total) > 1024 * 1024 ? 1024 * 1024 : (length - total));
        DWORD got = 0;
        if (!ReadFile(hFile, buf + total, chunk, &got, NULL) || got == 0) break;
        total += got;
    }
    if (buf && total == length) {
        buf[length] = 0;
        size_t bom = 0;
        TextEncoding encoding = forcedEncoding >= 0 ? (TextEncoding)forcedEncoding
                                                    : detect_text_encoding(buf, length, 1, &bom);
        if (forcedEncoding >= 0) bom = text_bom_length(buf, length, encoding);
        const char *content = buf + bom;
        size_t contentLength = length - bom;
        if (encoding != TEXT_ENCODING_UTF8) {
            size_t decodedLength = 0;
            decoded = text_decode_all(encoding, content, length - bom, &decodedLength);
            contentLength = decodedLength;
            if (!decoded) outOfMemory = 1;
            /* ANSI text can triple in size as UTF-8, past what an ordinary
               (32-bit) document holds; only large-file mode can take that. */
            else if (decodedLength > INT_MAX) tooLarge = 1;
            content = outOfMemory || tooLarge ? NULL : decoded;
        }
        if (content) {
            int decodedLargeMode = contentLength >=
                (size_t)g_largeFileThresholdMB * 1024 * 1024;
            /* Reloading a tab that was in large-file mode with a file that's
               since shrunk needs an ordinary, lexable document again. */
            if (g_largeFile) {
                sptr_t sciDoc = (sptr_t)SendMessageA(g_hEdit, SCI_CREATEDOCUMENT, 0, 0);
                if (sciDoc) replace_active_scintilla_document(sciDoc, 0);
                else decodedLargeMode = 1; /* keep the existing style-less document mode */
            }
            /* The threshold describes editor size, not merely bytes on disk:
               ANSI text can expand to three UTF-8 bytes per source byte. */
            g_largeFile = decodedLargeMode;
            g_hasBom = bom > 0;
            g_encoding = encoding;
            g_documents[g_activeDocument].encodingForced = forcedEncoding >= 0;
            g_switchingDocument = 1;
            set_edit_text(g_hEdit, content, (Sci_Position)contentLength);
            /* Matches toggle_indent_use_spaces(): when the setting is on, a
               file opened (or dropped) with tab-indentation should come in
               already converted rather than leaving a mix of tabs and spaces.
               Done inside the g_switchingDocument window and before
               SCI_EMPTYUNDOBUFFER so it's folded into the loaded state - no
               spurious dirty flag and nothing to undo back into tabs. */
            if (g_indentUseSpaces && !g_largeFile) convert_indentation(g_hEdit, 1);
            SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
            SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
            g_switchingDocument = 0;
            loaded = 1;
        }
    }
    if (loaded) {
        replace_wstring(&g_currentFile, displayPath ? displayPath : path);
        apply_document_language(g_hEdit, g_currentFile);
        if (g_hSplitEdit) apply_document_language(g_hSplitEdit, g_currentFile);
        restore_persisted_breakpoints(g_currentFile);
        ensure_large_file_editor_visible();
        /* An ordinary load's SCN_MODIFIED schedules this; a large-file
           load raises none, so clear the outline the old text built. */
        if (g_largeFile) update_outline();
        record_document_file_time(g_activeDocument);
        update_document_mapping();
        update_preview();
        set_dirty(0);
        recent_list_add(&g_recentFileList, displayPath ? displayPath : path);
        refresh_menu(g_hMainWnd);
        ok = 1;
    } else {
        if (largeFile && cancelled) {
            /* Cancellation is an expected outcome, not a read error. */
        } else MessageBoxW(g_hMainWnd, outOfMemory ? L"Not enough memory to open this file."
                    : tooLarge ? L"This file is too large to open as text below the large-file "
                                  L"threshold. Lower [View] LargeFileThresholdMB in markedit.ini "
                                  L"to open it in large-file mode."
                    : L"Could not read file.",
                    L"MarkEdit", MB_OK | MB_ICONERROR);
    }
    free(decoded);
    if (buf) free(buf);
    CloseHandle(hFile);
    free(displayPath);
    return ok;
}

static int load_file(const WCHAR *path) {
    return load_file_as(path, -1);
}

/* SCN_MODIFIED is delivered from inside Scintilla's edit operation, so the
   notification only flips the cheap per-document flag and posts this work.
   Reconfiguring lexers/views after control returns avoids replacing view
   state re-entrantly while preserving the ordinary document's undo history. */
static void apply_large_file_mode_after_growth(int documentIndex) {
    if (documentIndex < 0 || documentIndex >= g_documentCount ||
        documentIndex != g_activeDocument || !g_largeFile) return;
    KillTimer(g_hMainWnd, IDT_PREVIEW_UPDATE);
    KillTimer(g_hMainWnd, IDT_SPELL_CHECK);
    KillTimer(g_hMainWnd, IDT_OUTLINE_UPDATE);
    apply_document_language(g_hEdit, g_currentFile);
    if (g_hSplitEdit) apply_document_language(g_hSplitEdit, g_currentFile);
    ensure_large_file_editor_visible();
    update_outline();
    update_status_bar();
    if (g_previewVisible) update_preview();
    /* Remove any now-stale recovery copy made while the document was still
       below the threshold; large-file documents are intentionally excluded. */
    write_recovery_snapshot();
    refresh_menu(g_hMainWnd);
}

/* File > Reopen with Encoding: rereads the file from disk as `encoding`,
   for when detection guessed wrong. */
static void reopen_with_encoding(TextEncoding encoding) {
    if (!g_currentFile) {
        MessageBoxW(g_hMainWnd, L"Save the document first - there's no file to reopen yet.",
                    L"MarkEdit", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g_dirty && MessageBoxW(g_hMainWnd, L"Reopening discards your unsaved changes. Continue?",
                               L"MarkEdit", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    WCHAR *path = wcsdup_heap(g_currentFile);
    if (path) load_file_as(path, encoding);
    free(path);
}

/* Edit > Convert > Encoding: the text stays as it is; this only changes how
   the next save stores it, so it marks the document modified. */
static void set_document_encoding(TextEncoding encoding, int hasBom) {
    if (g_encoding == (int)encoding && g_hasBom == hasBom) return;
    g_encoding = encoding;
    g_hasBom = hasBom;
    set_dirty(1);
}

static int active_document_is_reusable(void) {
    return !g_currentFile && !g_dirty &&
           (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0) == 0;
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
        set_active_editor_document(g_documents[target].sciDocument);
        apply_editor_font(g_fontFaceName, g_fontPointSize);
        apply_document_language(g_hEdit, g_currentFile);
        if (g_hSplitEdit) apply_document_language(g_hSplitEdit, g_currentFile);
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
        if (!do_save()) return 0;
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

/* Inserts the contents of a chosen file at the current caret position in the
   active editor, replacing any selection - the same semantics SCI_REPLACESEL
   gives every other insert command in this file. */
static void do_insert_file(void) {
    if (!g_hEdit) return;
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
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrDefExt = L"md";
    if (GetOpenFileNameW(&ofn)) {
        char *text = read_text_file(path, NULL, NULL, NULL);
        if (text) {
            SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
            SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)text);
            SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
            SetFocus(g_hEdit);
        } else {
            MessageBoxW(g_hMainWnd, L"Could not read the selected file.", L"MarkEdit",
                        MB_OK | MB_ICONERROR);
        }
        free(text);
    }
    free(path);
}

static int is_image_file(const WCHAR *path) {
    const WCHAR *extension = PathFindExtensionW(path);
    return extension && extension_in_list(extension, L".png;.jpg;.jpeg;.gif;.bmp;.webp;.svg");
}

/* Create a collision-safe destination in an "images" folder next to the
   current Markdown file. The returned relative path always uses URL-style
   slashes so it can be inserted directly into Markdown. */
static int create_image_destination(const WCHAR *preferredName, const WCHAR *defaultExtension,
                                    WCHAR **destinationOut, WCHAR **relativeOut) {
    *destinationOut = NULL;
    *relativeOut = NULL;
    if (!g_currentFile) {
        if (!do_save()) return 0;
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

static int insert_markdown_reference(const WCHAR *relativePath, const WCHAR *altText, int isImage) {
    char *relative = to_utf8(relativePath);
    char *alt = to_utf8(altText && altText[0] ? altText : (isImage ? L"image" : L"file"));
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
        snprintf(markdown, cap, "%s[%s](<%s>)", isImage ? "!" : "", escapedAlt, relative);
        SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)markdown);
        SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
    }
    free(markdown);
    free(escapedAlt);
    free(relative);
    free(alt);
    return inserted;
}

static int insert_markdown_image(const WCHAR *relativePath, const WCHAR *altText) {
    return insert_markdown_reference(relativePath, altText, 1);
}

/* Links to a dropped file where it already sits on disk (relative to the
   current document's directory) instead of copying it into images/ - used
   for Shift+drop, which is meant for "point at this file" rather than
   "import this image into the document". Images still get an image link;
   everything else gets a plain link. */
static int shift_drop_link(const WCHAR *path) {
    /* PathRelativePathToW() guarantees a MAX_PATH-bounded write into
       `relative`, but `path` itself (the fallback `target` below, used
       whenever there's no current document or the two paths don't share a
       drive) comes from DragQueryFileW() and is NOT MAX_PATH-limited - so
       `normalized` is sized off the actual target length rather than a
       fixed stack buffer. */
    WCHAR relative[MAX_PATH];
    const WCHAR *target = path;
    if (g_currentFile) {
        WCHAR *documentDirectory = path_directory(g_currentFile);
        if (documentDirectory &&
            PathRelativePathToW(relative, documentDirectory, FILE_ATTRIBUTE_DIRECTORY,
                                 path, FILE_ATTRIBUTE_NORMAL)) {
            target = relative;
        }
        free(documentDirectory);
    }
    WCHAR *normalized = (WCHAR *)malloc((wcslen(target) + 1) * sizeof(WCHAR));
    if (!normalized) return 0;
    wcscpy(normalized, target);
    for (WCHAR *p = normalized; *p; p++) if (*p == L'\\') *p = L'/';
    if (normalized[0] == L'.' && normalized[1] == L'/')
        memmove(normalized, normalized + 2, (wcslen(normalized + 2) + 1) * sizeof(WCHAR));

    WCHAR *alt = wcsdup_heap(PathFindFileNameW(path));
    if (alt) PathRemoveExtensionW(alt);
    int inserted = insert_markdown_reference(normalized, alt, is_image_file(path));
    free(alt);
    free(normalized);
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
        if (!do_save()) return 1;
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
    /* Held at message-processing time, which in practice still reflects the
       physical key held through the drop gesture - not a hard real-time
       guarantee, but Shift-drop is a deliberate two-handed action so the
       window is wide enough to be reliable. */
    int shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
    int insertedImage = 0;
    int undoActive = 0;
    for (UINT i = 0; i < count; i++) {
        UINT cap = DragQueryFileW(hDrop, i, NULL, 0) + 1;
        WCHAR *path = (WCHAR *)malloc((size_t)cap * sizeof(WCHAR));
        if (path && DragQueryFileW(hDrop, i, path, cap)) {
            if (g_editorVisible && shiftHeld) {
                if (!undoActive) {
                    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
                    undoActive = 1;
                }
                if (insertedImage) SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)editor_eol_sequence());
                if (shift_drop_link(path)) insertedImage = 1;
            } else if (g_editorVisible && is_image_file(path)) {
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
    /* Written straight from Scintilla's own buffer: SCI_GETCHARACTERPOINTER
       closes the gap so the text is contiguous, and the pointer stays valid
       until the document is next modified. Only the encoding prompt below
       can let that happen (its message loop runs the file-watch timer), so
       the pointer is fetched again after it. A copy via get_edit_text()
       would need a second document-sized allocation, which a large-file
       document may not have room for. */
    Sci_Position len = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    const char *text = (const char *)SendMessageA(g_hEdit, SCI_GETCHARACTERPOINTER, 0, 0);
    if (!text) {
        MessageBoxW(g_hMainWnd, L"Not enough memory to save the document.", L"MarkEdit", MB_OK | MB_ICONERROR);
        return 0;
    }
    TextEncoding encoding = (TextEncoding)g_encoding;
    int hasBom = g_hasBom;
    LargeOperationProgress progress = {0};
    if (g_largeFile)
        large_operation_progress_begin(&progress, L"Saving large file", L"Checking text encoding...");
    int lossless = text_encoding_is_lossless_progress(text, (size_t)len, encoding,
        progress.dialog ? large_operation_progress_callback : NULL, &progress);
    if (lossless == -2) {
        large_operation_progress_end(&progress);
        return 0;
    }
    if (lossless < 0) {
        large_operation_progress_end(&progress);
        MessageBoxW(g_hMainWnd, L"Not enough memory to save the document.", L"MarkEdit", MB_OK | MB_ICONERROR);
        return 0;
    }
    if (!lossless) {
        large_operation_progress_end(&progress);
        WCHAR message[400];
        if (encoding == TEXT_ENCODING_ANSI)
            swprintf(message, sizeof(message) / sizeof(message[0]),
                     L"Some characters in this document can't be stored in ANSI (code page %u).\n\n"
                     L"Yes: save it as UTF-8 instead.\n"
                     L"No: save as ANSI anyway, replacing those characters with '?'.",
                     text_ansi_code_page());
        else
            swprintf(message, sizeof(message) / sizeof(message[0]),
                     L"This document contains bytes that aren't valid text - it was probably "
                     L"reopened with the wrong encoding - and they can't be stored in UTF-16.\n\n"
                     L"Yes: save it as UTF-8 instead, keeping those bytes as they are.\n"
                     L"No: save as UTF-16 anyway, replacing them with \x201C\xFFFD\x201D.");
        int answer = MessageBoxW(g_hMainWnd, message, L"MarkEdit", MB_YESNOCANCEL | MB_ICONWARNING);
        if (answer == IDCANCEL) return 0;
        if (answer == IDYES) {
            encoding = TEXT_ENCODING_UTF8;
            hasBom = 0;
        }
        /* The prompt's message loop runs timers - an external-change reload
           can replace the text under it - so the buffer is fetched again. */
        len = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
        text = (const char *)SendMessageA(g_hEdit, SCI_GETCHARACTERPOINTER, 0, 0);
        if (!text) {
            MessageBoxW(g_hMainWnd, L"Not enough memory to save the document.", L"MarkEdit", MB_OK | MB_ICONERROR);
            return 0;
        }
        if (g_largeFile)
            large_operation_progress_begin(&progress, L"Saving large file", L"Writing text...");
    } else {
        large_operation_progress_set_line(&progress, L"Writing text...");
    }
    WCHAR *displayPath = absolute_path(path);
    WCHAR *ioPath = extended_path(path);
    const WCHAR *target = ioPath ? ioPath : path;
    int ok = write_file_atomic_progress(target, text, (size_t)len, encoding, hasBom,
        progress.dialog ? large_operation_progress_callback : NULL, &progress);
    int cancelled = progress.cancelled;
    large_operation_progress_end(&progress);
    if (!ok) {
        free(displayPath);
        free(ioPath);
        if (!cancelled)
            MessageBoxW(g_hMainWnd, L"Could not write the complete file; the original was left unchanged.", L"MarkEdit", MB_OK | MB_ICONERROR);
        return 0;
    }
    g_encoding = encoding;
    g_hasBom = hasBom;
    /* Record the path before replace_wstring(): when saving the current file,
       `path` may alias g_currentFile, which replace_wstring() can free. */
    recent_list_add(&g_recentFileList, displayPath ? displayPath : path);
    replace_wstring(&g_currentFile, displayPath ? displayPath : path);
    apply_document_language(g_hEdit, g_currentFile);
    if (g_hSplitEdit) apply_document_language(g_hSplitEdit, g_currentFile);
    /* Save As changes the persistence key from the old path to the new one;
       write the current markers immediately instead of waiting for exit. */
    save_breakpoints_for_active_document(g_currentFile);
    free(ioPath);
    record_document_file_time(g_activeDocument);
    update_document_mapping();
    /* HTML's preview (see show_html_file_preview()) is a real navigation to
       the saved file, not a re-render of the live buffer like Markdown's -
       it only reflects this save's content once it's refreshed here. */
    if (g_activeLanguage == LANG_HTML) update_preview();
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
    if (!g_currentFile && g_activeLanguage == LANG_TEXT) {
        /* An untitled document defaults to plain text now (see
           language_for_path()); default Save As to match instead of always
           offering "Untitled.md", which would contradict the plain-text tab
           label and preview the user has been shown all along. */
        ofn.nFilterIndex = 4; /* "Text Files (*.txt)", the 4th filter entry above */
        ofn.lpstrDefExt = L"txt";
    } else {
        ofn.lpstrDefExt = L"md";
    }
    int result = GetSaveFileNameW(&ofn) ? save_to_path(path) : 0;
    free(path);
    return result;
}

static int do_save(void) {
    return g_currentFile ? save_to_path(g_currentFile) : do_saveas();
}

/* Saves every dirty tab in turn, the same switch-to-it-then-act pattern
   do_exit() uses to visit every document through the single shared editor.
   An untitled dirty document still prompts for a path via do_save()'s own
   Save As fallback, same as saving it individually would. Stop on the first
   cancellation or failure, and return to the originally selected tab. */
static int do_save_all(void) {
    int original = g_activeDocument;
    int ok = 1;
    for (int i = 0; i < g_documentCount; i++) {
        if (!g_documents[i].dirty) continue;
        switch_document(i);
        if (!do_save()) { ok = 0; break; }
    }
    if (original < g_documentCount) switch_document(original);
    return ok;
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
    size_t scriptCap = strlen(baseEscaped) + strlen(titleEscaped) + 512;
    char *script = (char *)malloc(scriptCap);
    if (!script) {
        free(path); free(baseUtf8); free(titleUtf8); free(baseEscaped); free(titleEscaped);
        return;
    }
    g_exportRequest++;
    snprintf(script, scriptCap,
        "(()=>{if(!window.exportRenderedHTML)return false;"
        "Promise.resolve().then(()=>window.exportRenderedHTML('%s','%s'))"
        ".then(html=>chrome.webview.postMessage('export-html:%lu:'+JSON.stringify(html)))"
        ".catch(()=>chrome.webview.postMessage('export-html:%lu:null'));return true;})()",
        baseEscaped, titleEscaped, g_exportRequest, g_exportRequest);
    g_exportPath = path;
    SetTimer(g_hMainWnd, IDT_HTML_EXPORT, 30000, NULL);
    update_preview();
    HRESULT hr = execute_webview_script(script, &g_exportScriptHandler);
    if (FAILED(hr)) {
        KillTimer(g_hMainWnd, IDT_HTML_EXPORT);
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
    if (g_pendingPrintAction != PRINT_ACTION_NONE) {
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
        g_pendingPrintAction = PRINT_ACTION_EXPORT_PDF;
    } else {
        g_pendingPrintAction = PRINT_ACTION_DIALOG;
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
    if (!g_webview || g_pendingPrintAction == PRINT_ACTION_NONE) return;
    if (g_pendingPrintAction == PRINT_ACTION_DIALOG) {
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
        g_pendingPrintAction = PRINT_ACTION_NONE;
    } else if (g_pendingPrintAction == PRINT_ACTION_EXPORT_PDF && g_pendingPdfPath) {
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
        g_pendingPrintAction = PRINT_ACTION_NONE;
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
    /* Holding a large-file tab's document for Reopen Closed Tab would keep
       the whole file in memory; it keeps only its path and is read from
       disk again on reopen (see do_reopen_closed_tab()). */
    if (removed.largeFile && removed.sciDocument) {
        SendMessageA(g_hEdit, SCI_RELEASEDOCUMENT, 0, (LPARAM)removed.sciDocument);
        removed.sciDocument = 0;
    }
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
    save_all_document_breakpoints();
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
        set_active_editor_document(newDoc);
        configure_scintilla(g_hEdit);
        apply_editor_font(g_fontFaceName, g_fontPointSize);
        if (g_hSplitEdit) apply_document_language(g_hSplitEdit, g_currentFile);
        SendMessageA(g_hEdit, SCI_CLEARALL, 0, 0);
        SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
        SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
        g_switchingDocument = 0;
        push_closed_document(removed, 0);
        g_recoverySnapshotStale = 1;
        write_recovery_snapshot();
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
    set_active_editor_document(g_documents[g_activeDocument].sciDocument);
    apply_editor_font(g_fontFaceName, g_fontPointSize);
    apply_document_language(g_hEdit, g_currentFile);
    if (g_hSplitEdit) apply_document_language(g_hSplitEdit, g_currentFile);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)g_documents[g_activeDocument].anchor,
                 (LPARAM)g_documents[g_activeDocument].caret);
    g_switchingDocument = 0;
    push_closed_document(removed, removeIndex);
    g_recoverySnapshotStale = 1;
    write_recovery_snapshot();
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
    if (!restored.sciDocument) { /* a large-file tab, see push_closed_document() */
        if (restored.path) open_file_in_tab(restored.path);
        free(restored.path);
        return;
    }
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
        set_active_editor_document(restored.sciDocument);
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
        set_active_editor_document(restored.sciDocument);
        g_switchingDocument = 0;
        TCITEMW item = {0};
        item.mask = TCIF_TEXT;
        item.pszText = L"";
        SendMessageW(g_hTabs, TCM_INSERTITEMW, (WPARAM)index, (LPARAM)&item);
    }
    apply_editor_font(g_fontFaceName, g_fontPointSize);
    apply_document_language(g_hEdit, g_currentFile);
    if (g_hSplitEdit) apply_document_language(g_hSplitEdit, g_currentFile);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)restored.anchor, (LPARAM)restored.caret);
    SendMessageA(g_hEdit, SCI_SETFIRSTVISIBLELINE, (WPARAM)restored.firstVisibleLine, 0);
    SendMessageA(g_hEdit, SCI_SETXOFFSET, (WPARAM)restored.xOffset, 0);
    TabCtrl_SetCurSel(g_hTabs, index);
    update_tab_label(index);
    update_linenumber_margin_width(g_hEdit);
    g_recoverySnapshotStale = 1;
    write_recovery_snapshot();
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
    if (doc->externalDeleted) {
        MessageBoxW(g_hMainWnd,
                    g_dirty
                        ? L"This file was deleted or moved on disk. Your unsaved text is still open; saving will recreate the old path."
                        : L"This file was deleted or moved on disk. Its text remains open; saving will recreate the old path.",
                    L"MarkEdit", MB_OK | MB_ICONWARNING);
        return;
    }
    const WCHAR *message = g_dirty
        ? L"This file changed on disk. Reload it and discard your unsaved changes?"
        : L"This file changed on disk. Reload the updated version?";
    if (MessageBoxW(g_hMainWnd, message, L"MarkEdit", MB_YESNO | MB_ICONQUESTION) == IDYES) {
        WCHAR *path = wcsdup_heap(g_currentFile);
        if (path) load_file_as(path, doc->encodingForced ? doc->encoding : -1);
        free(path);
    }
}

static void check_external_file_change(void) {
    int changedVisual = 0;
    for (int i = 0; i < g_documentCount; i++) {
        Document *doc = &g_documents[i];
        if (!doc->path) continue;
        WCHAR *ioPath = extended_path(doc->path);
        if (!ioPath) continue;
        WIN32_FILE_ATTRIBUTE_DATA data;
        int exists = GetFileAttributesExW(ioPath, GetFileExInfoStandard, &data);
        DWORD attributeError = exists ? ERROR_SUCCESS : GetLastError();
        free(ioPath);
        if (!exists) {
            if (attributeError != ERROR_FILE_NOT_FOUND && attributeError != ERROR_PATH_NOT_FOUND)
                continue;
            if (doc->hasFileTime && !doc->externalDeleted) {
                doc->externalDeleted = 1;
                doc->externalChanged = 1;
                changedVisual = 1;
            }
            doc->hasFileTime = 0;
            continue;
        }
        if (!doc->hasFileTime) {
            doc->fileTime = data.ftLastWriteTime;
            doc->hasFileTime = 1;
            if (doc->externalDeleted) {
                doc->externalDeleted = 0;
                doc->externalChanged = 1;
                changedVisual = 1;
            }
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
    save_all_document_breakpoints();
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
            CreateWindowExA(0, "BUTTON", "Go to",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
                48, 50, 76, 25, hwnd, (HMENU)IDOK, GetModuleHandleA(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                132, 50, 76, 25, hwnd, (HMENU)IDCANCEL, GetModuleHandleA(NULL), NULL);
            char current[32];
            Sci_Position line = (Sci_Position)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION,
                SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0), 0) + 1;
            snprintf(current, sizeof(current), "%lld", (long long)line);
            SetWindowTextA(g_hGotoEdit, current);
            SendMessageA(g_hGotoEdit, EM_SETSEL, 0, -1);
            SetFocus(g_hGotoEdit);
            popup_dialog_theme_opened(hwnd);
            return 0;
        }
        case WM_ERASEBKGND: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: {
            LRESULT handled = 0;
            LRESULT result = popup_dialog_theme_message(hwnd, msg, wParam, &handled);
            if (handled) return result;
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_BUTTON) { draw_popup_button(dis); return TRUE; }
            break;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                char value[32];
                GetWindowTextA(g_hGotoEdit, value, sizeof(value));
                long line = strtol(value, NULL, 10);
                Sci_Position lineCount = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
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
        case WM_NCDESTROY:
            popup_dialog_theme_destroyed();
            break;
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
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
            12, 64, 556, 286, hwnd, (HMENU)11, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Open",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
            404, 360, 78, 26, hwnd, (HMENU)IDOK, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            490, 360, 78, 26, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(NULL), NULL);
        themed_scrollbar_attach(state->list, THEMED_SCROLL_LISTBOX);
        refill_choice_list(state);
        SetFocus(state->edit);
        popup_dialog_theme_opened(hwnd);
        return 0;
    }
    if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
        case WM_ERASEBKGND: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
            LRESULT handled = 0;
            LRESULT result = popup_dialog_theme_message(hwnd, msg, wParam, &handled);
            if (handled) return result;
            break;
        }
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lParam;
            if (mis->CtlType == ODT_LISTBOX) { measure_themed_listbox_item(state->list, mis); return TRUE; }
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_BUTTON) { draw_popup_button(dis); return TRUE; }
            if (dis->CtlType == ODT_LISTBOX && dis->hwndItem == state->list) {
                draw_themed_listbox_item(state->list, dis);
                return TRUE;
            }
            break;
        }
        case WM_NCDESTROY:
            popup_dialog_theme_destroyed();
            break;
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
        CreateWindowExW(0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
            244, 76, 78, 26, hwnd, (HMENU)IDOK, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            330, 76, 78, 26, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(NULL), NULL);
        SendMessageW(state->edit, EM_SETSEL, 0, -1);
        SetFocus(state->edit);
        popup_dialog_theme_opened(hwnd);
        return 0;
    }
    if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);
    if (msg == WM_ERASEBKGND || msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT) {
        LRESULT handled = 0;
        LRESULT result = popup_dialog_theme_message(hwnd, msg, wParam, &handled);
        if (handled) return result;
    }
    if (msg == WM_DRAWITEM) {
        const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
        if (dis->CtlType == ODT_BUTTON) { draw_popup_button(dis); return TRUE; }
    }
    if (msg == WM_NCDESTROY) popup_dialog_theme_destroyed();
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

/* One line matched by a workspace-wide Find/Replace search - see
   do_replace_in_workspace() below. `label` is the "path:line — snippet"
   text shown for the match (without the checkbox prefix ReplaceReviewWndProc
   adds); `accepted` is whether the review dialog's checkbox is ticked. */
typedef struct ReplaceMatch {
    WCHAR *path;
    WCHAR *label;
    int line; /* 1-based */
    int accepted;
} ReplaceMatch;

typedef struct ReplaceReviewState {
    ReplaceMatch *matches;
    int count;
    HWND list;
    HWND countLabel;
    int confirmed;
} ReplaceReviewState;

static void update_replace_count_label(ReplaceReviewState *state) {
    if (!state->countLabel) return;
    int checked = 0;
    for (int i = 0; i < state->count; i++) if (state->matches[i].accepted) checked++;
    WCHAR text[64];
    swprintf(text, 64, L"%d of %d matches selected", checked, state->count);
    SetWindowTextW(state->countLabel, text);
}

static void refill_replace_list(ReplaceReviewState *state) {
    SendMessageW(state->list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < state->count; i++) {
        size_t cap = wcslen(state->matches[i].label) + 8;
        WCHAR *row = (WCHAR *)malloc(cap * sizeof(WCHAR));
        if (!row) continue;
        swprintf(row, cap, L"[%lc] %ls",
                 (wint_t)(state->matches[i].accepted ? L'x' : L' '),
                 state->matches[i].label);
        int idx = (int)SendMessageW(state->list, LB_ADDSTRING, 0, (LPARAM)row);
        if (idx >= 0) SendMessageW(state->list, LB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);
        free(row);
    }
    if (SendMessageW(state->list, LB_GETCOUNT, 0, 0) > 0)
        SendMessageW(state->list, LB_SETCURSEL, 0, 0);
    update_replace_count_label(state);
}

/* Flips the checked state of the match shown at listbox row `row` and
   rewrites just that row's text in place, preserving its selection. */
static void toggle_replace_row(ReplaceReviewState *state, int row) {
    if (row == LB_ERR) return;
    int matchIndex = (int)SendMessageW(state->list, LB_GETITEMDATA, (WPARAM)row, 0);
    if (matchIndex < 0 || matchIndex >= state->count) return;
    state->matches[matchIndex].accepted = !state->matches[matchIndex].accepted;
    size_t cap = wcslen(state->matches[matchIndex].label) + 8;
    WCHAR *text = (WCHAR *)malloc(cap * sizeof(WCHAR));
    if (text) {
        swprintf(text, cap, L"[%lc] %ls",
                 (wint_t)(state->matches[matchIndex].accepted ? L'x' : L' '),
                 state->matches[matchIndex].label);
        SendMessageW(state->list, LB_DELETESTRING, (WPARAM)row, 0);
        SendMessageW(state->list, LB_INSERTSTRING, (WPARAM)row, (LPARAM)text);
        SendMessageW(state->list, LB_SETITEMDATA, (WPARAM)row, (LPARAM)matchIndex);
        SendMessageW(state->list, LB_SETCURSEL, (WPARAM)row, 0);
        free(text);
    }
    update_replace_count_label(state);
}

/* Space toggles the selected row's checkbox, the same convention as a
   Windows Explorer checkbox listbox - listboxes don't offer this by
   themselves, so it's added via the same safe-subclassing API already used
   for g_hTabs/g_hEdit/g_hMinimap elsewhere in this file. */
static LRESULT CALLBACK ReplaceListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                 UINT_PTR subclassId, DWORD_PTR refData) {
    (void)subclassId;
    if (msg == WM_KEYDOWN && wParam == VK_SPACE) {
        toggle_replace_row((ReplaceReviewState *)refData, (int)SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ReplaceListSubclassProc, subclassId);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK ReplaceReviewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ReplaceReviewState *state = (ReplaceReviewState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        state = (ReplaceReviewState *)((CREATESTRUCTW *)lParam)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
        CreateWindowExW(0, L"STATIC", L"Uncheck any matches you don't want replaced:",
            WS_CHILD | WS_VISIBLE, 12, 10, 440, 20, hwnd, NULL, GetModuleHandleW(NULL), NULL);
        state->countLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
            456, 10, 112, 20, hwnd, NULL, GetModuleHandleW(NULL), NULL);
        state->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
            12, 34, 556, 316, hwnd, (HMENU)11, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Select All", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            12, 360, 96, 26, hwnd, (HMENU)12, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Select None", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            114, 360, 96, 26, hwnd, (HMENU)13, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Replace",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
            412, 360, 78, 26, hwnd, (HMENU)IDOK, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            496, 360, 78, 26, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(NULL), NULL);
        themed_scrollbar_attach(state->list, THEMED_SCROLL_LISTBOX);
        SetWindowSubclass(state->list, ReplaceListSubclassProc, 1, (DWORD_PTR)state);
        refill_replace_list(state);
        SetFocus(state->list);
        popup_dialog_theme_opened(hwnd);
        return 0;
    }
    if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
        case WM_ERASEBKGND: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
            LRESULT handled = 0;
            LRESULT result = popup_dialog_theme_message(hwnd, msg, wParam, &handled);
            if (handled) return result;
            break;
        }
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lParam;
            if (mis->CtlType == ODT_LISTBOX) { measure_themed_listbox_item(state->list, mis); return TRUE; }
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_BUTTON) { draw_popup_button(dis); return TRUE; }
            if (dis->CtlType == ODT_LISTBOX && dis->hwndItem == state->list) {
                draw_themed_listbox_item(state->list, dis);
                return TRUE;
            }
            break;
        }
        case WM_NCDESTROY:
            popup_dialog_theme_destroyed();
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == 11 && HIWORD(wParam) == LBN_DBLCLK) {
                toggle_replace_row(state, (int)SendMessageW(state->list, LB_GETCURSEL, 0, 0));
                return 0;
            }
            if (LOWORD(wParam) == 12) { /* Select All */
                for (int i = 0; i < state->count; i++) state->matches[i].accepted = 1;
                refill_replace_list(state);
                return 0;
            }
            if (LOWORD(wParam) == 13) { /* Select None */
                for (int i = 0; i < state->count; i++) state->matches[i].accepted = 0;
                refill_replace_list(state);
                return 0;
            }
            if (LOWORD(wParam) == IDOK) {
                state->confirmed = 1;
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
            break;
        case WM_SIZE: {
            int width = LOWORD(lParam), height = HIWORD(lParam);
            MoveWindow(state->countLabel, width - 124, 10, 112, 20, TRUE);
            MoveWindow(state->list, 12, 34, width - 24, height - 78, TRUE);
            return 0;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* Shows the checkbox review list for a workspace-wide replace; `matches`
   is edited in place (accepted flags) and the function returns 1 if the
   user clicked Replace, 0 if they cancelled. */
static int show_replace_review_dialog(ReplaceMatch *matches, int count) {
    static int registered = 0;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = ReplaceReviewWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MarkEditReplaceDialog";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 0;
        registered = 1;
    }
    ReplaceReviewState state = { matches, count, NULL, NULL, 0 };
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MarkEditReplaceDialog", L"Replace in Folder",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 440, g_hMainWnd, NULL,
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
    return state.confirmed;
}

static void find_in_edit(const char *needle, int matchCase, int down) {
    if (!needle || !needle[0]) return;
    Sci_Position len = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    Sci_Position selStart = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    int needleLen = (int)strlen(needle);
    Sci_Position foundAt = -1;
    SendMessageA(g_hEdit, SCI_SETSEARCHFLAGS, matchCase ? SCFIND_MATCHCASE : 0, 0);

    if (down) {
        SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)selEnd, 0);
        SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)len, 0);
        foundAt = (Sci_Position)SendMessageA(g_hEdit, SCI_SEARCHINTARGET, (WPARAM)needleLen, (LPARAM)needle);
        if (foundAt < 0) {
            SendMessageA(g_hEdit, SCI_SETTARGETSTART, 0, 0);
            SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)selStart, 0);
            foundAt = (Sci_Position)SendMessageA(g_hEdit, SCI_SEARCHINTARGET, (WPARAM)needleLen, (LPARAM)needle);
        }
    } else {
        SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)selStart, 0);
        SendMessageA(g_hEdit, SCI_SETTARGETEND, 0, 0);
        foundAt = (Sci_Position)SendMessageA(g_hEdit, SCI_SEARCHINTARGET, (WPARAM)needleLen, (LPARAM)needle);
        if (foundAt < 0) {
            SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)len, 0);
            SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)selEnd, 0);
            foundAt = (Sci_Position)SendMessageA(g_hEdit, SCI_SEARCHINTARGET, (WPARAM)needleLen, (LPARAM)needle);
        }
    }

    if (foundAt >= 0) {
        Sci_Position foundEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETTARGETEND, 0, 0);
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)foundAt, (LPARAM)foundEnd);
        SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
        SetFocus(g_hEdit);
    } else {
        MessageBeep(MB_ICONASTERISK);
    }
}

static void replace_selection_and_find(const char *findWhat, const char *replaceWith, int matchCase) {
    Sci_Position selStart = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (selEnd > selStart) {
        int findLen = (int)strlen(findWhat);
        SendMessageA(g_hEdit, SCI_SETSEARCHFLAGS, matchCase ? SCFIND_MATCHCASE : 0, 0);
        SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)selStart, 0);
        SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)selEnd, 0);
        Sci_Position found = (Sci_Position)SendMessageA(g_hEdit, SCI_SEARCHINTARGET, (WPARAM)findLen, (LPARAM)findWhat);
        Sci_Position foundEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETTARGETEND, 0, 0);
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
        size_t count = 0;
        int findLen = (int)strlen(findUtf8);
        int replaceLen = (int)strlen(replaceUtf8);
        Sci_Position position = 0;
        Sci_Position documentLength = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
        SendMessageA(g_hEdit, SCI_SETSEARCHFLAGS, matchCase ? SCFIND_MATCHCASE : 0, 0);
        SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
        while (findLen > 0 && position <= documentLength) {
            SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)position, 0);
            SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)documentLength, 0);
            Sci_Position found = (Sci_Position)SendMessageA(g_hEdit, SCI_SEARCHINTARGET,
                                           (WPARAM)findLen, (LPARAM)findUtf8);
            if (found < 0) break;
            Sci_Position oldEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETTARGETEND, 0, 0);
            SendMessageA(g_hEdit, SCI_REPLACETARGET, (WPARAM)replaceLen, (LPARAM)replaceUtf8);
            documentLength += replaceLen - (oldEnd - found);
            position = found + replaceLen;
            count++;
        }
        SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
        WCHAR msg[64];
        swprintf(msg, sizeof(msg) / sizeof(msg[0]), L"Replaced %zu occurrence(s).", count);
        MessageBoxW(g_hMainWnd, msg, L"MarkEdit", MB_OK | MB_ICONINFORMATION);
    }
    free(findUtf8);
    free(replaceUtf8);
}

/* ChooseFontW hands the chosen font back through LOGFONTW.lfFaceName, a
   fixed WCHAR[LF_FACESIZE] (32) field - a hard limit of that struct, not of
   anything downstream. A face name at or past that length (e.g. "Google
   Sans Code Medium Monospace", 33 characters) comes back silently truncated
   to 31 characters plus NUL, which then doesn't match any installed font:
   Scintilla falls back to a default and the setting "resets" itself on
   every restart. Reading the dialog's own font-name combo (control ID
   cmb1, from <dlgs.h> - the font dialog's standard layout is cmb1 name/
   cmb2 style/cmb3 size, with cmb4 for the color combo that only exists
   when CF_EFFECTS is set, which this dialog doesn't use) via this hook
   captures the real, untruncated name before ChooseFontW's return path
   discards the extra characters. */
static WCHAR g_choseFontFaceFull[MAX_FONT_FACE_NAME];
static int g_choseFontFaceFullValid = 0;

static UINT_PTR CALLBACK choose_font_hook(HWND hdlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    if (msg == WM_INITDIALOG) {
        /* CF_INITTOLOGFONTSTRUCT seeded the dialog from lf.lfFaceName, a
           WCHAR[32] LOGFONTW field truncated at do_choose_font()'s call
           site - so a saved name at or past that length shows up here
           mangled, looking like the previous choice was forgotten even
           though g_fontFaceName itself is intact. The default dialog
           procedure has already populated the combo by the time the hook
           sees WM_INITDIALOG, so overwrite it with the untruncated name. */
        SetDlgItemTextW(hdlg, cmb1, g_fontFaceName);
    } else if (msg == WM_COMMAND && LOWORD(wParam) == IDOK) {
        int len = GetDlgItemTextW(hdlg, cmb1, g_choseFontFaceFull,
                                   MAX_FONT_FACE_NAME);
        g_choseFontFaceFullValid = len > 0;
    }
    return 0; /* let the dialog's own procedure handle every message as usual */
}

static void do_choose_font(void) {
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    wcsncpy(lf.lfFaceName, g_fontFaceName, LF_FACESIZE - 1);
    lf.lfWeight = g_fontWeight;
    lf.lfItalic = (BYTE)g_fontItalic;
    HDC hdc = GetDC(g_hEdit);
    lf.lfHeight = -MulDiv(g_fontPointSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(g_hEdit, hdc);

    CHOOSEFONTW cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = g_hMainWnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOSCRIPTSEL | CF_ENABLEHOOK;
    cf.lpfnHook = choose_font_hook;

    g_choseFontFaceFullValid = 0;
    if (ChooseFontW(&cf)) {
        if (g_choseFontFaceFullValid) {
            wcsncpy(g_fontFaceName, g_choseFontFaceFull, MAX_FONT_FACE_NAME - 1);
        } else {
            wcsncpy(g_fontFaceName, lf.lfFaceName, LF_FACESIZE - 1);
        }
        g_fontFaceName[MAX_FONT_FACE_NAME - 1] = 0;
        g_fontPointSize = cf.iPointSize / 10;
        if (g_fontPointSize < 1) g_fontPointSize = 1;
        g_fontWeight = lf.lfWeight ? lf.lfWeight : FW_NORMAL;
        g_fontItalic = lf.lfItalic ? 1 : 0;
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

/* ---- Help > Markdown Reference and Keyboard Shortcuts. ----
   The page text itself lives in help_reference.c/.h. */

/* Opens a Help page as an ordinary unsaved document, so the live preview
   renders it and every construct in it can be edited and experimented with.
   Each page reuses its own tab if it's already open. */
static void show_help_page(ReferenceKind kind) {
    for (int i = 0; i < g_documentCount; i++) {
        if (g_documents[i].isReference == (int)kind) {
            switch_document(i);
            return;
        }
    }
    if (!create_document_tab()) return;
    g_documents[g_activeDocument].isReference = kind;
    /* create_document_tab() just defaulted this untitled tab's language to
       plain text (language_for_path(NULL)). This tab's content is always
       Markdown regardless, so force it back - passing any name that merely
       *ends* in ".md" is enough, since language_for_path only inspects the
       extension and never treats this string as a real path. */
    apply_document_language(g_hEdit, L"markdown-reference.md");
    if (g_hSplitEdit) apply_document_language(g_hSplitEdit, L"markdown-reference.md");
    g_switchingDocument = 1;
    const char *page = kind == REFERENCE_SHORTCUTS ? help_shortcuts_markdown()
                                                   : help_reference_markdown();
    set_edit_text(g_hEdit, page, (Sci_Position)strlen(page));
    SendMessageA(g_hEdit, SCI_SETSEL, 0, 0);
    SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    g_switchingDocument = 0;
    if (!g_previewVisible) toggle_preview(g_hMainWnd);
    update_tab_label(g_activeDocument);
    update_title();
    update_preview();
}

/* Help > About. The version is read from the exe's own VERSIONINFO
   resource (markedit.rc) rather than repeated here, so the two can't drift;
   VS_FIXEDFILEINFO is located by its signature, which avoids linking
   version.lib just for VerQueryValue(). */
static void show_about_box(void) {
    WCHAR version[32] = L"";
    HRSRC resource = FindResourceW(NULL, MAKEINTRESOURCEW(VS_VERSION_INFO), MAKEINTRESOURCEW(16) /* RT_VERSION */);
    HGLOBAL loaded = resource ? LoadResource(NULL, resource) : NULL;
    const BYTE *bytes = loaded ? (const BYTE *)LockResource(loaded) : NULL;
    DWORD size = resource ? SizeofResource(NULL, resource) : 0;
    for (DWORD offset = 0; bytes && offset + sizeof(VS_FIXEDFILEINFO) <= size; offset += 4) {
        const VS_FIXEDFILEINFO *info = (const VS_FIXEDFILEINFO *)(bytes + offset);
        if (info->dwSignature != 0xFEEF04BD) continue;
        swprintf(version, sizeof(version) / sizeof(version[0]), L"%u.%u.%u",
                 (unsigned)HIWORD(info->dwFileVersionMS), (unsigned)LOWORD(info->dwFileVersionMS),
                 (unsigned)HIWORD(info->dwFileVersionLS));
        break;
    }
    WCHAR text[512];
    swprintf(text, sizeof(text) / sizeof(text[0]),
             L"MarkEdit%ls%ls\n"
             L"A Markdown and source code editor for Windows.\n\n"
             L"Copyright (c) 2026 Les Farrell\n\n"
             L"Built with Scintilla, Lexilla, cmark-gfm, Microsoft Edge WebView2, "
             L"highlight.js, KaTeX, and Mermaid. See THIRD_PARTY_NOTICES.md for "
             L"their licences.",
             version[0] ? L" " : L"", version);
    MSGBOXPARAMSW box = {0};
    box.cbSize = sizeof(box);
    box.hwndOwner = g_hMainWnd;
    box.hInstance = GetModuleHandleW(NULL);
    box.lpszText = text;
    box.lpszCaption = L"About MarkEdit";
    box.dwStyle = MB_OK | MB_USERICON;
    box.lpszIcon = MAKEINTRESOURCEW(IDI_APPICON);
    MessageBoxIndirectW(&box);
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
   switches its private dark-mode preference at runtime. Draw just those
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
    COLORREF background = g_editorDarkMode
        ? (active ? RGB(0x21, 0x26, 0x2D) : THEME_DARK_BG)
        : (active ? RGB(0xE8, 0xEC, 0xF1) : THEME_LIGHT_BG);
    COLORREF foreground = (dis->itemState & (ODS_DISABLED | ODS_GRAYED))
        ? (g_editorDarkMode ? RGB(0x8B, 0x94, 0x9E) : RGB(0x65, 0x6D, 0x76))
        : theme_fg_color();

    fill_rect_color(dis->hDC, &dis->rcItem, background);

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
    char ansiLabel[48];
    snprintf(ansiLabel, sizeof(ansiLabel), "&ANSI (code page %u)", text_ansi_code_page());
    HMENU hReopenEncoding = CreatePopupMenu();
    AppendMenuA(hReopenEncoding, MF_STRING, IDM_FILE_REOPEN_UTF8, "UTF-&8");
    AppendMenuA(hReopenEncoding, MF_STRING, IDM_FILE_REOPEN_UTF16LE, "UTF-16 &LE");
    AppendMenuA(hReopenEncoding, MF_STRING, IDM_FILE_REOPEN_UTF16BE, "UTF-16 &BE");
    AppendMenuA(hReopenEncoding, MF_STRING, IDM_FILE_REOPEN_ANSI, ansiLabel);
    AppendMenuA(hFile, MF_POPUP | (g_currentFile ? 0 : MF_GRAYED), (UINT_PTR)hReopenEncoding,
                "Reopen with &Encoding");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_QUICK_OPEN, "&Quick Open in Folder...\tCtrl+K");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_FIND_FOLDER, "Find in F&older...");
    AppendMenuA(hFile, MF_STRING, IDM_REPLACE_WORKSPACE, "Replace in Fol&der...");
    AppendMenuA(hFile, MF_STRING, IDM_OPEN_WORKSPACE, "Open &Workspace Folder...");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_INSERT, "&Insert File at Cursor...");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_SAVE, "&Save\tCtrl+S");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_SAVEAS, "Save &As...\tCtrl+Shift+S");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_SAVE_ALL, "Save A&ll\tCtrl+Alt+S");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_EXPORT_HTML, "Export Rendered &HTML...");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_EXPORT_PDF, "Export Rendered &PDF...");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_PRINT, "&Print Rendered Preview...\tCtrl+P");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_OPEN_TERMINAL, "Open &Terminal Here");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_SHOW_EXPLORER, "Show in E&xplorer");
    AppendMenuA(hFile, MF_STRING, IDM_FILE_CLOSE, "&Close Tab\tCtrl+W");
    AppendMenuA(hFile, MF_STRING | (g_closedDocumentCount ? 0 : MF_GRAYED),
                IDM_FILE_REOPEN_CLOSED, "&Reopen Closed Tab\tCtrl+Shift+T");
    AppendMenuA(hFile, MF_STRING | (g_wikiBackCount ? 0 : MF_GRAYED),
                IDM_FILE_GO_BACK, "Go &Back\tAlt+Left");
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

    HMENU hRecentWorkspaces = CreatePopupMenu();
    if (g_recentFolderCount == 0) {
        AppendMenuA(hRecentWorkspaces, MF_STRING | MF_GRAYED, 0, "(no recent workspaces)");
    } else {
        for (int i = 0; i < g_recentFolderCount; i++) {
            size_t cap = wcslen(g_recentFolders[i]) * 2 + 16;
            WCHAR *escaped = (WCHAR *)malloc(cap * sizeof(WCHAR));
            WCHAR *label = (WCHAR *)malloc((cap + 16) * sizeof(WCHAR));
            if (escaped && label) {
                escape_ampersands(g_recentFolders[i], escaped, cap);
                swprintf(label, cap + 16, L"&%d %ls", i + 1, escaped);
                AppendMenuW(hRecentWorkspaces, MF_STRING, IDM_RECENT_WORKSPACE + i, label);
            }
            free(escaped);
            free(label);
        }
        AppendMenuA(hRecentWorkspaces, MF_SEPARATOR, 0, NULL);
        AppendMenuA(hRecentWorkspaces, MF_STRING, IDM_RECENT_WORKSPACE_CLEAR, "&Clear Recent Workspaces");
    }
    AppendMenuA(hFile, MF_POPUP, (UINT_PTR)hRecentWorkspaces, "Recent Wor&kspaces");
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
    HMENU hEncoding = CreatePopupMenu();
    AppendMenuA(hEncoding, MF_STRING, IDM_EDIT_ENCODING_UTF8, "UTF-&8");
    AppendMenuA(hEncoding, MF_STRING, IDM_EDIT_ENCODING_UTF8_BOM, "UTF-8 with &BOM");
    AppendMenuA(hEncoding, MF_STRING, IDM_EDIT_ENCODING_UTF16LE, "UTF-16 &LE");
    AppendMenuA(hEncoding, MF_STRING, IDM_EDIT_ENCODING_UTF16BE, "UTF-16 B&E");
    AppendMenuA(hEncoding, MF_STRING, IDM_EDIT_ENCODING_ANSI, ansiLabel);
    AppendMenuA(hConvert, MF_POPUP, (UINT_PTR)hEncoding, "En&coding");
    AppendMenuA(hEditMenu, MF_POPUP, (UINT_PTR)hConvert, "Con&vert");
    AppendMenuA(hEditMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_FIND, "&Find...\tCtrl+F");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_FINDNEXT, "Find &Next\tF3");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_REPLACE, "&Replace...\tCtrl+H");
    AppendMenuA(hEditMenu, MF_STRING, IDM_EDIT_GOTOLINE, "&Go to Line...\tCtrl+G");
    AppendMenuA(hMenuBar, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)hEditMenu, "&Edit");

    HMENU hView = CreatePopupMenu();
    AppendMenuA(hView, MF_STRING | (g_previewVisible ? MF_CHECKED : MF_UNCHECKED), IDM_VIEW_TOGGLEPREVIEW, "Show &Preview\tCtrl+Shift+V");
    AppendMenuA(hView, MF_STRING | (g_editorVisible ? MF_CHECKED : MF_UNCHECKED), IDM_VIEW_TOGGLEEDITOR, "Show &Editor\tCtrl+Shift+E");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hView, MF_STRING, IDM_VIEW_NEXT_TAB, "&Next Tab\tCtrl+Tab");
    AppendMenuA(hView, MF_STRING, IDM_VIEW_PREVIOUS_TAB, "&Previous Tab\tCtrl+Shift+Tab");
    AppendMenuA(hView, MF_STRING, IDM_VIEW_CYCLE_PANE, "Cycle &Panes\tCtrl+F6");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    HMENU hTheme = CreatePopupMenu();
    AppendMenuA(hTheme, MF_STRING, IDM_VIEW_THEME_GITHUB_LIGHT, "&Light");
    AppendMenuA(hTheme, MF_STRING, IDM_VIEW_THEME_GITHUB_DARK, "&Dark");
    CheckMenuRadioItem(hTheme, IDM_VIEW_THEME_GITHUB_LIGHT, IDM_VIEW_THEME_GITHUB_DARK,
                       g_editorDarkMode ? IDM_VIEW_THEME_GITHUB_DARK : IDM_VIEW_THEME_GITHUB_LIGHT,
                       MF_BYCOMMAND);
    AppendMenuA(hView, MF_POPUP, (UINT_PTR)hTheme, "&Editor Theme");
    HMENU hPreviewTheme = CreatePopupMenu();
    AppendMenuA(hPreviewTheme, MF_STRING, IDM_VIEW_PREVIEW_THEME_LIGHT, "GitHub &Light");
    AppendMenuA(hPreviewTheme, MF_STRING, IDM_VIEW_PREVIEW_THEME_DARK, "GitHub &Dark");
    CheckMenuRadioItem(hPreviewTheme, IDM_VIEW_PREVIEW_THEME_LIGHT, IDM_VIEW_PREVIEW_THEME_DARK,
                       g_previewDarkMode ? IDM_VIEW_PREVIEW_THEME_DARK : IDM_VIEW_PREVIEW_THEME_LIGHT,
                       MF_BYCOMMAND);
    AppendMenuA(hPreviewTheme, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hPreviewTheme, MF_STRING, IDM_VIEW_CUSTOM_CSS, "Custom Preview &CSS...");
    AppendMenuA(hPreviewTheme, MF_STRING | (g_customCssPath ? 0 : MF_GRAYED),
                IDM_VIEW_CLEAR_CUSTOM_CSS, "C&lear Custom Preview CSS");
    AppendMenuA(hView, MF_POPUP, (UINT_PTR)hPreviewTheme, "&Markdown Theme");
    AppendMenuA(hView, MF_STRING, IDM_FORMAT_FONT, "Fon&t...");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hView, MF_STRING | (g_previewOnLeft ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_PREVIEW_ON_LEFT, "Preview on &Left");
    AppendMenuA(hView, MF_STRING | (g_focusMode ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_FOCUS_MODE, "&Focus Mode\tCtrl+Shift+F11");
    AppendMenuA(hView, MF_STRING | (g_wordWrap ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_WORD_WRAP, "&Word Wrap\tAlt+Z");
    AppendMenuA(hView, MF_STRING | (g_spellCheckEnabled ? MF_CHECKED : MF_UNCHECKED) |
                (g_spellChecker ? 0 : MF_GRAYED), IDM_VIEW_SPELL_CHECK, "&Spell Check");
    AppendMenuA(hView, MF_STRING, IDM_VIEW_SPELL_LANGUAGE, "Spell Check &Language...");
    AppendMenuA(hView, MF_STRING | (g_outlineVisible ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_OUTLINE, "Document &Outline");
    AppendMenuA(hView, MF_STRING | (g_explorerVisible ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_EXPLORER, "&File Explorer\tCtrl+Shift+X");
    AppendMenuA(hView, MF_STRING | (g_minimapVisible ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_MINIMAP, "Mini&map");
    AppendMenuA(hView, MF_STRING | (g_splitEditorVisible ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_TOGGLESPLIT, "Split &Editor\tCtrl+\\");
    AppendMenuA(hView, MF_STRING | (g_splitEditorVisible ? 0 : MF_GRAYED),
                IDM_VIEW_RESET_SPLIT_EDITOR, "&Reset Split Editor");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hView, MF_STRING | (g_showWhitespace ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_SHOW_WHITESPACE, "Show W&hitespace");
    AppendMenuA(hView, MF_STRING | (g_indentGuides ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_INDENT_GUIDES, "&Indent Guides");
    AppendMenuA(hView, MF_STRING | (g_indentUseSpaces ? MF_CHECKED : MF_UNCHECKED),
                IDM_VIEW_USE_SPACES_INDENT, "&Use Spaces for Indentation");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hView, MF_STRING, IDM_VIEW_FOLD_TOGGLE, "Toggle Fo&ld");
    AppendMenuA(hView, MF_STRING, IDM_VIEW_FOLD_ALL, "Fold &All");
    AppendMenuA(hView, MF_STRING, IDM_VIEW_UNFOLD_ALL, "&Unfold All");
    AppendMenuA(hView, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hView, MF_STRING, IDM_VIEW_ZOOM_IN, "Zoom &In\tCtrl+=");
    AppendMenuA(hView, MF_STRING, IDM_VIEW_ZOOM_OUT, "Zoom &Out\tCtrl+-");
    AppendMenuA(hView, MF_STRING, IDM_VIEW_ZOOM_RESET, "&Reset Zoom\tCtrl+0");
    AppendMenuA(hMenuBar, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)hView, "&View");

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
    HMENU hTableOps = CreatePopupMenu();
    AppendMenuA(hTableOps, MF_STRING, IDM_FMT_TABLE_ROW_ABOVE, "Insert Row &Above");
    AppendMenuA(hTableOps, MF_STRING, IDM_FMT_TABLE_ROW_BELOW, "Insert Row &Below");
    AppendMenuA(hTableOps, MF_STRING, IDM_FMT_TABLE_ROW_DELETE, "&Delete Row");
    AppendMenuA(hTableOps, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hTableOps, MF_STRING, IDM_FMT_TABLE_COL_LEFT, "Insert Column &Left");
    AppendMenuA(hTableOps, MF_STRING, IDM_FMT_TABLE_COL_RIGHT, "Insert Column &Right");
    AppendMenuA(hTableOps, MF_STRING, IDM_FMT_TABLE_COL_DELETE, "Delete Colu&mn");
    AppendMenuA(hTableOps, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hTableOps, MF_STRING, IDM_TABLE_SORT_ASC, "&Sort Rows by This Column (Ascending)");
    AppendMenuA(hTableOps, MF_STRING, IDM_TABLE_SORT_DESC, "Sort Rows by This Column (Descending)");
    AppendMenuA(hTableOps, MF_SEPARATOR, 0, NULL);
    HMENU hTableAlign = CreatePopupMenu();
    AppendMenuA(hTableAlign, MF_STRING, IDM_TABLE_ALIGN_LEFT, "&Left");
    AppendMenuA(hTableAlign, MF_STRING, IDM_TABLE_ALIGN_CENTER, "&Center");
    AppendMenuA(hTableAlign, MF_STRING, IDM_TABLE_ALIGN_RIGHT, "&Right");
    AppendMenuA(hTableOps, MF_POPUP, (UINT_PTR)hTableAlign, "Align Colu&mn");
    AppendMenuA(hMarkdown, MF_POPUP, (UINT_PTR)hTableOps, "Table Row/Colu&mn");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_CODEBLOCK, "Code Bl&ock");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_FOOTNOTE, "&Footnote");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_HRULE, "&Horizontal Rule");
    AppendMenuA(hMarkdown, MF_STRING, IDM_FMT_ALERT, "&Alert");

    HMENU hJson = CreatePopupMenu();
    AppendMenuA(hJson, MF_STRING, IDM_FMT_JSON_PRETTY, "&Format JSON");
    AppendMenuA(hJson, MF_STRING, IDM_FMT_JSON_MINIFY, "&Minify JSON");

    HMENU hHtml = CreatePopupMenu();
    AppendMenuA(hHtml, MF_STRING, IDM_FMT_HTML_HTML, "&HTML");
    AppendMenuA(hHtml, MF_STRING, IDM_FMT_HTML_HEAD, "H&ead");
    AppendMenuA(hHtml, MF_STRING, IDM_FMT_HTML_BODY, "&Body");
    AppendMenuA(hHtml, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hHtml, MF_STRING, IDM_FMT_HTML_PARAGRAPH, "&Paragraph");
    AppendMenuA(hHtml, MF_STRING, IDM_FMT_HTML_BOLD, "B&old");
    AppendMenuA(hHtml, MF_STRING, IDM_FMT_HTML_ITALIC, "&Italic");
    AppendMenuA(hHtml, MF_STRING, IDM_FMT_HTML_UNDERLINE, "&Underline");
    AppendMenuA(hHtml, MF_STRING, IDM_FMT_HTML_DIV, "&Div");
    AppendMenuA(hHtml, MF_STRING, IDM_FMT_HTML_SPAN, "&Span");
    AppendMenuA(hHtml, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hHtml, MF_STRING, IDM_FMT_HTML_LINK, "&Link");
    AppendMenuA(hHtml, MF_STRING, IDM_FMT_HTML_IMAGE, "I&mage");
    AppendMenuA(hHtml, MF_STRING, IDM_FMT_HTML_TABLE, "&Table");
    AppendMenuA(hHtml, MF_STRING, IDM_FMT_HTML_BULLETLIST, "B&ullet List");
    AppendMenuA(hHtml, MF_STRING, IDM_FMT_HTML_NUMBERLIST, "&Numbered List");
    AppendMenuA(hHtml, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hHtml, MF_STRING, IDM_FMT_HTML_BREAK, "Line &Break");

    HMENU hTools = CreatePopupMenu();
    AppendMenuA(hTools, MF_STRING, IDM_EDIT_COMMAND_PALETTE, "Command &Palette...\tCtrl+Shift+P");
    AppendMenuA(hTools, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hTools, MF_POPUP, (UINT_PTR)hMarkdown, "&Markdown");
    AppendMenuA(hTools, MF_POPUP, (UINT_PTR)hJson, "&JSON");
    AppendMenuA(hTools, MF_POPUP, (UINT_PTR)hHtml, "&HTML");
    AppendMenuA(hTools, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hTools, MF_STRING, IDM_SETTINGS, "Settin&gs...");
    AppendMenuA(hMenuBar, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)hTools, "&Tools");

    HMENU hRun = CreatePopupMenu();
    AppendMenuA(hRun, MF_STRING, IDM_RUN_FILE, "&Run File\tCtrl+F5");
    AppendMenuA(hRun, MF_STRING, IDM_DEBUG_FILE, "&Debug File\tF5");
    AppendMenuA(hRun, MF_STRING, IDM_DEBUG_CONTINUE, "&Continue\tF6");
    AppendMenuA(hRun, MF_STRING, IDM_DEBUG_STEP_OVER, "Step &Over\tF10");
    AppendMenuA(hRun, MF_STRING, IDM_DEBUG_STEP_INTO, "Step &Into\tF11");
    AppendMenuA(hRun, MF_STRING, IDM_DEBUG_STEP_OUT, "Step O&ut\tShift+F11");
    AppendMenuA(hRun, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hRun, MF_STRING, IDM_DEBUG_BREAKPOINT, "Toggle &Breakpoint\tF9");
    AppendMenuA(hRun, MF_STRING, IDM_DEBUG_CLEAR_BREAKPOINTS, "&Clear All Breakpoints");
    AppendMenuA(hMenuBar, MF_POPUP | MF_OWNERDRAW, (UINT_PTR)hRun, "&Run");

    HMENU hHelp = CreatePopupMenu();
    AppendMenuA(hHelp, MF_STRING, IDM_HELP_REFERENCE, "&Markdown Reference");
    AppendMenuA(hHelp, MF_STRING, IDM_HELP_SHORTCUTS, "&Keyboard Shortcuts");
    AppendMenuA(hHelp, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hHelp, MF_STRING, IDM_HELP_ABOUT, "&About MarkEdit");
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
   The icon painters themselves live in toolbar_icons.c/.h; a NULL paint
   function here marks a separator (a small gap, no button). ---- */
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
    { IDM_VIEW_TOGGLESPLIT, toolbar_icon_splitview, "Toggle split editor" },
    { 0, NULL, NULL },
    { IDM_FILE_OPEN_TERMINAL, toolbar_icon_terminal, "Open terminal here" },
};
#define TOOLBAR_BUTTON_COUNT (int)(sizeof(TOOLBAR_BUTTONS) / sizeof(TOOLBAR_BUTTONS[0]))
#define TOOLBAR_BUTTON_SIZE (TOOLBAR_HEIGHT - 6)
static HWND g_hToolbarButtons[TOOLBAR_BUTTON_COUNT];
static HWND g_hToolbarTooltip;

/* Recolors the toolbar buttons' shared tooltip to match the current app
   theme (same GitHub light/dark palette as theme_bg_color()/theme_fg_color()).
   Visual styles are switched off for this control in create_toolbar() below,
   since themed tooltips ignore TTM_SETTIPBKCOLOR/TTM_SETTIPTEXTCOLOR and
   would otherwise stay the OS's light tooltip colors even in dark mode. */
static void apply_toolbar_tooltip_theme(void) {
    if (!g_hToolbarTooltip) return;
    SendMessageW(g_hToolbarTooltip, TTM_SETTIPBKCOLOR, (WPARAM)theme_bg_color(), 0);
    SendMessageW(g_hToolbarTooltip, TTM_SETTIPTEXTCOLOR, (WPARAM)theme_fg_color(), 0);
}

static void create_toolbar(HWND hwnd) {
    g_hToolbarTooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hwnd, NULL, GetModuleHandleA(NULL), NULL);
    if (g_hToolbarTooltip) {
        SetWindowPos(g_hToolbarTooltip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowTheme(g_hToolbarTooltip, L"", L"");
        apply_toolbar_tooltip_theme();
    }

    for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++) {
        if (!TOOLBAR_BUTTONS[i].paint) continue;
        WCHAR *tip = to_wide(TOOLBAR_BUTTONS[i].tip);
        g_hToolbarButtons[i] = CreateWindowExW(0, L"BUTTON", tip ? tip : L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)TOOLBAR_BUTTONS[i].id, GetModuleHandleA(NULL), NULL);
        /* WS_CLIPSIBLINGS: these buttons sit edge-to-edge and get MoveWindow'd
           on every layout() (including mid-drag during a live resize, where
           layout_toolbar_at()'s row-wrap can shift several at once) - without it,
           one button's owner-draw paint can bleed into a neighbour's area.
           The window text isn't displayed (owner-drawn), but is kept as the
           accessible name / tooltip-eligible text for the button. */
        if (g_hToolbarTooltip && tip) {
            TOOLINFOW ti = { 0 };
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = hwnd;
            ti.uId = (UINT_PTR)g_hToolbarButtons[i];
            ti.hinst = GetModuleHandleA(NULL);
            ti.lpszText = tip;
            SendMessageW(g_hToolbarTooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
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

    COLORREF fg = theme_fg_color();
    COLORREF pressedBg = g_editorDarkMode ? THEME_DARK_BORDER : RGB(0xE1, 0xE7, 0xEC);

    int pressed = (dis->itemState & ODS_SELECTED) != 0;
    fill_rect_color(dis->hDC, &dis->rcItem, pressed ? pressedBg : theme_bg_color());

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

/* The debugger stays in the main window instead of opening another tool
   window. Its buttons remain native controls for keyboard/accessibility,
   with owner-drawing used only to match the app's exact border palette. */
static void apply_debug_panel_theme(void) {
    if (!g_hDebugPanel) return;
    HWND controls[] = { g_hDebugPanel, g_hDebugContinue, g_hDebugStepOver,
                        g_hDebugStepInto, g_hDebugStepOut, g_hDebugClose,
                        g_hDebugVariables };
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++)
        if (controls[i]) {
            apply_explorer_theme(controls[i]);
            InvalidateRect(controls[i], NULL, TRUE);
        }
    if (g_hDebugVariables) {
        ListView_SetBkColor(g_hDebugVariables, theme_bg_color());
        ListView_SetTextBkColor(g_hDebugVariables, theme_bg_color());
        ListView_SetTextColor(g_hDebugVariables, theme_fg_color());
        HWND header = ListView_GetHeader(g_hDebugVariables);
        if (header) {
            apply_explorer_theme(header);
            InvalidateRect(header, NULL, TRUE);
        }
    }
    InvalidateRect(g_hDebugPanel, NULL, TRUE);
}

static void paint_debug_header(HWND header, HDC hdc) {
    RECT client;
    GetClientRect(header, &client);
    fill_rect_color(hdc, &client, theme_bg_color());

    HFONT font = (HFONT)SendMessageW(header, WM_GETFONT, 0, 0);
    if (!font) font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    int count = (int)SendMessageW(header, HDM_GETITEMCOUNT, 0, 0);
    for (int column = 0; column < count; column++) {
        RECT rc = {0};
        if (!SendMessageW(header, HDM_GETITEMRECT, (WPARAM)column, (LPARAM)&rc)) continue;
        WCHAR label[64] = L"";
        HDITEMW item = {0};
        item.mask = HDI_TEXT;
        item.pszText = label;
        item.cchTextMax = sizeof(label) / sizeof(label[0]);
        SendMessageW(header, HDM_GETITEMW, (WPARAM)column, (LPARAM)&item);

        RECT divider = { rc.right - 1, rc.top, rc.right, rc.bottom };
        fill_rect_color(hdc, &divider, theme_border_color());
        rc.left += 7;
        rc.right -= 5;
        SetTextColor(hdc, theme_fg_color());
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, label, -1, &rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    RECT bottom = { client.left, client.bottom - 1, client.right, client.bottom };
    fill_rect_color(hdc, &bottom, theme_border_color());
    SelectObject(hdc, oldFont);
}

static LRESULT CALLBACK DebugHeaderSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                                 LPARAM lParam, UINT_PTR id,
                                                 DWORD_PTR data) {
    (void)lParam; (void)id; (void)data;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        paint_debug_header(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_PRINTCLIENT) {
        paint_debug_header(hwnd, (HDC)wParam);
        return 0;
    }
    if (msg == WM_THEMECHANGED) {
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, DebugHeaderSubclassProc, 1);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void draw_debug_panel_button(const DRAWITEMSTRUCT *dis) {
    int pressed = (dis->itemState & ODS_SELECTED) != 0;
    int disabled = (dis->itemState & ODS_DISABLED) != 0;
    COLORREF fill = pressed
        ? (g_editorDarkMode ? THEME_DARK_BORDER : RGB(0xE1, 0xE7, 0xEC))
        : theme_bg_color();
    COLORREF text = disabled
        ? (g_editorDarkMode ? RGB(0x7D, 0x85, 0x90) : RGB(0x8C, 0x95, 0x9F))
        : theme_fg_color();
    fill_rect_color(dis->hDC, &dis->rcItem, fill);

    HPEN pen = CreatePen(PS_SOLID, 1, theme_border_color());
    HPEN oldPen = (HPEN)SelectObject(dis->hDC, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
    Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top,
              dis->rcItem.right, dis->rcItem.bottom);
    SelectObject(dis->hDC, oldBrush);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(pen);

    WCHAR label[64];
    GetWindowTextW(dis->hwndItem, label, sizeof(label) / sizeof(label[0]));
    RECT textRect = dis->rcItem;
    SetTextColor(dis->hDC, text);
    SetBkMode(dis->hDC, TRANSPARENT);
    DrawTextW(dis->hDC, label, -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (dis->itemState & ODS_FOCUS) {
        InflateRect(&textRect, -3, -3);
        DrawFocusRect(dis->hDC, &textRect);
    }
}

static LRESULT CALLBACK DebugPanelSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                                LPARAM lParam, UINT_PTR id,
                                                DWORD_PTR data) {
    (void)id; (void)data;
    if (msg == WM_SIZE) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int dpi = GetDpiForWindow(hwnd);
        int pad = MulDiv(8, dpi, 96);
        int row = MulDiv(26, dpi, 96);
        int buttonWidth = MulDiv(112, dpi, 96);
        int closeWidth = MulDiv(30, dpi, 96);
        int availableForButtons = rc.right - closeWidth - pad * 7;
        if (availableForButtons > 0 && buttonWidth * 4 > availableForButtons)
            buttonWidth = availableForButtons / 4;
        /* A panel too narrow for legible buttons hides them rather than
           honouring the minimum width, which would push the leftmost button's
           coordinates off the panel entirely. */
        int showButtons = buttonWidth >= MulDiv(48, dpi, 96);
        HWND buttons[] = { g_hDebugContinue, g_hDebugStepOver,
                           g_hDebugStepInto, g_hDebugStepOut };
        int x = rc.right - pad - closeWidth;
        MoveWindow(g_hDebugClose, x, pad / 2, closeWidth, row, TRUE);
        int statusWidth = 0;
        if (showButtons) {
            for (int i = 3; i >= 0; i--) {
                x -= pad + buttonWidth;
                MoveWindow(buttons[i], x, pad / 2, buttonWidth, row, TRUE);
                ShowWindow(buttons[i], SW_SHOW);
            }
            statusWidth = x - pad * 2;
        } else {
            for (int i = 0; i < 4; i++) ShowWindow(buttons[i], SW_HIDE);
        }
        if (showButtons && statusWidth > MulDiv(40, dpi, 96)) {
            MoveWindow(g_hDebugStatus, pad, pad / 2, statusWidth, row, TRUE);
            ShowWindow(g_hDebugStatus, SW_SHOW);
        } else {
            ShowWindow(g_hDebugStatus, SW_HIDE);
        }
        int top = row + pad;
        MoveWindow(g_hDebugVariables, pad + 1, top + 1, rc.right - pad * 2 - 2,
                   rc.bottom - top - pad - 2, TRUE);
        return 0;
    }
    if (msg == WM_COMMAND) {
        WORD command = LOWORD(wParam);
        if (command == ID_DEBUG_CLOSE) {
            g_debugPanelVisible = 0;
            ShowWindow(g_hDebugPanel, SW_HIDE);
            if (g_hMainWnd) {
                SendMessageW(g_hMainWnd, WM_SIZE, 0, 0);
                SetFocus(g_hEdit);
            }
            return 0;
        }
        if (command == IDM_DEBUG_CONTINUE || command == IDM_DEBUG_STEP_OVER ||
            command == IDM_DEBUG_STEP_INTO || command == IDM_DEBUG_STEP_OUT) {
            SendMessageW(g_hMainWnd, WM_COMMAND, command, 0);
            return 0;
        }
    }
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        fill_rect_color((HDC)wParam, &rc, theme_bg_color());
        RECT border = { rc.left, rc.top, rc.right, rc.top + 1 };
        fill_rect_color((HDC)wParam, &border, theme_border_color());
        int dpi = GetDpiForWindow(hwnd);
        int pad = MulDiv(8, dpi, 96);
        int top = MulDiv(26, dpi, 96) + pad;
        RECT edge = { pad, top, rc.right - pad, top + 1 };
        fill_rect_color((HDC)wParam, &edge, theme_border_color());
        edge = (RECT){ pad, rc.bottom - pad - 1, rc.right - pad, rc.bottom - pad };
        fill_rect_color((HDC)wParam, &edge, theme_border_color());
        edge = (RECT){ pad, top, pad + 1, rc.bottom - pad };
        fill_rect_color((HDC)wParam, &edge, theme_border_color());
        edge = (RECT){ rc.right - pad - 1, top, rc.right - pad, rc.bottom - pad };
        fill_rect_color((HDC)wParam, &edge, theme_border_color());
        return 1;
    }
    if (msg == WM_DRAWITEM) {
        const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
        if (dis->CtlType == ODT_BUTTON) {
            draw_debug_panel_button(dis);
            return TRUE;
        }
    }
    if (msg == WM_CTLCOLORSTATIC) {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, theme_fg_color());
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)(g_mainBkBrush ? g_mainBkBrush : GetStockObject(WHITE_BRUSH));
    }
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, DebugPanelSubclassProc, 1);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

/* Lay the toolbar buttons out left-to-right along the reserved top strip,
   with a wider gap standing in for a separator between groups. All buttons
   are the same size, sized to the toolbar's height. xOffset shifts the whole
   strip right of the file explorer panel, when one is docked. */
static int layout_toolbar_at(int xOffset, int width, int top) {
    int x = xOffset + 4, y = top + 3;
    for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++) {
        if (!TOOLBAR_BUTTONS[i].paint) {
            if (x > xOffset + 4) x += 10;
            continue;
        }
        HWND hBtn = g_hToolbarButtons[i];
        if (!hBtn) continue;
        if (x > xOffset + 4 && x + TOOLBAR_BUTTON_SIZE > xOffset + width - 4) {
            x = xOffset + 4;
            y += TOOLBAR_BUTTON_SIZE + 4;
        }
        MoveWindow(hBtn, x, y, TOOLBAR_BUTTON_SIZE, TOOLBAR_BUTTON_SIZE, TRUE);
        x += TOOLBAR_BUTTON_SIZE + 4;
    }
    return y + TOOLBAR_BUTTON_SIZE + 3;
}

/* Positions the editor within [x, x+width) x [y, y+h), carving a strip off
   its right edge for the minimap when one should be shown - regardless of
   which side of the splitter the editor itself is on, so the minimap always
   sits immediately next to the text it overviews. */
static void layout_editor_with_minimap(HWND hwnd, int x, int y, int width, int h) {
    g_splitEditorRegionY = y;
    int minimapWidth = (g_hMinimap && g_minimapVisible && !g_focusMode) ?
        MulDiv(MINIMAP_WIDTH, GetDpiForWindow(hwnd), 96) : 0;
    if (minimapWidth > width - 80) minimapWidth = 0; /* not enough room to keep the editor usable */

    /* With the preview pane also open, the minimap otherwise butts straight
       up against the editor with nothing to mark the boundary between them -
       leave a thin gap for the theme background to show through as a
       splitter line, matching the wider one between the editor and preview. */
    int minimapGap = (minimapWidth > 0 && g_previewVisible) ?
        MulDiv(MINIMAP_SPLITTER_WIDTH, GetDpiForWindow(hwnd), 96) : 0;
    if (minimapGap < 1 && minimapWidth > 0 && g_previewVisible) minimapGap = 1;

    int editorWidth = width - minimapWidth - minimapGap;

    /* The split pane stacks below the main editor, inside the same column
       the minimap sits beside (the minimap always spans the full height on
       the right - it's an overview of the whole document, not tied to
       either sub-pane). */
    if (g_hSplitEdit && g_splitEditorVisible && !g_focusMode) {
        int splitterHeight = MulDiv(SPLIT_EDITOR_SPLITTER_HEIGHT, GetDpiForWindow(hwnd), 96);
        if (splitterHeight < 1) splitterHeight = 1;
        if (g_splitEditorPos < 0)
            g_splitEditorPos = MulDiv(h - splitterHeight, g_splitEditorRatio, 1000);
        if (g_splitEditorPos < 60) g_splitEditorPos = 60;
        if (g_splitEditorPos > h - 60 - splitterHeight)
            g_splitEditorPos = (h - 60 - splitterHeight > 60) ? h - 60 - splitterHeight : 60;
        int mainHeight = g_splitEditorPos;
        int splitHeight = h - mainHeight - splitterHeight;

        /* Set before the MoveWindow calls below, not after: MoveWindow can
           trigger a synchronous repaint of this window before returning, and
           WM_ERASEBKGND (which draws this rect - see WM_ERASEBKGND in
           WndProc) would otherwise run against the stale pre-layout value. */
        g_splitEditorSplitterRect.left = x;
        g_splitEditorSplitterRect.right = x + editorWidth;
        g_splitEditorSplitterRect.top = y + mainHeight;
        g_splitEditorSplitterRect.bottom = y + mainHeight + splitterHeight;

        MoveWindow(g_hEdit, x, y, editorWidth, mainHeight, TRUE);
        ShowWindow(g_hEdit, SW_SHOW);
        MoveWindow(g_hSplitEdit, x, y + mainHeight + splitterHeight, editorWidth, splitHeight, TRUE);
        ShowWindow(g_hSplitEdit, SW_SHOW);

        /* Keep the persisted ratio in sync with the live pixel position, the
           same relationship save_layout_settings() maintains for the main
           editor/preview splitter's own ratio. */
        if (h - splitterHeight > 0) {
            g_splitEditorRatio = MulDiv(g_splitEditorPos, 1000, h - splitterHeight);
            if (g_splitEditorRatio < 50) g_splitEditorRatio = 50;
            if (g_splitEditorRatio > 950) g_splitEditorRatio = 950;
        }
    } else {
        g_splitEditorSplitterRect = (RECT){0, 0, 0, 0};
        MoveWindow(g_hEdit, x, y, editorWidth, h, TRUE);
        ShowWindow(g_hEdit, SW_SHOW);
        if (g_hSplitEdit) ShowWindow(g_hSplitEdit, SW_HIDE);
    }

    if (!g_hMinimap) return;
    if (minimapWidth > 0) {
        MoveWindow(g_hMinimap, x + editorWidth + minimapGap, y, minimapWidth, h, TRUE);
        ShowWindow(g_hMinimap, SW_SHOW);
        update_minimap_viewport();
    } else {
        ShowWindow(g_hMinimap, SW_HIDE);
    }
}

static void layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    g_splitterRect = (RECT){0, 0, 0, 0}; /* only the "both visible" branch below sets this */

    int statusHeight = 22;
    if (g_hStatus) {
        MoveWindow(g_hStatus, 0, h - statusHeight, w, statusHeight, TRUE);
        int parts[4] = { MulDiv(150, w > 0 ? GetDpiForWindow(hwnd) : 96, 96),
                         MulDiv(345, w > 0 ? GetDpiForWindow(hwnd) : 96, 96),
                         MulDiv(510, w > 0 ? GetDpiForWindow(hwnd) : 96, 96), -1 };
        SendMessageW(g_hStatus, SB_SETPARTS, 4, (LPARAM)parts);
        /* StatusSubclassProc paints from these part rectangles via
           SB_GETRECT, so a resize needs a repaint to match the new sizes.
           Resizing can't change the text, so the full-document word count
           only runs the first time (before any part text exists), not on
           every WM_SIZE or splitter-drag mouse move. */
        if (g_statusPartText[1][0]) InvalidateRect(g_hStatus, NULL, FALSE);
        else update_status_bar();
        h -= statusHeight;
    }

    /* The formatting toolbar and tab strip span the full window width, above
       both the file explorer panel and the editor/preview - the explorer
       panel is docked below them, not beside them. The toolbar sits directly
       under the menu bar, with the tab strip below it. The formatting toolbar
       edits Markdown source, so it's only useful (and only shown) while the
       editor pane is visible. */
    int y = 0;
    for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++) {
        if (g_hToolbarButtons[i])
            ShowWindow(g_hToolbarButtons[i], g_editorVisible && !g_focusMode ? SW_SHOW : SW_HIDE);
    }
    if (g_editorVisible && !g_focusMode) y = layout_toolbar_at(0, w, y);
    ShowWindow(g_hTabs, g_focusMode ? SW_HIDE : SW_SHOW);
    if (!g_focusMode) {
        MoveWindow(g_hTabs, 0, y, w, TAB_HEIGHT, TRUE);
        y += TAB_HEIGHT;
    }
    h -= y;
    if (h < 0) h = 0;

    /* File explorer panel: docked at the left edge of the remaining content
       area (below the tab strip/toolbar, above the status bar), opposite
       side from the outline pane. Everything to its right shifts its
       x-origin by contentX and lays out into the now-narrower w, the same
       way the outline pane's own paneWidth carve-out works at the other
       end. */
    int contentX = 0;
    if (g_hExplorerTree && g_explorerVisible && !g_focusMode) {
        int dpi = GetDpiForWindow(hwnd);
        if (g_explorerWidth < 0)
            g_explorerWidth = MulDiv(w, g_explorerWidthRatio, 1000);
        int minWidth = MulDiv(120, dpi, 96);
        int maxWidth = w * 2 / 3;
        if (g_explorerWidth < minWidth) g_explorerWidth = minWidth;
        if (maxWidth > minWidth && g_explorerWidth > maxWidth) g_explorerWidth = maxWidth;
        g_explorerSplitterRect = (RECT){ g_explorerWidth, y, g_explorerWidth + SPLITTER_WIDTH, y + h };
        MoveWindow(g_hExplorerTree, 0, y, g_explorerWidth, h, TRUE);
        ShowWindow(g_hExplorerTree, SW_SHOW);
        contentX = g_explorerWidth + SPLITTER_WIDTH;
    } else {
        g_explorerSplitterRect = (RECT){0, 0, 0, 0};
        if (g_hExplorerTree) ShowWindow(g_hExplorerTree, SW_HIDE);
    }
    w -= contentX;
    if (w < 0) w = 0;

    /* A debug session owns a docked strip beneath the editor/preview area.
       The explorer remains full-height, while code, preview, and outline all
       give up the same amount of vertical space so their bottoms stay level. */
    if (g_hDebugPanel && g_debugPanelVisible && !g_focusMode && h >= 160) {
        int debugHeight = MulDiv(190, GetDpiForWindow(hwnd), 96);
        if (debugHeight > h / 2) debugHeight = h / 2;
        MoveWindow(g_hDebugPanel, contentX, y + h - debugHeight, w, debugHeight, TRUE);
        ShowWindow(g_hDebugPanel, SW_SHOW);
        h -= debugHeight;
    } else if (g_hDebugPanel) {
        ShowWindow(g_hDebugPanel, SW_HIDE);
    }

    int paneWidth = w;
    if (g_hOutline && g_outlineVisible && g_editorVisible && !g_focusMode) {
        int outlineWidth = MulDiv(230, GetDpiForWindow(hwnd), 96);
        if (outlineWidth > w / 3) outlineWidth = w / 3;
        paneWidth = w - outlineWidth;
        MoveWindow(g_hOutline, contentX + paneWidth, y, outlineWidth, h, TRUE);
        ShowWindow(g_hOutline, SW_SHOW);
    } else if (g_hOutline) {
        ShowWindow(g_hOutline, SW_HIDE);
    }

    if (g_editorVisible && !g_previewVisible) {
        layout_editor_with_minimap(hwnd, contentX, y, paneWidth, h);
        ShowWindow(g_hPreview, SW_HIDE);
        RECT zero = { 0, 0, 0, 0 };
        set_webview_bounds(zero, FALSE);
        return;
    }

    if (!g_editorVisible && g_previewVisible) {
        ShowWindow(g_hEdit, SW_HIDE);
        if (g_hMinimap) ShowWindow(g_hMinimap, SW_HIDE);
        if (g_hSplitEdit) ShowWindow(g_hSplitEdit, SW_HIDE);
        ShowWindow(g_hPreview, SW_SHOW);
        MoveWindow(g_hPreview, contentX, y, paneWidth, h, TRUE);
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

    ShowWindow(g_hPreview, SW_SHOW);
    g_splitterRect = (RECT){ contentX + g_splitterPos, y, contentX + g_splitterPos + SPLITTER_WIDTH, y + h };
    int rightWidth = paneWidth - g_splitterPos - SPLITTER_WIDTH;
    RECT wvRc;
    if (g_previewOnLeft) {
        MoveWindow(g_hPreview, contentX, y, g_splitterPos, h, TRUE);
        layout_editor_with_minimap(hwnd, contentX + g_splitterPos + SPLITTER_WIDTH, y, rightWidth, h);
        wvRc = (RECT){ 0, 0, g_splitterPos, h };
    } else {
        layout_editor_with_minimap(hwnd, contentX, y, g_splitterPos, h);
        MoveWindow(g_hPreview, contentX + g_splitterPos + SPLITTER_WIDTH, y, rightWidth, h, TRUE);
        wvRc = (RECT){ 0, 0, rightWidth, h };
    }
    set_webview_bounds(wvRc, TRUE);
}

static void update_view_menu(HWND hwnd) {
    HMENU hMenuBar = GetMenu(hwnd);
    if (!hMenuBar) return;
    HMENU hView = GetSubMenu(hMenuBar, 2); /* File, Edit, View, Tools, Run, Help */
    CheckMenuItem(hView, IDM_VIEW_TOGGLEPREVIEW, MF_BYCOMMAND | (g_previewVisible ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_TOGGLEEDITOR, MF_BYCOMMAND | (g_editorVisible ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_PREVIEW_ON_LEFT, MF_BYCOMMAND | (g_previewOnLeft ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_WORD_WRAP, MF_BYCOMMAND | (g_wordWrap ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_SPELL_CHECK, MF_BYCOMMAND |
                  (g_spellCheckEnabled ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_SHOW_WHITESPACE, MF_BYCOMMAND | (g_showWhitespace ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_INDENT_GUIDES, MF_BYCOMMAND | (g_indentGuides ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_USE_SPACES_INDENT, MF_BYCOMMAND | (g_indentUseSpaces ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_OUTLINE, MF_BYCOMMAND | (g_outlineVisible ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_MINIMAP, MF_BYCOMMAND | (g_minimapVisible ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_EXPLORER, MF_BYCOMMAND | (g_explorerVisible ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(hView, IDM_VIEW_TOGGLESPLIT, MF_BYCOMMAND | (g_splitEditorVisible ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(hView, IDM_VIEW_RESET_SPLIT_EDITOR, MF_BYCOMMAND | (g_splitEditorVisible ? MF_ENABLED : MF_GRAYED));
    /* Found by the command it holds rather than by position, which drifts
       whenever an item is added above it. */
    HMENU hTheme = NULL;
    for (int i = GetMenuItemCount(hView) - 1; i >= 0 && !hTheme; i--) {
        HMENU sub = GetSubMenu(hView, i);
        if (sub && GetMenuState(sub, IDM_VIEW_THEME_GITHUB_LIGHT, MF_BYCOMMAND) != (UINT)-1) hTheme = sub;
    }
    if (hTheme) {
        CheckMenuRadioItem(hTheme, IDM_VIEW_THEME_GITHUB_LIGHT, IDM_VIEW_THEME_GITHUB_DARK,
                           g_editorDarkMode ? IDM_VIEW_THEME_GITHUB_DARK : IDM_VIEW_THEME_GITHUB_LIGHT,
                           MF_BYCOMMAND);
    }
}

static void toggle_preview(HWND hwnd) {
    if (g_previewVisible && !g_editorVisible) return; /* would leave nothing visible */
    g_previewVisible = !g_previewVisible;
    update_view_menu(hwnd);
    layout(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
    if (g_previewVisible) update_preview();
    else KillTimer(hwnd, IDT_PREVIEW_UPDATE);
    save_layout_settings();
}

static void toggle_editor(HWND hwnd) {
    if (g_editorVisible && !g_previewVisible) return; /* would leave nothing visible */
    g_editorVisible = !g_editorVisible;
    update_view_menu(hwnd);
    layout(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
    save_layout_settings();
}

static void toggle_panel_sides(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    /* g_splitterPos is relative to the content area right of the explorer
       panel, so the mirrored position has to be computed against that
       narrower width, not the full client width. */
    int contentWidth = rc.right - explorer_content_x(g_focusMode);
    if (g_splitterPos >= 0 && contentWidth > SPLITTER_WIDTH) {
        g_splitterPos = contentWidth - SPLITTER_WIDTH - g_splitterPos;
    }
    g_previewOnLeft = !g_previewOnLeft;
    save_layout_settings();
    update_view_menu(hwnd);
    layout(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
}

static void toggle_focus_mode(HWND hwnd) {
    g_focusMode = !g_focusMode;
    refresh_menu(hwnd);
    layout(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
    save_layout_settings();
    /* Focus mode hides the menu, so Ctrl+Shift+F11 is the only way back out.
       Keep focus on the editor where the accelerator is reachable - WebView2
       claims F11 for its own fullscreen whenever the preview has focus. */
    if (g_editorVisible) SetFocus(g_hEdit);
}

/* Cycles keyboard focus between the editor, preview, outline panel (when
   visible), and tab strip - Ctrl+F6 / Ctrl+Shift+F6, the same pane-cycling
   convention several other Windows apps use for keyboard-only navigation
   between major regions. WebView2 hosts its own separate HWND hierarchy, so
   giving it focus takes its own MoveFocus call rather than a plain
   SetFocus() on g_hPreview. */
static void cycle_pane_focus(int reverse) {
    HWND panes[5];
    int count = 0;
    if (g_editorVisible && g_hEdit) panes[count++] = g_hEdit;
    if (g_editorVisible && g_splitEditorVisible && g_hSplitEdit) panes[count++] = g_hSplitEdit;
    if (g_previewVisible && g_hPreview) panes[count++] = g_hPreview;
    if (g_outlineVisible && g_editorVisible && g_hOutline) panes[count++] = g_hOutline;
    if (g_hTabs) panes[count++] = g_hTabs;
    if (count == 0) return;

    HWND focused = GetFocus();
    int current = -1;
    for (int i = 0; i < count; i++) {
        if (panes[i] == focused || IsChild(panes[i], focused)) { current = i; break; }
    }
    HWND target = panes[current < 0 ? 0 : (current + (reverse ? count - 1 : 1)) % count];
    if (target == g_hPreview && g_webviewController) {
        ICoreWebView2Controller_MoveFocus(g_webviewController, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    } else {
        SetFocus(target);
    }
}

/* Edit-menu commands and their accelerators act on whichever editor pane has
   focus, so that Ctrl+C/Ctrl+Z/etc. do what the caret the user is actually
   looking at implies, rather than always reaching past a focused split pane
   to the main one. Menu clicks and accelerator keys don't change window
   focus in Win32, so GetFocus() here still reflects whichever pane the user
   was last typing in. */
static HWND active_edit_view(void) {
    return (g_hSplitEdit && GetFocus() == g_hSplitEdit) ? g_hSplitEdit : g_hEdit;
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

static void toggle_indent_use_spaces(HWND hwnd) {
    g_indentUseSpaces = !g_indentUseSpaces;
    apply_indentation(g_hEdit);
    /* Switching to spaces only affects how Tab now types going forward
       (apply_indentation() above) - without also converting what's already
       there, the document is left with a mix of tabs and spaces even though
       the setting says "spaces". g_hSplitEdit shares g_hEdit's document (see
       its declaration), so converting through g_hEdit is enough for both
       views. */
    if (g_indentUseSpaces) convert_indentation(g_hEdit, 1);
    update_view_menu(hwnd);
    save_layout_settings();
}

static void set_tab_width(int width) {
    if (width < 1) width = 1;
    if (width > 16) width = 16;
    if (width == g_tabWidth) return;
    g_tabWidth = width;
    apply_indentation(g_hEdit);
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
    if (g_hSplitEdit) SendMessageA(g_hSplitEdit, SCI_SETZOOM, (WPARAM)g_editorZoom, 0);
    /* Zoom changes the rendered glyph width SCI_TEXTWIDTH measures, same as a
       font face/size change does (see apply_editor_font()'s identical reset) -
       without this the gutter can stay sized for the pre-zoom font until the
       line count happens to cross a power-of-ten boundary. */
    g_lineNumberDigits = 0;
    update_linenumber_margin_width(g_hEdit);
    if (g_hSplitEdit) {
        g_splitLineNumberDigits = 0;
        update_linenumber_margin_width(g_hSplitEdit);
    }
    save_layout_settings();
}

static void reset_editor_zoom(void) {
    g_editorZoom = 0;
    SendMessageA(g_hEdit, SCI_SETZOOM, 0, 0);
    if (g_hSplitEdit) SendMessageA(g_hSplitEdit, SCI_SETZOOM, 0, 0);
    g_lineNumberDigits = 0;
    update_linenumber_margin_width(g_hEdit);
    if (g_hSplitEdit) {
        g_splitLineNumberDigits = 0;
        update_linenumber_margin_width(g_hSplitEdit);
    }
    save_layout_settings();
}

static void fold_toggle_at_cursor(void) {
    Sci_Position pos = (Sci_Position)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    Sci_Position line = (Sci_Position)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)pos, 0);
    SendMessageA(g_hEdit, SCI_TOGGLEFOLD, (WPARAM)line, 0);
}

static void fold_all_lines(int expand) {
    SendMessageA(g_hEdit, SCI_FOLDALL,
                 (WPARAM)(expand ? SC_FOLDACTION_EXPAND : SC_FOLDACTION_CONTRACT), 0);
}

/* Sublime/VSCode-style Ctrl+D: selects the word under the caret, or extends
   the multi-selection to the next occurrence of the current selection. */
static void select_next_occurrence(HWND hEdit) {
    SendMessageA(hEdit, SCI_MULTIPLESELECTADDNEXT, 0, 0);
}

/* Keeps the matching-brace highlight (STYLE_BRACELIGHT/STYLE_BRACEBAD, set up
   in apply_editor_theme) in sync with the caret. Called on every SCN_UPDATEUI. */
static void update_brace_highlight(HWND hEdit) {
    /* An unmatched brace makes SCI_BRACEMATCH scan to the document's end,
       on every caret move. */
    HWND other = hEdit == g_hSplitEdit ? g_hEdit : g_hSplitEdit;
    if (other) SendMessageA(other, SCI_BRACEHIGHLIGHT, (WPARAM)-1, (LPARAM)-1);
    Sci_Position bracePos = g_largeFile ? -1 : brace_position_near_caret(hEdit);
    if (bracePos < 0) {
        SendMessageA(hEdit, SCI_BRACEHIGHLIGHT, (WPARAM)-1, (LPARAM)-1);
        return;
    }
    Sci_Position match = (Sci_Position)SendMessageA(hEdit, SCI_BRACEMATCH, (WPARAM)bracePos, 0);
    if (match >= 0) SendMessageA(hEdit, SCI_BRACEHIGHLIGHT, (WPARAM)bracePos, (LPARAM)match);
    else SendMessageA(hEdit, SCI_BRACEBADLIGHT, (WPARAM)bracePos, 0);
}

#define WORD_COMPLETION_MIN_PREFIX 2
#define WORD_COMPLETION_MAX_WORDS 100

static int compare_words_ordinal(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static BOOL CALLBACK find_autocomplete_popup_proc(HWND hwnd, LPARAM lParam) {
    WCHAR className[16];
    if (GetClassNameW(hwnd, className, 16) == 0 || wcscmp(className, L"ListBoxX") != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    *(HWND *)lParam = hwnd;
    return FALSE;
}

/* SCI_AUTOCSHOW's popup is a native listbox (Win32 class "ListBoxX", with
   the actual WC_LISTBOX scrollbar-owning control as its one child) that
   Scintilla itself never dark-mode themes - its scrollbar is left to the
   system and paints a plain light scrollbar even in dark mode (unlike
   hEdit's, it can't be swapped for a themed_scrollbar.h one, since Scintilla
   creates and owns this window). Scintilla doesn't hand
   back the popup's HWND, so it's found by class name right after showing it
   (SCI_AUTOCSHOW creates/shows the popup synchronously), scoped to this
   process so a second Scintilla-based app's own popup is never touched.
   The popup frame itself (WS_POPUP | WS_THICKFRAME) also needs
   apply_popup_titlebar_theme()'s DWM dark-mode attribute, same as this
   app's own popups get - without it, Windows 11 paints that frame's rounded
   top strip in the light system color regardless of the listbox's own
   theme, showing as a solid white bar across the top of the list. */
static void theme_autocomplete_popup(void) {
    HWND popup = NULL;
    EnumWindows(find_autocomplete_popup_proc, (LPARAM)&popup);
    if (!popup) return;
    apply_popup_titlebar_theme(popup, g_editorDarkMode);
    HWND list = GetWindow(popup, GW_CHILD);
    if (list) apply_explorer_theme(list);
}

/* Plain word completion, not lexer/keyword-driven (see the AutoC setup in
   configure_scintilla()): as the caret's word grows to WORD_COMPLETION_MIN_PREFIX
   characters, show every other word elsewhere in the document that starts
   with it (case-insensitively, via SCFIND_WORDSTART - the same search
   primitive ctrl_click_jump_to_occurrence() uses). Called on every
   SCN_CHARADDED; does nothing once the list is already showing, since
   Scintilla narrows the visible entries itself as more of the word is
   typed. */
static void update_word_completions(HWND hEdit) {
    if (g_largeFile) return;
    Sci_Position pos = (Sci_Position)SendMessageA(hEdit, SCI_GETCURRENTPOS, 0, 0);
    Sci_Position wordStart = (Sci_Position)SendMessageA(hEdit, SCI_WORDSTARTPOSITION, (WPARAM)pos, (LPARAM)1);
    Sci_Position prefixLen = pos - wordStart;
    if (prefixLen < WORD_COMPLETION_MIN_PREFIX) return;
    if (SendMessageA(hEdit, SCI_AUTOCACTIVE, 0, 0)) return;

    char *prefix = (char *)malloc((size_t)prefixLen + 1);
    if (!prefix) return;
    struct Sci_TextRangeFull prefixRange = { { wordStart, pos }, prefix };
    SendMessageA(hEdit, SCI_GETTEXTRANGEFULL, 0, (LPARAM)&prefixRange);
    prefix[prefixLen] = 0;

    Sci_Position docLength = (Sci_Position)SendMessageA(hEdit, SCI_GETLENGTH, 0, 0);
    SendMessageA(hEdit, SCI_SETSEARCHFLAGS, SCFIND_WORDSTART, 0);

    char *words[WORD_COMPLETION_MAX_WORDS];
    int wordCount = 0;
    Sci_Position searchFrom = 0;
    while (wordCount < WORD_COMPLETION_MAX_WORDS) {
        SendMessageA(hEdit, SCI_SETTARGETSTART, (WPARAM)searchFrom, 0);
        SendMessageA(hEdit, SCI_SETTARGETEND, (WPARAM)docLength, 0);
        Sci_Position matchStart = (Sci_Position)SendMessageA(hEdit, SCI_SEARCHINTARGET, (WPARAM)prefixLen, (LPARAM)prefix);
        if (matchStart < 0) break;
        Sci_Position matchTargetEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETTARGETEND, 0, 0);
        searchFrom = matchTargetEnd;

        if (matchStart == wordStart) continue; /* the word being typed itself */
        Sci_Position wordEnd = (Sci_Position)SendMessageA(hEdit, SCI_WORDENDPOSITION, (WPARAM)matchStart, (LPARAM)1);
        Sci_Position candidateLen = wordEnd - matchStart;
        if (candidateLen <= prefixLen) continue;

        char *candidate = (char *)malloc((size_t)candidateLen + 1);
        if (!candidate) continue;
        struct Sci_TextRangeFull candidateRange = { { matchStart, wordEnd }, candidate };
        SendMessageA(hEdit, SCI_GETTEXTRANGEFULL, 0, (LPARAM)&candidateRange);
        candidate[candidateLen] = 0;

        int duplicate = 0;
        for (int w = 0; w < wordCount; w++) {
            if (strcmp(words[w], candidate) == 0) { duplicate = 1; break; }
        }
        if (duplicate) { free(candidate); continue; }
        words[wordCount++] = candidate;
    }
    free(prefix);

    if (wordCount == 0) { SendMessageA(hEdit, SCI_AUTOCCANCEL, 0, 0); return; }

    qsort(words, (size_t)wordCount, sizeof(char *), compare_words_ordinal);

    size_t total = 0;
    for (int w = 0; w < wordCount; w++) total += strlen(words[w]) + 1;
    char *list = (char *)malloc(total);
    if (list) {
        size_t o = 0;
        for (int w = 0; w < wordCount; w++) {
            size_t wordLen = strlen(words[w]);
            memcpy(list + o, words[w], wordLen);
            o += wordLen;
            if (w + 1 < wordCount) list[o++] = ' ';
        }
        list[o] = 0;
        SendMessageA(hEdit, SCI_AUTOCSHOW, (WPARAM)prefixLen, (LPARAM)list);
        theme_autocomplete_popup();
        free(list);
    }
    for (int w = 0; w < wordCount; w++) free(words[w]);
}

/* Rewrites every line ending to eolMode (SC_EOL_CRLF/CR/LF) and switches the
   document's own EOL mode to match, so newly typed newlines follow suit. */
static void convert_line_endings(HWND hEdit, int eolMode) {
    SendMessageA(hEdit, SCI_CONVERTEOLS, (WPARAM)eolMode, 0);
    SendMessageA(hEdit, SCI_SETEOLMODE, (WPARAM)eolMode, 0);
    update_status_position();
}

/* Sublime/VSCode-style Ctrl+Shift+L: every occurrence of the word under the
   caret (or of the current selection) becomes its own selection at once. */
static void select_all_occurrences(HWND hEdit) {
    SendMessageA(hEdit, SCI_MULTIPLESELECTADDEACH, 0, 0);
}

/* Bookmarks are Scintilla markers on MARKER_BOOKMARK, which live on the
   document buffer itself - each tab keeps its own set automatically as
   SCI_SETDOCPOINTER swaps between them. */
static void toggle_bookmark_at_line(Sci_Position line) {
    int mask = (int)SendMessageA(g_hEdit, SCI_MARKERGET, (WPARAM)line, 0);
    if (mask & (1 << MARKER_BOOKMARK))
        SendMessageA(g_hEdit, SCI_MARKERDELETE, (WPARAM)line, (LPARAM)MARKER_BOOKMARK);
    else
        SendMessageA(g_hEdit, SCI_MARKERADD, (WPARAM)line, (LPARAM)MARKER_BOOKMARK);
}

static void toggle_bookmark_at_cursor(HWND hEdit) {
    Sci_Position pos = (Sci_Position)SendMessageA(hEdit, SCI_GETCURRENTPOS, 0, 0);
    toggle_bookmark_at_line((Sci_Position)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)pos, 0));
}

static void goto_bookmark(HWND hEdit, int forward) {
    Sci_Position pos = (Sci_Position)SendMessageA(hEdit, SCI_GETCURRENTPOS, 0, 0);
    Sci_Position line = (Sci_Position)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)pos, 0);
    Sci_Position found;
    if (forward) {
        found = (Sci_Position)SendMessageA(hEdit, SCI_MARKERNEXT, (WPARAM)(line + 1), (LPARAM)(1 << MARKER_BOOKMARK));
        if (found < 0) found = (Sci_Position)SendMessageA(hEdit, SCI_MARKERNEXT, 0, (LPARAM)(1 << MARKER_BOOKMARK));
    } else {
        found = (Sci_Position)SendMessageA(hEdit, SCI_MARKERPREVIOUS, (WPARAM)(line - 1), (LPARAM)(1 << MARKER_BOOKMARK));
        if (found < 0) {
            Sci_Position lastLine = (Sci_Position)SendMessageA(hEdit, SCI_GETLINECOUNT, 0, 0) - 1;
            found = (Sci_Position)SendMessageA(hEdit, SCI_MARKERPREVIOUS, (WPARAM)lastLine, (LPARAM)(1 << MARKER_BOOKMARK));
        }
    }
    if (found < 0) return;
    Sci_Position gotoPos = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)found, 0);
    SendMessageA(hEdit, SCI_SETSEL, (WPARAM)gotoPos, (LPARAM)gotoPos);
    SendMessageA(hEdit, SCI_SCROLLCARET, 0, 0);
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
    if (extension_in_list(extension, L".py;.pyw")) return DEBUG_SOURCE_PYTHON;
    if (extension_in_list(extension, L".c")) return DEBUG_SOURCE_C;
    if (extension_in_list(extension, L".cc;.cpp;.cxx")) return DEBUG_SOURCE_CPP;
    if (extension_in_list(extension, L".lua")) return DEBUG_SOURCE_LUA;
    return DEBUG_SOURCE_NONE;
}

/* Only Python/C/C++ have a debugger backend (pdb, GDB); Lua is Run-only, so
   it must not be treated as debuggable just because debug_source_kind()
   returns something other than DEBUG_SOURCE_NONE for it - that value also
   drives whether clicking the margin sets a breakpoint vs. a bookmark. */
static int is_debuggable_source(DebugSourceKind kind) {
    return kind == DEBUG_SOURCE_PYTHON || kind == DEBUG_SOURCE_C || kind == DEBUG_SOURCE_CPP;
}

static int collect_breakpoint_lines(int **outLines);

/* INI keys cannot safely contain arbitrary Windows paths, so each absolute,
   case-folded path gets a stable 64-bit FNV-1a key. The value is simply a
   comma-separated list of 1-based line numbers; deleting every breakpoint
   deletes the key as well. */
static int breakpoint_profile_key(const WCHAR *path, WCHAR *key, size_t capacity) {
    if (!path || !path[0] || !key || capacity < 18) return 0;
    WCHAR *normalized = absolute_path(path);
    if (!normalized) normalized = wcsdup_heap(path);
    if (!normalized) return 0;
    CharLowerBuffW(normalized, (DWORD)wcslen(normalized));
    ULONGLONG hash = 1469598103934665603ULL;
    for (const WCHAR *character = normalized; *character; character++) {
        hash ^= (unsigned char)(*character & 0xFF);
        hash *= 1099511628211ULL;
        hash ^= (unsigned char)((*character >> 8) & 0xFF);
        hash *= 1099511628211ULL;
    }
    free(normalized);
    return swprintf(key, capacity, L"F%016llX", (unsigned long long)hash) > 0;
}

static void save_breakpoints_for_active_document(const WCHAR *path) {
    if (!path || !is_debuggable_source(debug_source_kind(path))) return;
    WCHAR key[32];
    WCHAR *settings = get_settings_path();
    if (!settings || !breakpoint_profile_key(path, key, sizeof(key) / sizeof(key[0]))) {
        free(settings);
        return;
    }
    int *lines = NULL;
    int count = collect_breakpoint_lines(&lines);
    if (count <= 0) {
        WritePrivateProfileStringW(L"Breakpoints", key, NULL, settings);
    } else {
        size_t capacity = (size_t)count * 24 + 1;
        WCHAR *value = (WCHAR *)malloc(capacity * sizeof(WCHAR));
        if (value) {
            size_t used = 0;
            value[0] = 0;
            for (int i = 0; i < count; i++) {
                int written = swprintf(value + used, capacity - used,
                                       i ? L",%d" : L"%d", lines[i] + 1);
                if (written < 0) break;
                used += (size_t)written;
            }
            WritePrivateProfileStringW(L"Breakpoints", key, value, settings);
            free(value);
        }
    }
    free(lines);
    free(settings);
}

static void restore_persisted_breakpoints(const WCHAR *path) {
    SendMessageA(g_hEdit, SCI_MARKERDELETEALL, (WPARAM)MARKER_BREAKPOINT, 0);
    if (!path || !is_debuggable_source(debug_source_kind(path))) return;
    WCHAR key[32];
    WCHAR *settings = get_settings_path();
    if (!settings || !breakpoint_profile_key(path, key, sizeof(key) / sizeof(key[0]))) {
        free(settings);
        return;
    }
    WCHAR *value = read_profile_string(L"Breakpoints", key, settings);
    free(settings);
    if (!value) return;
    Sci_Position lineCount = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
    WCHAR *cursor = value;
    while (*cursor) {
        WCHAR *end = NULL;
        long line = wcstol(cursor, &end, 10);
        if (end == cursor) {
            cursor++;
            continue;
        }
        if (line > 0 && (Sci_Position)line <= lineCount)
            SendMessageA(g_hEdit, SCI_MARKERADD, (WPARAM)(line - 1), MARKER_BREAKPOINT);
        cursor = end;
        while (*cursor == L',' || *cursor == L' ' || *cursor == L'\t') cursor++;
    }
    free(value);
}

static void save_all_document_breakpoints(void) {
    if (!g_hEdit || g_documentCount <= 0) return;
    sptr_t active = (sptr_t)SendMessageA(g_hEdit, SCI_GETDOCPOINTER, 0, 0);
    int oldSwitching = g_switchingDocument;
    g_switchingDocument = 1;
    for (int i = 0; i < g_documentCount; i++) {
        if (!g_documents[i].sciDocument || !g_documents[i].path) continue;
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_documents[i].sciDocument);
        save_breakpoints_for_active_document(g_documents[i].path);
    }
    if (active) SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)active);
    g_switchingDocument = oldSwitching;
}

/* Debugger breakpoints are separate from general-purpose bookmarks, but share
   the same narrow marker margin. Scintilla keeps them attached to their lines
   as text is inserted or removed. */
static void toggle_breakpoint_at_line(Sci_Position line) {
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
    save_breakpoints_for_active_document(g_currentFile);
}

static void toggle_breakpoint_at_cursor(void) {
    Sci_Position pos = (Sci_Position)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    Sci_Position line = (Sci_Position)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)pos, 0);
    toggle_breakpoint_at_line(line);
}

static void clear_debug_breakpoints(void) {
    if (!g_hEdit || g_documentCount <= 0) return;
    sptr_t active = (sptr_t)SendMessageA(g_hEdit, SCI_GETDOCPOINTER, 0, 0);
    int oldSwitching = g_switchingDocument;
    g_switchingDocument = 1;
    for (int i = 0; i < g_documentCount; i++) {
        if (!g_documents[i].sciDocument) continue;
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_documents[i].sciDocument);
        SendMessageA(g_hEdit, SCI_MARKERDELETEALL, (WPARAM)MARKER_BREAKPOINT, 0);
    }
    if (active) SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)active);
    g_switchingDocument = oldSwitching;

    /* The command says "all", so remove entries for closed files too rather
       than allowing their persisted markers to reappear when reopened. */
    WCHAR *settings = get_settings_path();
    if (settings) {
        WritePrivateProfileStringW(L"Breakpoints", NULL, NULL, settings);
        free(settings);
    }
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
    Sci_Position line = -1;
    while ((line = (Sci_Position)SendMessageA(g_hEdit, SCI_MARKERNEXT, (WPARAM)(line + 1),
                                      (LPARAM)(1 << MARKER_BREAKPOINT))) >= 0) {
        if (count >= capacity) {
            int next = capacity ? capacity * 2 : 8;
            int *grown = (int *)realloc(lines, (size_t)next * sizeof(int));
            if (!grown) break;
            lines = grown;
            capacity = next;
        }
        /* Debugger "file:line" commands take an int; breakpoints are only
           offered on Python/C/C++ sources, nowhere near 2^31 lines. */
        lines[count++] = (int)line;
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

/* Looks for one candidate executable name inside a user-configured folder
   override (Settings > Compiler's GCC/Clang/Zig/Lua path boxes) rather than
   searching PATH - the counterpart to executable_on_search_path() used
   whenever an override is set. */
static WCHAR *executable_in_directory(const WCHAR *directory, const WCHAR *name) {
    if (!directory || !directory[0]) return NULL;
    WCHAR *candidate = path_join(directory, name);
    if (candidate && is_existing_file(candidate)) return candidate;
    free(candidate);
    return NULL;
}

/* A release keeps GDB beside MarkEdit under third_party/gdb/bin. Developer
   builds use the same layout at the repository root. An explicit setting is
   authoritative; otherwise prefer that reproducibly packaged copy and retain
   PATH as a fallback for older/custom distributions. */
static WCHAR *find_gdb_executable(void) {
    if (g_gdbBinOverride) return executable_in_directory(g_gdbBinOverride, L"gdb.exe");

    WCHAR *module = get_module_path();
    WCHAR *directory = module ? path_directory(module) : NULL;
    WCHAR *candidate = directory
        ? path_join(directory, L"third_party\\gdb\\bin\\gdb.exe")
        : NULL;
    free(module);
    free(directory);
    if (candidate && is_existing_file(candidate)) return candidate;
    free(candidate);
    return executable_on_search_path(L"gdb.exe");
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

typedef enum { FLAG_QUOTE_POWERSHELL, FLAG_QUOTE_CMD } FlagQuoteStyle;

/* Re-quotes one already-tokenized extra-compiler-flag for the target build
   command's shell: powershell_literal() above for the PowerShell console
   launch_native_file() builds, or cmd.exe's own quoting (double quotes,
   backslash-escaped) for the cmd /k chain launch_msvc_file() builds. */
static WCHAR *quote_token_for_shell(const WCHAR *token, FlagQuoteStyle style) {
    if (style == FLAG_QUOTE_POWERSHELL) return powershell_literal(token);
    size_t length = wcslen(token);
    int needsQuotes = length == 0;
    for (size_t i = 0; i < length && !needsQuotes; i++)
        if (token[i] == L' ' || token[i] == L'\t' || token[i] == L'"') needsQuotes = 1;
    if (!needsQuotes) return wcsdup_heap(token);
    WCHAR *quoted = (WCHAR *)malloc((length * 2 + 3) * sizeof(WCHAR));
    if (!quoted) return NULL;
    size_t out = 0;
    quoted[out++] = L'"';
    for (size_t i = 0; i < length; i++) {
        if (token[i] == L'"') quoted[out++] = L'\\';
        quoted[out++] = token[i];
    }
    quoted[out++] = L'"';
    quoted[out] = 0;
    return quoted;
}

/* Splits the free-form "extra compiler flags" text from Settings > Compiler
   the way a shell would - via CommandLineToArgvW, so a quoted flag like
   -D"FOO=bar baz" keeps its embedded space instead of being cut in two - and
   re-quotes each token for the target command shell, so the text can't break
   out of the generated build command. Returns NULL when there's nothing to
   add, so callers can treat that the same as "no extra flags configured". */
static WCHAR *quote_extra_compiler_flags(const WCHAR *flags, FlagQuoteStyle style) {
    if (!flags || !flags[0]) return NULL;
    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(flags, &argc);
    if (!argv || argc == 0) {
        if (argv) LocalFree(argv);
        return NULL;
    }
    size_t capacity = 4;
    for (int i = 0; i < argc; i++) capacity += wcslen(argv[i]) * 2 + 4;
    WCHAR *joined = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (joined) {
        joined[0] = 0;
        size_t used = 0;
        for (int i = 0; i < argc; i++) {
            WCHAR *quoted = quote_token_for_shell(argv[i], style);
            if (!quoted) continue;
            int written = swprintf(joined + used, capacity - used, used ? L" %ls" : L"%ls", quoted);
            free(quoted);
            if (written > 0) used += (size_t)written;
        }
    }
    LocalFree(argv);
    return joined;
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
        SendMessageA(g_hEdit, SCI_MARKERDELETEALL, MARKER_DEBUG_ARROW, 0);
    }
    SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)active);
    g_switchingDocument = oldSwitching;
}

static WCHAR *debug_location_to_wide(const char *text);

static void set_debug_panel_status(const WCHAR *status) {
    if (g_hDebugStatus) SetWindowTextW(g_hDebugStatus, status ? status : L"Debugger");
}

static void show_debug_panel(void) {
    g_debugPanelVisible = 1;
    if (g_hMainWnd) layout(g_hMainWnd);
}

static void add_debug_variable(const char *name, const char *value) {
    if (!g_hDebugVariables || !name || !name[0]) return;
    WCHAR *wideName = debug_location_to_wide(name);
    WCHAR *wideValue = debug_location_to_wide(value ? value : "");
    if (!wideName || !wideValue) {
        free(wideName); free(wideValue);
        return;
    }
    LVITEMW item = {0};
    item.mask = LVIF_TEXT;
    item.iItem = ListView_GetItemCount(g_hDebugVariables);
    item.pszText = wideName;
    int row = (int)SendMessageW(g_hDebugVariables, LVM_INSERTITEMW, 0, (LPARAM)&item);
    if (row >= 0) {
        LVITEMW subitem = {0};
        subitem.mask = LVIF_TEXT;
        subitem.iSubItem = 1; subitem.pszText = wideValue;
        SendMessageW(g_hDebugVariables, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&subitem);
    }
    free(wideName); free(wideValue);
}

/* Both debugger backends append a small variables section to the same state
   snapshot that carries the source location. Python emits tab-separated
   name/type/value rows; GDB emits its native `name = value` rows. Rewrites
   `snapshot` in place, and only ever at or after the marker, so a location
   prefix parsed either side of this call still reads correctly. */
static void update_debug_variables(char *snapshot) {
    if (!g_hDebugVariables || !snapshot) return;
    char *variables = strstr(snapshot, "__MARKEDIT_VARIABLES__");
    if (!variables) return;
    variables += strlen("__MARKEDIT_VARIABLES__");
    while (*variables == '\r' || *variables == '\n') variables++;
    ListView_DeleteAllItems(g_hDebugVariables);
    char *line = variables;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t length = strlen(line);
        while (length && (line[length - 1] == '\r' || line[length - 1] == ' '))
            line[--length] = 0;
        if (line[0] && strcmp(line, "Arguments:") != 0 && strcmp(line, "Locals:") != 0) {
            char *firstTab = strchr(line, '\t');
            if (firstTab) {
                *firstTab++ = 0;
                char *secondTab = strchr(firstTab, '\t');
                if (secondTab) {
                    *secondTab++ = 0;
                    add_debug_variable(line, secondTab);
                }
            } else {
                char *equals = strstr(line, " = ");
                if (equals) {
                    *equals = 0;
                    add_debug_variable(line, equals + 3);
                }
            }
        }
        line = next;
    }
}

static void stop_debug_location_tracking(void) {
    int hadSession = g_activeDebuggerKind != 0;
    if (g_hMainWnd) KillTimer(g_hMainWnd, IDT_DEBUG_LOCATION);
    if (g_debugJob) {
        /* JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE terminates PowerShell, GDB, and
           the inferior together, including when MarkEdit itself is exiting. */
        CloseHandle(g_debugJob);
        g_debugJob = NULL;
        /* Closing the job requests termination; wait briefly for its file
           handles to close before removing the generated debugger files. */
        if (g_debugProcess) WaitForSingleObject(g_debugProcess, 1000);
    }
    if (g_debugProcess) {
        CloseHandle(g_debugProcess);
        g_debugProcess = NULL;
    }
    if (g_debugLocationPath) DeleteFileW(g_debugLocationPath);
    if (g_debugCommandPath) DeleteFileW(g_debugCommandPath);
    if (g_debugOutputPath) DeleteFileW(g_debugOutputPath);
    free(g_debugLocationPath); g_debugLocationPath = NULL;
    free(g_debugWorkingDirectory); g_debugWorkingDirectory = NULL;
    free(g_debugCommandPath); g_debugCommandPath = NULL;
    free(g_debugOutputPath); g_debugOutputPath = NULL;
    free(g_lastDebugSource); g_lastDebugSource = NULL;
    free(g_lastDebugState); g_lastDebugState = NULL;
    g_debugLocationSeen = 0;
    g_lastDebugLine = -1;
    g_activeDebuggerKind = 0;
    clear_debug_execution_markers();
    if (hadSession) set_debug_panel_status(L"Debug session ended");
}

static void start_debug_location_tracking(const WCHAR *locationPath,
                                          const WCHAR *workingDirectory,
                                          int debuggerKind, HANDLE process,
                                          const WCHAR *commandPath,
                                          const WCHAR *outputPath) {
    stop_debug_location_tracking();
    g_debugLocationPath = wcsdup_heap(locationPath);
    g_debugWorkingDirectory = wcsdup_heap(workingDirectory);
    g_debugCommandPath = wcsdup_heap(commandPath);
    g_debugOutputPath = wcsdup_heap(outputPath);
    g_debugProcess = process;
    if (debuggerKind == 2 && process) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo = {0};
        jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        g_debugJob = CreateJobObjectW(NULL, NULL);
        if (!g_debugJob ||
            !SetInformationJobObject(g_debugJob, JobObjectExtendedLimitInformation,
                                     &jobInfo, sizeof(jobInfo)) ||
            !AssignProcessToJobObject(g_debugJob, process)) {
            if (g_debugJob) CloseHandle(g_debugJob);
            g_debugJob = NULL;
        }
    }
    g_debugLocationSeen = 1; /* The launcher creates a `starting` sentinel. */
    g_activeDebuggerKind = debuggerKind;
    if (g_hDebugVariables) ListView_DeleteAllItems(g_hDebugVariables);
    set_debug_panel_status(L"Debugger starting...");
    show_debug_panel();
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
    /* GDB invokes hook-stop once more after the inferior exits. At that
       point `frame` reports "No stack" instead of a source location. Treat
       that sentinel as session completion; closing the job also closes the
       otherwise-idle hidden GDB prompt. */
    if (g_activeDebuggerKind == 2 && strstr(text, "Error while running hook_stop:") &&
        strstr(text, "No stack.")) {
        free(text);
        stop_debug_location_tracking();
        return;
    }
    if (g_lastDebugState && strcmp(g_lastDebugState, text) == 0) {
        free(text);
        return;
    }
    char *stateCopy = (char *)malloc((size_t)length + 1);
    if (stateCopy) {
        memcpy(stateCopy, text, (size_t)length + 1);
        free(g_lastDebugState);
        g_lastDebugState = stateCopy;
    }
/* Order matters: the variables section is located with strstr() from the start
       of the snapshot, so it has to run before parse_debug_location()
       NUL-terminates the snapshot just after the leading "file\nline". In
       exchange it only rewrites separators at or after the marker, which
       leaves the location prefix both backends put first intact. */
    update_debug_variables(text);
    WCHAR *source = NULL;
    int line = 0;
    int parsed = parse_debug_location(text, &source, &line);
    free(text);
    if (!parsed) return;
    WCHAR status[MAX_PATH + 64];
    swprintf(status, sizeof(status) / sizeof(status[0]), L"Paused at %ls:%d",
             PathFindFileNameW(source), line);
    set_debug_panel_status(status);
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
    SendMessageA(g_hEdit, SCI_MARKERADD, (WPARAM)(line - 1), MARKER_DEBUG_ARROW);
    SendMessageA(g_hEdit, SCI_ENSUREVISIBLE, (WPARAM)(line - 1), 0);
    Sci_Position visibleLines = (Sci_Position)SendMessageA(g_hEdit, SCI_LINESONSCREEN, 0, 0);
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
                  action == IDM_DEBUG_STEP_INTO ? L"sv" :
                  action == IDM_DEBUG_STEP_OUT ? L"rv" : L"cv";
    else if (g_activeDebuggerKind == 2)
        command = action == IDM_DEBUG_STEP_OVER ? L"next" :
                  action == IDM_DEBUG_STEP_INTO ? L"step" :
                  action == IDM_DEBUG_STEP_OUT ? L"finish" : L"continue";
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
                      (size_t)breakpointCount * 64 + 5120;
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
        L"                stream.write(frame.f_code.co_filename + '\\n' + str(frame.f_lineno) + "
        L"'\\n__MARKEDIT_VARIABLES__\\n')\n"
        L"                for key in sorted(frame.f_locals, key=str):\n"
        L"                    if str(key).startswith('__'):\n"
        L"                        continue\n"
        L"                    value = frame.f_locals[key]\n"
        L"                    try:\n"
        L"                        rendered = _markedit_pprint.pformat(value, width=100, compact=True)\n"
        L"                    except Exception as error:\n"
        L"                        rendered = '<unavailable: ' + repr(error) + '>'\n"
        L"                    rendered = rendered.replace('\\\\', '\\\\\\\\').replace('\\t', '\\\\t').replace('\\r', '\\\\r').replace('\\n', '\\\\n')\n"
        L"                    if len(rendered) > 2000:\n"
        L"                        rendered = rendered[:1997] + '...'\n"
        L"                    stream.write(str(key) + '\\t' + type(value).__name__ + '\\t' + rendered + '\\n')\n"
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
        L"    'alias rv return',\n"
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
            L"'pdb: F10/n next, F11/s step, Shift+F11/r return, F6/c continue; vars, globals, b <line>, p <expr>, where, q'; "
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
            start_debug_location_tracking(debugLocation, directory, 1, execute.hProcess,
                                          pdbDriver, NULL);
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
        WCHAR *lua = g_luaBinOverride ? executable_in_directory(g_luaBinOverride, names[i])
                                       : executable_on_search_path(names[i]);
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
        /* A return of 0 is "not set"; a return >= the buffer size means the
           value didn't fit and the buffer's contents are left undefined -
           both mean "skip this one", not just the zero case. */
        DWORD len = GetEnvironmentVariableW(envNames[e], programFiles, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) continue;
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
   compiler same as before this setting existed). Each of GCC/Clang/Zig also
   has its own optional folder override (Settings > Compiler), searched
   instead of PATH exactly like g_vcvarsallOverridePath already does for
   MSVC's vcvarsall.bat. */
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
        const WCHAR *override = kinds[i] == NATIVE_COMPILER_GCC ? g_gccBinOverride :
            kinds[i] == NATIVE_COMPILER_CLANG ? g_clangBinOverride : g_zigBinOverride;
        WCHAR *compiler = override ? executable_in_directory(override, names[i])
                                    : executable_on_search_path(names[i]);
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
        /* GDB stays hidden, but its inferior gets an independent visible
           console so the program's own output and input remain accessible. */
        L"set new-console on\n"
        L"set width 0\n"
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
        L"  set print pretty off\n"
        L"  echo \\n__MARKEDIT_VARIABLES__\\n\n"
        L"  echo Arguments:\\n\n"
        L"  info args\n"
        L"  echo Locals:\\n\n"
        L"  info locals\n"
        L"  set logging enabled off\n"
        L"  set print pretty on\n"
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
    WCHAR *extraFlags = quote_extra_compiler_flags(g_compilerExtraFlags, FLAG_QUOTE_CMD);
    size_t capacity = wcslen(vcvarsallPath) + wcslen(output) * 3 + wcslen(g_currentFile) +
        (extraFlags ? wcslen(extraFlags) : 0) + 512;
    WCHAR *command = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!command) {
        free(vcvarsallPath); free(output); free(directory); free(extraFlags);
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
        L"cl.exe /nologo %ls %ls /O2 /EHsc /Fe:\"%ls\" \"%ls\" && "
        L"(\"%ls\" & echo. & echo Process exited with code !errorlevel!) & "
        L"del /f /q \"%ls\" >nul 2>nul\"",
        vcvarsallPath, kind == DEBUG_SOURCE_C ? L"C" : L"C++", std, extraFlags ? extraFlags : L"",
        output, g_currentFile, output, output);
    free(vcvarsallPath);
    free(extraFlags);
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
    WCHAR *debugger = debug ? find_gdb_executable() : NULL;
    if (!compiler) {
        free(debugger);
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
                    L"GDB was not found. Restore third_party\\gdb\\bin\\gdb.exe, "
                    L"choose a GDB folder in Settings, or add gdb.exe to PATH.",
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
    WCHAR *extraFlags = quote_extra_compiler_flags(g_compilerExtraFlags, FLAG_QUOTE_POWERSHELL);
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
                      (quotedDebugLocation ? wcslen(quotedDebugLocation) : 0) +
                      (extraFlags ? wcslen(extraFlags) : 0) + 2048;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!parameters) goto native_launch_cleanup;

    const WCHAR *compilerMode = compilerKind == NATIVE_COMPILER_ZIG
        ? (kind == DEBUG_SOURCE_C ? L" cc" : L" c++") : L"";
    const WCHAR *standard = kind == DEBUG_SOURCE_C ? L"-std=gnu11" : L"-std=gnu++17";
    const WCHAR *buildFlags = debug
        ? L"'-g' '-O0' '-fno-omit-frame-pointer'" : L"'-O2'";
    /* Debugger output and controls now live in MarkEdit. Keep the host
       PowerShell console hidden for GDB sessions and let it exit with GDB;
       ordinary Run still leaves its visible results console open. */
    const WCHAR *shellOptions = debug
        ? L"-NoLogo -NoProfile -WindowStyle Hidden -Command"
        : L"-NoLogo -NoProfile -NoExit -Command";
    int written = swprintf(parameters, capacity,
        L"%ls \"Write-Host 'Building %ls...'; "
        L"& %ls%ls '%ls' %ls %ls '-o' %ls %ls; "
        L"if ($LASTEXITCODE -eq 0) { ",
        shellOptions, kind == DEBUG_SOURCE_C ? L"C" : L"C++", quotedCompiler, compilerMode,
        standard, buildFlags, extraFlags ? extraFlags : L"", quotedOutput, quotedSource);
    if (written < 0) goto native_parameters_cleanup;
    size_t used = (size_t)written;

    if (debug) {
        written = swprintf(parameters + used, capacity - used,
            L"Write-Host 'gdb: next/n, step/s, finish, continue/c, vars, print <expr>, bt, watch <expr>, quit'; "
            L"Write-Host 'Arguments and locals are shown automatically whenever execution stops.'; "
            L"& %ls '-q' '-x' %ls %ls; ",
            quotedDebugger, quotedGdbCommands, quotedOutput);
        if (written < 0) goto native_parameters_cleanup;
        /* `used` is advanced once, below, after this if/else - adding it here
           too double-counted this segment's length, corrupting everything
           swprintf'd after it (see the shared code review for the failure). */
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
    execute.nShow = debug ? SW_HIDE : SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        MessageBoxW(g_hMainWnd, L"The C/C++ build console could not be started.",
                    debug ? L"Debug C/C++" : L"Run C/C++", MB_OK | MB_ICONERROR);
    } else {
        if (debug) {
            commandFileHandedOff = 1;
            start_debug_location_tracking(debugLocation, directory, 2, execute.hProcess,
                                          gdbCommands, output);
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
    free(extraFlags);
}

static void launch_active_source_file(int debug) {
    if (!g_currentFile) {
        if (!do_save()) return;
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
/* True when `line` (length lineLen) is exactly an HTML comment delimiter,
   ignoring surrounding whitespace. */
static int line_is_comment_delimiter(const char *line, Sci_Position lineLen, const char *delimiter) {
    Sci_Position i = 0, end = lineLen;
    while (i < end && (line[i] == ' ' || line[i] == '\t')) i++;
    while (end > i && (line[end - 1] == ' ' || line[end - 1] == '\t')) end--;
    int delimiterLen = (int)strlen(delimiter);
    return end - i == delimiterLen && memcmp(line + i, delimiter, (size_t)delimiterLen) == 0;
}

/* Markdown has no line-comment syntax, so the selected line range is wrapped
   in an HTML comment on its own lines - and unwrapped when already exactly
   that. */
static void toggle_comment(HWND hEdit) {
    Sci_Position firstLine, lastLine;
    editor_selected_line_range(hEdit, &firstLine, &lastLine);

    Sci_Position length;
    char *text = get_edit_text(hEdit, &length);
    if (!text) return;
    Sci_Position blockStart = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)firstLine, 0);
    Sci_Position blockEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETLINEENDPOSITION, (WPARAM)lastLine, 0);

    Sci_Position openEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETLINEENDPOSITION, (WPARAM)firstLine, 0);
    Sci_Position closeStart = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)lastLine, 0);
    if (lastLine - firstLine >= 1 &&
        line_is_comment_delimiter(text + blockStart, openEnd - blockStart, "<!--") &&
        line_is_comment_delimiter(text + closeStart, blockEnd - closeStart, "-->")) {
        Sci_Position innerStart = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)(firstLine + 1), 0);
        /* lastLine == firstLine + 1 means the delimiters are on adjacent
           lines with nothing between them - the inner content is empty
           rather than computed from a line range (which would go negative). */
        Sci_Position innerLength = lastLine - firstLine >= 2
            ? (Sci_Position)SendMessageA(hEdit, SCI_GETLINEENDPOSITION, (WPARAM)(lastLine - 1), 0) - innerStart
            : 0;
        char *inner = (char *)malloc((size_t)innerLength + 1);
        if (inner) {
            memcpy(inner, text + innerStart, (size_t)innerLength);
            inner[innerLength] = 0;
            SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
            SendMessageA(hEdit, SCI_SETSEL, (WPARAM)blockStart, (LPARAM)blockEnd);
            SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)inner);
            SendMessageA(hEdit, SCI_SETSEL, (WPARAM)blockStart, (LPARAM)(blockStart + innerLength));
            SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
            free(inner);
        }
        free(text);
        SetFocus(hEdit);
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
        SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
        SendMessageA(hEdit, SCI_SETSEL, (WPARAM)blockStart, (LPARAM)blockEnd);
        SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)wrapped);
        SendMessageA(hEdit, SCI_SETSEL, (WPARAM)blockStart, (LPARAM)(blockStart + (int)wrappedLen));
        SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
        free(wrapped);
    }
    free(text);
    SetFocus(hEdit);
}

/* True when the line begins a fenced code block delimiter (``` or ~~~,
   after up to three leading spaces as CommonMark allows). */
static int line_starts_fence(Sci_Position line) {
    Sci_Position lineStart = (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
    Sci_Position lineEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)line, 0);
    Sci_Position i = lineStart;
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
static int fence_body_range(Sci_Position line, Sci_Position *bodyStart, Sci_Position *bodyEnd) {
    Sci_Position lineCount = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
    Sci_Position openLine = -1, closeLine = -1;

    for (Sci_Position scan = line; scan >= 0; scan--) {
        if (!line_starts_fence(scan)) continue;
        /* A fence delimiter on the caret's own line is the opener only if the
           caret sits on the opening one; scanning up finds that either way. */
        openLine = scan;
        break;
    }
    if (openLine < 0) return 0;

    for (Sci_Position scan = openLine + 1; scan < lineCount; scan++) {
        if (line_starts_fence(scan)) { closeLine = scan; break; }
    }
    if (closeLine < 0 || line > closeLine) return 0;

    *bodyStart = (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)(openLine + 1), 0);
    *bodyEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)(closeLine - 1), 0);
    return closeLine > openLine + 1;
}

/* Reformats the selection, or the fenced block at the caret when there is no
   selection. Leaves the document untouched if the text isn't valid JSON. */
static void format_json(int indent) {
    Sci_Position start = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position end = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);

    if (start == end) {
        Sci_Position caretLine = (Sci_Position)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)start, 0);
        if (!fence_body_range(caretLine, &start, &end)) {
            MessageBoxW(g_hMainWnd,
                        L"Place the caret inside a fenced code block, or select the JSON to format.",
                        L"MarkEdit", MB_OK | MB_ICONINFORMATION);
            return;
        }
    }

    /* json_reformat() takes an int length. */
    if (end - start > INT_MAX) {
        MessageBoxW(g_hMainWnd, L"That JSON is too large to format.",
                    L"MarkEdit", MB_OK | MB_ICONWARNING);
        return;
    }
    Sci_Position length;
    char *text = get_edit_text(g_hEdit, &length);
    if (!text) return;

    char *formatted = json_reformat(text + start, (int)(end - start), editor_eol_sequence(), indent);
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
    if (g_largeFile) return;
    /* The list is only read back by clicks on it, and toggle_outline()
       rebuilds it when the pane is shown, so a hidden pane skips the scan. */
    if (!g_outlineVisible || g_activeLanguage != LANG_MARKDOWN) return;
    /* Suspend repainting so re-adding every heading doesn't flicker. */
    SendMessageW(g_hOutline, WM_SETREDRAW, FALSE, 0);
    Sci_Position lines = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
    for (Sci_Position lineNumber = 0; lineNumber < lines && g_outlineCount < 1024; lineNumber++) {
        Sci_Position linePosition = (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)lineNumber, 0);
        int style = (int)SendMessageA(g_hEdit, SCI_GETSTYLEAT, (WPARAM)linePosition, 0);
        if (style < SCE_MARKDOWN_HEADER1 || style > SCE_MARKDOWN_HEADER6) continue;
        Sci_Position length = (Sci_Position)SendMessageA(g_hEdit, SCI_LINELENGTH, (WPARAM)lineNumber, 0);
        if (length <= 0 || length > 65535) continue;
        char *line = (char *)malloc((size_t)length + 1);
        if (!line) break;
        Sci_Position got = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINE, (WPARAM)lineNumber, (LPARAM)line);
        line[got >= 0 && got <= length ? got : length] = 0;
        int start = 0, textLength = 0;
        int level = parse_atx_heading(line, (int)got, &start, &textLength);
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
    SendMessageW(g_hOutline, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hOutline, NULL, TRUE);
}

static void toggle_outline(HWND hwnd) {
    g_outlineVisible = !g_outlineVisible;
    update_outline();
    update_view_menu(hwnd);
    layout(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
    save_layout_settings();
}

/* One-time, document-independent setup for the minimap's Scintilla view:
   read-only, no margins or scrollbars of its own, unwrapped so each document
   line maps to exactly one minimap line, and no visible caret. Font, theme,
   and zoom are applied separately (see apply_editor_font()) since those also
   need to be reapplied whenever the editor's font or theme changes. */
static void configure_minimap(HWND hMinimap) {
    SendMessageA(hMinimap, SCI_SETCODEPAGE, SC_CP_UTF8, 0);
    SendMessageA(hMinimap, SCI_SETREADONLY, TRUE, 0);
    SendMessageA(hMinimap, SCI_SETUNDOCOLLECTION, FALSE, 0);
    SendMessageA(hMinimap, SCI_SETVSCROLLBAR, FALSE, 0);
    SendMessageA(hMinimap, SCI_SETHSCROLLBAR, FALSE, 0);
    SendMessageA(hMinimap, SCI_SETWRAPMODE, SC_WRAP_NONE, 0);
    SendMessageA(hMinimap, SCI_SETMARGINWIDTHN, MARGIN_LINENUMBERS, 0);
    SendMessageA(hMinimap, SCI_SETMARGINWIDTHN, MARGIN_BOOKMARKS, 0);
    SendMessageA(hMinimap, SCI_SETMARGINWIDTHN, MARGIN_FOLD, 0);
    SendMessageA(hMinimap, SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE, 0);
    SendMessageA(hMinimap, SCI_SETCARETLINEVISIBLE, FALSE, 0);
    SendMessageA(hMinimap, SCI_SETMOUSEDOWNCAPTURES, FALSE, 0);
    SendMessageA(hMinimap, SCI_SETCURSOR, SC_CURSORARROW, 0);
    SendMessageA(hMinimap, SCI_SETZOOM, 0, 0); /* sizing is done via STYLESETSIZEFRACTIONAL, see apply_editor_font() */
    SendMessageA(hMinimap, SCI_SETEXTRAASCENT, (WPARAM)MINIMAP_EXTRA_ASCENT, 0);
    SendMessageA(hMinimap, SCI_SETEXTRADESCENT, (WPARAM)MINIMAP_EXTRA_DESCENT, 0);
}

/* Recomputes the minimap's own scroll position (for documents too long to
   fit the minimap at once) and the visible-region box drawn over it, then
   asks for a repaint. Called whenever the main editor scrolls, its content
   changes size, or the minimap is resized/shown. */
static void update_minimap_viewport(void) {
    if (!g_hMinimap || !g_hEdit || !g_minimapVisible || !IsWindowVisible(g_hMinimap)) return;
    HWND view = tracked_edit_view();

    Sci_Position totalLines = (Sci_Position)SendMessageA(view, SCI_GETLINECOUNT, 0, 0);
    Sci_Position mainFirstVisible = (Sci_Position)SendMessageA(view, SCI_GETFIRSTVISIBLELINE, 0, 0);
    Sci_Position mainFirstDocLine = (Sci_Position)SendMessageA(view, SCI_DOCLINEFROMVISIBLE, (WPARAM)mainFirstVisible, 0);
    Sci_Position mainLinesOnScreen = (Sci_Position)SendMessageA(view, SCI_LINESONSCREEN, 0, 0);

    int minimapLineHeight = (int)SendMessageA(g_hMinimap, SCI_TEXTHEIGHT, 0, 0);
    if (minimapLineHeight < 1) minimapLineHeight = 1;
    RECT mmRect;
    GetClientRect(g_hMinimap, &mmRect);
    int minimapLinesOnScreen = (mmRect.bottom - mmRect.top) / minimapLineHeight;
    if (minimapLinesOnScreen < 1) minimapLinesOnScreen = 1;

    /* Documents that fit entirely within the minimap's height stay pinned at
       the top; longer ones scroll to keep the main editor's viewport roughly
       centered within the minimap. */
    Sci_Position minimapFirstDocLine = 0;
    if (totalLines > minimapLinesOnScreen) {
        minimapFirstDocLine = mainFirstDocLine - (minimapLinesOnScreen - mainLinesOnScreen) / 2;
        Sci_Position maxFirst = totalLines - minimapLinesOnScreen;
        if (minimapFirstDocLine > maxFirst) minimapFirstDocLine = maxFirst;
        if (minimapFirstDocLine < 0) minimapFirstDocLine = 0;
    }
    Sci_Position minimapFirstVisible = (Sci_Position)SendMessageA(g_hMinimap, SCI_VISIBLEFROMDOCLINE, (WPARAM)minimapFirstDocLine, 0);
    SendMessageA(g_hMinimap, SCI_SETFIRSTVISIBLELINE, (WPARAM)minimapFirstVisible, 0);

    /* Pixels within the minimap: the clamping above keeps the main view's
       first line within one minimap height of the minimap's own. */
    int newViewTop = (int)(mainFirstDocLine - minimapFirstDocLine) * minimapLineHeight;
    int newViewHeight = (int)mainLinesOnScreen * minimapLineHeight;
    if (newViewHeight < 4) newViewHeight = 4;

    /* This runs on every caret move/scroll tick (see SCN_UPDATEUI), so only
       repaint the visible-region box when it actually moved or resized. */
    if (newViewTop == g_minimapViewTop && newViewHeight == g_minimapViewHeight) return;
    g_minimapViewTop = newViewTop;
    g_minimapViewHeight = newViewHeight;
    InvalidateRect(g_hMinimap, NULL, FALSE);
}

/* Scrolls the main editor so the document line under minimap-local y is
   centered in its viewport - used for both a single click and drag-to-scroll. */
static void minimap_scroll_main_to_y(int y) {
    if (!g_hEdit || !g_hMinimap) return;
    HWND view = tracked_edit_view();
    Sci_Position pos = (Sci_Position)SendMessageA(g_hMinimap, SCI_POSITIONFROMPOINT, (WPARAM)0, (LPARAM)y);
    if (pos < 0) return;
    Sci_Position docLine = (Sci_Position)SendMessageA(g_hMinimap, SCI_LINEFROMPOSITION, (WPARAM)pos, 0);
    Sci_Position mainLinesOnScreen = (Sci_Position)SendMessageA(view, SCI_LINESONSCREEN, 0, 0);
    Sci_Position target = docLine - mainLinesOnScreen / 2;
    if (target < 0) target = 0;
    Sci_Position targetVisible = (Sci_Position)SendMessageA(view, SCI_VISIBLEFROMDOCLINE, (WPARAM)target, 0);
    SendMessageA(view, SCI_SETFIRSTVISIBLELINE, (WPARAM)targetVisible, 0);
}

static LRESULT CALLBACK MinimapSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR id, DWORD_PTR data) {
    (void)id; (void)data;
    switch (msg) {
        case WM_LBUTTONDOWN:
            g_minimapDragging = 1;
            SetCapture(hwnd);
            minimap_scroll_main_to_y(GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSEMOVE:
            if (g_minimapDragging) minimap_scroll_main_to_y(GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONUP:
            if (g_minimapDragging) { g_minimapDragging = 0; ReleaseCapture(); }
            return 0;
        case WM_CAPTURECHANGED:
            g_minimapDragging = 0;
            return 0;
        /* Swallowed rather than forwarded to Scintilla's default handling,
           which would otherwise open its own context menu or place a caret
           in what is meant to be a read-only overview. */
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
        case WM_LBUTTONDBLCLK:
            return 0;
        case WM_MOUSEWHEEL:
            if (g_hEdit) SendMessageA(tracked_edit_view(), WM_MOUSEWHEEL, wParam, lParam);
            return 0;
        case WM_PAINT: {
            LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
            RECT rc;
            GetClientRect(hwnd, &rc);
            RECT box = { rc.left, g_minimapViewTop, rc.right, g_minimapViewTop + g_minimapViewHeight };
            if (box.top < rc.top) box.top = rc.top;
            if (box.bottom > rc.bottom) box.bottom = rc.bottom;
            if (box.bottom > box.top) {
                HDC hdc = GetDC(hwnd);
                HBRUSH brush = CreateSolidBrush(g_editorDarkMode ? RGB(0x6E, 0x76, 0x81) : RGB(0x8B, 0x94, 0x9E));
                FrameRect(hdc, &box, brush);
                DeleteObject(brush);
                ReleaseDC(hwnd, hdc);
            }
            return result;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, MinimapSubclassProc, 1);
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void toggle_minimap(HWND hwnd) {
    g_minimapVisible = !g_minimapVisible;
    update_view_menu(hwnd);
    layout(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
    save_layout_settings();
}

static void toggle_split_editor(HWND hwnd) {
    g_splitEditorVisible = !g_splitEditorVisible;
    if (!g_splitEditorVisible) g_lastEditView = g_hEdit;
    update_view_menu(hwnd);
    layout(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
    save_layout_settings();
}

/* Puts the split-editor divider back at an even 50/50, the same default
   layout() falls back to for a brand-new profile. g_splitEditorPos is the
   live pixel value layout() actually drags around; resetting only the ratio
   would leave it untouched until the next resize recomputed a position from
   it (see layout_editor_with_minimap()). */
static void reset_split_editor(HWND hwnd) {
    g_splitEditorRatio = 500;
    g_splitEditorPos = -1;
    layout(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
    save_layout_settings();
}

static char *editor_line_text(Sci_Position lineNumber, int *outLength) {
    Sci_Position length = (Sci_Position)SendMessageA(g_hEdit, SCI_LINELENGTH, (WPARAM)lineNumber, 0);
    if (length < 0 || length > 1024 * 1024) return NULL;
    char *line = (char *)malloc((size_t)length + 1);
    if (!line) return NULL;
    Sci_Position got = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINE, (WPARAM)lineNumber, (LPARAM)line);
    if (got < 0) got = 0;
    if (got > length) got = length;
    line[got] = 0;
    while (got > 0 && (line[got - 1] == '\r' || line[got - 1] == '\n')) line[--got] = 0;
    if (outLength) *outLength = (int)got; /* at most 1 MiB, checked above */
    return line;
}

/* Cells are heap-allocated to the trimmed cell's exact length rather than
   copied into a fixed-size slot, so a cell wider than some arbitrary cap
   isn't silently truncated - and, since show_table_editor() writes these
   cells straight back into the document, truncation here would otherwise
   permanently lose part of the user's table content on every re-edit. */
static int table_row_cells(const char *line, char *cells[], int maximum) {
    int count = 0;
    const char *cursor = line;
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (*cursor == '|') cursor++;

    /* Trailing whitespace after the row's closing pipe (common when a table
       was hand-padded or pasted in) is insignificant and must not become a
       phantom extra cell, so the scan is bounded to strip it up front. */
    const char *limit = cursor + strlen(cursor);
    while (limit > cursor && (limit[-1] == ' ' || limit[-1] == '\t')) limit--;

    while (cursor < limit && count < maximum) {
        const char *end = memchr(cursor, '|', (size_t)(limit - cursor));
        if (!end) end = limit;
        const char *start = cursor;
        while (start < end && (*start == ' ' || *start == '\t')) start++;
        const char *cellEnd = end;
        while (cellEnd > start && (cellEnd[-1] == ' ' || cellEnd[-1] == '\t')) cellEnd--;
        size_t length = (size_t)(cellEnd - start);
        char *cell = (char *)malloc(length + 1);
        if (cell) {
            memcpy(cell, start, length);
            cell[length] = 0;
        }
        cells[count] = cell;
        count++;
        if (end >= limit) break;
        cursor = end + 1;
    }
    return count;
}

static void free_table_cells(char *cells[51][20]) {
    for (int row = 0; row < 51; row++)
        for (int column = 0; column < 20; column++)
            free(cells[row][column]);
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
    Sci_Position caret = (Sci_Position)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    Sci_Position caretLine = (Sci_Position)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)caret, 0);
    Sci_Position lineCount = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
    Sci_Position first = caretLine, last = caretLine;
    int existing = 0;
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

    char *cells[51][20] = {0};
    int existingRows = 0, columns = 3;
    if (existing) {
        char *header = editor_line_text(first, NULL);
        if (header) { columns = table_row_cells(header, cells[0], 20); free(header); }
        if (columns < 1) columns = 1;
        existingRows = 1;
        for (Sci_Position sourceLine = first + 2; sourceLine <= last && existingRows < 51; sourceLine++) {
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
                         rowsText, sizeof(rowsText) / sizeof(rowsText[0]))) {
        free_table_cells(cells);
        return;
    }
    swprintf(value, sizeof(value) / sizeof(value[0]), L"%d", columns);
    if (!show_text_input(L"Table Editor", L"Columns:", value,
                         columnsText, sizeof(columnsText) / sizeof(columnsText[0]))) {
        free_table_cells(cells);
        return;
    }
    int dataRows = (int)wcstol(rowsText, NULL, 10);
    int requestedColumns = (int)wcstol(columnsText, NULL, 10);
    if (dataRows < 1 || dataRows > 50 || requestedColumns < 1 || requestedColumns > 20) {
        MessageBoxW(g_hMainWnd, L"Use 1–50 data rows and 1–20 columns.", L"Table Editor",
                    MB_OK | MB_ICONWARNING);
        free_table_cells(cells);
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
                if (existing && column < 20 && cells[0][column] && cells[0][column][0]) cell = cells[0][column];
                else { snprintf(fallback, sizeof(fallback), "Header %d", column + 1); cell = fallback; }
            } else if (row == 1) cell = "---";
            else if (existing && row - 1 < existingRows && column < 20 && cells[row - 1][column]) cell = cells[row - 1][column];
            if (!append_dynamic(&table, &length, &capacity, " ") ||
                !append_dynamic(&table, &length, &capacity, cell) ||
                !append_dynamic(&table, &length, &capacity, " |")) break;
        }
        append_dynamic(&table, &length, &capacity, eol);
    }
    free_table_cells(cells);
    if (!table) return;
    Sci_Position replaceStart, replaceEnd;
    if (existing) {
        replaceStart = (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)first, 0);
        replaceEnd = last + 1 < lineCount
            ? (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)(last + 1), 0)
            : (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
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

/* ---- caret-relative table row/column insert and delete -----------------
   Quicker alternatives to the Table Editor dialog above: these act directly
   on whichever table the caret is currently in, and where in it, rather than
   prompting for a whole new row/column count and rebuilding the table from
   scratch. */

typedef struct TableCaretInfo {
    Sci_Position firstLine, lastLine; /* inclusive line range of the whole table */
    Sci_Position row;                 /* 0 = header, 1 = separator, 2+ = data row */
    int column;               /* 0-based column the caret is in */
} TableCaretInfo;

/* How many pipe-delimited cells of `line` end strictly before byte `offset`,
   using the same leading-whitespace/leading-pipe skipping table_row_cells()
   uses, so the result lines up with that function's own cell indexing. */
static int table_column_at_offset(const char *line, Sci_Position offset) {
    const char *cursor = line;
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (*cursor == '|') cursor++;
    int column = 0;
    for (const char *p = line; p < line + offset && *p; p++) {
        if (p >= cursor && *p == '|') column++;
    }
    return column;
}

static void free_row_cells(char *cells[], int count) {
    for (int i = 0; i < count; i++) free(cells[i]);
}

/* Locates the Markdown table (if any) containing the caret the same way
   show_table_editor() does, plus which row and column the caret is in.
   Returns 0 (leaving *info untouched) if the caret isn't in a table with at
   least a header and a separator row. */
static int find_table_caret_info(TableCaretInfo *info) {
    Sci_Position caret = (Sci_Position)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    Sci_Position caretLine = (Sci_Position)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)caret, 0);
    Sci_Position lineCount = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
    char *line = editor_line_text(caretLine, NULL);
    if (!line || !strchr(line, '|')) { free(line); return 0; }

    Sci_Position first = caretLine, last = caretLine;
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
    if (last - first < 1) { free(line); return 0; } /* need a header and a separator row */

    Sci_Position lineStart = (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)caretLine, 0);
    info->firstLine = first;
    info->lastLine = last;
    info->row = caretLine - first;
    info->column = table_column_at_offset(line, caret - lineStart);
    free(line);
    return 1;
}

static void report_not_in_table(void) {
    MessageBoxW(g_hMainWnd, L"Place the caret inside a Markdown table.", L"Table",
                MB_OK | MB_ICONINFORMATION);
}

/* Inserts a new, empty data row either immediately above or below the
   caret's current row. From the header or separator row - where "above"/
   "below" would mean inserting before the header or between header and
   separator, neither a valid table shape - the new row always lands as the
   first data row instead. */
static void insert_table_row_relative(int below) {
    TableCaretInfo info;
    if (!find_table_caret_info(&info)) { report_not_in_table(); return; }

    char *header = editor_line_text(info.firstLine, NULL);
    char *headerCells[128];
    int columns = header ? table_row_cells(header, headerCells, 128) : 0;
    free_row_cells(headerCells, columns);
    free(header);
    if (columns < 1) columns = 1;

    Sci_Position targetLine = info.row <= 1
        ? info.firstLine + 2 /* first data row, right after the separator */
        : info.firstLine + info.row + (below ? 1 : 0);

    Sci_Position lineCount = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
    int appendAtEnd = targetLine >= lineCount;
    Sci_Position insertPos = appendAtEnd
        ? (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0)
        : (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)targetLine, 0);

    const char *eol = editor_eol_sequence();
    char *rowText = NULL; size_t rowLength = 0, rowCapacity = 0;
    if (appendAtEnd && position_needs_leading_eol(insertPos))
        append_dynamic(&rowText, &rowLength, &rowCapacity, eol);
    for (int c = 0; c < columns; c++)
        append_dynamic(&rowText, &rowLength, &rowCapacity, "|  ");
    append_dynamic(&rowText, &rowLength, &rowCapacity, "|");
    if (!appendAtEnd) append_dynamic(&rowText, &rowLength, &rowCapacity, eol);
    if (!rowText) return;

    SendMessageA(g_hEdit, SCI_INSERTTEXT, (WPARAM)insertPos, (LPARAM)rowText);
    free(rowText);
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)insertPos, (LPARAM)insertPos);
    SetFocus(g_hEdit);
}

/* Deletes the data row the caret is in. The header and separator rows can't
   be deleted this way - the Table Editor is the tool for reshaping those. */
static void delete_table_row_at_cursor(void) {
    TableCaretInfo info;
    if (!find_table_caret_info(&info)) { report_not_in_table(); return; }
    if (info.row <= 1) {
        MessageBoxW(g_hMainWnd, L"Place the caret in a data row to delete it.", L"Table",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    Sci_Position line = info.firstLine + info.row;
    Sci_Position lineCount = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
    Sci_Position start, end;
    if (line + 1 < lineCount) {
        start = (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
        end = (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)(line + 1), 0);
    } else {
        /* Deleting the document's very last line: remove the EOL before it
           instead of one that doesn't exist after it, so no blank line is
           left behind at the end of the table. */
        end = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
        start = line > 0
            ? (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)(line - 1), 0)
            : (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
    }
    SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)start, 0);
    SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)end, 0);
    SendMessageA(g_hEdit, SCI_REPLACETARGET, 0, (LPARAM)"");
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)start, (LPARAM)start);
    SetFocus(g_hEdit);
}

/* Rewrites every row of the table (header, separator, and every data row) as
   one undo step, inserting a new cell at `at` in each - shared by insert-
   column-left/right below. */
static void insert_table_column_at(const TableCaretInfo *info, int at) {
    Sci_Position lineCount = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
    /* When the table is the last thing in the document, its last line may
       have no trailing EOL of its own - so the rebuilt text mustn't add one
       either, or every insert would tack on a newline the file never had. */
    int atDocumentEnd = info->lastLine + 1 >= lineCount;
    const char *eol = editor_eol_sequence();
    char *out = NULL; size_t outLength = 0, outCapacity = 0;
    int ok = 1;
    for (Sci_Position lineNumber = info->firstLine; ok && lineNumber <= info->lastLine; lineNumber++) {
        char *lineText = editor_line_text(lineNumber, NULL);
        char *cells[128];
        int count = table_row_cells(lineText ? lineText : "", cells, 128);
        free(lineText);
        Sci_Position rowKind = lineNumber - info->firstLine; /* 0 header, 1 separator, 2+ data */
        int insertAt = at > count ? count : at;
        const char *newCell = rowKind == 1 ? "---" : rowKind == 0 ? "New" : "";

        if (!append_dynamic(&out, &outLength, &outCapacity, "|")) ok = 0;
        for (int c = 0; ok && c <= count; c++) {
            if (c == insertAt) {
                if (!append_dynamic(&out, &outLength, &outCapacity, " ") ||
                    !append_dynamic(&out, &outLength, &outCapacity, newCell) ||
                    !append_dynamic(&out, &outLength, &outCapacity, " |")) { ok = 0; break; }
            }
            if (c < count) {
                const char *cell = cells[c] ? cells[c] : "";
                if (!append_dynamic(&out, &outLength, &outCapacity, " ") ||
                    !append_dynamic(&out, &outLength, &outCapacity, cell) ||
                    !append_dynamic(&out, &outLength, &outCapacity, " |")) { ok = 0; break; }
            }
        }
        free_row_cells(cells, count);
        if (ok && !(atDocumentEnd && lineNumber == info->lastLine))
            append_dynamic(&out, &outLength, &outCapacity, eol);
    }
    if (!ok || !out) { free(out); return; }

    Sci_Position replaceStart = (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)info->firstLine, 0);
    Sci_Position replaceEnd = atDocumentEnd
        ? (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0)
        : (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)(info->lastLine + 1), 0);
    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)replaceStart, 0);
    SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)replaceEnd, 0);
    SendMessageA(g_hEdit, SCI_REPLACETARGET, (WPARAM)outLength, (LPARAM)out);
    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
    free(out);
    SetFocus(g_hEdit);
}

static void insert_table_column_relative(int rightSide) {
    TableCaretInfo info;
    if (!find_table_caret_info(&info)) { report_not_in_table(); return; }
    insert_table_column_at(&info, info.column + (rightSide ? 1 : 0));
}

/* Deletes the column the caret is in, from every row of the table. Refuses
   on a table's last remaining column, since a table needs at least one. */
static void delete_table_column_at_cursor(void) {
    TableCaretInfo info;
    if (!find_table_caret_info(&info)) { report_not_in_table(); return; }

    char *header = editor_line_text(info.firstLine, NULL);
    char *headerCells[128];
    int headerCount = header ? table_row_cells(header, headerCells, 128) : 0;
    free_row_cells(headerCells, headerCount);
    free(header);
    if (headerCount <= 1) {
        MessageBoxW(g_hMainWnd, L"A table needs at least one column.", L"Table",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    int removeAt = info.column >= headerCount ? headerCount - 1 : info.column;

    Sci_Position lineCount = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
    /* When the table is the last thing in the document, its last line may
       have no trailing EOL of its own - so the rebuilt text mustn't add one
       either, or every delete would tack on a newline the file never had. */
    int atDocumentEnd = info.lastLine + 1 >= lineCount;
    const char *eol = editor_eol_sequence();
    char *out = NULL; size_t outLength = 0, outCapacity = 0;
    int ok = 1;
    for (Sci_Position lineNumber = info.firstLine; ok && lineNumber <= info.lastLine; lineNumber++) {
        char *lineText = editor_line_text(lineNumber, NULL);
        char *cells[128];
        int count = table_row_cells(lineText ? lineText : "", cells, 128);
        free(lineText);

        if (!append_dynamic(&out, &outLength, &outCapacity, "|")) ok = 0;
        for (int c = 0; ok && c < count; c++) {
            if (c == removeAt) continue;
            const char *cell = cells[c] ? cells[c] : "";
            if (!append_dynamic(&out, &outLength, &outCapacity, " ") ||
                !append_dynamic(&out, &outLength, &outCapacity, cell) ||
                !append_dynamic(&out, &outLength, &outCapacity, " |")) { ok = 0; break; }
        }
        free_row_cells(cells, count);
        if (ok && !(atDocumentEnd && lineNumber == info.lastLine))
            append_dynamic(&out, &outLength, &outCapacity, eol);
    }
    if (!ok || !out) { free(out); return; }

    Sci_Position replaceStart = (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)info.firstLine, 0);
    Sci_Position replaceEnd = atDocumentEnd
        ? (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0)
        : (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)(info.lastLine + 1), 0);
    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)replaceStart, 0);
    SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)replaceEnd, 0);
    SendMessageA(g_hEdit, SCI_REPLACETARGET, (WPARAM)outLength, (LPARAM)out);
    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
    free(out);
    SetFocus(g_hEdit);
}

/* ---- caret-relative table sort and column alignment --------------------- */

typedef struct TableSortRow { char *lineText; char *key; } TableSortRow;
static int g_tableSortNumeric = 0, g_tableSortDescending = 0;

static int table_cell_is_numeric(const char *cell) {
    while (*cell == ' ' || *cell == '\t') cell++;
    if (!*cell) return 0;
    char *end = NULL;
    strtod(cell, &end);
    if (end == cell) return 0;
    while (*end == ' ' || *end == '\t') end++;
    return *end == 0;
}

static int compare_table_sort_rows(const void *a, const void *b) {
    const char *keyA = ((const TableSortRow *)a)->key;
    const char *keyB = ((const TableSortRow *)b)->key;
    if (!keyA) keyA = "";
    if (!keyB) keyB = "";
    int result;
    if (g_tableSortNumeric) {
        double valueA = strtod(keyA, NULL), valueB = strtod(keyB, NULL);
        result = valueA < valueB ? -1 : (valueA > valueB ? 1 : 0);
    } else {
        result = StrCmpIA(keyA, keyB);
    }
    return g_tableSortDescending ? -result : result;
}

/* Reorders the data rows of the table the caret is in by the cell in the
   caret's column - numerically if every data row's cell there parses as a
   plain number, case-insensitively as text otherwise. The header and
   separator rows are left exactly where they are. */
static void sort_table_rows(int descending) {
    TableCaretInfo info;
    if (!find_table_caret_info(&info)) { report_not_in_table(); return; }
    Sci_Position dataRowCount = info.lastLine - info.firstLine - 1;
    if (dataRowCount < 2) return; /* nothing to reorder */

    TableSortRow *rows = (TableSortRow *)calloc((size_t)dataRowCount, sizeof(TableSortRow));
    if (!rows) return;
    int numeric = 1;
    for (Sci_Position i = 0; i < dataRowCount; i++) {
        rows[i].lineText = editor_line_text(info.firstLine + 2 + i, NULL);
        char *cells[128];
        int count = table_row_cells(rows[i].lineText ? rows[i].lineText : "", cells, 128);
        const char *cell = info.column < count && cells[info.column] ? cells[info.column] : "";
        rows[i].key = _strdup(cell);
        if (numeric && !table_cell_is_numeric(cell)) numeric = 0;
        free_row_cells(cells, count);
    }

    g_tableSortNumeric = numeric;
    g_tableSortDescending = descending;
    qsort(rows, (size_t)dataRowCount, sizeof(TableSortRow), compare_table_sort_rows);

    Sci_Position lineCount = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
    /* When the table is the last thing in the document, its last line may
       have no trailing EOL of its own - so the rebuilt text mustn't add one
       either, or every sort would tack on a newline the file never had. */
    int atDocumentEnd = info.lastLine + 1 >= lineCount;

    char *header = editor_line_text(info.firstLine, NULL);
    char *separator = editor_line_text(info.firstLine + 1, NULL);
    const char *eol = editor_eol_sequence();
    char *out = NULL; size_t outLength = 0, outCapacity = 0;
    int ok = append_dynamic(&out, &outLength, &outCapacity, header ? header : "") &&
             append_dynamic(&out, &outLength, &outCapacity, eol) &&
             append_dynamic(&out, &outLength, &outCapacity, separator ? separator : "") &&
             append_dynamic(&out, &outLength, &outCapacity, eol);
    for (Sci_Position i = 0; ok && i < dataRowCount; i++) {
        ok = append_dynamic(&out, &outLength, &outCapacity, rows[i].lineText ? rows[i].lineText : "");
        if (ok && !(atDocumentEnd && i == dataRowCount - 1))
            ok = append_dynamic(&out, &outLength, &outCapacity, eol);
    }
    free(header);
    free(separator);
    for (Sci_Position i = 0; i < dataRowCount; i++) { free(rows[i].lineText); free(rows[i].key); }
    free(rows);
    if (!ok || !out) { free(out); return; }

    Sci_Position replaceStart = (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)info.firstLine, 0);
    Sci_Position replaceEnd = atDocumentEnd
        ? (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0)
        : (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)(info.lastLine + 1), 0);
    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)replaceStart, 0);
    SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)replaceEnd, 0);
    SendMessageA(g_hEdit, SCI_REPLACETARGET, (WPARAM)outLength, (LPARAM)out);
    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
    free(out);
    SetFocus(g_hEdit);
}

/* Sets the GFM alignment marker (":---", ":---:", "---:", or plain "---")
   of the caret's column in the table's separator row, leaving every other
   column's existing alignment untouched. Re-running with the same marker is
   idempotent since the existing colons are replaced outright, not toggled. */
static void set_table_column_alignment(const char *marker) {
    TableCaretInfo info;
    if (!find_table_caret_info(&info)) { report_not_in_table(); return; }

    Sci_Position separatorLine = info.firstLine + 1;
    char *separator = editor_line_text(separatorLine, NULL);
    char *cells[128];
    int count = table_row_cells(separator ? separator : "", cells, 128);
    if (info.column < 0 || info.column >= count) {
        free_row_cells(cells, count);
        free(separator);
        report_not_in_table();
        return;
    }
    free(cells[info.column]);
    cells[info.column] = _strdup(marker);

    char *out = NULL; size_t outLength = 0, outCapacity = 0;
    int ok = append_dynamic(&out, &outLength, &outCapacity, "|");
    for (int c = 0; ok && c < count; c++) {
        const char *cell = cells[c] ? cells[c] : "---";
        ok = append_dynamic(&out, &outLength, &outCapacity, " ") &&
             append_dynamic(&out, &outLength, &outCapacity, cell) &&
             append_dynamic(&out, &outLength, &outCapacity, " |");
    }
    free_row_cells(cells, count);
    free(separator);
    if (!ok || !out) { free(out); return; }

    Sci_Position replaceStart = (Sci_Position)SendMessageA(g_hEdit, SCI_POSITIONFROMLINE, (WPARAM)separatorLine, 0);
    Sci_Position replaceEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINEENDPOSITION, (WPARAM)separatorLine, 0);
    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)replaceStart, 0);
    SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)replaceEnd, 0);
    SendMessageA(g_hEdit, SCI_REPLACETARGET, (WPARAM)outLength, (LPARAM)out);
    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
    free(out);
    SetFocus(g_hEdit);
}

/* ---- folder tools and command palette ---------------------------------- */

typedef struct WorkspaceFiles {
    WCHAR **paths;
    WCHAR **labels;
    int *lines;
    int count;
    int capacity;
    int truncated;
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

/* Reuses the same LANGUAGES table language_for_path() draws from, rather
   than maintaining a second, separate extension list here that would
   otherwise silently drift and hide files the app can actually open and
   highlight (.lua, .cs, .java, .go, .rs, .php, etc.) from the workspace
   tree/quick-open/find-in-folder. */
static int workspace_file_extension(const WCHAR *name) {
    const WCHAR *extension = PathFindExtensionW(name);
    if (!*extension) return 0;
    for (int i = 0; i < LANGUAGE_COUNT; i++)
        if (extension_in_list(extension, LANGUAGES[i].extensions)) return 1;
    return 0;
}

static void enumerate_workspace_files(WorkspaceFiles *files, const WCHAR *directory, int depth) {
    if (depth > 24 || files->count >= 5000) { files->truncated = 1; return; }
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
    if (files->count >= 5000) files->truncated = 1;
    FindClose(search);
}

static void warn_incomplete_workspace_scan(const WCHAR *title, int enumerationTruncated,
                                           int skippedLarge, int readFailed,
                                           int resultsTruncated) {
    if (!enumerationTruncated && !skippedLarge && !readFailed && !resultsTruncated) return;
    WCHAR message[640];
    int used = swprintf(message, sizeof(message) / sizeof(message[0]),
                        L"Some workspace content was not searched:");
    if (enumerationTruncated && used > 0)
        used += swprintf(message + used, sizeof(message) / sizeof(message[0]) - (size_t)used,
                         L"\n\u2022 The 5,000-file or 24-folder-depth limit was reached.");
    if (skippedLarge && used > 0)
        used += swprintf(message + used, sizeof(message) / sizeof(message[0]) - (size_t)used,
                         L"\n\u2022 %d file%ls larger than 4 MB %ls skipped.", skippedLarge,
                         skippedLarge == 1 ? L"" : L"s", skippedLarge == 1 ? L"was" : L"were");
    if (readFailed && used > 0)
        used += swprintf(message + used, sizeof(message) / sizeof(message[0]) - (size_t)used,
                         L"\n\u2022 %d file%ls could not be read.", readFailed,
                         readFailed == 1 ? L"" : L"s");
    if (resultsTruncated && used > 0)
        swprintf(message + used, sizeof(message) / sizeof(message[0]) - (size_t)used,
                 L"\n\u2022 Only the first 2,000 matches are shown.");
    MessageBoxW(g_hMainWnd, message, title, MB_OK | MB_ICONWARNING);
}

static int collect_workspace_files_at(WorkspaceFiles *files, const WCHAR *root) {
    memset(files, 0, sizeof(*files));
    files->root = wcsdup_heap(root);
    if (!files->root) return 0;
    enumerate_workspace_files(files, files->root, 0);
    return 1;
}

static int collect_workspace_files(WorkspaceFiles *files) {
    WCHAR *root = choose_workspace_folder();
    if (!root) { memset(files, 0, sizeof(*files)); return 0; }
    int ok = collect_workspace_files_at(files, root);
    free(root);
    return ok;
}

static void do_quick_open_in(const WCHAR *root) {
    WorkspaceFiles files;
    if (!collect_workspace_files_at(&files, root)) return;
    if (files.truncated)
        warn_incomplete_workspace_scan(L"Quick Open", 1, 0, 0, 0);
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

static void do_quick_open(void) {
    WCHAR *root = choose_workspace_folder();
    if (!root) return;
    do_quick_open_in(root);
    free(root);
}

/* Records `root` as the current workspace folder: persists it, moves it to
   the front of Recent Workspaces, refreshes the menu, and - if the explorer
   panel is open - points it at the new folder. Shared by every place that
   adopts a workspace folder so they can't drift out of sync with each other
   (do_open_recent_workspace's own "folder no longer exists" early-out is
   deliberately not part of this, since only that one caller needs it). */
static void adopt_workspace_folder(HWND hwnd, const WCHAR *root) {
    replace_wstring(&g_currentWorkspaceFolder, root);
    recent_list_add(&g_recentFolderList, root);
    save_workspace_settings();
    refresh_menu(hwnd);
    if (g_explorerVisible) explorer_set_root(g_currentWorkspaceFolder);
}

/* Opens a workspace folder and immediately lets the user fuzzy-pick a file
   from it, same as Quick Open - the folder is remembered so Recent
   Workspaces can return to it without browsing for it again. */
static void do_open_workspace(void) {
    WCHAR *root = choose_workspace_folder();
    if (!root) return;
    adopt_workspace_folder(g_hMainWnd, root);
    do_quick_open_in(root);
    free(root);
}

static void do_open_recent_workspace(int index) {
    if (index < 0 || index >= g_recentFolderCount) return;
    WCHAR *root = wcsdup_heap(g_recentFolders[index]);
    if (!root) return;
    DWORD attrs = GetFileAttributesW(root);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBoxW(g_hMainWnd, L"That folder could not be found.", L"MarkEdit",
                    MB_OK | MB_ICONINFORMATION);
        recent_list_remove(&g_recentFolderList, root);
        refresh_menu(g_hMainWnd);
        free(root);
        return;
    }
    adopt_workspace_folder(g_hMainWnd, root);
    do_quick_open_in(root);
    free(root);
}

/* Mirrors toggle_outline()/toggle_preview(): flip the visibility flag, then
   menu/layout/repaint/persist. Turning the panel on for the first time (no
   workspace chosen yet) reuses the same folder-picker Quick Open already
   falls back to, so there's no separate "no folder open" empty state to
   design for. */
static void toggle_explorer_panel(HWND hwnd) {
    g_explorerVisible = !g_explorerVisible;
    if (g_explorerVisible) {
        if (g_currentWorkspaceFolder) {
            explorer_set_root(g_currentWorkspaceFolder);
        } else {
            WCHAR *root = choose_workspace_folder();
            if (!root) { g_explorerVisible = 0; update_view_menu(hwnd); return; }
            adopt_workspace_folder(hwnd, root); /* g_explorerVisible is already true, so this also sets the root */
            free(root);
        }
    } else {
        explorer_stop_watch();
    }
    update_view_menu(hwnd);
    layout(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
    save_layout_settings();
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
    int skippedLarge = 0, readFailed = 0, resultsTruncated = 0;
    for (int i = 0; i < files.count && results.count < 2000; i++) {
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        if (!GetFileAttributesExW(files.paths[i], GetFileExInfoStandard, &attributes)) {
            readFailed++;
            continue;
        }
        if (attributes.nFileSizeHigh || attributes.nFileSizeLow > 4 * 1024 * 1024) {
            skippedLarge++;
            continue;
        }
        char *content = read_text_file(files.paths[i], NULL, NULL, NULL);
        if (!content) { readFailed++; continue; }
        char *cursor = content;
        int lineNumber = 1;
        while (*cursor && results.count < 2000) {
            size_t terminatorLength = 0;
            char *end = text_line_break(cursor, &terminatorLength);
            char saved = *end;
            *end = 0;
            if (utf8_case_find(cursor, query)) {
                char snippet[181];
                size_t snippetLength = strlen(cursor);
                if (snippetLength > 180) snippetLength = 180;
                while (snippetLength && ((unsigned char)cursor[snippetLength] & 0xC0) == 0x80)
                    snippetLength--;
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
            *end = saved;
            if (!terminatorLength) break;
            cursor = end + terminatorLength; lineNumber++;
        }
        if (results.count >= 2000) resultsTruncated = 1;
        free(content);
    }
    if (results.count >= 2000 && files.count > 0) resultsTruncated = 1;
    int enumerationTruncated = files.truncated;
    free(query);
    free_workspace_files(&files);
    warn_incomplete_workspace_scan(L"Find in Folder", enumerationTruncated, skippedLarge,
                                   readFailed, resultsTruncated);
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

/* Finds the open tab (if any) showing `path`, the same case-insensitive
   comparison open_file_in_tab() uses to detect an already-open document. */
static int find_open_document_for_path(const WCHAR *path) {
    for (int i = 0; i < g_documentCount; i++) {
        if (g_documents[i].path && _wcsicmp(g_documents[i].path, path) == 0) return i;
    }
    return -1;
}

/* Silently replaces a clean (non-dirty) document's buffer with freshly
   written content - used after a workspace replace changes a file that's
   open in a tab but has no unsaved edits to lose, so the tab reflects the
   new content instead of going stale. Swaps the shared editor's document
   pointer the same transient way get_document_text() does, so this works
   for a background tab without visibly switching to it. */
static void reload_document_buffer(int index, const char *content) {
    if (!g_hEdit || index < 0 || index >= g_documentCount) return;
    sptr_t current = (sptr_t)SendMessageA(g_hEdit, SCI_GETDOCPOINTER, 0, 0);
    int oldSwitching = g_switchingDocument;
    g_switchingDocument = 1;
    if (current != g_documents[index].sciDocument)
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_documents[index].sciDocument);
    set_edit_text(g_hEdit, content, (Sci_Position)strlen(content));
    SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    if (current != g_documents[index].sciDocument)
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)current);
    g_switchingDocument = oldSwitching;
    g_documents[index].dirty = 0;
    record_document_file_time(index);
    update_tab_label(index);
    update_document_mapping();
    if (index == g_activeDocument) {
        update_preview();
        update_title();
        update_outline();
    }
}

/* Returns a newly allocated copy of `line` with every case-insensitive
   occurrence of `find` replaced by `replace`, or NULL if `find` doesn't
   occur (or is empty - callers must not pass one, since StrStrIA would
   "find" it at every position and loop forever). */
static char *replace_all_in_line(const char *line, const char *find, const char *replace) {
    if (!find[0] || !utf8_case_find(line, find)) return NULL;
    size_t findLen = strlen(find), replaceLen = strlen(replace);
    size_t cap = strlen(line) + replaceLen + 64;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t outLen = 0;
    const char *cursor = line;
    const char *hit;
    while ((hit = utf8_case_find(cursor, find)) != NULL) {
        size_t prefixLen = (size_t)(hit - cursor);
        size_t needed = outLen + prefixLen + replaceLen + 1;
        if (needed > cap) {
            cap = needed * 2;
            char *grown = (char *)realloc(out, cap);
            if (!grown) { free(out); return NULL; }
            out = grown;
        }
        memcpy(out + outLen, cursor, prefixLen); outLen += prefixLen;
        memcpy(out + outLen, replace, replaceLen); outLen += replaceLen;
        cursor = hit + findLen;
    }
    size_t restLen = strlen(cursor);
    size_t needed = outLen + restLen + 1;
    if (needed > cap) {
        char *grown = (char *)realloc(out, needed);
        if (!grown) { free(out); return NULL; }
        out = grown;
    }
    memcpy(out + outLen, cursor, restLen); outLen += restLen;
    out[outLen] = 0;
    return out;
}

/* Applies the checked matches: one file at a time, replaces every
   occurrence of `find` with `replace` on each accepted line only (matching
   the line-level granularity the review checklist offers), then writes the
   result with write_file_atomic(). `matches` must be sorted by path, which
   the scan in do_replace_in_workspace() below already produces. */
static void apply_workspace_replace(ReplaceMatch *matches, int count, const char *find, const char *replace) {
    int filesChanged = 0;
    int filesFailed = 0;
    int filesSkipped = 0;
    int i = 0;
    while (i < count) {
        const WCHAR *path = matches[i].path;
        int fileEnd = i;
        while (fileEnd < count && _wcsicmp(matches[fileEnd].path, path) == 0) fileEnd++;

        int anyAccepted = 0;
        for (int j = i; j < fileEnd; j++) if (matches[j].accepted) { anyAccepted = 1; break; }
        if (!anyAccepted) { i = fileEnd; continue; }

        int openIndex = find_open_document_for_path(path);
        Sci_Position length = 0;
        char *content = NULL;
        int hasBom = 0, encoding = TEXT_ENCODING_UTF8;
        if (openIndex >= 0) {
            content = get_document_text(openIndex, &length);
            hasBom = g_documents[openIndex].hasBom;
            encoding = g_documents[openIndex].encoding;
        } else {
            int fileLength = 0;
            content = read_text_file(path, &fileLength, &encoding, &hasBom);
            length = fileLength;
        }
        if (!content) { filesFailed++; i = fileEnd; continue; }
        /* The line rebuild below walks NUL-terminated lines, so text with a
           NUL in it (possible in a UTF-16 file) would be written back cut
           short - leave such a file alone. */
        if (strlen(content) != (size_t)length) {
            filesSkipped++;
            free(content);
            i = fileEnd;
            continue;
        }
        char *body = content;

        size_t outCapacity = (size_t)length + 256;
        char *out = (char *)malloc(outCapacity);
        size_t outLength = 0;
        int changed = 0;
        if (out) {
            char *cursor = body;
            int lineNumber = 1;
            while (*cursor) {
                size_t terminatorLength = 0;
                char *end = text_line_break(cursor, &terminatorLength);
                char terminator[2] = {0, 0};
                if (terminatorLength) memcpy(terminator, end, terminatorLength);
                *end = 0;
                int accept = 0;
                for (int j = i; j < fileEnd; j++)
                    if (matches[j].line == lineNumber && matches[j].accepted) { accept = 1; break; }
                char *lineOut = accept ? replace_all_in_line(cursor, find, replace) : NULL;
                const char *piece = lineOut ? lineOut : cursor;
                size_t pieceLen = strlen(piece);
                if (lineOut) changed = 1;
                size_t needed = outLength + pieceLen + terminatorLength + 1;
                if (needed > outCapacity) {
                    outCapacity = needed * 2;
                    char *grown = (char *)realloc(out, outCapacity);
                    if (!grown) { free(lineOut); free(out); out = NULL; break; }
                    out = grown;
                }
                memcpy(out + outLength, piece, pieceLen); outLength += pieceLen;
                free(lineOut);
                if (!terminatorLength) break;
                memcpy(out + outLength, terminator, terminatorLength);
                outLength += terminatorLength;
                memcpy(end, terminator, terminatorLength);
                cursor = end + terminatorLength;
                lineNumber++;
            }
            if (out) out[outLength] = 0;
        }

        if (out && changed) {
            WCHAR *ioPath = extended_path(path);
            const WCHAR *target = ioPath ? ioPath : path;
            /* A replacement the file's encoding can't hold skips the file
               rather than silently becoming '?' or U+FFFD. */
            if (text_encoding_is_lossless(out, outLength, (TextEncoding)encoding) != 1) {
                filesSkipped++;
            } else if (write_file_atomic(target, out, outLength, (TextEncoding)encoding, hasBom)) {
                filesChanged++;
                /* `out` was built from the live editor buffer (including any
                   other unsaved edits), so it's exactly what was just written
                   to disk regardless of the tab's prior dirty state - syncing
                   the buffer to it (and clearing dirty) is correct either way.
                   The previous dirty-only branch instead left the buffer
                   holding the pre-replacement text, so the next Ctrl+S
                   silently overwrote this file again, undoing the replacement
                   that had just been written. */
                if (openIndex >= 0) reload_document_buffer(openIndex, out);
            } else {
                filesFailed++;
            }
            free(ioPath);
        }
        free(out);
        free(content);
        i = fileEnd;
    }
    WCHAR message[512];
    if (filesChanged == 0 && filesFailed == 0 && filesSkipped == 0) {
        swprintf(message, 512, L"No files were changed.");
    } else {
        /* Aggregated into one dialog rather than one per failed file, which
           otherwise forced dismissing a modal for every read-only/locked file
           in a row before a large batch replace could finish. */
        int used = swprintf(message, 512, L"Updated %d file%ls.", filesChanged, filesChanged == 1 ? L"" : L"s");
        if (filesFailed && used > 0)
            used += swprintf(message + used, 512 - (size_t)used,
                             L" %d file%ls could not be written and %ls skipped.",
                             filesFailed, filesFailed == 1 ? L"" : L"s", filesFailed == 1 ? L"was" : L"were");
        if (filesSkipped && used > 0)
            swprintf(message + used, 512 - (size_t)used,
                     L" %d file%ls left unchanged (a NUL character in the file, or a "
                     L"replacement its encoding can't store).",
                     filesSkipped, filesSkipped == 1 ? L" was" : L"s were");
    }
    MessageBoxW(g_hMainWnd, message, L"Replace in Folder",
               MB_OK | (filesFailed || filesSkipped ? MB_ICONWARNING : MB_ICONINFORMATION));
}

/* Same folder enumeration and line scan as do_find_in_folder(), but every
   match is collected up front (as a ReplaceMatch, sorted by path since the
   file loop produces them that way) and reviewed with checkboxes before
   anything is written - see show_replace_review_dialog(). */
static void do_replace_in_workspace(void) {
    WorkspaceFiles files;
    if (!collect_workspace_files(&files)) return;

    WCHAR findWide[256];
    if (!show_text_input(L"Replace in Folder", L"Text to find (case-insensitive):", L"",
                         findWide, sizeof(findWide) / sizeof(findWide[0])) || !findWide[0]) {
        free_workspace_files(&files); return;
    }
    WCHAR replaceWide[256];
    if (!show_text_input(L"Replace in Folder", L"Replace with:", L"",
                         replaceWide, sizeof(replaceWide) / sizeof(replaceWide[0]))) {
        free_workspace_files(&files); return;
    }
    char *find = to_utf8(findWide);
    char *replace = to_utf8(replaceWide);
    if (!find || !replace) { free(find); free(replace); free_workspace_files(&files); return; }

    ReplaceMatch *matches = NULL;
    int matchCount = 0, matchCapacity = 0;
    int skippedLarge = 0, readFailed = 0, resultsTruncated = 0;
    for (int i = 0; i < files.count && matchCount < 2000; i++) {
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        if (!GetFileAttributesExW(files.paths[i], GetFileExInfoStandard, &attributes)) {
            readFailed++;
            continue;
        }
        if (attributes.nFileSizeHigh || attributes.nFileSizeLow > 4 * 1024 * 1024) {
            skippedLarge++;
            continue;
        }
        char *content = read_text_file(files.paths[i], NULL, NULL, NULL);
        if (!content) { readFailed++; continue; }
        char *cursor = content;
        int lineNumber = 1;
        while (*cursor && matchCount < 2000) {
            size_t terminatorLength = 0;
            char *end = text_line_break(cursor, &terminatorLength);
            char saved = *end;
            *end = 0;
            if (utf8_case_find(cursor, find)) {
                char snippet[181];
                size_t snippetLength = strlen(cursor);
                if (snippetLength > 180) snippetLength = 180;
                while (snippetLength && ((unsigned char)cursor[snippetLength] & 0xC0) == 0x80)
                    snippetLength--;
                memcpy(snippet, cursor, snippetLength); snippet[snippetLength] = 0;
                while (snippetLength && (snippet[snippetLength - 1] == '\r' ||
                       snippet[snippetLength - 1] == '\n')) snippet[--snippetLength] = 0;
                WCHAR *wideSnippet = to_wide(snippet);
                size_t capacity = wcslen(files.labels[i]) + (wideSnippet ? wcslen(wideSnippet) : 0) + 48;
                WCHAR *label = (WCHAR *)malloc(capacity * sizeof(WCHAR));
                if (label) {
                    swprintf(label, capacity, L"%ls:%d — %ls", files.labels[i], lineNumber,
                             wideSnippet ? wideSnippet : L"");
                    if (matchCount >= matchCapacity) {
                        int next = matchCapacity ? matchCapacity * 2 : 128;
                        ReplaceMatch *grown = (ReplaceMatch *)realloc(matches, (size_t)next * sizeof(ReplaceMatch));
                        if (grown) { matches = grown; matchCapacity = next; }
                    }
                    if (matchCount < matchCapacity) {
                        matches[matchCount].path = wcsdup_heap(files.paths[i]);
                        matches[matchCount].label = label;
                        matches[matchCount].line = lineNumber;
                        matches[matchCount].accepted = 1;
                        matchCount++;
                        label = NULL;
                    }
                }
                free(label);
                free(wideSnippet);
            }
            *end = saved;
            if (!terminatorLength) break;
            cursor = end + terminatorLength; lineNumber++;
        }
        if (matchCount >= 2000) resultsTruncated = 1;
        free(content);
    }
    if (matchCount >= 2000 && files.count > 0) resultsTruncated = 1;
    int enumerationTruncated = files.truncated;
    free_workspace_files(&files);
    warn_incomplete_workspace_scan(L"Replace in Folder", enumerationTruncated, skippedLarge,
                                   readFailed, resultsTruncated);

    if (matchCount == 0) {
        MessageBoxW(g_hMainWnd, L"No matches were found in that folder.", L"Replace in Folder",
                    MB_OK | MB_ICONINFORMATION);
        free(find); free(replace);
        return;
    }

    if (show_replace_review_dialog(matches, matchCount))
        apply_workspace_replace(matches, matchCount, find, replace);

    for (int i = 0; i < matchCount; i++) { free(matches[i].path); free(matches[i].label); }
    free(matches);
    free(find);
    free(replace);
}

static int path_at_or_below(const WCHAR *path, const WCHAR *parent) {
    size_t length = path && parent ? wcslen(parent) : 0;
    if (!length || _wcsnicmp(path, parent, length) != 0) return 0;
    return path[length] == 0 || path[length] == L'\\' || path[length] == L'/';
}

static char *relative_markdown_target(const WCHAR *sourcePath, const WCHAR *targetPath) {
    WCHAR *directory = path_directory(sourcePath);
    WCHAR relative[MAX_PATH];
    int ok = directory && PathRelativePathToW(relative, directory, FILE_ATTRIBUTE_DIRECTORY,
                                               targetPath, FILE_ATTRIBUTE_NORMAL);
    free(directory);
    if (!ok) return NULL;
    WCHAR *start = relative;
    if (start[0] == L'.' && (start[1] == L'\\' || start[1] == L'/')) start += 2;
    for (WCHAR *character = start; *character; character++)
        if (*character == L'\\') *character = L'/';
    return to_utf8(start);
}

static char *rewrite_links_for_source(const char *text, const WCHAR *sourcePath,
                                      const WCHAR *oldPath, const WCHAR *newPath,
                                      int *changes) {
    *changes = 0;
    char *oldTarget = relative_markdown_target(sourcePath, oldPath);
    char *newTarget = relative_markdown_target(sourcePath, newPath);
    if (!oldTarget || !newTarget) {
        free(oldTarget); free(newTarget);
        *changes = -1;
        return NULL;
    }
    char *rewritten = markdown_rewrite_link_targets(text, oldTarget, newTarget, changes);
    size_t oldLength = strlen(oldTarget), newLength = strlen(newTarget);
    if (oldTarget[0] != '.' && newTarget[0] != '.' &&
        oldLength <= SIZE_MAX - 3 && newLength <= SIZE_MAX - 3) {
        char *dotOld = (char *)malloc(oldLength + 3);
        char *dotNew = (char *)malloc(newLength + 3);
        if (dotOld && dotNew) {
            snprintf(dotOld, oldLength + 3, "./%s", oldTarget);
            snprintf(dotNew, newLength + 3, "./%s", newTarget);
            int dotChanges = 0;
            char *withDots = markdown_rewrite_link_targets(rewritten ? rewritten : text,
                                                            dotOld, dotNew, &dotChanges);
            if (withDots) { free(rewritten); rewritten = withDots; }
            *changes += dotChanges;
        }
        free(dotOld); free(dotNew);
    }
    free(oldTarget); free(newTarget);
    return rewritten;
}

static void replace_open_document_text(int index, const char *text) {
    if (index < 0 || index >= g_documentCount || !text) return;
    sptr_t current = (sptr_t)SendMessageA(g_hEdit, SCI_GETDOCPOINTER, 0, 0);
    Sci_Position caret = (Sci_Position)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    Sci_Position anchor = (Sci_Position)SendMessageA(g_hEdit, SCI_GETANCHOR, 0, 0);
    Sci_Position first = (Sci_Position)SendMessageA(g_hEdit, SCI_GETFIRSTVISIBLELINE, 0, 0);
    int xOffset = (int)SendMessageA(g_hEdit, SCI_GETXOFFSET, 0, 0);
    int oldSwitching = g_switchingDocument;
    g_switchingDocument = 1;
    if (current != g_documents[index].sciDocument)
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_documents[index].sciDocument);
    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(g_hEdit, SCI_TARGETWHOLEDOCUMENT, 0, 0);
    SendMessageA(g_hEdit, SCI_REPLACETARGET, (WPARAM)-1, (LPARAM)text);
    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
    if (current != g_documents[index].sciDocument)
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)current);
    Sci_Position length = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    if (caret > length) caret = length;
    if (anchor > length) anchor = length;
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)anchor, (LPARAM)caret);
    SendMessageA(g_hEdit, SCI_SETFIRSTVISIBLELINE, (WPARAM)first, 0);
    SendMessageA(g_hEdit, SCI_SETXOFFSET, (WPARAM)xOffset, 0);
    g_switchingDocument = oldSwitching;
    g_documents[index].dirty = 1;
    g_recoverySnapshotStale = 1;
    update_tab_label(index);
}

static void update_workspace_links_after_rename(const WCHAR *oldPath, const WCHAR *newPath) {
    if (!g_currentWorkspaceFolder) return;
    WorkspaceFiles files;
    if (!collect_workspace_files_at(&files, g_currentWorkspaceFolder)) return;
    int updatedFiles = 0, failedFiles = 0;
    for (int i = 0; i < files.count; i++) {
        if (language_for_path(files.paths[i]) != LANG_MARKDOWN) continue;
        int openIndex = find_open_document_for_path(files.paths[i]);
        Sci_Position openLength = 0;
        int fileLength = 0, encoding = TEXT_ENCODING_UTF8, hasBom = 0;
        char *text = openIndex >= 0
            ? get_document_text(openIndex, &openLength)
            : read_text_file(files.paths[i], &fileLength, &encoding, &hasBom);
        if (!text) { failedFiles++; continue; }
        size_t sourceLength = openIndex >= 0 ? (size_t)openLength : (size_t)fileLength;
        if (strlen(text) != sourceLength) { free(text); failedFiles++; continue; }
        int changes = 0;
        char *rewritten = rewrite_links_for_source(text, files.paths[i], oldPath, newPath, &changes);
        free(text);
        if (changes < 0) { free(rewritten); failedFiles++; continue; }
        if (!rewritten || !changes) { free(rewritten); continue; }
        if (openIndex >= 0) {
            replace_open_document_text(openIndex, rewritten);
            updatedFiles++;
        } else {
            size_t length = strlen(rewritten);
            WCHAR *ioPath = extended_path(files.paths[i]);
            const WCHAR *target = ioPath ? ioPath : files.paths[i];
            if (text_encoding_is_lossless(rewritten, length, (TextEncoding)encoding) == 1 &&
                write_file_atomic(target, rewritten, length, (TextEncoding)encoding, hasBom))
                updatedFiles++;
            else
                failedFiles++;
            free(ioPath);
        }
        free(rewritten);
    }
    int truncated = files.truncated;
    free_workspace_files(&files);
    if (failedFiles || truncated) {
        WCHAR message[320];
        if (failedFiles)
            swprintf(message, sizeof(message) / sizeof(message[0]),
                     L"The file was renamed and %d Markdown file%ls %ls updated, but %d file%ls could not be updated%ls.",
                     updatedFiles, updatedFiles == 1 ? L"" : L"s", updatedFiles == 1 ? L"was" : L"were",
                     failedFiles, failedFiles == 1 ? L"" : L"s",
                     truncated ? L" and the workspace scan reached its limit" : L"");
        else
            swprintf(message, sizeof(message) / sizeof(message[0]),
                     L"The file was renamed and %d Markdown file%ls %ls updated, but the workspace scan reached its limit.",
                     updatedFiles, updatedFiles == 1 ? L"" : L"s",
                     updatedFiles == 1 ? L"was" : L"were");
        MessageBoxW(g_hMainWnd, message, L"Rename", MB_OK | MB_ICONWARNING);
    }
}

static void explorer_path_renamed(const WCHAR *oldPath, const WCHAR *newPath, int isDirectory) {
    if (!oldPath || !newPath) return;
    for (int i = 0; i < g_documentCount; i++) {
        if (!g_documents[i].path ||
            !(isDirectory ? path_at_or_below(g_documents[i].path, oldPath)
                           : _wcsicmp(g_documents[i].path, oldPath) == 0)) continue;
        const WCHAR *suffix = isDirectory ? g_documents[i].path + wcslen(oldPath) : L"";
        WCHAR *retargeted = (WCHAR *)malloc((wcslen(newPath) + wcslen(suffix) + 1) * sizeof(WCHAR));
        if (!retargeted) continue;
        wcscpy(retargeted, newPath);
        wcscat(retargeted, suffix);
        recent_list_remove(&g_recentFileList, g_documents[i].path);
        replace_wstring(&g_documents[i].path, retargeted);
        recent_list_add(&g_recentFileList, retargeted);
        record_document_file_time(i);
        update_tab_label(i);
        free(retargeted);
    }
    if (!isDirectory) update_workspace_links_after_rename(oldPath, newPath);
    apply_document_language(g_hEdit, g_currentFile);
    if (g_hSplitEdit) apply_document_language(g_hSplitEdit, g_currentFile);
    update_document_mapping();
    update_title();
    update_preview();
    update_outline();
    refresh_menu(g_hMainWnd);
    g_recoverySnapshotStale = 1;
}

typedef struct PaletteCommand { const WCHAR *label; WORD command; } PaletteCommand;
static const PaletteCommand PALETTE_COMMANDS[] = {
    {L"File: New", IDM_FILE_NEW}, {L"File: Open", IDM_FILE_OPEN},
    {L"File: Insert File at Cursor", IDM_FILE_INSERT},
    {L"File: Quick Open in Folder", IDM_FILE_QUICK_OPEN},
    {L"File: Find in Folder", IDM_FILE_FIND_FOLDER}, {L"File: Replace in Folder", IDM_REPLACE_WORKSPACE},
    {L"File: Open Workspace Folder", IDM_OPEN_WORKSPACE},
    {L"Tools: Settings", IDM_SETTINGS},
    {L"File: Save", IDM_FILE_SAVE},
    {L"File: Save As", IDM_FILE_SAVEAS}, {L"File: Save All", IDM_FILE_SAVE_ALL},
    {L"File: Export Rendered HTML", IDM_FILE_EXPORT_HTML},
    {L"File: Export Rendered PDF", IDM_FILE_EXPORT_PDF}, {L"File: Print Rendered Preview", IDM_FILE_PRINT},
    {L"File: Open Terminal Here", IDM_FILE_OPEN_TERMINAL}, {L"File: Show in Explorer", IDM_FILE_SHOW_EXPLORER},
    {L"File: Close Tab", IDM_FILE_CLOSE},
    {L"File: Reopen Closed Tab", IDM_FILE_REOPEN_CLOSED}, {L"File: Go Back", IDM_FILE_GO_BACK},
    {L"Edit: Undo", IDM_EDIT_UNDO},
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
    {L"Edit: Convert Encoding to UTF-8", IDM_EDIT_ENCODING_UTF8},
    {L"Edit: Convert Encoding to UTF-8 with BOM", IDM_EDIT_ENCODING_UTF8_BOM},
    {L"Edit: Convert Encoding to UTF-16 LE", IDM_EDIT_ENCODING_UTF16LE},
    {L"Edit: Convert Encoding to UTF-16 BE", IDM_EDIT_ENCODING_UTF16BE},
    {L"Edit: Convert Encoding to ANSI", IDM_EDIT_ENCODING_ANSI},
    {L"File: Reopen with Encoding UTF-8", IDM_FILE_REOPEN_UTF8},
    {L"File: Reopen with Encoding UTF-16 LE", IDM_FILE_REOPEN_UTF16LE},
    {L"File: Reopen with Encoding UTF-16 BE", IDM_FILE_REOPEN_UTF16BE},
    {L"File: Reopen with Encoding ANSI", IDM_FILE_REOPEN_ANSI},
    {L"Edit: Convert Line Endings to CRLF", IDM_EDIT_EOL_CRLF},
    {L"Edit: Convert Line Endings to LF", IDM_EDIT_EOL_LF},
    {L"Edit: Convert Line Endings to CR", IDM_EDIT_EOL_CR},
    {L"View: Font", IDM_FORMAT_FONT},
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
    {L"Format: Insert Table Row Above", IDM_FMT_TABLE_ROW_ABOVE},
    {L"Format: Insert Table Row Below", IDM_FMT_TABLE_ROW_BELOW},
    {L"Format: Delete Table Row", IDM_FMT_TABLE_ROW_DELETE},
    {L"Format: Insert Table Column Left", IDM_FMT_TABLE_COL_LEFT},
    {L"Format: Insert Table Column Right", IDM_FMT_TABLE_COL_RIGHT},
    {L"Format: Delete Table Column", IDM_FMT_TABLE_COL_DELETE},
    {L"Format: Sort Table Rows Ascending", IDM_TABLE_SORT_ASC},
    {L"Format: Sort Table Rows Descending", IDM_TABLE_SORT_DESC},
    {L"Format: Align Table Column Left", IDM_TABLE_ALIGN_LEFT},
    {L"Format: Align Table Column Center", IDM_TABLE_ALIGN_CENTER},
    {L"Format: Align Table Column Right", IDM_TABLE_ALIGN_RIGHT},
    {L"Format: Format JSON", IDM_FMT_JSON_PRETTY},
    {L"Format: Minify JSON", IDM_FMT_JSON_MINIFY},
    {L"Format: HTML", IDM_FMT_HTML_HTML}, {L"Format: HTML Head", IDM_FMT_HTML_HEAD},
    {L"Format: HTML Body", IDM_FMT_HTML_BODY}, {L"Format: HTML Paragraph", IDM_FMT_HTML_PARAGRAPH},
    {L"Format: HTML Bold", IDM_FMT_HTML_BOLD}, {L"Format: HTML Italic", IDM_FMT_HTML_ITALIC},
    {L"Format: HTML Underline", IDM_FMT_HTML_UNDERLINE}, {L"Format: HTML Div", IDM_FMT_HTML_DIV},
    {L"Format: HTML Span", IDM_FMT_HTML_SPAN}, {L"Format: HTML Line Break", IDM_FMT_HTML_BREAK},
    {L"Format: HTML Link", IDM_FMT_HTML_LINK}, {L"Format: HTML Image", IDM_FMT_HTML_IMAGE},
    {L"Format: HTML Table", IDM_FMT_HTML_TABLE},
    {L"Format: HTML Bullet List", IDM_FMT_HTML_BULLETLIST},
    {L"Format: HTML Numbered List", IDM_FMT_HTML_NUMBERLIST},
    {L"Run: Run File", IDM_RUN_FILE}, {L"Run: Debug File", IDM_DEBUG_FILE},
    {L"Run: Continue", IDM_DEBUG_CONTINUE}, {L"Run: Step Over", IDM_DEBUG_STEP_OVER},
    {L"Run: Step Into", IDM_DEBUG_STEP_INTO},
    {L"Run: Step Out", IDM_DEBUG_STEP_OUT},
    {L"Run: Toggle Breakpoint", IDM_DEBUG_BREAKPOINT},
    {L"Run: Clear All Breakpoints", IDM_DEBUG_CLEAR_BREAKPOINTS},
    {L"View: Toggle Preview", IDM_VIEW_TOGGLEPREVIEW}, {L"View: Toggle Editor", IDM_VIEW_TOGGLEEDITOR},
    {L"View: Toggle Outline", IDM_VIEW_OUTLINE}, {L"View: Toggle Minimap", IDM_VIEW_MINIMAP},
    {L"View: Toggle File Explorer", IDM_VIEW_EXPLORER},
    {L"View: Toggle Split Editor", IDM_VIEW_TOGGLESPLIT},
    {L"View: Reset Split Editor", IDM_VIEW_RESET_SPLIT_EDITOR},
    {L"View: Focus Mode", IDM_VIEW_FOCUS_MODE},
    {L"View: Next Tab", IDM_VIEW_NEXT_TAB}, {L"View: Previous Tab", IDM_VIEW_PREVIOUS_TAB},
    {L"View: Cycle Panes", IDM_VIEW_CYCLE_PANE},
    {L"View: Preview on Left", IDM_VIEW_PREVIEW_ON_LEFT},
    {L"View: Word Wrap", IDM_VIEW_WORD_WRAP}, {L"View: Spell Check", IDM_VIEW_SPELL_CHECK},
    {L"View: Spell Check Language", IDM_VIEW_SPELL_LANGUAGE}, {L"View: Show Whitespace", IDM_VIEW_SHOW_WHITESPACE},
    {L"View: Indent Guides", IDM_VIEW_INDENT_GUIDES},
    {L"View: Use Spaces for Indentation", IDM_VIEW_USE_SPACES_INDENT}, {L"View: Zoom In", IDM_VIEW_ZOOM_IN},
    {L"View: Zoom Out", IDM_VIEW_ZOOM_OUT}, {L"View: Reset Zoom", IDM_VIEW_ZOOM_RESET},
    {L"View: Editor Theme: GitHub Light", IDM_VIEW_THEME_GITHUB_LIGHT},
    {L"View: Editor Theme: GitHub Dark", IDM_VIEW_THEME_GITHUB_DARK},
    {L"View: Markdown Theme: GitHub Light", IDM_VIEW_PREVIEW_THEME_LIGHT},
    {L"View: Markdown Theme: GitHub Dark", IDM_VIEW_PREVIEW_THEME_DARK},
    {L"View: Custom Preview CSS", IDM_VIEW_CUSTOM_CSS},
    {L"View: Clear Custom Preview CSS", IDM_VIEW_CLEAR_CUSTOM_CSS}, {L"View: Toggle Fold", IDM_VIEW_FOLD_TOGGLE},
    {L"View: Fold All", IDM_VIEW_FOLD_ALL}, {L"View: Unfold All", IDM_VIEW_UNFOLD_ALL},
    {L"Help: Markdown Reference", IDM_HELP_REFERENCE},
    {L"Help: Keyboard Shortcuts", IDM_HELP_SHORTCUTS},
    {L"Help: About MarkEdit", IDM_HELP_ABOUT}
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

/* g_splitterPos is a width relative to the content area to the right of the
   explorer panel, not an absolute client x - g_splitterRect (set by layout(),
   already shifted by however much the explorer panel currently occupies) is
   what's actually drawn, so hit-testing against it (rather than recomputing
   from g_splitterPos, which used to be equivalent back when there was no
   panel to shift past) keeps this in sync with what the user sees. */
static int splitter_contains_x(int x) {
    return g_splitterRect.right > g_splitterRect.left &&
           x >= g_splitterRect.left && x <= g_splitterRect.right;
}

static int explorer_splitter_contains_x(int x) {
    return g_explorerVisible && !g_focusMode &&
           x >= g_explorerSplitterRect.left && x <= g_explorerSplitterRect.right;
}

/* Unlike splitter_contains_x() above (which only needs an x-range, since
   that splitter spans the full client height), the split-editor splitter
   is nested inside the editor region and needs both x and y bounds. */
static int split_editor_splitter_contains(int x, int y) {
    return g_hSplitEdit && g_splitEditorVisible &&
           x >= g_splitEditorSplitterRect.left && x <= g_splitEditorSplitterRect.right &&
           y >= g_splitEditorSplitterRect.top && y <= g_splitEditorSplitterRect.bottom;
}

/* Applies g_previewDarkMode to the rendered Markdown preview only, leaving
   the editor and app chrome (select_theme(), below) untouched. Used both as
   the Markdown Theme control's own handler and by select_theme() itself to
   keep the preview in step with the main light/dark toggle. */
static void select_preview_theme(HWND hwnd, int darkMode) {
    g_previewDarkMode = darkMode;
    save_theme_settings();
    apply_preview_theme();
    apply_webview_background_colour();
    apply_preview_host_theme();
    refresh_menu(hwnd);
}

/* Applies darkMode to the editor and all the surrounding app chrome (menus,
   dialogs, tabs, toolbar, title bar), then syncs the rendered Markdown
   preview to match - this is the main light/dark toggle (View > Editor
   Theme, Settings > Appearance). The separate Markdown Theme control
   (select_preview_theme(), above) can still be used afterwards to override
   just the preview on its own. */
static void select_theme(HWND hwnd, int darkMode) {
    g_editorDarkMode = darkMode;
    save_theme_settings();
    apply_menu_theme(hwnd, darkMode);
    apply_editor_theme(g_hEdit);
    if (g_hMinimap) {
        apply_editor_theme(g_hMinimap);
        strip_minimap_bold();
        SendMessageA(g_hMinimap, SCI_SETZOOM, 0, 0);
    }
    if (g_hSplitEdit) apply_editor_theme(g_hSplitEdit);
    apply_main_theme(hwnd);
    for (int i = 0; i < TOOLBAR_BUTTON_COUNT; i++) {
        if (g_hToolbarButtons[i]) InvalidateRect(g_hToolbarButtons[i], NULL, TRUE);
    }
    apply_toolbar_tooltip_theme();
    if (g_hTabs) InvalidateRect(g_hTabs, NULL, TRUE);
    if (g_hStatus) InvalidateRect(g_hStatus, NULL, TRUE);
    select_preview_theme(hwnd, darkMode);
}

static int g_highContrastActive = 0;

/* Detects whether Windows' high-contrast accessibility mode is on and, if
   it just changed, picks whichever of the app's own two themes better
   matches the system's window background - high contrast is a distinct
   system setting from this app's own light/dark toggle, and this app has no
   dedicated third palette for it, so the closest pragmatic response is to
   follow the OS's own light/dark leaning rather than build a whole separate
   rendering path. Wired to WM_SYSCOLORCHANGE and the "ImmersiveColorSet"
   WM_SETTINGCHANGE notification, the two ways Windows announces a system
   theme/contrast change, plus once at startup. Turning high contrast back
   off leaves whichever theme was already selected alone. */
static void detect_high_contrast(void) {
    HIGHCONTRASTW hc = {0};
    hc.cbSize = sizeof(hc);
    int active = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
                 (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    if (active == g_highContrastActive) return;
    g_highContrastActive = active;
    if (!active || !g_hMainWnd) return;
    COLORREF windowColor = GetSysColor(COLOR_WINDOW);
    int luminance = (GetRValue(windowColor) * 299 + GetGValue(windowColor) * 587 +
                     GetBValue(windowColor) * 114) / 1000;
    select_theme(g_hMainWnd, luminance < 128);
}

/* ---- Settings dialog ----------------------------------------------------
   A single window over six categories (left-hand list, one panel of
   controls shown at a time) that just launches or toggles the same actions
   already reachable from the View menu one at a time - do_choose_font(),
   select_theme(), toggle_word_wrap(), etc. Every control applies and
   persists immediately via whichever of those it calls, the same as
   clicking the equivalent menu item would, so there's nothing to commit or
   revert: Close (and Esc) just dismiss the window. */
typedef struct SettingsDialogState {
    HWND categoryList;
    int activeCategory;
    HWND themeLightRadio, themeDarkRadio;
    HWND fontLabel, fontChooseButton;
    HWND wordWrapCheck;
    HWND previewOnLeftCheck, customCssCaption, customCssLabel, customCssChooseButton, customCssClearButton;
    HWND previewThemeCaption, previewThemeLightRadio, previewThemeDarkRadio;
    HWND spellCheckCheck, showWhitespaceCheck, indentGuidesCheck, minimapCheck;
    HWND indentUseSpacesCheck, tabWidthCaption, tabWidthEdit;
    HWND spellLanguageCaption, spellLanguageLabel, spellLanguageChooseButton;
    HWND compilerPathsHintCaption;
    HWND vcvarsallCaption, vcvarsallEdit, vcvarsallBrowseButton;
    HWND gccCaption, gccEdit, gccBrowseButton;
    HWND clangCaption, clangEdit, clangBrowseButton;
    HWND zigCaption, zigEdit, zigBrowseButton;
    HWND luaCaption, luaEdit, luaBrowseButton;
    HWND gdbCaption, gdbEdit, gdbBrowseButton;
    HWND compilerFlagsCaption, compilerFlagsEdit;
} SettingsDialogState;

static void update_settings_font_label(SettingsDialogState *state) {
    WCHAR text[MAX_FONT_FACE_NAME + 32];
    swprintf(text, sizeof(text) / sizeof(text[0]), L"%ls, %dpt", g_fontFaceName, g_fontPointSize);
    SetWindowTextW(state->fontLabel, text);
}

static void update_settings_css_label(SettingsDialogState *state) {
    SetWindowTextW(state->customCssLabel, g_customCssPath && g_customCssPath[0] ? g_customCssPath : L"(none)");
    EnableWindow(state->customCssClearButton, g_customCssPath && g_customCssPath[0]);
}

/* Mirrors choose_spell_language()'s own label building (LOCALE_SLOCALIZEDDISPLAYNAME
   over the raw BCP-47 tag) so the Editor category shows the same name the
   language picker itself lists it under. */
static void update_settings_spell_label(SettingsDialogState *state) {
    WCHAR displayName[LOCALE_NAME_MAX_LENGTH] = L"";
    if (g_spellLanguage[0])
        GetLocaleInfoEx(g_spellLanguage, LOCALE_SLOCALIZEDDISPLAYNAME, displayName, LOCALE_NAME_MAX_LENGTH);
    SetWindowTextW(state->spellLanguageLabel, displayName[0] ? displayName : g_spellLanguage);
}

static int is_flag_edit_whitespace(WCHAR ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

/* Applied on EN_KILLFOCUS from any of the Compiler category's edit boxes
   (the extra-flags box and the GCC/Clang/Zig/Lua/MSVC path overrides alike),
   the same as set_tab_width() is for the Editor category's - trims
   surrounding whitespace, persists via save_compiler_settings(), and is a
   no-op when the trimmed text hasn't actually changed so tabbing through a
   field without editing it doesn't touch the ini file. For the flags box,
   quote_extra_compiler_flags() (see launch_native_file()/launch_msvc_file())
   re-splits the saved text into individual flags at build time. */
static void set_compiler_string_setting(WCHAR **slot, const WCHAR *text) {
    size_t start = 0, end = text ? wcslen(text) : 0;
    while (start < end && is_flag_edit_whitespace(text[start])) start++;
    while (end > start && is_flag_edit_whitespace(text[end - 1])) end--;
    WCHAR *value = NULL;
    if (end > start) {
        value = (WCHAR *)malloc((end - start + 1) * sizeof(WCHAR));
        if (value) {
            for (size_t i = 0; i < end - start; i++) value[i] = text[start + i];
            value[end - start] = 0;
        }
    }
    if (value && *slot && wcscmp(value, *slot) == 0) {
        free(value);
        return;
    }
    if (!value && !*slot) return;
    free(*slot);
    *slot = value;
    save_compiler_settings();
}

/* The MSVC path box's Browse button: unlike the GCC/Clang/Zig/Lua boxes
   below, an override here names one specific file (vcvarsall.bat) rather
   than a folder to search, so this picks a file the same way
   do_choose_custom_css() does instead of reusing choose_workspace_folder(). */
static void choose_vcvarsall_path(HWND editControl) {
    DWORD cap = 32768;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    if (g_vcvarsallOverridePath) wcsncpy(path, g_vcvarsallOverridePath, cap - 1);
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"vcvarsall.bat\0vcvarsall.bat\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) {
        replace_wstring(&g_vcvarsallOverridePath, path);
        save_compiler_settings();
        SetWindowTextW(editControl, g_vcvarsallOverridePath);
    }
    free(path);
}

/* Shared by the GCC/Clang/Zig/Lua Browse buttons: each of those overrides
   names a folder to search for that toolchain's executable(s) instead of
   PATH (see find_native_compiler()/find_lua_interpreter()), so picking one
   is just choose_workspace_folder()'s existing folder picker reused for a
   different setting. */
static void choose_compiler_folder(WCHAR **slot, HWND editControl) {
    WCHAR *folder = choose_workspace_folder();
    if (!folder) return;
    replace_wstring(slot, folder);
    free(folder);
    save_compiler_settings();
    SetWindowTextW(editControl, *slot);
}

static void update_settings_category(SettingsDialogState *state) {
    HWND all[] = {
        state->themeLightRadio, state->themeDarkRadio,
        state->fontLabel, state->fontChooseButton,
        state->wordWrapCheck,
        state->previewOnLeftCheck, state->customCssCaption, state->customCssLabel,
        state->customCssChooseButton, state->customCssClearButton,
        state->previewThemeCaption, state->previewThemeLightRadio, state->previewThemeDarkRadio,
        state->spellCheckCheck, state->showWhitespaceCheck, state->indentGuidesCheck, state->minimapCheck,
        state->indentUseSpacesCheck, state->tabWidthCaption, state->tabWidthEdit,
        state->spellLanguageCaption, state->spellLanguageLabel, state->spellLanguageChooseButton,
        state->compilerPathsHintCaption,
        state->vcvarsallCaption, state->vcvarsallEdit, state->vcvarsallBrowseButton,
        state->gccCaption, state->gccEdit, state->gccBrowseButton,
        state->clangCaption, state->clangEdit, state->clangBrowseButton,
        state->zigCaption, state->zigEdit, state->zigBrowseButton,
        state->luaCaption, state->luaEdit, state->luaBrowseButton,
        state->gdbCaption, state->gdbEdit, state->gdbBrowseButton,
        state->compilerFlagsCaption, state->compilerFlagsEdit,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        if (all[i]) ShowWindow(all[i], SW_HIDE);
    switch (state->activeCategory) {
        case 0:
            ShowWindow(state->themeLightRadio, SW_SHOW);
            ShowWindow(state->themeDarkRadio, SW_SHOW);
            break;
        case 1:
            ShowWindow(state->fontLabel, SW_SHOW);
            ShowWindow(state->fontChooseButton, SW_SHOW);
            break;
        case 2:
            ShowWindow(state->wordWrapCheck, SW_SHOW);
            break;
        case 3:
            ShowWindow(state->previewOnLeftCheck, SW_SHOW);
            ShowWindow(state->customCssCaption, SW_SHOW);
            ShowWindow(state->customCssLabel, SW_SHOW);
            ShowWindow(state->customCssChooseButton, SW_SHOW);
            ShowWindow(state->customCssClearButton, SW_SHOW);
            ShowWindow(state->previewThemeCaption, SW_SHOW);
            ShowWindow(state->previewThemeLightRadio, SW_SHOW);
            ShowWindow(state->previewThemeDarkRadio, SW_SHOW);
            break;
        case 4:
            ShowWindow(state->spellCheckCheck, SW_SHOW);
            ShowWindow(state->showWhitespaceCheck, SW_SHOW);
            ShowWindow(state->indentGuidesCheck, SW_SHOW);
            ShowWindow(state->minimapCheck, SW_SHOW);
            ShowWindow(state->indentUseSpacesCheck, SW_SHOW);
            ShowWindow(state->tabWidthCaption, SW_SHOW);
            ShowWindow(state->tabWidthEdit, SW_SHOW);
            ShowWindow(state->spellLanguageCaption, SW_SHOW);
            ShowWindow(state->spellLanguageLabel, SW_SHOW);
            ShowWindow(state->spellLanguageChooseButton, SW_SHOW);
            break;
        case 5:
            ShowWindow(state->compilerPathsHintCaption, SW_SHOW);
            ShowWindow(state->vcvarsallCaption, SW_SHOW);
            ShowWindow(state->vcvarsallEdit, SW_SHOW);
            ShowWindow(state->vcvarsallBrowseButton, SW_SHOW);
            ShowWindow(state->gccCaption, SW_SHOW);
            ShowWindow(state->gccEdit, SW_SHOW);
            ShowWindow(state->gccBrowseButton, SW_SHOW);
            ShowWindow(state->clangCaption, SW_SHOW);
            ShowWindow(state->clangEdit, SW_SHOW);
            ShowWindow(state->clangBrowseButton, SW_SHOW);
            ShowWindow(state->zigCaption, SW_SHOW);
            ShowWindow(state->zigEdit, SW_SHOW);
            ShowWindow(state->zigBrowseButton, SW_SHOW);
            ShowWindow(state->luaCaption, SW_SHOW);
            ShowWindow(state->luaEdit, SW_SHOW);
            ShowWindow(state->luaBrowseButton, SW_SHOW);
            ShowWindow(state->gdbCaption, SW_SHOW);
            ShowWindow(state->gdbEdit, SW_SHOW);
            ShowWindow(state->gdbBrowseButton, SW_SHOW);
            ShowWindow(state->compilerFlagsCaption, SW_SHOW);
            ShowWindow(state->compilerFlagsEdit, SW_SHOW);
            break;
    }
}

/* The shared g_popupBkBrush is normally created once when a popup dialog
   opens and never changes for that dialog's lifetime (see its own comment
   above) - a fine assumption for every other popup here, none of which can
   change the theme while they're open. This one can (the Appearance
   radios), so picking a theme needs to also recreate that brush and re-skin
   the controls that were themed at creation time, then force everything to
   repaint with the new colors instead of leaving stale ones on screen. */
static void refresh_settings_dialog_theme(HWND hwnd, SettingsDialogState *state) {
    popup_dialog_theme_created(hwnd);
    apply_explorer_theme(state->categoryList);
    /* Checkboxes/radios stay unthemed (see the WM_CREATE block below for why);
       nothing to redo here since that opt-out doesn't depend on light/dark. */
    RedrawWindow(hwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsDialogState *state = (SettingsDialogState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        state = (SettingsDialogState *)((CREATESTRUCTW *)lParam)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
        HINSTANCE inst = GetModuleHandleW(NULL);
        const WCHAR *categories[] = { L"Appearance", L"Font", L"Word Wrap", L"Preview", L"Editor", L"Compiler" };
        state->categoryList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
            12, 12, 140, 300, hwnd, (HMENU)10, inst, NULL);
        for (int i = 0; i < 6; i++) SendMessageW(state->categoryList, LB_ADDSTRING, 0, (LPARAM)categories[i]);
        apply_explorer_theme(state->categoryList);

        int px = 168, py = 16;
        state->themeLightRadio = CreateWindowExW(0, L"BUTTON", L"Light",
            WS_CHILD | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
            px, py, 220, 22, hwnd, (HMENU)20, inst, NULL);
        state->themeDarkRadio = CreateWindowExW(0, L"BUTTON", L"Dark",
            WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON,
            px, py + 28, 220, 22, hwnd, (HMENU)21, inst, NULL);
        SendMessageW(g_editorDarkMode ? state->themeDarkRadio : state->themeLightRadio, BM_SETCHECK, BST_CHECKED, 0);

        state->fontLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, px, py, 320, 20, hwnd, NULL, inst, NULL);
        state->fontChooseButton = CreateWindowExW(0, L"BUTTON", L"Choose Font...",
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, px, py + 28, 140, 26, hwnd, (HMENU)30, inst, NULL);

        state->wordWrapCheck = CreateWindowExW(0, L"BUTTON", L"Wrap long lines in the editor",
            WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, px, py, 320, 22, hwnd, (HMENU)40, inst, NULL);
        SendMessageW(state->wordWrapCheck, BM_SETCHECK, g_wordWrap ? BST_CHECKED : BST_UNCHECKED, 0);

        state->previewOnLeftCheck = CreateWindowExW(0, L"BUTTON", L"Show preview on the left",
            WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, px, py, 320, 22, hwnd, (HMENU)50, inst, NULL);
        SendMessageW(state->previewOnLeftCheck, BM_SETCHECK, g_previewOnLeft ? BST_CHECKED : BST_UNCHECKED, 0);
        state->previewThemeCaption = CreateWindowExW(0, L"STATIC", L"Markdown theme:", WS_CHILD,
            px, py + 36, 320, 20, hwnd, NULL, inst, NULL);
        state->previewThemeLightRadio = CreateWindowExW(0, L"BUTTON", L"GitHub Light",
            WS_CHILD | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
            px, py + 58, 220, 22, hwnd, (HMENU)53, inst, NULL);
        state->previewThemeDarkRadio = CreateWindowExW(0, L"BUTTON", L"GitHub Dark",
            WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON,
            px, py + 86, 220, 22, hwnd, (HMENU)54, inst, NULL);
        SendMessageW(g_previewDarkMode ? state->previewThemeDarkRadio : state->previewThemeLightRadio,
                     BM_SETCHECK, BST_CHECKED, 0);
        state->customCssCaption = CreateWindowExW(0, L"STATIC", L"Custom preview CSS:", WS_CHILD,
            px, py + 122, 320, 20, hwnd, NULL, inst, NULL);
        state->customCssLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, px, py + 144, 320, 20, hwnd, NULL, inst, NULL);
        state->customCssChooseButton = CreateWindowExW(0, L"BUTTON", L"Choose CSS...",
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, px, py + 170, 120, 26, hwnd, (HMENU)51, inst, NULL);
        state->customCssClearButton = CreateWindowExW(0, L"BUTTON", L"Clear",
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, px + 128, py + 170, 90, 26, hwnd, (HMENU)52, inst, NULL);

        state->spellCheckCheck = CreateWindowExW(0, L"BUTTON", L"Spell check",
            WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, px, py, 320, 22, hwnd, (HMENU)60, inst, NULL);
        SendMessageW(state->spellCheckCheck, BM_SETCHECK, g_spellCheckEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(state->spellCheckCheck, g_spellChecker != NULL);
        state->showWhitespaceCheck = CreateWindowExW(0, L"BUTTON", L"Show whitespace",
            WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, px, py + 28, 320, 22, hwnd, (HMENU)61, inst, NULL);
        SendMessageW(state->showWhitespaceCheck, BM_SETCHECK, g_showWhitespace ? BST_CHECKED : BST_UNCHECKED, 0);
        state->indentGuidesCheck = CreateWindowExW(0, L"BUTTON", L"Indent guides",
            WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, px, py + 56, 320, 22, hwnd, (HMENU)62, inst, NULL);
        SendMessageW(state->indentGuidesCheck, BM_SETCHECK, g_indentGuides ? BST_CHECKED : BST_UNCHECKED, 0);
        state->minimapCheck = CreateWindowExW(0, L"BUTTON", L"Minimap",
            WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, px, py + 84, 320, 22, hwnd, (HMENU)63, inst, NULL);
        SendMessageW(state->minimapCheck, BM_SETCHECK, g_minimapVisible ? BST_CHECKED : BST_UNCHECKED, 0);
        state->indentUseSpacesCheck = CreateWindowExW(0, L"BUTTON", L"Use spaces for indentation",
            WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, px, py + 112, 320, 22, hwnd, (HMENU)64, inst, NULL);
        SendMessageW(state->indentUseSpacesCheck, BM_SETCHECK, g_indentUseSpaces ? BST_CHECKED : BST_UNCHECKED, 0);
        state->tabWidthCaption = CreateWindowExW(0, L"STATIC", L"Spaces per tab:", WS_CHILD,
            px, py + 142, 120, 20, hwnd, NULL, inst, NULL);
        state->tabWidthEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, px + 124, py + 140, 50, 24, hwnd, (HMENU)65, inst, NULL);
        WCHAR tabWidthText[16];
        swprintf(tabWidthText, sizeof(tabWidthText) / sizeof(tabWidthText[0]), L"%d", g_tabWidth);
        SetWindowTextW(state->tabWidthEdit, tabWidthText);

        state->spellLanguageCaption = CreateWindowExW(0, L"STATIC", L"Spell check language:", WS_CHILD,
            px, py + 172, 200, 20, hwnd, NULL, inst, NULL);
        state->spellLanguageLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
            px, py + 194, 320, 20, hwnd, NULL, inst, NULL);
        state->spellLanguageChooseButton = CreateWindowExW(0, L"BUTTON", L"Choose...",
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, px, py + 220, 120, 26, hwnd, (HMENU)66, inst, NULL);
        EnableWindow(state->spellLanguageChooseButton, g_spellChecker != NULL);
        update_settings_spell_label(state);

        state->compilerPathsHintCaption = CreateWindowExW(0, L"STATIC",
            L"Optional overrides (leave blank to search PATH automatically):", WS_CHILD,
            px, py, 380, 18, hwnd, NULL, inst, NULL);
        state->vcvarsallCaption = CreateWindowExW(0, L"STATIC", L"MSVC vcvars:", WS_CHILD,
            px, py + 24, 90, 20, hwnd, NULL, inst, NULL);
        state->vcvarsallEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, px + 96, py + 22, 176, 24, hwnd, (HMENU)71, inst, NULL);
        state->vcvarsallBrowseButton = CreateWindowExW(0, L"BUTTON", L"Browse...",
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, px + 280, py + 22, 90, 24, hwnd, (HMENU)72, inst, NULL);
        SetWindowTextW(state->vcvarsallEdit, g_vcvarsallOverridePath ? g_vcvarsallOverridePath : L"");

        state->gccCaption = CreateWindowExW(0, L"STATIC", L"GCC/G++:", WS_CHILD,
            px, py + 58, 90, 20, hwnd, NULL, inst, NULL);
        state->gccEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, px + 96, py + 56, 176, 24, hwnd, (HMENU)73, inst, NULL);
        state->gccBrowseButton = CreateWindowExW(0, L"BUTTON", L"Browse...",
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, px + 280, py + 56, 90, 24, hwnd, (HMENU)74, inst, NULL);
        SetWindowTextW(state->gccEdit, g_gccBinOverride ? g_gccBinOverride : L"");

        state->clangCaption = CreateWindowExW(0, L"STATIC", L"Clang:", WS_CHILD,
            px, py + 92, 90, 20, hwnd, NULL, inst, NULL);
        state->clangEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, px + 96, py + 90, 176, 24, hwnd, (HMENU)75, inst, NULL);
        state->clangBrowseButton = CreateWindowExW(0, L"BUTTON", L"Browse...",
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, px + 280, py + 90, 90, 24, hwnd, (HMENU)76, inst, NULL);
        SetWindowTextW(state->clangEdit, g_clangBinOverride ? g_clangBinOverride : L"");

        state->zigCaption = CreateWindowExW(0, L"STATIC", L"Zig:", WS_CHILD,
            px, py + 126, 90, 20, hwnd, NULL, inst, NULL);
        state->zigEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, px + 96, py + 124, 176, 24, hwnd, (HMENU)77, inst, NULL);
        state->zigBrowseButton = CreateWindowExW(0, L"BUTTON", L"Browse...",
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, px + 280, py + 124, 90, 24, hwnd, (HMENU)78, inst, NULL);
        SetWindowTextW(state->zigEdit, g_zigBinOverride ? g_zigBinOverride : L"");

        state->luaCaption = CreateWindowExW(0, L"STATIC", L"Lua:", WS_CHILD,
            px, py + 160, 90, 20, hwnd, NULL, inst, NULL);
        state->luaEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, px + 96, py + 158, 176, 24, hwnd, (HMENU)79, inst, NULL);
        state->luaBrowseButton = CreateWindowExW(0, L"BUTTON", L"Browse...",
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, px + 280, py + 158, 90, 24, hwnd, (HMENU)80, inst, NULL);
        SetWindowTextW(state->luaEdit, g_luaBinOverride ? g_luaBinOverride : L"");

        state->gdbCaption = CreateWindowExW(0, L"STATIC", L"GDB:", WS_CHILD,
            px, py + 194, 90, 20, hwnd, NULL, inst, NULL);
        state->gdbEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, px + 96, py + 192, 176, 24, hwnd, (HMENU)81, inst, NULL);
        state->gdbBrowseButton = CreateWindowExW(0, L"BUTTON", L"Browse...",
            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, px + 280, py + 192, 90, 24, hwnd, (HMENU)82, inst, NULL);
        SetWindowTextW(state->gdbEdit, g_gdbBinOverride ? g_gdbBinOverride : L"");

        state->compilerFlagsCaption = CreateWindowExW(0, L"STATIC",
            L"Extra flags passed to Run/Debug C/C++ builds:", WS_CHILD,
            px, py + 228, 380, 20, hwnd, NULL, inst, NULL);
        state->compilerFlagsEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, px, py + 252, 380, 24, hwnd, (HMENU)70, inst, NULL);
        SetWindowTextW(state->compilerFlagsEdit, g_compilerExtraFlags ? g_compilerExtraFlags : L"");

        /* Visual-styled BS_AUTOCHECKBOX/BS_AUTORADIOBUTTON controls draw their
           label via UxTheme and ignore the WM_CTLCOLORSTATIC/BTN text colour
           popup_dialog_theme_message() sets below - "DarkMode_Explorer" doesn't
           change that for the Button window class, so in dark mode the label
           stayed at the theme's default near-black regardless. Unthemeing
           these (empty subapp/idlist names) drops back to classic button
           rendering, which does draw the label through GDI with whatever
           colour WM_CTLCOLORBTN/STATIC returns. */
        HWND checkboxes[] = { state->wordWrapCheck, state->previewOnLeftCheck, state->spellCheckCheck,
                              state->showWhitespaceCheck, state->indentGuidesCheck, state->minimapCheck,
                              state->indentUseSpacesCheck,
                              state->themeLightRadio, state->themeDarkRadio,
                              state->previewThemeLightRadio, state->previewThemeDarkRadio };
        for (size_t i = 0; i < sizeof(checkboxes) / sizeof(checkboxes[0]); i++)
            SetWindowTheme(checkboxes[i], L"", L"");

        CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
            496, 330, 78, 26, hwnd, (HMENU)IDCANCEL, inst, NULL);

        update_settings_font_label(state);
        update_settings_css_label(state);
        SendMessageW(state->categoryList, LB_SETCURSEL, 0, 0);
        update_settings_category(state);
        SetFocus(state->categoryList);
        popup_dialog_theme_opened(hwnd);
        return 0;
    }
    if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
        case WM_ERASEBKGND: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN:
        case WM_CTLCOLORLISTBOX: {
            LRESULT handled = 0;
            LRESULT result = popup_dialog_theme_message(hwnd, msg, wParam, &handled);
            if (handled) return result;
            break;
        }
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lParam;
            if (mis->CtlType == ODT_LISTBOX) { measure_themed_listbox_item(state->categoryList, mis); return TRUE; }
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_BUTTON) { draw_popup_button(dis); return TRUE; }
            if (dis->CtlType == ODT_LISTBOX && dis->hwndItem == state->categoryList) {
                draw_themed_listbox_item(state->categoryList, dis);
                return TRUE;
            }
            break;
        }
        case WM_NCDESTROY:
            popup_dialog_theme_destroyed();
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == 10 && HIWORD(wParam) == LBN_SELCHANGE) {
                int selection = (int)SendMessageW(state->categoryList, LB_GETCURSEL, 0, 0);
                if (selection != LB_ERR) { state->activeCategory = selection; update_settings_category(state); }
                return 0;
            }
            if (LOWORD(wParam) == 65 && HIWORD(wParam) == EN_KILLFOCUS) {
                WCHAR buf[16];
                GetWindowTextW(state->tabWidthEdit, buf, sizeof(buf) / sizeof(buf[0]));
                set_tab_width((int)wcstol(buf, NULL, 10));
                swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%d", g_tabWidth);
                SetWindowTextW(state->tabWidthEdit, buf);
                return 0;
            }
            if (HIWORD(wParam) == EN_KILLFOCUS) {
                /* Shared by the extra-flags box and every compiler path
                   override box: each maps its control ID to the HWND/global
                   slot pair set_compiler_string_setting() should trim and
                   persist into. */
                struct { WORD id; HWND edit; WCHAR **slot; } fields[] = {
                    { 70, state->compilerFlagsEdit, &g_compilerExtraFlags },
                    { 71, state->vcvarsallEdit, &g_vcvarsallOverridePath },
                    { 73, state->gccEdit, &g_gccBinOverride },
                    { 75, state->clangEdit, &g_clangBinOverride },
                    { 77, state->zigEdit, &g_zigBinOverride },
                    { 79, state->luaEdit, &g_luaBinOverride },
                    { 81, state->gdbEdit, &g_gdbBinOverride },
                };
                for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
                    if (LOWORD(wParam) != fields[i].id) continue;
                    WCHAR buf[1024];
                    GetWindowTextW(fields[i].edit, buf, sizeof(buf) / sizeof(buf[0]));
                    set_compiler_string_setting(fields[i].slot, buf);
                    SetWindowTextW(fields[i].edit, *fields[i].slot ? *fields[i].slot : L"");
                    return 0;
                }
            }
            if (HIWORD(wParam) != BN_CLICKED) break;
            switch (LOWORD(wParam)) {
                case 20: select_theme(g_hMainWnd, 0); refresh_settings_dialog_theme(hwnd, state); break;
                case 21: select_theme(g_hMainWnd, 1); refresh_settings_dialog_theme(hwnd, state); break;
                case 30: do_choose_font(); update_settings_font_label(state); break;
                case 40: toggle_word_wrap(g_hMainWnd); break;
                case 50: toggle_panel_sides(g_hMainWnd); break;
                case 51: do_choose_custom_css(); update_settings_css_label(state); break;
                case 52: clear_custom_css(); update_settings_css_label(state); break;
                case 53: select_preview_theme(g_hMainWnd, 0); break;
                case 54: select_preview_theme(g_hMainWnd, 1); break;
                case 60: toggle_spell_check(g_hMainWnd); break;
                case 61: toggle_show_whitespace(g_hMainWnd); break;
                case 62: toggle_indent_guides(g_hMainWnd); break;
                case 63: toggle_minimap(g_hMainWnd); break;
                case 64: toggle_indent_use_spaces(g_hMainWnd); break;
                case 66: choose_spell_language(); update_settings_spell_label(state); break;
                case 72: choose_vcvarsall_path(state->vcvarsallEdit); break;
                case 74: choose_compiler_folder(&g_gccBinOverride, state->gccEdit); break;
                case 76: choose_compiler_folder(&g_clangBinOverride, state->clangEdit); break;
                case 78: choose_compiler_folder(&g_zigBinOverride, state->zigEdit); break;
                case 80: choose_compiler_folder(&g_luaBinOverride, state->luaEdit); break;
                case 82: choose_compiler_folder(&g_gdbBinOverride, state->gdbEdit); break;
                case IDCANCEL: DestroyWindow(hwnd); break;
            }
            return 0;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void show_settings_dialog(void) {
    static int registered = 0;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MarkEditSettingsDialog";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        registered = 1;
    }
    SettingsDialogState state = {0};
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MarkEditSettingsDialog", L"Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 400, g_hMainWnd, NULL,
        GetModuleHandleW(NULL), &state);
    if (!dialog) return;
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
}

/* Menu commands, grouped by the feature they operate on. */
static int handle_file_command(HWND hwnd, WORD cmd) {
    switch (cmd) {
        case IDM_FILE_NEW:
            create_document_tab();
            break;
        case IDM_FILE_OPEN:
            do_open();
            break;
        case IDM_FILE_INSERT:
            do_insert_file();
            break;
        case IDM_FILE_QUICK_OPEN:
            do_quick_open();
            break;
        case IDM_FILE_FIND_FOLDER:
            do_find_in_folder();
            break;
        case IDM_REPLACE_WORKSPACE:
            do_replace_in_workspace();
            break;
        case IDM_OPEN_WORKSPACE:
            do_open_workspace();
            break;
        case IDM_RECENT_WORKSPACE_CLEAR:
            recent_list_clear(&g_recentFolderList);
            refresh_menu(hwnd);
            break;
        case IDM_FILE_SAVE:
            do_save();
            break;
        case IDM_FILE_SAVEAS:
            do_saveas();
            break;
        case IDM_FILE_SAVE_ALL:
            do_save_all();
            break;
        case IDM_FILE_EXPORT_HTML:
            do_export_html();
            break;
        case IDM_FILE_EXPORT_PDF:
            prepare_rendered_print(1);
            break;
        case IDM_FILE_PRINT:
            prepare_rendered_print(0);
            break;
        case IDM_FILE_OPEN_TERMINAL:
            open_terminal_here();
            break;
        case IDM_FILE_SHOW_EXPLORER:
            reveal_in_explorer();
            break;
        case IDM_FILE_CLOSE:
            do_close_document_at(g_activeDocument);
            break;
        case IDM_FILE_REOPEN_CLOSED:
            do_reopen_closed_tab();
            break;
        case IDM_FILE_GO_BACK:
            go_wiki_back();
            break;
        case IDM_FILE_RECENT_CLEAR:
            recent_list_clear(&g_recentFileList);
            refresh_menu(hwnd);
            break;
        case IDM_SETTINGS:
            show_settings_dialog();
            break;
        case IDM_FILE_EXIT:
            do_exit();
            break;
        default:
            return 0;
    }
    return 1;
}

static int handle_edit_command(WORD cmd) {
    switch (cmd) {
        case IDM_EDIT_UNDO:
            SendMessageA(active_edit_view(), SCI_UNDO, 0, 0);
            break;
        case IDM_EDIT_REDO:
            SendMessageA(active_edit_view(), SCI_REDO, 0, 0);
            break;
        case IDM_EDIT_CUT:
            SendMessageA(active_edit_view(), SCI_CUT, 0, 0);
            break;
        case IDM_EDIT_COPY:
            SendMessageA(active_edit_view(), SCI_COPY, 0, 0);
            break;
        case IDM_EDIT_PASTE:
            /* Image paste always lands in the main pane - it can prompt to
               save the document first, which isn't worth threading through
               to the split pane for what's a rare combination. */
            if (!paste_image_from_clipboard()) SendMessageA(active_edit_view(), SCI_PASTE, 0, 0);
            break;
        case IDM_EDIT_DELETE:
            SendMessageA(active_edit_view(), SCI_CLEAR, 0, 0);
            break;
        case IDM_EDIT_SELECTALL:
            SendMessageA(active_edit_view(), SCI_SELECTALL, 0, 0);
            break;
        case IDM_EDIT_DUPLICATE_LINE:
            SendMessageA(active_edit_view(), SCI_LINEDUPLICATE, 0, 0);
            break;
        case IDM_EDIT_MOVE_LINE_UP:
            SendMessageA(active_edit_view(), SCI_MOVESELECTEDLINESUP, 0, 0);
            break;
        case IDM_EDIT_MOVE_LINE_DOWN:
            SendMessageA(active_edit_view(), SCI_MOVESELECTEDLINESDOWN, 0, 0);
            break;
        case IDM_EDIT_DELETE_LINE:
            SendMessageA(active_edit_view(), SCI_LINEDELETE, 0, 0);
            break;
        case IDM_EDIT_JOIN_LINES:
            SendMessageA(active_edit_view(), SCI_LINESJOIN, 0, 0);
            break;
        case IDM_EDIT_TOGGLE_COMMENT:
            toggle_comment(active_edit_view());
            break;
        case IDM_EDIT_TRIM_WHITESPACE:
            trim_trailing_whitespace(active_edit_view());
            break;
        case IDM_EDIT_SELECT_NEXT:
            select_next_occurrence(active_edit_view());
            break;
        case IDM_EDIT_SELECT_ALL_OCCURRENCES:
            select_all_occurrences(active_edit_view());
            break;
        case IDM_EDIT_MATCH_BRACE:
            jump_to_matching_brace(active_edit_view());
            break;
        case IDM_EDIT_BOOKMARK_TOGGLE:
            toggle_bookmark_at_cursor(active_edit_view());
            break;
        case IDM_EDIT_BOOKMARK_NEXT:
            goto_bookmark(active_edit_view(), 1);
            break;
        case IDM_EDIT_BOOKMARK_PREV:
            goto_bookmark(active_edit_view(), 0);
            break;
        case IDM_EDIT_BOOKMARK_CLEAR:
            clear_all_bookmarks();
            break;
        case IDM_EDIT_UPPERCASE:
            SendMessageA(active_edit_view(), SCI_UPPERCASE, 0, 0);
            break;
        case IDM_EDIT_LOWERCASE:
            SendMessageA(active_edit_view(), SCI_LOWERCASE, 0, 0);
            break;
        case IDM_EDIT_TITLECASE:
            apply_title_case(active_edit_view());
            break;
        case IDM_EDIT_SORT_ASC:
            transform_selected_lines(active_edit_view(), 0, editor_eol_sequence());
            break;
        case IDM_EDIT_SORT_DESC:
            transform_selected_lines(active_edit_view(), 1, editor_eol_sequence());
            break;
        case IDM_EDIT_SORT_DEDUPE:
            transform_selected_lines(active_edit_view(), 2, editor_eol_sequence());
            break;
        case IDM_EDIT_TABS_TO_SPACES:
            convert_indentation(active_edit_view(), 1);
            break;
        case IDM_EDIT_SPACES_TO_TABS:
            convert_indentation(active_edit_view(), 0);
            break;
        case IDM_EDIT_EOL_CRLF:
            convert_line_endings(active_edit_view(), SC_EOL_CRLF);
            break;
        case IDM_EDIT_ENCODING_UTF8:
            set_document_encoding(TEXT_ENCODING_UTF8, 0);
            break;
        case IDM_EDIT_ENCODING_UTF8_BOM:
            set_document_encoding(TEXT_ENCODING_UTF8, 1);
            break;
        case IDM_EDIT_ENCODING_UTF16LE:
            set_document_encoding(TEXT_ENCODING_UTF16LE, 1);
            break;
        case IDM_EDIT_ENCODING_UTF16BE:
            set_document_encoding(TEXT_ENCODING_UTF16BE, 1);
            break;
        case IDM_EDIT_ENCODING_ANSI:
            set_document_encoding(TEXT_ENCODING_ANSI, 0);
            break;
        case IDM_FILE_REOPEN_UTF8:
            reopen_with_encoding(TEXT_ENCODING_UTF8);
            break;
        case IDM_FILE_REOPEN_UTF16LE:
            reopen_with_encoding(TEXT_ENCODING_UTF16LE);
            break;
        case IDM_FILE_REOPEN_UTF16BE:
            reopen_with_encoding(TEXT_ENCODING_UTF16BE);
            break;
        case IDM_FILE_REOPEN_ANSI:
            reopen_with_encoding(TEXT_ENCODING_ANSI);
            break;
        case IDM_EDIT_EOL_LF:
            convert_line_endings(active_edit_view(), SC_EOL_LF);
            break;
        case IDM_EDIT_EOL_CR:
            convert_line_endings(active_edit_view(), SC_EOL_CR);
            break;
        case IDM_EDIT_FIND:
            show_find_dialog(0);
            break;
        case IDM_EDIT_REPLACE:
            show_find_dialog(1);
            break;
        case IDM_EDIT_GOTOLINE:
            show_goto_line_dialog();
            break;
        case IDM_EDIT_COMMAND_PALETTE:
            show_command_palette();
            break;
        case IDM_EDIT_FINDNEXT:
            if (g_findWhat[0]) {
                char *findUtf8 = to_utf8(g_findWhat);
                if (findUtf8) find_in_edit(findUtf8, (g_fr.Flags & FR_MATCHCASE) ? 1 : 0, 1);
                free(findUtf8);
            } else show_find_dialog(0);
            break;
        default:
            return 0;
    }
    return 1;
}

static int handle_format_command(WORD cmd) {
    switch (cmd) {
        case IDM_FORMAT_FONT:
            do_choose_font();
            break;
        case IDM_FMT_JSON_PRETTY:
            format_json(1);
            break;
        case IDM_FMT_JSON_MINIFY:
            format_json(0);
            break;
        case IDM_FMT_HTML_HTML:
            toolbar_wrap_tag("html");
            break;
        case IDM_FMT_HTML_HEAD:
            toolbar_wrap_tag("head");
            break;
        case IDM_FMT_HTML_BODY:
            toolbar_wrap_tag("body");
            break;
        case IDM_FMT_HTML_PARAGRAPH:
            toolbar_wrap_tag("p");
            break;
        case IDM_FMT_HTML_BOLD:
            toolbar_wrap_tag("b");
            break;
        case IDM_FMT_HTML_ITALIC:
            toolbar_wrap_tag("i");
            break;
        case IDM_FMT_HTML_UNDERLINE:
            toolbar_wrap_tag("u");
            break;
        case IDM_FMT_HTML_DIV:
            toolbar_wrap_tag("div");
            break;
        case IDM_FMT_HTML_SPAN:
            toolbar_wrap_tag("span");
            break;
        case IDM_FMT_HTML_BREAK:
            SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)"<br>");
            SetFocus(g_hEdit);
            break;
        case IDM_FMT_HTML_LINK:
            toolbar_insert_html_link();
            break;
        case IDM_FMT_HTML_IMAGE:
            toolbar_insert_html_image();
            break;
        case IDM_FMT_HTML_TABLE:
            toolbar_insert_html_table();
            break;
        case IDM_FMT_HTML_BULLETLIST:
            toolbar_insert_html_list(0);
            break;
        case IDM_FMT_HTML_NUMBERLIST:
            toolbar_insert_html_list(1);
            break;
        case IDM_FMT_TABLE_EDITOR:
            show_table_editor();
            break;
        case IDM_FMT_TABLE_ROW_ABOVE:
            insert_table_row_relative(0);
            break;
        case IDM_FMT_TABLE_ROW_BELOW:
            insert_table_row_relative(1);
            break;
        case IDM_FMT_TABLE_ROW_DELETE:
            delete_table_row_at_cursor();
            break;
        case IDM_FMT_TABLE_COL_LEFT:
            insert_table_column_relative(0);
            break;
        case IDM_FMT_TABLE_COL_RIGHT:
            insert_table_column_relative(1);
            break;
        case IDM_FMT_TABLE_COL_DELETE:
            delete_table_column_at_cursor();
            break;
        case IDM_TABLE_SORT_ASC:
            sort_table_rows(0);
            break;
        case IDM_TABLE_SORT_DESC:
            sort_table_rows(1);
            break;
        case IDM_TABLE_ALIGN_LEFT:
            set_table_column_alignment(":---");
            break;
        case IDM_TABLE_ALIGN_CENTER:
            set_table_column_alignment(":---:");
            break;
        case IDM_TABLE_ALIGN_RIGHT:
            set_table_column_alignment("---:");
            break;
        default:
            return 0;
    }
    return 1;
}

static int handle_debug_command(WORD cmd) {
    switch (cmd) {
        case IDM_RUN_FILE:
            launch_active_source_file(0);
            break;
        case IDM_DEBUG_FILE:
            launch_active_source_file(1);
            break;
        case IDM_DEBUG_CONTINUE:
            run_debugger_command(IDM_DEBUG_CONTINUE);
            break;
        case IDM_DEBUG_STEP_OVER:
            run_debugger_command(IDM_DEBUG_STEP_OVER);
            break;
        case IDM_DEBUG_STEP_INTO:
            run_debugger_command(IDM_DEBUG_STEP_INTO);
            break;
        case IDM_DEBUG_STEP_OUT:
            run_debugger_command(IDM_DEBUG_STEP_OUT);
            break;
        case IDM_DEBUG_BREAKPOINT:
            toggle_breakpoint_at_cursor();
            break;
        case IDM_DEBUG_CLEAR_BREAKPOINTS:
            clear_debug_breakpoints();
            break;
        default:
            return 0;
    }
    return 1;
}

static int handle_view_command(HWND hwnd, WORD cmd) {
    switch (cmd) {
        case IDM_VIEW_TOGGLEPREVIEW:
            toggle_preview(hwnd);
            break;
        case IDM_VIEW_TOGGLEEDITOR:
            toggle_editor(hwnd);
            break;
        case IDM_VIEW_PREVIEW_ON_LEFT:
            toggle_panel_sides(hwnd);
            break;
        case IDM_VIEW_CUSTOM_CSS:
            do_choose_custom_css();
            break;
        case IDM_VIEW_CLEAR_CUSTOM_CSS:
            clear_custom_css();
            break;
        case IDM_VIEW_FOCUS_MODE:
            toggle_focus_mode(hwnd);
            break;
        case IDM_VIEW_WORD_WRAP:
            toggle_word_wrap(hwnd);
            break;
        case IDM_VIEW_SPELL_CHECK:
            toggle_spell_check(hwnd);
            break;
        case IDM_VIEW_SPELL_LANGUAGE:
            choose_spell_language();
            break;
        case IDM_VIEW_SHOW_WHITESPACE:
            toggle_show_whitespace(hwnd);
            break;
        case IDM_VIEW_INDENT_GUIDES:
            toggle_indent_guides(hwnd);
            break;
        case IDM_VIEW_USE_SPACES_INDENT:
            toggle_indent_use_spaces(hwnd);
            break;
        case IDM_VIEW_FOLD_TOGGLE:
            fold_toggle_at_cursor();
            break;
        case IDM_VIEW_FOLD_ALL:
            fold_all_lines(0);
            break;
        case IDM_VIEW_UNFOLD_ALL:
            fold_all_lines(1);
            break;
        case IDM_VIEW_OUTLINE:
            toggle_outline(hwnd);
            break;
        case IDM_VIEW_MINIMAP:
            toggle_minimap(hwnd);
            break;
        case IDM_VIEW_EXPLORER:
            toggle_explorer_panel(hwnd);
            break;
        case IDM_VIEW_TOGGLESPLIT:
            toggle_split_editor(hwnd);
            break;
        case IDM_VIEW_RESET_SPLIT_EDITOR:
            reset_split_editor(hwnd);
            break;
        case IDM_VIEW_ZOOM_IN:
            adjust_editor_zoom(1);
            break;
        case IDM_VIEW_ZOOM_OUT:
            adjust_editor_zoom(-1);
            break;
        case IDM_VIEW_ZOOM_RESET:
            reset_editor_zoom();
            break;
        case IDM_VIEW_NEXT_TAB:
            if (g_documentCount > 1) switch_document((g_activeDocument + 1) % g_documentCount);
            break;
        case IDM_VIEW_PREVIOUS_TAB:
            if (g_documentCount > 1) switch_document((g_activeDocument + g_documentCount - 1) % g_documentCount);
            break;
        case IDM_VIEW_CYCLE_PANE:
            cycle_pane_focus(0);
            break;
        case IDM_VIEW_CYCLE_PANE_PREV:
            cycle_pane_focus(1);
            break;
        case IDM_VIEW_THEME_GITHUB_LIGHT:
        case IDM_VIEW_THEME_GITHUB_DARK:
            select_theme(hwnd, cmd == IDM_VIEW_THEME_GITHUB_DARK);
            break;
        case IDM_VIEW_PREVIEW_THEME_LIGHT:
        case IDM_VIEW_PREVIEW_THEME_DARK:
            select_preview_theme(hwnd, cmd == IDM_VIEW_PREVIEW_THEME_DARK);
            break;
        default:
            return 0;
    }
    return 1;
}

static int handle_help_command(WORD cmd) {
    switch (cmd) {
        case IDM_HELP_REFERENCE:
            show_help_page(REFERENCE_MARKDOWN);
            break;
        case IDM_HELP_SHORTCUTS:
            show_help_page(REFERENCE_SHORTCUTS);
            break;
        case IDM_HELP_ABOUT:
            show_about_box();
            break;
        default:
            return 0;
    }
    return 1;
}

static void handle_main_command(HWND hwnd, WORD cmd) {
    if (cmd >= IDM_FILE_RECENT_0 && cmd < IDM_FILE_RECENT_0 + MAX_RECENT_FILES) {
        do_open_recent(cmd - IDM_FILE_RECENT_0);
        return;
    }
    if (cmd >= IDM_RECENT_WORKSPACE && cmd < IDM_RECENT_WORKSPACE + MAX_RECENT_FOLDERS) {
        do_open_recent_workspace(cmd - IDM_RECENT_WORKSPACE);
        return;
    }
    if (cmd >= IDM_FMT_BOLD && cmd <= IDM_FMT_ALERT) {
        toolbar_command(cmd);
        return;
    }

    /* Each handler reports whether it recognized the command. */
    if (handle_file_command(hwnd, cmd)) return;
    if (handle_edit_command(cmd)) return;
    if (handle_format_command(cmd)) return;
    if (handle_debug_command(cmd)) return;
    if (handle_view_command(hwnd, cmd)) return;
    if (handle_help_command(cmd)) return;
}

static RECT tab_close_rectangle(const RECT *tabRect) {
    RECT close = *tabRect;
    close.left = close.right - 23;
    close.right -= 5;
    close.top += 3;
    close.bottom -= 3;
    return close;
}

static void draw_tab_item(const DRAWITEMSTRUCT *dis) {
    int index = (int)dis->itemID;
    if (index < 0 || index >= g_documentCount) return;
    int selected = TabCtrl_GetCurSel(g_hTabs) == index;
    COLORREF bg = g_editorDarkMode ? THEME_DARK_BG
        : (selected ? THEME_LIGHT_BG : RGB(0xF0, 0xF3, 0xF6));
    COLORREF fg = g_editorDarkMode
        ? (selected ? RGB(0xF0, 0xF6, 0xFC) : RGB(0xB1, 0xBA, 0xC4))
        : THEME_LIGHT_FG;
    fill_rect_color(dis->hDC, &dis->rcItem, bg);
    HPEN borderPen = CreatePen(PS_SOLID, 1, theme_border_color());
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

    /* tab_close_rectangle() is wider than it is tall (a roomier click
       target), so inset-padding it directly for the X stretched the cross
       into a near-flat bowtie instead of a diagonal. Draw the glyph in a
       square centered on that rect instead, sized to its shorter side, so
       the strokes stay a proper 45-degree X regardless of the hit box's
       aspect ratio. */
    RECT close = tab_close_rectangle(&dis->rcItem);
    int side = (close.right - close.left < close.bottom - close.top)
        ? (close.right - close.left) : (close.bottom - close.top);
    int cx = (close.left + close.right) / 2, cy = (close.top + close.bottom) / 2;
    int pad = 3;
    if (pad * 2 >= side) pad = side / 2 - 1;
    if (pad < 1) pad = 1;
    RECT glyph = { cx - side / 2, cy - side / 2, cx + side / 2, cy + side / 2 };
    int padTop = pad + 1, padBottom = pad;
    HPEN closePen = CreatePen(PS_SOLID, 2, fg);
    SelectObject(dis->hDC, closePen);
    MoveToEx(dis->hDC, glyph.left + pad, glyph.top + padTop, NULL);
    LineTo(dis->hDC, glyph.right - pad, glyph.bottom - padBottom);
    MoveToEx(dis->hDC, glyph.right - pad, glyph.top + padTop, NULL);
    LineTo(dis->hDC, glyph.left + pad, glyph.bottom - padBottom);
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

/* Shared owner-draw for the app's plain, single-column LISTBOX controls (the
   outline panel and the searchable picker's result list) so both follow the
   app's light/dark theme - a plain LISTBOX always paints in system colors
   regardless of SetWindowTheme. Parameterized by HWND since both listboxes
   need identical drawing. */
static void measure_themed_listbox_item(HWND listbox, MEASUREITEMSTRUCT *mis) {
    HFONT font = (HFONT)SendMessageW(listbox, WM_GETFONT, 0, 0);
    HDC hdc = GetDC(listbox);
    HFONT oldFont = font ? (HFONT)SelectObject(hdc, font) : NULL;
    TEXTMETRICW tm;
    GetTextMetricsW(hdc, &tm);
    if (oldFont) SelectObject(hdc, oldFont);
    ReleaseDC(listbox, hdc);
    mis->itemHeight = (UINT)(tm.tmHeight + tm.tmExternalLeading + MulDiv(6, GetDpiForWindow(g_hMainWnd), 96));
}

static void draw_themed_listbox_item(HWND listbox, const DRAWITEMSTRUCT *dis) {
    if ((int)dis->itemID < 0) return;
    int selected = (dis->itemState & ODS_SELECTED) != 0;
    COLORREF bg = g_editorDarkMode
        ? (selected ? RGB(0x26, 0x4F, 0x78) : THEME_DARK_BG)
        : (selected ? RGB(0xB6, 0xD7, 0xFF) : THEME_LIGHT_BG);
    COLORREF fg = theme_fg_color();
    fill_rect_color(dis->hDC, &dis->rcItem, bg);

    /* LB_GETTEXT overflows whatever buffer it's given rather than truncating
       to it, so the buffer has to be sized from LB_GETTEXTLEN first - a
       heading line (or a long file path in the picker) can run well past
       any fixed stack buffer. */
    int textLen = (int)SendMessageW(listbox, LB_GETTEXTLEN, dis->itemID, 0);
    if (textLen <= 0) return;
    WCHAR stackLabel[256];
    WCHAR *heapLabel = NULL;
    WCHAR *label = stackLabel;
    if (textLen >= (int)(sizeof(stackLabel) / sizeof(stackLabel[0]))) {
        heapLabel = (WCHAR *)malloc(((size_t)textLen + 1) * sizeof(WCHAR));
        if (!heapLabel) return;
        label = heapLabel;
    }
    int length = (int)SendMessageW(listbox, LB_GETTEXT, dis->itemID, (LPARAM)label);
    if (length > 0) {
        RECT textRect = dis->rcItem;
        textRect.left += 4;
        SetTextColor(dis->hDC, fg);
        SetBkMode(dis->hDC, TRANSPARENT);
        DrawTextW(dis->hDC, label, length, &textRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    free(heapLabel);
}

static int line_has_list_marker(const char *line, Sci_Position length) {
    Sci_Position i = 0;
    while (i < length && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i + 1 < length && (line[i] == '-' || line[i] == '*' || line[i] == '+') &&
        (line[i + 1] == ' ' || line[i + 1] == '\t')) return 1;
    Sci_Position digits = i;
    while (i < length && line[i] >= '0' && line[i] <= '9') i++;
    return i > digits && i + 1 < length && (line[i] == '.' || line[i] == ')') &&
        (line[i + 1] == ' ' || line[i + 1] == '\t');
}

/* Preserve real tabs for indentation, but use spaces for alignment after
   ordinary text. A collapsed caret anywhere in a Markdown list item indents
   the entire item, which makes nesting work without first moving the caret. */
static int smart_tab(HWND hEdit, int reverse) {
    if (g_largeFile) return 0; /* copies the whole line, see smart_newline() */
    Sci_Position start = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position end = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (start != end) return 0; /* retain Scintilla's block-selection behavior */

    Sci_Position lineNumber = (Sci_Position)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)start, 0);
    Sci_Position lineStart = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)lineNumber, 0);
    Sci_Position lineEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETLINEENDPOSITION, (WPARAM)lineNumber, 0);
    Sci_Position lineLength = lineEnd - lineStart;
    char *line = (char *)malloc((size_t)lineLength + 1);
    if (!line) return 0;
    struct Sci_TextRangeFull range = { { lineStart, lineEnd }, line };
    SendMessageA(hEdit, SCI_GETTEXTRANGEFULL, 0, (LPARAM)&range);
    line[lineLength] = 0;

    int leadingLength = 0;
    while (leadingLength < lineLength &&
           (line[leadingLength] == ' ' || line[leadingLength] == '\t')) leadingLength++;
    int listItem = line_has_list_marker(line, lineLength);

    if (reverse && leadingLength > 0) {
        int remove = 0;
        int tabWidth = (int)SendMessageA(hEdit, SCI_GETTABWIDTH, 0, 0);
        if (tabWidth < 1) tabWidth = 4;
        if (line[0] == '\t') remove = 1;
        else while (remove < leadingLength && remove < tabWidth && line[remove] == ' ') remove++;
        SendMessageA(hEdit, SCI_SETTARGETSTART, (WPARAM)lineStart, 0);
        SendMessageA(hEdit, SCI_SETTARGETEND, (WPARAM)(lineStart + remove), 0);
        SendMessageA(hEdit, SCI_REPLACETARGET, 0, (LPARAM)"");
        free(line);
        return 1;
    }

    if (!reverse && listItem) {
        SendMessageA(hEdit, SCI_INSERTTEXT, (WPARAM)lineStart, (LPARAM)"\t");
        SendMessageA(hEdit, SCI_SETSEL, (WPARAM)(start + 1), (LPARAM)(start + 1));
        free(line);
        return 1;
    }

    free(line);
    if (reverse || start <= lineStart + leadingLength) return 0;

    int tabWidth = (int)SendMessageA(hEdit, SCI_GETTABWIDTH, 0, 0);
    if (tabWidth < 1) tabWidth = 4;
    Sci_Position column = (Sci_Position)SendMessageA(hEdit, SCI_GETCOLUMN, (WPARAM)start, 0);
    int spaces = tabWidth - column % tabWidth;
    char padding[32];
    if (spaces >= (int)sizeof(padding)) spaces = (int)sizeof(padding) - 1;
    memset(padding, ' ', (size_t)spaces);
    padding[spaces] = 0;
    SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)padding);
    return 1;
}

static int smart_markdown_newline(HWND hEdit) {
    Sci_Position selectionStart = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selectionEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (selectionStart != selectionEnd) return 0;
    Sci_Position caret = selectionEnd;
    if (caret > 0 && (int)SendMessageA(hEdit, SCI_GETSTYLEAT, (WPARAM)(caret - 1), 0) == SCE_MARKDOWN_CODEBK)
        return 0;
    Sci_Position line = (Sci_Position)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)caret, 0);
    Sci_Position lineStart = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
    Sci_Position beforeLength = caret - lineStart;
    if (beforeLength <= 0) return 0;
    char *before = (char *)malloc((size_t)beforeLength + 1);
    if (!before) return 0;
    struct Sci_TextRangeFull range;
    range.chrg.cpMin = lineStart;
    range.chrg.cpMax = caret;
    range.lpstrText = before;
    SendMessageA(hEdit, SCI_GETTEXTRANGEFULL, 0, (LPARAM)&range);
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
        }
    }
    int hasMarker = markerEnd > markerStart;
    int hasQuote = quoteEnd > indentEnd;
    if (!hasMarker && !hasQuote) { free(before); return 0; }
    int contentStart = hasMarker ? markerEnd : quoteEnd;
    int contentEmpty = 1;
    for (Sci_Position p = contentStart; p < beforeLength; p++) {
        if (before[p] != ' ' && before[p] != '\t') { contentEmpty = 0; break; }
    }

    SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
    if (contentEmpty) {
        /* An empty list item or quote terminates that construct. Nested quote
           prefixes remain when terminating a list nested inside a quote. */
        int keep = hasMarker ? quoteEnd : indentEnd;
        SendMessageA(hEdit, SCI_SETSEL, (WPARAM)(lineStart + keep), (LPARAM)caret);
        SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)"");
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
            SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)insert);
            free(insert);
        }
    }
    SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
    free(before);
    return 1;
}

static int ends_with_word(const char *text, Sci_Position length, const char *word) {
    int wordLength = (int)strlen(word);
    if (length < wordLength || memcmp(text + length - wordLength, word,
                                      (size_t)wordLength) != 0) return 0;
    return length == wordLength || !(isalnum((unsigned char)text[length - wordLength - 1]) ||
                                      text[length - wordLength - 1] == '_');
}

static int style_allows_indent(HWND hEdit, Sci_Position position) {
    int style = (int)SendMessageA(hEdit, SCI_GETSTYLEAT, (WPARAM)position, 0);
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

static int line_opens_indent(HWND hEdit, const char *before, Sci_Position contentStart,
                             Sci_Position trimmedEnd, Sci_Position lineStart, char *closingOut) {
    if (closingOut) *closingOut = 0;
    if (trimmedEnd <= contentStart || before[contentStart] == '#') return 0;
    if (!style_allows_indent(hEdit, lineStart + trimmedEnd - 1)) return 0;
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
static int smart_indented_newline(HWND hEdit) {
    Sci_Position selectionStart = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selectionEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (selectionStart != selectionEnd) return 0;
    Sci_Position caret = selectionEnd;
    Sci_Position lineNumber = (Sci_Position)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)caret, 0);
    Sci_Position lineStart = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)lineNumber, 0);
    Sci_Position lineEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETLINEENDPOSITION, (WPARAM)lineNumber, 0);
    Sci_Position beforeLength = caret - lineStart;
    if (beforeLength < 0) return 0;
    char *before = (char *)malloc((size_t)beforeLength + 1);
    if (!before) return 0;
    struct Sci_TextRangeFull range = { { lineStart, caret }, before };
    SendMessageA(hEdit, SCI_GETTEXTRANGEFULL, 0, (LPARAM)&range);
    before[beforeLength] = 0;

    int indentLength = 0;
    while (indentLength < beforeLength &&
           (before[indentLength] == ' ' || before[indentLength] == '\t')) indentLength++;
    Sci_Position trimmedEnd = beforeLength;
    while (trimmedEnd > indentLength &&
           (before[trimmedEnd - 1] == ' ' || before[trimmedEnd - 1] == '\t')) trimmedEnd--;
    char closing = 0;
    int addIndent = line_opens_indent(hEdit, before, indentLength, trimmedEnd, lineStart, &closing);

    Sci_Position after = caret;
    while (after < lineEnd) {
        char character = (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)after, 0);
        if (character != ' ' && character != '\t') break;
        after++;
    }
    int pairedClose = addIndent && closing && after < lineEnd &&
        (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)after, 0) == closing;
    if (pairedClose) {
        Sci_Position rest = after + 1;
        while (rest < lineEnd) {
            char character = (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)rest, 0);
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
    SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
    if (pairedClose && after > caret)
        SendMessageA(hEdit, SCI_SETSEL, (WPARAM)caret, (LPARAM)after);
    SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)insert);
    if (pairedClose)
        SendMessageA(hEdit, SCI_SETSEL, (WPARAM)(caret + (int)firstLength),
                     (LPARAM)(caret + (int)firstLength));
    SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
    free(insert);
    free(before);
    return 1;
}

static int smart_newline(HWND hEdit) {
    /* Both copy the line up to the caret - in a large file with few line
       breaks, that can be most of the document on every Enter. */
    if (g_largeFile) return 0;
    if (g_activeLanguage == LANG_MARKDOWN && smart_markdown_newline(hEdit)) return 1;
    return smart_indented_newline(hEdit);
}

/* Typing a closing brace on an otherwise blank indented line brings it back
   one indentation level. Existing auto-paired closers are still skipped by
   smart_pair_character() instead of being replaced. */
static int smart_closing_indent(HWND hEdit, char typed) {
    int valid = typed == '}' && (g_activeLanguage == LANG_CPP ||
        g_activeLanguage == LANG_BASH || g_activeLanguage == LANG_POWERSHELL ||
        g_activeLanguage == LANG_JSON);
    if (typed == ']' && g_activeLanguage == LANG_JSON) valid = 1;
    if (!valid) return 0;
    Sci_Position start = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position end = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (start != end || (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)end, 0) == typed)
        return 0;
    Sci_Position line = (Sci_Position)SendMessageA(hEdit, SCI_LINEFROMPOSITION, (WPARAM)start, 0);
    Sci_Position lineStart = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
    for (Sci_Position position = lineStart; position < start; position++) {
        char character = (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)position, 0);
        if (character != ' ' && character != '\t') return 0;
    }
    if (start == lineStart) return 0;
    Sci_Position removeStart = start;
    if ((char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)(start - 1), 0) == '\t') {
        removeStart--;
    } else {
        int tabWidth = (int)SendMessageA(hEdit, SCI_GETTABWIDTH, 0, 0);
        if (tabWidth < 1) tabWidth = 4;
        while (removeStart > lineStart && start - removeStart < tabWidth &&
               (char)SendMessageA(hEdit, SCI_GETCHARAT,
                                  (WPARAM)(removeStart - 1), 0) == ' ') removeStart--;
    }
    char replacement[2] = { typed, 0 };
    SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(hEdit, SCI_SETSEL, (WPARAM)removeStart, (LPARAM)start);
    SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)replacement);
    SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
    return 1;
}

static int smart_pair_character(HWND hEdit, char typed) {
    char closing = typed == '(' ? ')' : typed == '[' ? ']' : typed == '{' ? '}' : typed == '`' ? '`' : 0;
    Sci_Position start = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position end = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (typed == '`' && start == end) {
        if ((char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)end, 0) == '`') {
            SendMessageA(hEdit, SCI_SETSEL, (WPARAM)(end + 1), (LPARAM)(end + 1));
            return 1;
        }
        if (start >= 2 && (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)(start - 1), 0) == '`' &&
            (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)(start - 2), 0) == '`') return 0;
    }
    if (!closing) {
        if ((typed == ')' || typed == ']' || typed == '}') && start == end &&
            (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)end, 0) == typed) {
            SendMessageA(hEdit, SCI_SETSEL, (WPARAM)(end + 1), (LPARAM)(end + 1));
            return 1;
        }
        return 0;
    }
    Sci_Position length = end - start;
    char *selected = (char *)malloc((size_t)length + 1);
    if (!selected) return 0;
    if (length > 0) SendMessageA(hEdit, SCI_GETSELTEXT, 0, (LPARAM)selected);
    else selected[0] = 0;
    size_t cap = (size_t)length + 3;
    char *replacement = (char *)malloc(cap);
    if (!replacement) { free(selected); return 0; }
    replacement[0] = typed;
    if (length > 0) memcpy(replacement + 1, selected, (size_t)length);
    replacement[length + 1] = closing;
    replacement[length + 2] = 0;
    SendMessageA(hEdit, SCI_BEGINUNDOACTION, 0, 0);
    SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)replacement);
    if (length > 0) SendMessageA(hEdit, SCI_SETSEL, (WPARAM)(start + 1), (LPARAM)(start + 1 + length));
    else SendMessageA(hEdit, SCI_SETSEL, (WPARAM)(start + 1), (LPARAM)(start + 1));
    SendMessageA(hEdit, SCI_ENDUNDOACTION, 0, 0);
    free(replacement);
    free(selected);
    return 1;
}

static int smart_pair_backspace(HWND hEdit) {
    Sci_Position start = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position end = (Sci_Position)SendMessageA(hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (start != end || start <= 0) return 0;
    char before = (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)(start - 1), 0);
    char after = (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)start, 0);
    if (!((before == '(' && after == ')') || (before == '[' && after == ']') ||
          (before == '{' && after == '}') || (before == '`' && after == '`'))) return 0;
    SendMessageA(hEdit, SCI_SETSEL, (WPARAM)(start - 1), (LPARAM)(start + 1));
    SendMessageA(hEdit, SCI_REPLACESEL, 0, (LPARAM)"");
    return 1;
}

static int show_spelling_context_menu(HWND hwnd, LPARAM lParam) {
    if (!g_spellCheckEnabled || !g_spellChecker ||
        (g_activeLanguage != LANG_MARKDOWN && g_activeLanguage != LANG_TEXT))
        return 0;

    POINT screenPoint;
    Sci_Position position;
    if ((int)GET_X_LPARAM(lParam) == -1 && (int)GET_Y_LPARAM(lParam) == -1) {
        position = (Sci_Position)SendMessageA(hwnd, SCI_GETCURRENTPOS, 0, 0);
        if (!GetCaretPos(&screenPoint)) screenPoint = (POINT){0, 0};
        ClientToScreen(hwnd, &screenPoint);
    } else {
        screenPoint.x = GET_X_LPARAM(lParam);
        screenPoint.y = GET_Y_LPARAM(lParam);
        POINT clientPoint = screenPoint;
        ScreenToClient(hwnd, &clientPoint);
        position = (Sci_Position)SendMessageA(hwnd, SCI_POSITIONFROMPOINTCLOSE,
                                     (WPARAM)clientPoint.x, (LPARAM)clientPoint.y);
    }
    if (position < 0) return 0;

    SendMessageA(hwnd, SCI_SETINDICATORCURRENT, INDICATOR_SPELLING, 0);
    if (!SendMessageA(hwnd, SCI_INDICATORVALUEAT, INDICATOR_SPELLING, position) && position > 0)
        position--;
    if (!SendMessageA(hwnd, SCI_INDICATORVALUEAT, INDICATOR_SPELLING, position)) return 0;

    Sci_Position start = (Sci_Position)SendMessageA(hwnd, SCI_INDICATORSTART, INDICATOR_SPELLING, position);
    Sci_Position end = (Sci_Position)SendMessageA(hwnd, SCI_INDICATOREND, INDICATOR_SPELLING, position);
    if (start < 0 || end <= start) return 0;
    Sci_Position byteLength = end - start;
    char *utf8Word = (char *)malloc((size_t)byteLength + 1);
    if (!utf8Word) return 1;
    struct Sci_TextRangeFull range = { { start, end }, utf8Word };
    SendMessageA(hwnd, SCI_GETTEXTRANGEFULL, 0, (LPARAM)&range);
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
            Sci_Position inserted = (Sci_Position)SendMessageA(hwnd, SCI_REPLACETARGET,
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

/* Ctrl+Alt+click jumps to the next occurrence of the word under the pointer -
   a lightweight stand-in for "go to definition" without a real per-language
   symbol index: for an identifier this typically lands on its declaration,
   and repeated Ctrl+Alt+clicks cycle through every other use in the file.
   (Plain Ctrl+click is already Scintilla's native gesture for adding a
   multi-cursor selection - see SCI_SETMULTIPLESELECTION in configure_scintilla()
   - so this needs its own modifier combination instead.)
   Returns 0 (unhandled, so the caller falls back to a normal click) when
   there's no word under the pointer to jump from. */
static int ctrl_click_jump_to_occurrence(HWND hEdit, int x, int y) {
    Sci_Position position = (Sci_Position)SendMessageA(hEdit, SCI_POSITIONFROMPOINTCLOSE, (WPARAM)x, (LPARAM)y);
    if (position < 0) return 0;

    Sci_Position wordStart = (Sci_Position)SendMessageA(hEdit, SCI_WORDSTARTPOSITION, (WPARAM)position, (LPARAM)1);
    Sci_Position wordEnd = (Sci_Position)SendMessageA(hEdit, SCI_WORDENDPOSITION, (WPARAM)position, (LPARAM)1);
    if (wordEnd <= wordStart) return 0;

    Sci_Position wordLength = wordEnd - wordStart;
    char *word = (char *)malloc((size_t)wordLength + 1);
    if (!word) return 1;
    struct Sci_TextRangeFull range = { { wordStart, wordEnd }, word };
    SendMessageA(hEdit, SCI_GETTEXTRANGEFULL, 0, (LPARAM)&range);
    word[wordLength] = 0;

    Sci_Position docLength = (Sci_Position)SendMessageA(hEdit, SCI_GETLENGTH, 0, 0);
    SendMessageA(hEdit, SCI_SETSEARCHFLAGS, SCFIND_WHOLEWORD | SCFIND_MATCHCASE, 0);

    /* Search past the clicked word first, wrapping back to the start of the
       document if nothing turns up after it - the same wrap-around shape
       find_in_edit() uses for its "down" search. */
    SendMessageA(hEdit, SCI_SETTARGETSTART, (WPARAM)wordEnd, 0);
    SendMessageA(hEdit, SCI_SETTARGETEND, (WPARAM)docLength, 0);
    Sci_Position foundAt = (Sci_Position)SendMessageA(hEdit, SCI_SEARCHINTARGET, (WPARAM)wordLength, (LPARAM)word);
    if (foundAt < 0) {
        SendMessageA(hEdit, SCI_SETTARGETSTART, 0, 0);
        SendMessageA(hEdit, SCI_SETTARGETEND, (WPARAM)wordStart, 0);
        foundAt = (Sci_Position)SendMessageA(hEdit, SCI_SEARCHINTARGET, (WPARAM)wordLength, (LPARAM)word);
    }
    free(word);

    if (foundAt >= 0) {
        Sci_Position foundEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETTARGETEND, 0, 0);
        SendMessageA(hEdit, SCI_SETSEL, (WPARAM)foundAt, (LPARAM)foundEnd);
        SendMessageA(hEdit, SCI_SCROLLCARET, 0, 0);
    } else {
        MessageBeep(MB_ICONASTERISK); /* only occurrence in the file */
    }
    return 1;
}

static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR id, DWORD_PTR data) {
    (void)data;
    if (msg == WM_CHAR) {
        if (wParam == '\t' && smart_tab(hwnd, (GetKeyState(VK_SHIFT) & 0x8000) != 0)) return 0;
        if (wParam == '\r' && smart_newline(hwnd)) return 0;
        if (wParam == '\b' && smart_pair_backspace(hwnd)) return 0;
        if (wParam < 128 && smart_closing_indent(hwnd, (char)wParam)) return 0;
        if (wParam < 128 && smart_pair_character(hwnd, (char)wParam)) return 0;
    } else if (msg == WM_CONTEXTMENU) {
        if (show_spelling_context_menu(hwnd, lParam)) return 0;
    } else if (msg == WM_LBUTTONDOWN) {
        if ((wParam & MK_CONTROL) && (GetKeyState(VK_MENU) & 0x8000) &&
            ctrl_click_jump_to_occurrence(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)))
            return 0;
    } else if (msg == WM_NCDESTROY) {
        /* This proc is shared by both g_hEdit (subclass id 2) and
           g_hSplitEdit (id 3) - remove whichever one this hwnd was actually
           registered under rather than a hardcoded id. */
        RemoveWindowSubclass(hwnd, EditSubclassProc, id);
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
        fill_rect_color(buffer, &client, g_editorDarkMode ? THEME_DARK_BG : RGB(0xF6, 0xF8, 0xFA));

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

/* explorer_init()'s callback: file_explorer.c cannot call open_file_in_tab()
   by name since it's static to this translation unit. */
static void explorer_request_open(const WCHAR *path) {
    open_file_in_tab(path);
}

static void explorer_request_change_root(const WCHAR *path) {
    adopt_workspace_folder(g_hMainWnd, path);
}

static void initialize_main_window(HWND hwnd) {
    g_hMainWnd = hwnd;

    /* The process preference is selected before window creation; complete the
       per-window opt-in before attaching the menu so its first paint matches. */
    apply_menu_theme(hwnd, g_editorDarkMode);

    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(hwnd, hdc);

    if (!g_focusMode) SetMenu(hwnd, build_menu());
    create_toolbar(hwnd);

    g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd, (HMENU)ID_STATUS, GetModuleHandleW(NULL), NULL);
    SetWindowSubclass(g_hStatus, StatusSubclassProc, 1, 0);

    /* LBS_HASSTRINGS keeps the listbox owning a copy of each LB_ADDSTRING
       text (as before) so draw_themed_listbox_item() can fetch it back via
       LB_GETTEXT - without it, an owner-draw listbox instead treats the
       LB_ADDSTRING data as an opaque per-item value, which here is a
       heap pointer that update_outline() frees right after inserting it. */
    g_hOutline = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
        LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        0, 0, 0, 0, hwnd, (HMENU)ID_OUTLINE, GetModuleHandleW(NULL), NULL);
    themed_scrollbar_attach(g_hOutline, THEMED_SCROLL_LISTBOX);

    g_hDebugPanel = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_CLIPCHILDREN,
        0, 0, 0, 0, hwnd, (HMENU)ID_DEBUG_PANEL, GetModuleHandleW(NULL), NULL);
    SetWindowSubclass(g_hDebugPanel, DebugPanelSubclassProc, 1, 0);
    g_hDebugStatus = CreateWindowExW(0, L"STATIC", L"Debugger",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_LEFTNOWORDWRAP,
        0, 0, 0, 0, g_hDebugPanel, (HMENU)ID_DEBUG_STATUS, GetModuleHandleW(NULL), NULL);
    g_hDebugContinue = CreateWindowExW(0, L"BUTTON", L"Continue  F6",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, g_hDebugPanel, (HMENU)IDM_DEBUG_CONTINUE, GetModuleHandleW(NULL), NULL);
    g_hDebugStepOver = CreateWindowExW(0, L"BUTTON", L"Step Over  F10",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, g_hDebugPanel, (HMENU)IDM_DEBUG_STEP_OVER, GetModuleHandleW(NULL), NULL);
    g_hDebugStepInto = CreateWindowExW(0, L"BUTTON", L"Step Into  F11",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, g_hDebugPanel, (HMENU)IDM_DEBUG_STEP_INTO, GetModuleHandleW(NULL), NULL);
    g_hDebugStepOut = CreateWindowExW(0, L"BUTTON", L"Step Out  Shift+F11",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, g_hDebugPanel, (HMENU)IDM_DEBUG_STEP_OUT, GetModuleHandleW(NULL), NULL);
    g_hDebugClose = CreateWindowExW(0, L"BUTTON", L"X",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, g_hDebugPanel, (HMENU)ID_DEBUG_CLOSE, GetModuleHandleW(NULL), NULL);
    g_hDebugVariables = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
        LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
        0, 0, 0, 0, g_hDebugPanel, (HMENU)ID_DEBUG_VARIABLES, GetModuleHandleW(NULL), NULL);
    ListView_SetExtendedListViewStyle(g_hDebugVariables,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    LVCOLUMNW debugColumn = {0};
    debugColumn.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    debugColumn.pszText = L"Name"; debugColumn.cx = MulDiv(220, dpi, 96);
    SendMessageW(g_hDebugVariables, LVM_INSERTCOLUMNW, 0, (LPARAM)&debugColumn);
    debugColumn.pszText = L"Value"; debugColumn.cx = MulDiv(640, dpi, 96); debugColumn.iSubItem = 1;
    SendMessageW(g_hDebugVariables, LVM_INSERTCOLUMNW, 1, (LPARAM)&debugColumn);
    HWND debugHeader = ListView_GetHeader(g_hDebugVariables);
    if (debugHeader) SetWindowSubclass(debugHeader, DebugHeaderSubclassProc, 1, 0);
    HFONT debugFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND debugControls[] = { g_hDebugStatus, g_hDebugContinue, g_hDebugStepOver,
                             g_hDebugStepInto, g_hDebugStepOut, g_hDebugClose,
                             g_hDebugVariables };
    for (size_t i = 0; i < sizeof(debugControls) / sizeof(debugControls[0]); i++)
        SendMessageW(debugControls[i], WM_SETFONT, (WPARAM)debugFont, TRUE);
    apply_debug_panel_theme();

    explorer_init(hwnd, explorer_request_open, show_text_input, reveal_path_in_explorer,
                  explorer_path_renamed, explorer_request_change_root);
    if (g_explorerVisible && g_currentWorkspaceFolder) explorer_set_root(g_currentWorkspaceFolder);

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
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd, (HMENU)ID_EDIT, GetModuleHandleA(NULL), NULL);
    SetWindowSubclass(g_hEdit, EditSubclassProc, 2, 0);
    configure_scintilla(g_hEdit);

    g_hMinimap = CreateWindowExA(0, "Scintilla", "",
        WS_CHILD | (g_minimapVisible ? WS_VISIBLE : 0),
        0, 0, 0, 0, hwnd, (HMENU)ID_MINIMAP, GetModuleHandleA(NULL), NULL);
    SetWindowSubclass(g_hMinimap, MinimapSubclassProc, 1, 0);
    configure_minimap(g_hMinimap);

    g_hSplitEdit = CreateWindowExA(0, "Scintilla", "",
        WS_CHILD | (g_splitEditorVisible ? WS_VISIBLE : 0),
        0, 0, 0, 0, hwnd, (HMENU)ID_SPLIT_EDIT, GetModuleHandleA(NULL), NULL);
    SetWindowSubclass(g_hSplitEdit, EditSubclassProc, 3, 0);
    configure_scintilla(g_hSplitEdit);
    g_lastEditView = g_hEdit;

    apply_editor_font(g_fontFaceName, g_fontPointSize);

    g_documentCount = 1;
    g_activeDocument = 0;
    memset(&g_documents[0], 0, sizeof(g_documents[0]));
    g_documents[0].sciDocument = (sptr_t)SendMessageA(g_hEdit, SCI_GETDOCPOINTER, 0, 0);
    SendMessageA(g_hEdit, SCI_ADDREFDOCUMENT, 0, (LPARAM)g_documents[0].sciDocument);
    SendMessageA(g_hMinimap, SCI_SETDOCPOINTER, 0, (LPARAM)g_documents[0].sciDocument);
    SendMessageA(g_hSplitEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_documents[0].sciDocument);
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

    SetTimer(hwnd, IDT_FILE_WATCH, 1500, NULL);
    SetTimer(hwnd, IDT_AUTOSAVE, 5000, NULL);
    update_status_bar();
    update_outline();
    layout(hwnd);
}

/* Runs the slow, non-essential startup work that would otherwise keep the
   window from appearing promptly: spell-checker COM activation, the
   WebView2 environment/controller handshake, and reopening the
   command-line file or the previous session's tabs. WinMain posts
   WM_APP_DEFERRED_INIT right after creating the window instead of running
   this inline, so it runs on the first pass through the message loop -
   after the window is already shown - rather than stretching out the
   startup wait cursor while nothing is on screen yet. */
static void run_deferred_startup_init(void) {
    detect_high_contrast();
    if (g_comInitialized) init_spell_checker();
    init_webview();
    if (g_pendingCliFile) {
        load_file(g_pendingCliFile);
        free(g_pendingCliFile);
        g_pendingCliFile = NULL;
    } else if (!restore_recovery_snapshot()) {
        restore_session_documents();
    }
}

static void handle_editor_notification(HWND hwnd, NMHDR *notification) {
    int fromMain = notification->hwndFrom == g_hEdit;
    int fromSplit = g_hSplitEdit && notification->hwndFrom == g_hSplitEdit;
    if (!fromMain && !fromSplit) return;

    struct SCNotification *scn = (struct SCNotification *)notification;
    if (notification->code == SCN_UPDATEUI) {
        g_lastEditView = notification->hwndFrom;
        if ((scn->updated & SC_UPDATE_V_SCROLL) && g_editorVisible && g_previewVisible) {
            if (g_ignoreNextEditorScroll) g_ignoreNextEditorScroll = 0;
            else SetTimer(hwnd, IDT_SCROLL_SYNC, 35, NULL);
        }
        update_brace_highlight(notification->hwndFrom);
        update_status_position();
        update_minimap_viewport();
        return;
    }
    if (notification->code == SCN_CHARADDED) {
        g_lastEditView = notification->hwndFrom;
        update_word_completions(notification->hwndFrom);
        return;
    }
    if (notification->code == SCN_MARGINCLICK) {
        /* scn->position is a document offset, and bookmarks/breakpoints are
           markers on the shared document - shared by both panes - so this
           works the same regardless of which pane the click came from. */
        if (scn->margin == MARGIN_BOOKMARKS) {
            Sci_Position line = (Sci_Position)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)scn->position, 0);
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

    /* Everything below is document-level bookkeeping, not view-level, so it
       applies regardless of which pane the edit came from - both panes are
       views of the same shared document (without this, typing only in the
       split pane would never mark the tab dirty or stale the recovery
       snapshot, silently losing track of real unsaved edits). */
    if (!g_switchingDocument && !g_dirty) set_dirty(1);
    if (!g_switchingDocument) g_recoverySnapshotStale = 1;
    if (!g_switchingDocument && !g_largeFile) {
        Sci_Position length = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
        Sci_Position threshold = (Sci_Position)g_largeFileThresholdMB * 1024 * 1024;
        if (length >= threshold) {
            g_largeFile = 1;
            PostMessageW(hwnd, WM_APP_APPLY_LARGE_FILE_MODE, (WPARAM)g_activeDocument, 0);
        }
    }
    /* An HTML preview shows the saved file (refreshed from save_to_path()),
       so re-rendering it after each pause in typing would just reload the
       same page; every other language previews the unsaved text. */
    if (!g_largeFile && g_previewVisible && g_activeLanguage != LANG_HTML)
        SetTimer(hwnd, IDT_PREVIEW_UPDATE, 150, NULL);
    if (!g_largeFile) schedule_spell_check();
    /* update_status_bar() does a full-document SCI_GETTEXT copy plus an O(n)
       word-count scan - too costly to run on every single keystroke.
       Debounced like the outline update just below it, so a burst of typing
       pays for one scan at the pause instead of one per character. */
    SetTimer(hwnd, IDT_STATUS_UPDATE, 200, NULL);
    if (!g_largeFile) SetTimer(hwnd, IDT_OUTLINE_UPDATE, 500, NULL);
    if (scn->linesAdded != 0) {
        update_linenumber_margin_width(g_hEdit);
        if (g_hSplitEdit) update_linenumber_margin_width(g_hSplitEdit);
    }
}

static void destroy_main_window(HWND hwnd) {
    save_all_document_breakpoints();
    save_layout_settings();
    KillTimer(hwnd, IDT_PREVIEW_UPDATE);
    KillTimer(hwnd, IDT_FILE_WATCH);
    KillTimer(hwnd, IDT_SCROLL_SYNC);
    KillTimer(hwnd, IDT_SPELL_CHECK);
    KillTimer(hwnd, IDT_AUTOSAVE);
    KillTimer(hwnd, IDT_OUTLINE_UPDATE);
    KillTimer(hwnd, IDT_STATUS_UPDATE);
    KillTimer(hwnd, IDT_PRINT_PREVIEW);
    KillTimer(hwnd, IDT_HTML_EXPORT);
    KillTimer(hwnd, IDT_EXPLORER_REFRESH);
    explorer_shutdown();
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
    for (int i = 0; i < g_wikiBackCount; i++) free(g_wikiBackStack[i]);
    PostQuitMessage(0);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_msgFindReplace) {
        handle_findmsg((FINDREPLACEW *)lParam);
        return 0;
    }
    switch (msg) {
        case WM_APP_APPLY_LARGE_FILE_MODE:
            apply_large_file_mode_after_growth((int)wParam);
            return 0;
        case WM_CREATE:
            initialize_main_window(hwnd);
            return 0;
        case WM_NCPAINT: {
            LRESULT result = DefWindowProcW(hwnd, msg, wParam, lParam);
            erase_menu_bottom_seam(hwnd);
            return result;
        }
        /* Regaining focus repaints the non-client area (active vs. inactive
           caption colour) straight from DefWindowProc's own WM_NCACTIVATE
           handling, without a separate WM_NCPAINT reaching this WndProc -
           so the seam has to be re-painted here too, or it comes back every
           time the window is refocused. */
        case WM_NCACTIVATE: {
            LRESULT result = DefWindowProcW(hwnd, msg, wParam, lParam);
            erase_menu_bottom_seam(hwnd);
            return result;
        }
        case WM_CTLCOLORSTATIC: {
            if ((HWND)lParam == g_hPreview && g_previewBkBrush) {
                HDC hdcStatic = (HDC)wParam;
                SetBkColor(hdcStatic, preview_bg_color());
                SetTextColor(hdcStatic, g_previewDarkMode ? THEME_DARK_FG : RGB(0x00, 0x00, 0x00));
                return (LRESULT)g_previewBkBrush;
            }
            /* g_hDebugPanel is itself a STATIC child, so its class asks the
               main window for a brush after WM_ERASEBKGND. Returning the app
               brush here prevents the class's default COLOR_WINDOW paint
               from putting white seams back over the themed background. */
            if ((HWND)lParam == g_hDebugPanel && g_mainBkBrush) {
                HDC hdcStatic = (HDC)wParam;
                SetBkColor(hdcStatic, theme_bg_color());
                SetTextColor(hdcStatic, theme_fg_color());
                return (LRESULT)g_mainBkBrush;
            }
            break;
        }
        /* draw_themed_listbox_item() themes every item's own rectangle, but the
           blank strip below the last item (and the whole control before any
           items exist) is still erased by the system using this message's
           returned brush - without handling it, that strip stays white in
           dark mode regardless of the owner-draw above it. */
        case WM_CTLCOLORLISTBOX: {
            if ((HWND)lParam == g_hOutline && g_mainBkBrush) {
                HDC hdcList = (HDC)wParam;
                SetBkColor(hdcList, theme_bg_color());
                SetTextColor(hdcList, theme_fg_color());
                return (LRESULT)g_mainBkBrush;
            }
            break;
        }
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lParam;
            if (mis->CtlType == ODT_MENU && mis->itemData) {
                measure_main_menu_item(mis);
                return TRUE;
            }
            if (mis->CtlType == ODT_LISTBOX && mis->CtlID == ID_OUTLINE) {
                measure_themed_listbox_item(g_hOutline, mis);
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
            if (dis->CtlType == ODT_LISTBOX && dis->hwndItem == g_hOutline) {
                draw_themed_listbox_item(g_hOutline, dis);
                return TRUE;
            }
            break;
        }
        /* g_hEdit/g_hPreview/the toolbar buttons cover the rest of the client
           area themselves, so this only ever shows through on the toolbar
           strip's margins and the splitter gap. */
        case WM_ERASEBKGND: {
            /* The class has no background brush, so this must always paint. */
            if (!g_mainBkBrush) g_mainBkBrush = CreateSolidBrush(theme_bg_color());
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wParam, &rc, g_mainBkBrush);
            /* The splitter gaps are otherwise the same colour as the panes
               either side of them (both share the main background), so
               without this they're invisible rather than reading as
               draggable dividers. */
            paint_splitter_gap((HDC)wParam, &g_splitterRect);
            paint_splitter_gap((HDC)wParam, &g_explorerSplitterRect);
            if (g_hSplitEdit && g_splitEditorVisible)
                paint_splitter_gap((HDC)wParam, &g_splitEditorSplitterRect);
            return 1;
        }
        case WM_TIMER:
            if (wParam == IDT_HTML_EXPORT) {
                ExportDone_Invoke(NULL, E_FAIL, NULL);
                return 0;
            }
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
            if (wParam == IDT_STATUS_UPDATE) {
                KillTimer(hwnd, IDT_STATUS_UPDATE);
                update_status_bar();
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
            if (wParam == IDT_EXPLORER_REFRESH) {
                KillTimer(hwnd, IDT_EXPLORER_REFRESH);
                explorer_handle_change_message();
                return 0;
            }
            break;
        case WM_SIZE:
            layout(hwnd);
            /* layout() recomputes g_splitterRect, but resizing doesn't
               automatically re-run WM_ERASEBKGND (which is what actually
               paints the splitter gap) for its new position/size. */
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        case WM_SETCURSOR: {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (splitter_contains_x(pt.x) || explorer_splitter_contains_x(pt.x)) {
                SetCursor(LoadCursorA(NULL, (LPCSTR)IDC_SIZEWE));
                return TRUE;
            }
            if (split_editor_splitter_contains(pt.x, pt.y)) {
                SetCursor(LoadCursorA(NULL, (LPCSTR)IDC_SIZENS));
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            if (splitter_contains_x(x)) {
                g_dragging = 1;
                SetCapture(hwnd);
            } else if (explorer_splitter_contains_x(x)) {
                g_draggingExplorer = 1;
                SetCapture(hwnd);
            } else if (split_editor_splitter_contains(x, y)) {
                g_draggingSplitEditor = 1;
                SetCapture(hwnd);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            /* Mouse moves arrive far faster than the panes can be resized, so
               skip the relayout when the splitter has not actually moved.
               g_splitterPos is relative to the content area right of the
               explorer panel, but the mouse position is an absolute client
               x, so it needs the same offset subtracted back out. */
            int splitterMouseX = GET_X_LPARAM(lParam) - explorer_content_x(g_focusMode);
            if (g_dragging && splitterMouseX != g_splitterPos) {
                g_splitterPos = splitterMouseX;
                layout(hwnd);
                /* See the WM_SIZE handler: layout() alone doesn't re-run
                   WM_ERASEBKGND, which is what paints the splitter gap. */
                InvalidateRect(hwnd, NULL, TRUE);
            } else if (g_draggingExplorer && GET_X_LPARAM(lParam) != g_explorerWidth) {
                g_explorerWidth = GET_X_LPARAM(lParam);
                layout(hwnd);
                InvalidateRect(hwnd, NULL, TRUE);
            } else if (g_draggingSplitEditor) {
                int localY = GET_Y_LPARAM(lParam) - g_splitEditorRegionY;
                if (localY != g_splitEditorPos) {
                    g_splitEditorPos = localY;
                    layout(hwnd);
                    InvalidateRect(hwnd, NULL, TRUE);
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            /* At most one of these three is ever set at a time (WM_LBUTTONDOWN's
               own if/else-if chain is mutually exclusive), so ending whichever
               drag was in progress is the same three lines regardless of which. */
            if (g_dragging || g_draggingExplorer || g_draggingSplitEditor) {
                g_dragging = g_draggingExplorer = g_draggingSplitEditor = 0;
                ReleaseCapture();
                save_layout_settings();
            }
            return 0;
        }
        case WM_DROPFILES:
            handle_dropfiles((HDROP)wParam);
            return 0;
        case WM_SYSCOLORCHANGE:
            detect_high_contrast();
            return 0;
        case WM_SETTINGCHANGE:
            if (lParam && wcscmp((const WCHAR *)lParam, L"ImmersiveColorSet") == 0) detect_high_contrast();
            break;
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
            } else if (g_hExplorerTree && notification->hwndFrom == g_hExplorerTree) {
                explorer_handle_notify(hwnd, notification);
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
        case WM_APP_DEFERRED_INIT:
            run_deferred_startup_init();
            return 0;
        case MARKEDIT_WM_EXPLORER_CHANGED:
            /* Debounced rather than refreshing immediately: a burst of
               changes (e.g. a git operation) collapses into one refresh
               shortly after the burst ends, same idea as the outline/status
               update timers above. */
            SetTimer(hwnd, IDT_EXPLORER_REFRESH, 400, NULL);
            return 0;
        case WM_CLOSE:
            do_exit();
            return 0;
        case WM_QUERYENDSESSION:
            save_all_document_breakpoints();
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

    INITCOMMONCONTROLSEX commonControls = { sizeof(commonControls),
        ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&commonControls);

    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    WCHAR *cliFile = (argv && argc > 1) ? wcsdup_heap(argv[1]) : NULL;
    if (argv) LocalFree(argv);
    g_commandLinePreviewMode = cliFile != NULL;
    int cliFileIsMarkdown = cliFile && language_for_path(cliFile)->id == LANG_MARKDOWN;
    if (cliFileIsMarkdown) {
        /* command-line launch to view a Markdown file: show just the rendered preview */
        g_previewVisible = 1;
        g_editorVisible = 0;
    } else {
        /* plain launch, or a command-line file that isn't Markdown: show just the editor */
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
    apply_menu_theme(NULL, g_editorDarkMode);
    load_layout_settings();
    load_find_settings();
    load_compiler_settings();
    if (cliFileIsMarkdown) {
        /* File-association/command-line launches of a Markdown file are
           preview-only for this session, without overwriting the user's
           normal panel visibility. */
        g_previewVisible = 1;
        g_editorVisible = 0;
    } else if (g_commandLinePreviewMode) {
        /* Command-line launch of a non-Markdown file: show it in the editor
           for this session, same reasoning as above but the other pane. */
        g_previewVisible = 0;
        g_editorVisible = 1;
    }
    recent_list_load(&g_recentFileList);
    prune_missing_recent_files();
    recent_list_load(&g_recentFolderList);
    load_workspace_settings();

    HRESULT comResult = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    g_comInitialized = SUCCEEDED(comResult);

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
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, L"MarkEditWndClass", L"Untitled.txt - MarkEdit",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        windowX, windowY, windowWidth, windowHeight,
        NULL, NULL, hInstance, NULL);

    if (hIconBig) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    if (hIconSmall) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);

    g_pendingCliFile = cliFile;
    PostMessageW(hwnd, WM_APP_DEFERRED_INIT, 0, 0);

    ACCEL accels[] = {
        { FVIRTKEY | FCONTROL, 'N', IDM_FILE_NEW },
        { FVIRTKEY | FCONTROL, 'O', IDM_FILE_OPEN },
        { FVIRTKEY | FCONTROL, 'K', IDM_FILE_QUICK_OPEN },
        { FVIRTKEY | FCONTROL | FSHIFT, 'P', IDM_EDIT_COMMAND_PALETTE },
        { FVIRTKEY | FCONTROL, 'S', IDM_FILE_SAVE },
        { FVIRTKEY | FCONTROL, 'P', IDM_FILE_PRINT },
        { FVIRTKEY | FCONTROL | FSHIFT, 'S', IDM_FILE_SAVEAS },
        { FVIRTKEY | FCONTROL | FALT, 'S', IDM_FILE_SAVE_ALL },
        { FVIRTKEY | FCONTROL, 'A', IDM_EDIT_SELECTALL },
        { FVIRTKEY | FCONTROL, 'V', IDM_EDIT_PASTE },
        { FVIRTKEY | FCONTROL, 'Y', IDM_EDIT_REDO },
        { FVIRTKEY | FCONTROL, 'F', IDM_EDIT_FIND },
        { FVIRTKEY | FCONTROL, 'H', IDM_EDIT_REPLACE },
        { FVIRTKEY | FCONTROL, 'G', IDM_EDIT_GOTOLINE },
        { FVIRTKEY | FCONTROL, 'W', IDM_FILE_CLOSE },
        { FVIRTKEY | FCONTROL | FSHIFT, 'T', IDM_FILE_REOPEN_CLOSED },
        { FVIRTKEY | FALT, VK_LEFT, IDM_FILE_GO_BACK },
        { FVIRTKEY | FCONTROL, VK_TAB, IDM_VIEW_NEXT_TAB },
        { FVIRTKEY | FCONTROL | FSHIFT, VK_TAB, IDM_VIEW_PREVIOUS_TAB },
        { FVIRTKEY, VK_F3, IDM_EDIT_FINDNEXT },
        { FVIRTKEY | FCONTROL | FSHIFT, 'V', IDM_VIEW_TOGGLEPREVIEW },
        { FVIRTKEY | FCONTROL | FSHIFT, 'E', IDM_VIEW_TOGGLEEDITOR },
        { FVIRTKEY | FCONTROL | FSHIFT, 'X', IDM_VIEW_EXPLORER },
        { FVIRTKEY | FCONTROL | FSHIFT, VK_F11, IDM_VIEW_FOCUS_MODE },
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
        { FVIRTKEY | FCONTROL, VK_OEM_5, IDM_VIEW_TOGGLESPLIT }, /* Ctrl+\ */
        { FVIRTKEY | FCONTROL, VK_F6, IDM_VIEW_CYCLE_PANE },
        { FVIRTKEY | FCONTROL | FSHIFT, VK_F6, IDM_VIEW_CYCLE_PANE_PREV },
        { FVIRTKEY, VK_F10, IDM_DEBUG_STEP_OVER },
        { FVIRTKEY, VK_F11, IDM_DEBUG_STEP_INTO },
        { FVIRTKEY | FSHIFT, VK_F11, IDM_DEBUG_STEP_OUT },
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
    g_hAccel = hAccel;

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
