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
#include "markdown.h"
#include "menu_commands.h"
#include "path_utils.h"
#include "plugin_host.h"
#include "plugin_settings.h"
#include "settings.h"
#include "recent_list.h"
#include "text_codec.h"
#include "workspace_text.h"
#include "theme.h"
#include "themed_scrollbar.h"
#include "toolbar_icons.h"

#include <WebView2.h>
#include "../third_party/scintilla/include/Scintilla.h"
#include "../third_party/lexilla/include/Lexilla.h"
#include "../third_party/lexilla/include/SciLexer.h"

#include "markedit.h"

typedef struct AppWindows {
    HWND edit, preview, tabs, status, outline, minimap, splitEdit, lastEditView;
    HWND debugPanel, debugStatus, debugVariables;
    HWND debugContinue, debugStepOver, debugStepInto, debugStepOut, debugClose;
} AppWindows;
static AppWindows g_windows;
#define g_hEdit g_windows.edit
#define g_hPreview g_windows.preview
#define g_hTabs g_windows.tabs
#define g_hStatus g_windows.status
#define g_hOutline g_windows.outline
#define g_hMinimap g_windows.minimap
#define g_hSplitEdit g_windows.splitEdit
#define g_lastEditView g_windows.lastEditView
#define g_hDebugPanel g_windows.debugPanel
#define g_hDebugStatus g_windows.debugStatus
#define g_hDebugVariables g_windows.debugVariables
#define g_hDebugContinue g_windows.debugContinue
#define g_hDebugStepOver g_windows.debugStepOver
#define g_hDebugStepInto g_windows.debugStepInto
#define g_hDebugStepOut g_windows.debugStepOut
#define g_hDebugClose g_windows.debugClose
HWND g_hMainWnd;
/* The main window's accelerator table, forwarded into from the preview's
   AcceleratorKeyPressed handler - see AccelKey_Invoke() - since disabling
   WebView2's own browser accelerators (AreBrowserAcceleratorKeysEnabled)
   stops it acting on them but does not, by itself, deliver them to the
   host's message loop. Set once in WinMain. */
static HACCEL g_hAccel = NULL;
static int g_lastPluginLoadSucceeded = 1;
static HACCEL create_main_accelerators(void);
static void rebuild_main_accelerators(void);
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

typedef struct DocumentStore {
    Document documents[MAX_DOCUMENTS];
    int count;
    int active;
    unsigned nextUntitledNumber;
    int switching;
} DocumentStore;
static DocumentStore g_documentStore = { .nextUntitledNumber = 1 };
#define g_documents g_documentStore.documents
#define g_documentCount g_documentStore.count
#define g_activeDocument g_documentStore.active
#define g_nextUntitledNumber g_documentStore.nextUntitledNumber
#define g_switchingDocument g_documentStore.switching
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
int g_splitEditorRatio = 500; /* main pane size as thousandths of the editor region's split axis */
int g_splitEditorVertical = 0; /* nonzero places the second pane to the right instead of below */
static int g_splitEditorPos = -1; /* pixel position along the split axis, local to the editor region */
static RECT g_splitEditorSplitterRect = {0};

static HWND tracked_edit_view(void) {
    if (g_lastEditView == g_hSplitEdit && g_hSplitEdit && g_splitEditorVisible)
        return g_hSplitEdit;
    return g_hEdit;
}
static RECT g_splitterRect = {0}; /* the main editor/preview splitter's own bounds, for drawing its line */
static RECT g_explorerSplitterRect = {0}; /* the explorer panel's splitter, empty while the panel is hidden */
static POINT g_splitEditorRegionOrigin = {0}; /* window-relative editor origin, refreshed every layout() */
static int g_draggingSplitEditor = 0;
static int g_draggingExplorer = 0;
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

/* ---- copying text out of the Scintilla control ---- */

/* A NUL-terminated heap copy of [start, end), or NULL. Caller frees. */
static char *get_text_range(HWND hEdit, Sci_Position start, Sci_Position end) {
    if (end < start) return NULL;
    char *text = (char *)malloc((size_t)(end - start) + 1);
    if (!text) return NULL;
    struct Sci_TextRangeFull range = { { start, end }, text };
    SendMessageA(hEdit, SCI_GETTEXTRANGEFULL, 0, (LPARAM)&range);
    text[end - start] = 0;
    return text;
}

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
   list is populated by update_word_completions() rather than by the lexer. */
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
static int *linenumber_digit_cache(HWND hEdit) {
    return hEdit == g_hSplitEdit ? &g_splitLineNumberDigits : &g_lineNumberDigits;
}

static void update_linenumber_margin_width(HWND hEdit) {
    int *cache = linenumber_digit_cache(hEdit);
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

/* Resizes the gutter after a font or zoom change: the glyph width changed
   even if the line count (and so the cached digit count) didn't. */
static void remeasure_linenumber_margin(HWND hEdit) {
    *linenumber_digit_cache(hEdit) = 0;
    update_linenumber_margin_width(hEdit);
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
static void apply_language_to_view(HWND hEdit, const WCHAR *path) {
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

/* Applies the language for `path` to every editable view of the active
   document: the main pane and, when it exists, the split pane. */
static void apply_document_language(const WCHAR *path) {
    apply_language_to_view(g_hEdit, path);
    if (g_hSplitEdit) apply_language_to_view(g_hSplitEdit, path);
}

/* ---- Native Windows spell checking for Markdown prose. -----------------
   Scintilla stores UTF-8 byte positions while ISpellChecker reports UTF-16
   offsets, so each reported range is converted before applying an indicator.
   Lexer styles keep code, links, rules, and Markdown punctuation unmarked. */

static const GUID MARKEDIT_CLSID_SPELL_CHECKER_FACTORY =
    {0x7ab36653, 0x1796, 0x484b, {0xbd, 0xfa, 0xe7, 0x4f, 0x1d, 0xb7, 0xc1, 0xdc}};
static const GUID MARKEDIT_IID_SPELL_CHECKER_FACTORY =
    {0x8e018a9d, 0x2415, 0x4677, {0xbf, 0x08, 0x79, 0x4e, 0xa6, 0x1f, 0x94, 0xbb}};

static void copy_locale_name(WCHAR destination[LOCALE_NAME_MAX_LENGTH], const WCHAR *source) {
    wcsncpy(destination, source, LOCALE_NAME_MAX_LENGTH - 1);
    destination[LOCALE_NAME_MAX_LENGTH - 1] = 0;
}

static int create_spell_checker_for_language(const WCHAR *requested, int reportErrors) {
    ISpellCheckerFactory *factory = NULL;
    HRESULT hr = CoCreateInstance(&MARKEDIT_CLSID_SPELL_CHECKER_FACTORY, NULL,
        CLSCTX_INPROC_SERVER, &MARKEDIT_IID_SPELL_CHECKER_FACTORY, (void **)&factory);
    if (FAILED(hr) || !factory) return 0;

    WCHAR language[LOCALE_NAME_MAX_LENGTH] = L"";
    if (requested && requested[0]) {
        copy_locale_name(language, requested);
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
                copy_locale_name(language, first);
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
        copy_locale_name(g_spellLanguage, language);
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
    int count = 0;
    if (SUCCEEDED(ISpellCheckerFactory_get_SupportedLanguages(factory, &languages)) && languages) {
        int capacity = 0;
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
                                      L"Choose an installed Windows dictionary:", labels, count, L"Open");
    if (selected >= 0 && selected < count) {
        WCHAR locale[LOCALE_NAME_MAX_LENGTH];
        WCHAR *dash = wcsrchr(labels[selected], L'—');
        const WCHAR *source = dash ? dash + 1 : labels[selected];
        while (*source == L' ') source++;
        copy_locale_name(locale, source);
        if (create_spell_checker_for_language(locale, 1)) {
            g_spellCheckEnabled = 1;
            save_layout_settings(); /* persists the language and the enabled state */
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

    int wideLength = 0;
    WCHAR *wide = to_wide_n(utf8, (size_t)textLength, &wideLength);
    if (!wide || wideLength <= 0) {
        free(wide);
        free(utf8);
        return;
    }

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

/* Paints the menu bar's background with the current theme brush. */
static void set_menu_background(HMENU menu) {
    if (!g_menuBkBrush) return;
    MENUINFO info = {0};
    info.cbSize = sizeof(info);
    info.fMask = MIM_BACKGROUND;
    info.hbrBack = g_menuBkBrush;
    SetMenuInfo(menu, &info);
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
        if (menu) set_menu_background(menu);
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

static void configure_scintilla_editing(HWND hEdit) {
    SendMessageA(hEdit, SCI_SETCODEPAGE, SC_CP_UTF8, 0);
    SendMessageA(hEdit, SCI_SETEOLMODE, SC_EOL_CRLF, 0);
    apply_indentation(hEdit);
    SendMessageA(hEdit, SCI_SETINDENT, 0, 0); /* 0 = indent width follows tab width */
    SendMessageA(hEdit, SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT, 0);

    /* Additional carets edit in step with the main one; the smart-input
       handlers in EditSubclassProc still act on the main caret alone. */
    SendMessageA(hEdit, SCI_SETMULTIPLESELECTION, TRUE, 0);
    SendMessageA(hEdit, SCI_SETADDITIONALSELECTIONTYPING, TRUE, 0);
    SendMessageA(hEdit, SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH, 0);
}

static void configure_scintilla_view(HWND hEdit) {
    themed_scrollbar_attach(hEdit, THEMED_SCROLL_SCINTILLA);
    SendMessageA(hEdit, SCI_SETSCROLLWIDTHTRACKING, TRUE, 0);
    SendMessageA(hEdit, SCI_SETSCROLLWIDTH, 1, 0);
    apply_word_wrap(hEdit);
    apply_show_whitespace(hEdit);
    apply_indent_guides(hEdit);
    SendMessageA(hEdit, SCI_SETZOOM, (WPARAM)g_editorZoom, 0);
}

static void configure_scintilla_margins(HWND hEdit) {
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
}

static void configure_scintilla_feedback(HWND hEdit) {
    /* Syntax-highlight the source using one of Lexilla's bundled lexers
       (vendored trimmed to the languages listed in LANGUAGES - see
       third_party/lexilla). ILexer5* ownership passes to Scintilla once
       handed to SCI_SETILEXER. apply_language_to_view() picks the lexer
       from the file extension; new documents start out as Markdown.

       Explicit text colors, rather than relying on Scintilla's own defaults,
       so the caret-line highlight below is guaranteed to contrast with them.
       apply_editor_theme() sets the actual color values (light or dark),
       including the lexer's per-construct colors. */
    apply_language_to_view(hEdit, g_currentFile);

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

static void configure_scintilla(HWND hEdit) {
    configure_scintilla_editing(hEdit);
    configure_scintilla_view(hEdit);
    configure_scintilla_margins(hEdit);
    configure_scintilla_feedback(hEdit);
}

/* Apply a font face/point size to the Scintilla control by setting the
   default style (Scintilla styles text via numeric style IDs rather than an
   HFONT). SCI_STYLECLEARALL below resets every style - including the
   Markdown lexer's per-construct styles - back to STYLE_DEFAULT's font, so
   apply_editor_theme() must run afterwards to reapply them. */
static void apply_font_to_editor_view(HWND hEdit, const char *faceUtf8) {
    SendMessageA(hEdit, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)faceUtf8);
    SendMessageA(hEdit, SCI_STYLESETSIZE, STYLE_DEFAULT, g_fontPointSize);
    SendMessageA(hEdit, SCI_STYLESETWEIGHT, STYLE_DEFAULT, g_fontWeight);
    SendMessageA(hEdit, SCI_STYLESETITALIC, STYLE_DEFAULT, g_fontItalic);
    SendMessageA(hEdit, SCI_STYLECLEARALL, 0, 0); /* copies STYLE_DEFAULT (fore/back included) to all other styles, including STYLE_LINENUMBER */

    /* STYLECLEARALL just overwrote STYLE_LINENUMBER's fore/back with
       STYLE_DEFAULT's, undoing apply_editor_theme()'s margin colors (which
       are deliberately different from the text area). Re-apply the theme so
       the gutter doesn't revert to the wrong colors after a font change. */
    apply_editor_theme(hEdit);

    /* The margin's digit width depends on the font, so keep both in sync. */
    SendMessageA(hEdit, SCI_STYLESETFONT, STYLE_LINENUMBER, (LPARAM)faceUtf8);
    SendMessageA(hEdit, SCI_STYLESETSIZE, STYLE_LINENUMBER, g_fontPointSize);
    remeasure_linenumber_margin(hEdit);
}

/* Applies the configured editor font (g_fontFaceName, g_fontPointSize,
   g_fontWeight, g_fontItalic) to every view onto the document. */
static void apply_editor_font(void) {
    char *faceUtf8 = to_utf8(g_fontFaceName);
    if (!faceUtf8) faceUtf8 = _strdup("Consolas");
    if (!faceUtf8) return;
    apply_font_to_editor_view(g_hEdit, faceUtf8);

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
    if (g_hSplitEdit) apply_font_to_editor_view(g_hSplitEdit, faceUtf8);
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


/* Some commands briefly point g_hEdit at other tabs' documents to read or
   change them (recovery snapshots, breakpoints, execution markers, link
   rewrites). SCI_SETDOCPOINTER resets the view's selection and scroll
   position, so these bracket that work and put the active tab's view back
   afterwards. */
typedef struct BackgroundDocumentAccess {
    sptr_t document;
    Sci_Position caret;
    Sci_Position anchor;
    Sci_Position firstVisibleLine;
    int xOffset;
    int oldSwitching;
} BackgroundDocumentAccess;

static void begin_background_document_access(BackgroundDocumentAccess *access) {
    access->document = (sptr_t)SendMessageA(g_hEdit, SCI_GETDOCPOINTER, 0, 0);
    access->caret = (Sci_Position)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    access->anchor = (Sci_Position)SendMessageA(g_hEdit, SCI_GETANCHOR, 0, 0);
    access->firstVisibleLine = (Sci_Position)SendMessageA(g_hEdit, SCI_GETFIRSTVISIBLELINE, 0, 0);
    access->xOffset = (int)SendMessageA(g_hEdit, SCI_GETXOFFSET, 0, 0);
    access->oldSwitching = g_switchingDocument;
    g_switchingDocument = 1;
}

static void end_background_document_access(const BackgroundDocumentAccess *access) {
    if (access->document &&
        (sptr_t)SendMessageA(g_hEdit, SCI_GETDOCPOINTER, 0, 0) != access->document) {
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)access->document);
    }
    Sci_Position length = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    Sci_Position caret = access->caret > length ? length : access->caret;
    Sci_Position anchor = access->anchor > length ? length : access->anchor;
    SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)anchor, (LPARAM)caret);
    SendMessageA(g_hEdit, SCI_SETFIRSTVISIBLELINE, (WPARAM)access->firstVisibleLine, 0);
    SendMessageA(g_hEdit, SCI_SETXOFFSET, (WPARAM)access->xOffset, 0);
    g_switchingDocument = access->oldSwitching;
}

static char *get_document_text(int index, Sci_Position *outLength) {
    if (!g_hEdit || index < 0 || index >= g_documentCount) return NULL;
    sptr_t current = (sptr_t)SendMessageA(g_hEdit, SCI_GETDOCPOINTER, 0, 0);
    if (current == g_documents[index].sciDocument) return get_edit_text(g_hEdit, outLength);
    BackgroundDocumentAccess access;
    begin_background_document_access(&access);
    SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_documents[index].sciDocument);
    char *text = get_edit_text(g_hEdit, outLength);
    end_background_document_access(&access);
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
        apply_document_language(g_currentFile);
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

/* Backing storage for the status bar's four segments (0: Ln/Col, 1: Words/
   Characters, 2: encoding, 3: save state). The strip is fully repainted by
   StatusSubclassProc below instead of through SB_SETTEXT + owner-draw:
   comctl32 draws a light 3D bevel between parts by itself regardless of
   SBT_OWNERDRAW/SBT_NOBORDERS, which reads as bright seams against a dark
   theme. Parts must own their text for as long as the control might repaint
   them, so a transient stack buffer would otherwise be read back as a
   dangling pointer once these functions return. */
static WCHAR g_statusPartText[4][128];

/* The status bar deliberately computes from Scintilla's UTF-8 document model:
   character counts are Unicode-aware while columns remain editor columns. */
/* Ln/Col, encoding, and dirty-status fields only - all O(1) Scintilla
   queries, safe to call on every caret move or scroll tick. Word/character
   counts need a full-document scan, so they're left to update_status_bar()
   and only refreshed when the text can actually have changed. */
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
    int words = 0;
    char *text = get_edit_text(g_hEdit, &ignored);
    if (text) {
        int inWord = 0;
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

static void show_plugin_reload_status(const WCHAR *text) {
    if (!g_hStatus) return;
    KillTimer(g_hMainWnd, IDT_STATUS_UPDATE);
    wcsncpy(g_statusPartText[3], text, 127);
    g_statusPartText[3][127] = 0;
    InvalidateRect(g_hStatus, NULL, FALSE);
    SetTimer(g_hMainWnd, IDT_PLUGIN_STATUS, 1800, NULL);
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

/* ---- recently opened files and workspace folders, most-recent-first.
   Both lists share one implementation; only the backing array, its cap,
   and the settings.json section/key names differ. ---- */
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
    /* Give the file explorer a useful root on a fresh install without
       interrupting its first open with a folder-picker dialog. */
    replace_wstring(&g_currentWorkspaceFolder, value && value[0] ? value : L"C:\\");
    free(value);
    free(path);
}

static void save_workspace_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    write_setting_string(L"Workspace", L"CurrentFolder",
                         g_currentWorkspaceFolder ? g_currentWorkspaceFolder : L"", path);
    free(path);
}



#include "internal/preview.inc"

#include "internal/documents.inc"

#include "internal/dialogs.inc"

/* ---- Help > Markdown Reference and Keyboard Shortcuts. ----
   The pages are built into the exe (markedit.rc): the reference is
   src/help/markdown-reference.md as it stands, and the shortcuts page is
   generated by src/help/keyboard-shortcuts.lua from the loaded menus and
   the command table, so it matches the real shortcuts. */

/* Help > Keyboard Shortcuts, as heap Markdown, or NULL after reporting why
   it couldn't be built. */
static char *generate_shortcuts_page(void) {
    int commandCount = 0;
    while (menu_command_at(commandCount)) commandCount++;
    int pluginCount = plugin_host_command_count();
    PluginShortcut *shortcuts = (PluginShortcut *)malloc(
        (size_t)(commandCount + pluginCount + 1) * sizeof(*shortcuts));
    int count = 0;
    for (int i = 0; shortcuts && i < commandCount; i++) {
        const MenuCommand *command = menu_command_at(i);
        if (command->shortcut) shortcuts[count++] = (PluginShortcut){ command->name, command->shortcut };
    }
    for (int i = 0; shortcuts && i < pluginCount; i++) {
        const char *keys = plugin_host_command_shortcut(i);
        const char *label = plugin_host_command_label(i);
        if (keys && label) shortcuts[count++] = (PluginShortcut){ label, keys };
    }
    size_t length;
    const char *code = builtin_script(IDR_HELP_SHORTCUTS, &length);
    char *page = NULL;
    char *error;
    if (!shortcuts) error = _strdup("out of memory");
    else if (!code) error = _strdup("The keyboard shortcuts page is missing from markedit.exe.");
    else error = plugin_host_render_page("keyboard-shortcuts.lua", code, length, shortcuts, count, &page);
    free(shortcuts);
    if (error) {
        WCHAR *text = to_wide(error);
        MessageBoxW(g_hMainWnd, text ? text : L"The keyboard shortcuts page could not be built.",
                    L"MarkEdit Help", MB_OK | MB_ICONWARNING);
        free(text);
        free(error);
    }
    return page;
}

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
    char *generated = NULL;
    const char *page;
    size_t pageLength;
    if (kind == REFERENCE_SHORTCUTS) {
        page = generated = generate_shortcuts_page();
        pageLength = generated ? strlen(generated) : 0;
    } else {
        page = builtin_script(IDR_HELP_MARKDOWN, &pageLength);
    }
    if (!page) {
        if (kind != REFERENCE_SHORTCUTS)
            MessageBoxW(g_hMainWnd, L"The Markdown reference is missing from markedit.exe.", L"MarkEdit Help",
                        MB_OK | MB_ICONWARNING);
        return;
    }
    if (!create_document_tab()) {
        free(generated);
        return;
    }
    g_documents[g_activeDocument].isReference = kind;
    /* create_document_tab() just defaulted this untitled tab's language to
       plain text (language_for_path(NULL)). This tab's content is always
       Markdown regardless, so force it back - passing any name that merely
       *ends* in ".md" is enough, since language_for_path only inspects the
       extension and never treats this string as a real path. */
    apply_document_language(L"markdown-reference.md");
    g_switchingDocument = 1;
    set_edit_text(g_hEdit, page, (Sci_Position)pageLength);
    free(generated);
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

#include "internal/ui_shell.inc"

#include "internal/outline_minimap.inc"

#include "internal/workspace.inc"

#include "internal/settings_dialog.inc"
#include "internal/command_dispatch.inc"

/* ---- plugin host callbacks: the app's dialogs and the active document ---- */

#define PLUGIN_PROMPT_MAX_CHARS 4096

/* Text a plugin passes that isn't valid UTF-8 can't be shown, so the dialog
   is treated as cancelled. */
static char *plugin_prompt(const char *title, const char *label, const char *initial) {
    WCHAR *wideTitle = to_wide(title);
    WCHAR *wideLabel = to_wide(label);
    WCHAR *wideInitial = to_wide(initial);
    WCHAR *answer = (WCHAR *)malloc(PLUGIN_PROMPT_MAX_CHARS * sizeof(WCHAR));
    char *result = NULL;
    if (wideTitle && wideLabel && answer &&
        show_text_input(wideTitle, wideLabel, wideInitial, answer, PLUGIN_PROMPT_MAX_CHARS))
        result = to_utf8(answer);
    free(answer);
    free(wideInitial);
    free(wideLabel);
    free(wideTitle);
    return result;
}

static int plugin_choose(const char *title, const char *label, const char *const *items, int count) {
    WCHAR *wideTitle = to_wide(title);
    WCHAR *wideLabel = to_wide(label);
    WCHAR **labels = (WCHAR **)calloc((size_t)count, sizeof(*labels));
    int ready = wideTitle && wideLabel && labels;
    for (int i = 0; ready && i < count; i++) ready = (labels[i] = to_wide(items[i])) != NULL;
    int chosen = ready ? show_choice_dialog(wideTitle, wideLabel, labels, count, L"OK") : -1;
    for (int i = 0; labels && i < count; i++) free(labels[i]);
    free(labels);
    free(wideLabel);
    free(wideTitle);
    return chosen;
}

static void plugin_message(const char *title, const char *text, const char *kind) {
    WCHAR *wideTitle = to_wide(title);
    WCHAR *wideText = to_wide(text);
    UINT icon = strcmp(kind, "error") == 0 ? MB_ICONERROR
              : strcmp(kind, "warning") == 0 ? MB_ICONWARNING : MB_ICONINFORMATION;
    MessageBoxW(g_hMainWnd, wideText ? wideText : L"(This message isn't valid UTF-8 text.)",
                wideTitle ? wideTitle : L"MarkEdit", MB_OK | icon);
    free(wideText);
    free(wideTitle);
}

static int plugin_save_binary(const char *title, const char *defaultName,
                              const char *data, size_t length) {
    const DWORD capacity = 32768;
    WCHAR *path = (WCHAR *)calloc(capacity, sizeof(WCHAR));
    WCHAR *wideDefault = to_wide(defaultName);
    WCHAR *wideTitle = to_wide(title);
    if (!path || !wideDefault || !wideTitle) {
        free(path); free(wideDefault); free(wideTitle);
        return -1;
    }
    wcsncpy(path, wideDefault, capacity - 1);
    const WCHAR *extension = PathFindExtensionW(wideDefault);
    const WCHAR *defaultExtension = extension && extension[0] == L'.' ? extension + 1 : NULL;
    OPENFILENAMEW dialog = {0};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_hMainWnd;
    dialog.lpstrFilter = L"Binary Files (*.*)\0*.*\0All Files (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = capacity;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrDefExt = defaultExtension;
    dialog.lpstrTitle = wideTitle;
    int result = 0;
    if (GetSaveFileNameW(&dialog)) result = write_bytes_file(path, data, length) ? 1 : -1;
    free(wideTitle);
    free(wideDefault);
    free(path);
    return result;
}

static int plugin_confirm(const char *title, const char *text) {
    WCHAR *wideTitle = to_wide(title), *wideText = to_wide(text);
    int result = MessageBoxW(g_hMainWnd, wideText ? wideText : L"Confirm?",
                             wideTitle ? wideTitle : L"MarkEdit", MB_YESNO | MB_ICONQUESTION);
    free(wideTitle); free(wideText);
    return result == IDYES;
}

static void plugin_status(const char *text) {
    WCHAR *wide = to_wide(text);
    if (wide) show_plugin_reload_status(wide);
    free(wide);
}

static void plugin_log(const char *plugin, const char *text) {
    OutputDebugStringA("[MarkEdit plugin: ");
    OutputDebugStringA(plugin ? plugin : "unknown");
    OutputDebugStringA("] ");
    OutputDebugStringA(text);
    OutputDebugStringA("\n");
}

static char *plugin_pick_path(const char *title, int folder) {
    if (folder) {
        WCHAR *path = choose_workspace_folder();
        char *result = path ? to_utf8(path) : NULL;
        free(path);
        return result;
    }
    WCHAR path[32768] = L"";
    WCHAR *wideTitle = to_wide(title);
    OPENFILENAMEW dialog = {0};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_hMainWnd;
    dialog.lpstrFilter = L"All Files (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = (DWORD)(sizeof(path) / sizeof(path[0]));
    dialog.lpstrTitle = wideTitle;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    char *result = GetOpenFileNameW(&dialog) ? to_utf8(path) : NULL;
    free(wideTitle);
    return result;
}

static int plugin_invoke_command(const char *name) {
    const MenuCommand *command = menu_command_find(name);
    if (!command || !g_hMainWnd) return 0;
    /* Queue it: executing a command while Lua owns the editor's undo group
       could invalidate that group or recursively enter the plugin host. */
    return PostMessageW(g_hMainWnd, WM_COMMAND, command->id, 0) != 0;
}

static int plugin_document_count(void) { return g_documentCount; }
static int plugin_document_active(void) { return g_activeDocument; }

typedef enum {
    PLUGIN_DOCUMENT_ACTIVATE, PLUGIN_DOCUMENT_NEW, PLUGIN_DOCUMENT_OPEN,
    PLUGIN_DOCUMENT_SAVE, PLUGIN_DOCUMENT_SAVE_AS, PLUGIN_DOCUMENT_CLOSE
} PluginDocumentActionKind;
typedef struct {
    PluginDocumentActionKind kind;
    int index;
    WCHAR *path;
} PluginDocumentAction;

static int queue_plugin_document_action(PluginDocumentActionKind kind, int index,
                                        const char *path) {
    PluginDocumentAction *action = (PluginDocumentAction *)calloc(1, sizeof(*action));
    if (!action) return 0;
    action->kind = kind;
    action->index = index;
    action->path = path ? to_wide(path) : NULL;
    if ((path && !action->path) ||
        !PostMessageW(g_hMainWnd, WM_APP_PLUGIN_DOCUMENT_ACTION, 0, (LPARAM)action)) {
        free(action->path); free(action);
        return 0;
    }
    return 1;
}

static int plugin_document_activate(int index) {
    if (index < 0 || index >= g_documentCount) return 0;
    return queue_plugin_document_action(PLUGIN_DOCUMENT_ACTIVATE, index, NULL);
}
static int plugin_document_new(void) {
    return queue_plugin_document_action(PLUGIN_DOCUMENT_NEW, 0, NULL);
}
static int plugin_document_open(const char *path) {
    return queue_plugin_document_action(PLUGIN_DOCUMENT_OPEN, 0, path);
}
static int plugin_document_save(int saveAs) {
    return queue_plugin_document_action(saveAs ? PLUGIN_DOCUMENT_SAVE_AS : PLUGIN_DOCUMENT_SAVE,
                                        0, NULL);
}
static int plugin_document_close(int index) {
    if (index < 0 || index >= g_documentCount) return 0;
    return queue_plugin_document_action(PLUGIN_DOCUMENT_CLOSE, index, NULL);
}
static char *plugin_document_path(int index) {
    return index >= 0 && index < g_documentCount && g_documents[index].path
        ? to_utf8(g_documents[index].path) : NULL;
}
static int plugin_document_modified(int index) {
    return index >= 0 && index < g_documentCount && g_documents[index].dirty;
}
static const char *plugin_document_encoding(void) {
    static const char *const names[] = { "utf-8", "utf-16le", "utf-16be", "ansi" };
    return g_encoding >= TEXT_ENCODING_UTF8 && g_encoding <= TEXT_ENCODING_ANSI
        ? names[g_encoding] : "utf-8";
}
static char *plugin_workspace_path(void) {
    return g_currentWorkspaceFolder ? to_utf8(g_currentWorkspaceFolder) : NULL;
}

static int plugin_schedule_deferred(int token, unsigned delayMs, int cancel) {
    if (!g_hMainWnd || token <= 0) return 0;
    UINT_PTR timer = (UINT_PTR)(IDT_PLUGIN_DEFER_BASE + token);
    if (cancel) { KillTimer(g_hMainWnd, timer); return 1; }
    return SetTimer(g_hMainWnd, timer, delayMs ? delayMs : 1, NULL) != 0;
}

static char *plugin_file_path(void) {
    return g_currentFile ? to_utf8(g_currentFile) : NULL;
}

static const char *plugin_language(void) {
    static const char *const names[LANGUAGE_COUNT] = {
        "markdown", "json", "xml", "html", "yaml", "cpp", "python", "bash", "powershell", "lua", "text",
    };
    return g_activeLanguage >= 0 && g_activeLanguage < LANGUAGE_COUNT ? names[g_activeLanguage] : "text";
}

static char *plugin_config_section(const char *plugin) {
    size_t length = strlen(plugin) + 8;
    char *section = (char *)malloc(length);
    if (section) snprintf(section, length, "Plugin:%s", plugin);
    return section;
}

static char *plugin_config_get(const char *plugin, const char *key) {
    char *section = plugin_config_section(plugin);
    WCHAR *path = get_settings_path();
    char *value = path && section ? plugin_settings_get(path, section, key) : NULL;
    free(section);
    free(path);
    return value;
}

static int plugin_config_set(const char *plugin, const char *key, const char *value) {
    char *section = plugin_config_section(plugin);
    WCHAR *path = get_settings_path();
    int ok = path && section && plugin_settings_set(path, section, key, value);
    free(section);
    free(path);
    return ok;
}

static char *plugin_state_key(const char *scope, const char *name) {
    size_t size = strlen(scope) + strlen(name) + 2;
    char *key = (char *)malloc(size);
    if (key) snprintf(key, size, "%s:%s", scope, name);
    return key;
}

static int plugin_is_enabled(const char *scope, const char *name) {
    WCHAR *path = get_settings_path();
    char *key = plugin_state_key(scope, name);
    char *value = path && key ? plugin_settings_get(path, "LuaPlugins", key) : NULL;
    int enabled = !value || strcmp(value, "0") != 0;
    free(value); free(key); free(path);
    return enabled;
}

static int set_plugin_enabled(const char *scope, const char *name, int enabled) {
    WCHAR *path = get_settings_path();
    char *key = plugin_state_key(scope, name);
    int ok = path && key && plugin_settings_set(path, "LuaPlugins", key, enabled ? "1" : "0");
    free(key);
    free(path);
    return ok;
}

/* ---- loading the menu definition and plugins ---- */

#define MENU_DEFINITION_MAX_BYTES (1024 * 1024)

/* %APPDATA%\markedit\menus.lua: the user's menu definition, when it exists. */
static WCHAR *user_settings_item_path(const WCHAR *name) {
    WCHAR *settings = get_settings_path();
    WCHAR *directory = settings ? path_directory(settings) : NULL;
    WCHAR *path = directory ? path_join(directory, name) : NULL;
    free(directory);
    free(settings);
    return path;
}

static WCHAR *user_menu_definition_path(void) {
    return user_settings_item_path(L"menus.lua");
}

static void install_plugin_type_definitions(const WCHAR *directory) {
    WCHAR *types = path_join(directory, L"types");
    if (types) CreateDirectoryW(types, NULL);
    WCHAR *destination = types ? path_join(types, L"markedit.lua") : NULL;
    if (destination) {
        WCHAR *module = module_directory();
        WCHAR *source = module ? path_join(module, L"plugins\\types\\markedit.lua") : NULL;
        if (source) CopyFileW(source, destination, FALSE);
        free(source); free(module);
    }
    free(destination);
    free(types);
}

/* Adds metadata used by Lua Language Server. Neither entry is executable, and
   the plugin loader ignores them. The generated API file follows app updates;
   an existing .luarc.json is left untouched so users can customise it. */
static void install_plugin_editor_metadata(const WCHAR *directory) {
    install_plugin_type_definitions(directory);
    WCHAR *luarc = path_join(directory, L".luarc.json");
    static const char metadata[] =
        "{\n  \"workspace.library\": [\"types\"],\n  \"diagnostics.globals\": "
        "[\"editor\", \"ui\", \"menu\", \"commands\", \"events\", \"config\", \"app\"]\n}\n";
    if (luarc && GetFileAttributesW(luarc) == INVALID_FILE_ATTRIBUTES)
        write_bytes_file(luarc, metadata, sizeof(metadata) - 1);
    free(luarc);
}

/* %APPDATA%\markedit\plugins, created with editor metadata when needed. */
static WCHAR *user_plugin_directory(void) {
    WCHAR *directory = user_settings_item_path(L"plugins");
    if (!directory) return NULL;
    CreateDirectoryW(directory, NULL);
    install_plugin_editor_metadata(directory);
    return directory;
}

/* A Lua script built into the exe (markedit.rc). Not NUL-terminated. */
static const char *builtin_script(int id, size_t *length) {
    HRSRC resource = FindResourceW(NULL, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL loaded = resource ? LoadResource(NULL, resource) : NULL;
    *length = resource ? (size_t)SizeofResource(NULL, resource) : 0;
    return loaded ? (const char *)LockResource(loaded) : NULL;
}

/* Joins source resources before compiling them, so formatting can live in
   focused files while sharing one lexical scope and its local helpers. */
static char *builtin_script_parts(const int *ids, int count, size_t *length) {
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        size_t partLength;
        if (!builtin_script(ids[i], &partLength) || partLength > SIZE_MAX - total - 1) return NULL;
        total += partLength + 1;
    }
    char *joined = (char *)malloc(total ? total : 1);
    if (!joined) return NULL;
    size_t used = 0;
    for (int i = 0; i < count; i++) {
        size_t partLength;
        const char *part = builtin_script(ids[i], &partLength);
        memcpy(joined + used, part, partLength);
        used += partLength;
        joined[used++] = '\n';
    }
    *length = used;
    return joined;
}

/* Appends "<prefix><text>" as a line of a newline-separated report. */
static char *append_report(char *report, const char *prefix, const char *text) {
    size_t used = report ? strlen(report) : 0;
    size_t needed = used + strlen(prefix) + strlen(text) + 2;
    char *grown = (char *)realloc(report, needed);
    if (!grown) return report;
    snprintf(grown + used, needed - used, "%s%s%s", used ? "\n" : "", prefix, text);
    return grown;
}

/* Loads the user's menu definition, or the built-in one when there's none
   or it fails to load. Unknown command and list names are reported and
   their items left out. Returns the report, or NULL. */
static char *load_menu_definition(void) {
    char *report = NULL;
    WCHAR *path = user_menu_definition_path();
    if (path && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        size_t codeLength = 0;
        char *code = read_utf8_text_file_length(path, MENU_DEFINITION_MAX_BYTES, &codeLength);
        char *error = code ? plugin_host_load_menu("menus.lua", code, codeLength) : NULL;
        if (!code) report = append_report(report, "", "menus.lua: could not be read as UTF-8 text (1 MiB at most).");
        if (error) report = append_report(report, "", error);
        if (!code || error) report = append_report(report, "", "Using the built-in menus instead.");
        free(error);
        free(code);
    }
    free(path);
    int count;
    if (!plugin_host_menu(&count)) {
        size_t length;
        const char *code = builtin_script(IDR_DEFAULT_MENUS, &length);
        char *error = code ? plugin_host_load_menu("built-in menus.lua", code, length) : NULL;
        if (!code) report = append_report(report, "", "The built-in menus are missing from markedit.exe.");
        if (error) report = append_report(report, "", error);
        free(error);
    }
    const PluginMenuItem *menus = plugin_host_menu(&count);
    char *problems = menus ? menu_definition_problems(menus, count) : NULL;
    if (problems) report = append_report(report, "menus.lua: ", problems);
    free(problems);
    return report;
}

static char *load_builtin_formatting(char *report) {
    static const int formattingParts[] = {
        IDR_BUILTIN_FORMATTING_BASE, IDR_BUILTIN_FORMATTING_JSON,
        IDR_BUILTIN_FORMATTING_TABLES, IDR_BUILTIN_FORMATTING_TEXT,
    };
    size_t length;
    char *formatting = builtin_script_parts(formattingParts,
        (int)(sizeof(formattingParts) / sizeof(formattingParts[0])), &length);
    char *formattingError = formatting ? plugin_host_load_builtin("formatting.lua", formatting, length) : NULL;
    if (!formatting) report = append_report(report, "", "The formatting commands are missing from markedit.exe.");
    if (formattingError) report = append_report(report, "", formattingError);
    free(formattingError);
    free(formatting);
    return report;
}

static char *load_plugin_directory(char *report, const WCHAR *directory, const char *scope) {
    char *error = directory ? plugin_host_load_directory(directory, scope) : NULL;
    if (error) report = append_report(report, "", error);
    free(error);
    return report;
}

static WCHAR *bundled_plugin_directory(void) {
    WCHAR *directory = module_directory();
    WCHAR *pluginDirectory = directory ? path_join(directory, L"plugins") : NULL;
    free(directory);
    return pluginDirectory;
}

static void show_plugin_load_errors(const char *report) {
    const WCHAR *errorPath = plugin_host_error_path();
    if (!g_hMainWnd || !errorPath) { show_plugin_error(report); return; }
    WCHAR *wide = to_wide(report);
    size_t size = (wide ? wcslen(wide) : 0) + 80;
    WCHAR *question = (WCHAR *)malloc(size * sizeof(WCHAR));
    if (question) swprintf(question, size, L"%ls\n\nOpen the first failing script?",
                           wide ? wide : L"A plugin failed.");
    int answer = MessageBoxW(g_hMainWnd, question ? question : L"A plugin failed. Open it?",
                             L"MarkEdit Plugins", MB_YESNO | MB_ICONWARNING);
    free(question);
    free(wide);
    if (answer != IDYES || !open_file_in_tab(errorPath)) return;
    int line = plugin_host_error_line();
    if (line > 0) SendMessageA(g_hEdit, SCI_GOTOLINE, (WPARAM)(line - 1), 0);
}

/* Recreates the Lua state, then loads menus, built-in formatting, bundled
   extensions and personal extensions in that order. */
static void load_menus_and_plugins(void) {
    static const PluginHostCallbacks callbacks = {
        .prompt = plugin_prompt, .choose = plugin_choose, .file_path = plugin_file_path,
        .language = plugin_language, .message = plugin_message,
        .config_get = plugin_config_get, .config_set = plugin_config_set,
        .plugin_enabled = plugin_is_enabled, .save_binary = plugin_save_binary,
        .confirm = plugin_confirm, .status = plugin_status, .log = plugin_log,
        .pick_path = plugin_pick_path, .invoke_command = plugin_invoke_command,
        .document_count = plugin_document_count, .document_active = plugin_document_active,
        .document_activate = plugin_document_activate, .document_new = plugin_document_new,
        .document_open = plugin_document_open, .document_save = plugin_document_save,
        .document_close = plugin_document_close, .document_path = plugin_document_path,
        .document_modified = plugin_document_modified,
        .document_encoding = plugin_document_encoding, .workspace_path = plugin_workspace_path,
        .schedule_deferred = plugin_schedule_deferred,
    };
    plugin_host_set_callbacks(&callbacks);
    plugin_host_shutdown();
    char *report = load_builtin_formatting(load_menu_definition());
    WCHAR *bundled = bundled_plugin_directory();
    WCHAR *user = user_plugin_directory();
    report = load_plugin_directory(report, bundled, "bundled");
    report = load_plugin_directory(report, user, "user");
    free(bundled);
    free(user);
    g_lastPluginLoadSucceeded = report == NULL;
    if (report) show_plugin_load_errors(report);
    free(report);
}

/* Plugins > Customize Menus: opens the user's menu definition, first
   copying the built-in one there if there isn't one yet. */
static void customize_menus(void) {
    WCHAR *path = user_menu_definition_path();
    if (!path) return;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        size_t length;
        const char *code = builtin_script(IDR_DEFAULT_MENUS, &length);
        if (!code || !write_bytes_file(path, code, length)) {
            MessageBoxW(g_hMainWnd, L"Could not create menus.lua in the settings folder.", L"MarkEdit",
                        MB_OK | MB_ICONERROR);
            free(path);
            return;
        }
    }
    open_file_in_tab(path);
    free(path);
}

static void open_plugin_folder(void) {
    WCHAR *directory = user_plugin_directory();
    if (directory) {
        HINSTANCE result = ShellExecuteW(g_hMainWnd, L"open", directory, NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)result <= 32)
            MessageBoxW(g_hMainWnd, L"Could not open the personal plugins folder.",
                        L"MarkEdit Plugins", MB_OK | MB_ICONERROR);
    }
    free(directory);
}

static int valid_plugin_file_name(const char *name) {
    size_t length = strlen(name);
    if (!length || length >= 100 || name[0] == '.' || strstr(name, "..")) return 0;
    for (size_t i = 0; i < length; i++)
        if (!(isalnum((unsigned char)name[i]) || name[i] == '-' ||
              name[i] == '_' || name[i] == '.')) return 0;
    return 1;
}

static WCHAR *new_plugin_path(const char *name) {
    size_t length = strlen(name);
    char fileName[128];
    snprintf(fileName, sizeof(fileName), "%s%s", name,
             length >= 4 && _stricmp(name + length - 4, ".lua") == 0 ? "" : ".lua");
    WCHAR *wideName = to_wide(fileName);
    WCHAR *directory = user_plugin_directory();
    WCHAR *path = directory && wideName ? path_join(directory, wideName) : NULL;
    free(directory);
    free(wideName);
    return path;
}

static void new_plugin(void) {
    char *answer = plugin_prompt("New Plugin", "File name:", "my-plugin.lua");
    if (!answer) return;
    if (!valid_plugin_file_name(answer)) {
        MessageBoxW(g_hMainWnd, L"Use a simple file name containing letters, digits, '-' or '_'.",
                    L"New Plugin", MB_OK | MB_ICONWARNING);
        free(answer); return;
    }
    WCHAR *path = new_plugin_path(answer);
    static const char starter[] =
        "-- MarkEdit plugin\n"
        "menu.add(\"My Plugin/Hello\", function()\n"
        "  ui.message(\"Hello from Lua!\")\n"
        "end, \"Ctrl+Alt+H\")\n";
    if (!path || GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_hMainWnd, L"A plugin with that name already exists.", L"New Plugin",
                    MB_OK | MB_ICONWARNING);
    } else if (!write_bytes_file(path, starter, sizeof(starter) - 1)) {
        MessageBoxW(g_hMainWnd, L"Could not create the plugin file.", L"New Plugin",
                    MB_OK | MB_ICONERROR);
    } else {
        open_file_in_tab(path);
    }
    free(path);
    free(answer);
}

static void free_plugin_choices(char **items, int count) {
    if (items) for (int i = 0; i < count; i++) free(items[i]);
    free(items);
}

static char **plugin_choices(int count) {
    char **items = (char **)calloc((size_t)count, sizeof(*items));
    for (int i = 0; items && i < count; i++) {
        const char *name = plugin_host_extension_name(i);
        const char *scope = plugin_host_extension_scope(i);
        size_t size = strlen(name) + strlen(scope) + 16;
        items[i] = (char *)malloc(size);
        if (!items[i]) { free_plugin_choices(items, count); return NULL; }
        snprintf(items[i], size, "[%c] %s / %s",
                 plugin_host_extension_is_enabled(i) ? 'x' : ' ', scope, name);
    }
    return items;
}

static void reload_plugin_ui(HWND hwnd, const WCHAR *successMessage) {
    load_menus_and_plugins();
    refresh_menu(hwnd);
    rebuild_main_accelerators();
    if (g_lastPluginLoadSucceeded && successMessage)
        show_plugin_reload_status(successMessage);
}

static void toggle_plugin(int index) {
    const char *name = plugin_host_extension_name(index);
    const char *scope = plugin_host_extension_scope(index);
    int enable = !plugin_host_extension_is_enabled(index);
    if (!set_plugin_enabled(scope, name, enable)) {
        MessageBoxW(g_hMainWnd, L"Could not save the plugin state.", L"Manage Plugins",
                    MB_OK | MB_ICONERROR);
        return;
    }
    reload_plugin_ui(g_hMainWnd, enable ? L"Plugin enabled" : L"Plugin disabled");
}

static void manage_plugins(void) {
    int count = plugin_host_extension_count();
    if (!count) {
        MessageBoxW(g_hMainWnd, L"No plugins were found.", L"Manage Plugins",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    char **items = plugin_choices(count);
    const char **choices = (const char **)items;
    int selected = items ? plugin_choose("Manage Plugins",
        "Choose a plugin to enable or disable:", choices, count) : -1;
    if (selected >= 0 && selected < count) toggle_plugin(selected);
    free_plugin_choices(items, count);
}

/* Lua commands act on the pane being edited: the split editor's when it
   has the focus, otherwise the main one. */
static void run_plugin_command(int index) {
    int wasDirty = g_dirty;
    char *error = plugin_host_run(index, active_edit_view());
    sync_dirty_after_plugin(wasDirty);
    if (error) show_plugin_error(error);
    free(error);
    /* A command may toggle its own menu.checkbox or change a setting used by
       another checkbox through config.set(). The HMENU is a retained Win32
       object, so refresh its state explicitly after Lua returns. */
    update_view_menu(g_hMainWnd);
    if (!plugin_host_is_busy()) queue_plugin_tab_check();
    SetFocus(active_edit_view());
}

static void handle_main_command(HWND hwnd, WORD cmd) {
    if (cmd >= IDM_PLUGIN_COMMAND_0 && cmd < IDM_PLUGIN_COMMAND_0 + PLUGIN_MAX_COMMANDS) {
        run_plugin_command(cmd - IDM_PLUGIN_COMMAND_0);
        return;
    }
    /* A built-in command registered in Lua (commands.register) runs there,
       from the menu, toolbar or a shortcut alike. The formatting Lua
       is how the Markdown, HTML and JSON formatting commands exist at all. */
    const MenuCommand *builtIn = menu_command_for_id(cmd);
    int override = builtIn ? plugin_host_find_override(builtIn->name) : -1;
    if (override >= 0) {
        run_plugin_command(override);
        return;
    }
    if (cmd == IDM_PLUGIN_RELOAD) {
        reload_plugin_ui(hwnd, L"Plugins reloaded");
        return;
    }
    if (cmd == IDM_PLUGIN_CUSTOMIZE_MENUS) {
        customize_menus();
        return;
    }
    if (cmd == IDM_PLUGIN_OPEN_FOLDER) { open_plugin_folder(); return; }
    if (cmd == IDM_PLUGIN_NEW) { new_plugin(); return; }
    if (cmd == IDM_PLUGIN_MANAGE) { manage_plugins(); return; }
    if (cmd >= IDM_FILE_RECENT_0 && cmd < IDM_FILE_RECENT_0 + MAX_RECENT_FILES) {
        do_open_recent(cmd - IDM_FILE_RECENT_0);
        return;
    }
    if (cmd >= IDM_RECENT_WORKSPACE && cmd < IDM_RECENT_WORKSPACE + MAX_RECENT_FOLDERS) {
        do_open_recent_workspace(cmd - IDM_RECENT_WORKSPACE);
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

#include "internal/editor_input.inc"
static void explorer_request_open(const WCHAR *path) {
    open_file_in_tab(path);
}

static void explorer_request_change_root(const WCHAR *path) {
    adopt_workspace_folder(g_hMainWnd, path);
}

static int window_dpi(HWND hwnd) {
    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(hwnd, hdc);
    return dpi;
}

static void create_navigation_controls(HWND hwnd) {
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
}

static void create_debug_controls(HWND hwnd, int dpi) {
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
}

static void create_document_tabs(HWND hwnd, int dpi) {
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
}

static void create_editor_views(HWND hwnd) {
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

    apply_editor_font();
}

static void initialize_first_document(void) {
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
}

static void create_preview_control(HWND hwnd) {
    apply_preview_host_theme();
    apply_main_theme(hwnd);
    g_hPreview = CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, hwnd, (HMENU)ID_PREVIEW, GetModuleHandleA(NULL), NULL);
}

static void initialize_main_window(HWND hwnd) {
    g_hMainWnd = hwnd;

    /* The process preference is selected before window creation; complete the
       per-window opt-in before attaching the menu so its first paint matches. */
    apply_menu_theme(hwnd, g_editorDarkMode);
    int dpi = window_dpi(hwnd);

    if (!g_focusMode) SetMenu(hwnd, build_menu());
    create_toolbar(hwnd);
    create_navigation_controls(hwnd);
    create_debug_controls(hwnd, dpi);

    explorer_init(hwnd, explorer_request_open, show_text_input, reveal_path_in_explorer,
                  explorer_path_renamed, explorer_request_change_root);
    if (g_explorerVisible && g_currentWorkspaceFolder) explorer_set_root(g_currentWorkspaceFolder);

    create_document_tabs(hwnd, dpi);
    create_editor_views(hwnd);
    initialize_first_document();
    create_preview_control(hwnd);

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
    /* The menu was built before the spell checker existed. */
    update_view_menu(g_hMainWnd);
    init_webview();
    if (g_pendingCliFile) {
        load_file(g_pendingCliFile);
        free(g_pendingCliFile);
        g_pendingCliFile = NULL;
    } else if (!restore_recovery_snapshot()) {
        restore_session_documents();
    }
}

static void handle_editor_ui_update(HWND hwnd, HWND editor, const struct SCNotification *scn) {
    g_lastEditView = editor;
    if ((scn->updated & SC_UPDATE_V_SCROLL) && g_editorVisible && g_previewVisible) {
            if (g_ignoreNextEditorScroll) g_ignoreNextEditorScroll = 0;
            else SetTimer(hwnd, IDT_SCROLL_SYNC, 35, NULL);
    }
    update_brace_highlight(editor);
    update_status_position();
    update_minimap_viewport();
    if (scn->updated & SC_UPDATE_SELECTION) SetTimer(hwnd, IDT_PLUGIN_SELECTION, 100, NULL);
}

static void handle_editor_margin_click(const struct SCNotification *scn) {
    if (scn->margin != MARGIN_BOOKMARKS) return;

    /* Markers belong to the shared document, so either editor pane can update
       them. Shift+Click always selects a bookmark in debuggable files. */
    Sci_Position line = (Sci_Position)SendMessageA(
        g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)scn->position, 0);
    if (!(scn->modifiers & SCMOD_SHIFT) &&
        is_debuggable_source(debug_source_kind(g_currentFile)))
        toggle_breakpoint_at_line(line);
    else
        toggle_bookmark_at_line(line);
}

static void enter_large_file_mode_if_needed(HWND hwnd) {
    if (g_switchingDocument || g_largeFile) return;

    Sci_Position length = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    Sci_Position threshold = (Sci_Position)g_largeFileThresholdMB * 1024 * 1024;
    if (length < threshold) return;

    g_largeFile = 1;
    PostMessageW(hwnd, WM_APP_APPLY_LARGE_FILE_MODE, (WPARAM)g_activeDocument, 0);
}

static void schedule_updates_after_edit(HWND hwnd, const struct SCNotification *scn) {
    /* HTML previews show the saved file; other previews show unsaved text. */
    if (!g_largeFile && g_previewVisible && g_activeLanguage != LANG_HTML)
        SetTimer(hwnd, IDT_PREVIEW_UPDATE, 150, NULL);
    if (!g_largeFile) schedule_spell_check();

    /* Full-document status and outline scans are debounced across typing. */
    SetTimer(hwnd, IDT_STATUS_UPDATE, 200, NULL);
    if (!g_largeFile) SetTimer(hwnd, IDT_OUTLINE_UPDATE, 500, NULL);
    if (scn->linesAdded != 0) {
        update_linenumber_margin_width(g_hEdit);
        if (g_hSplitEdit) update_linenumber_margin_width(g_hSplitEdit);
    }
}

static void handle_document_modified(HWND hwnd, const struct SCNotification *scn) {
    if (!(scn->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) return;

    /* Everything below is document-level bookkeeping, not view-level, so it
       applies regardless of which pane the edit came from - both panes are
       views of the same shared document (without this, typing only in the
       split pane would never mark the tab dirty or stale the recovery
       snapshot, silently losing track of real unsaved edits). */
    if (!g_switchingDocument && !g_dirty) set_dirty(1);
    if (!g_switchingDocument) g_recoverySnapshotStale = 1;
    enter_large_file_mode_if_needed(hwnd);
    schedule_updates_after_edit(hwnd, scn);
}

static void handle_editor_notification(HWND hwnd, NMHDR *notification) {
    int fromMain = notification->hwndFrom == g_hEdit;
    int fromSplit = g_hSplitEdit && notification->hwndFrom == g_hSplitEdit;
    if (!fromMain && !fromSplit) return;

    struct SCNotification *scn = (struct SCNotification *)notification;
    switch (notification->code) {
        case SCN_UPDATEUI:
            handle_editor_ui_update(hwnd, notification->hwndFrom, scn);
            break;
        case SCN_CHARADDED:
            g_lastEditView = notification->hwndFrom;
            update_word_completions(notification->hwndFrom);
            break;
        case SCN_MARGINCLICK:
            handle_editor_margin_click(scn);
            break;
        case SCN_MODIFIED:
            handle_document_modified(hwnd, scn);
            break;
    }
}

static void destroy_main_window(HWND hwnd) {
    fire_plugin_event("shutdown", g_currentFile);
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
    KillTimer(hwnd, IDT_PLUGIN_STATUS);
    KillTimer(hwnd, IDT_PLUGIN_SELECTION);
    explorer_shutdown();
    plugin_host_shutdown();
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

static int handle_control_color(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT *result) {
    HWND control = (HWND)lParam;
    HDC hdc = (HDC)wParam;
    if (msg == WM_CTLCOLORSTATIC && control == g_hPreview && g_previewBkBrush) {
        SetBkColor(hdc, preview_bg_color());
        SetTextColor(hdc, g_previewDarkMode ? THEME_DARK_FG : RGB(0x00, 0x00, 0x00));
        *result = (LRESULT)g_previewBkBrush;
        return 1;
    }
    if (msg == WM_CTLCOLORSTATIC && control == g_hDebugPanel && g_mainBkBrush) {
        SetBkColor(hdc, theme_bg_color());
        SetTextColor(hdc, theme_fg_color());
        *result = (LRESULT)g_mainBkBrush;
        return 1;
    }
    if (msg == WM_CTLCOLORLISTBOX && control == g_hOutline && g_mainBkBrush) {
        SetBkColor(hdc, theme_bg_color());
        SetTextColor(hdc, theme_fg_color());
        *result = (LRESULT)g_mainBkBrush;
        return 1;
    }
    return 0;
}

static int handle_owner_measure(LPARAM lParam) {
    MEASUREITEMSTRUCT *item = (MEASUREITEMSTRUCT *)lParam;
    if (item->CtlType == ODT_MENU && item->itemData)
        measure_main_menu_item(item);
    else if (item->CtlType == ODT_LISTBOX && item->CtlID == ID_OUTLINE)
        measure_themed_listbox_item(g_hOutline, item);
    else
        return 0;
    return 1;
}

static int handle_owner_draw(LPARAM lParam) {
    const DRAWITEMSTRUCT *item = (const DRAWITEMSTRUCT *)lParam;
    if (item->CtlType == ODT_MENU && item->itemData)
        draw_main_menu_item(item);
    else if (item->CtlType == ODT_BUTTON)
        draw_toolbar_button(item);
    else if (item->CtlType == ODT_TAB && item->hwndItem == g_hTabs)
        draw_tab_item(item);
    else if (item->CtlType == ODT_LISTBOX && item->hwndItem == g_hOutline)
        draw_themed_listbox_item(g_hOutline, item);
    else
        return 0;
    return 1;
}

static LRESULT paint_main_background(HWND hwnd, HDC hdc) {
    if (!g_mainBkBrush) g_mainBkBrush = CreateSolidBrush(theme_bg_color());
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    FillRect(hdc, &clientRect, g_mainBkBrush);
    paint_splitter_gap(hdc, &g_splitterRect);
    paint_splitter_gap(hdc, &g_explorerSplitterRect);
    if (g_hSplitEdit && g_splitEditorVisible)
        paint_splitter_gap(hdc, &g_splitEditorSplitterRect);
    return 1;
}

static int handle_main_timer(HWND hwnd, UINT_PTR timerId) {
    if (timerId >= IDT_PLUGIN_DEFER_BASE) {
        if (plugin_host_is_busy()) {
            SetTimer(hwnd, timerId, 50, NULL);
            return 1;
        }
        KillTimer(hwnd, timerId);
        int wasDirty = g_dirty;
        char *error = plugin_host_run_deferred((int)(timerId - IDT_PLUGIN_DEFER_BASE), g_hEdit);
        sync_dirty_after_plugin(wasDirty);
        if (error) show_plugin_error(error);
        free(error);
        return 1;
    }
    switch (timerId) {
        case IDT_HTML_EXPORT: ExportDone_Invoke(NULL, E_FAIL, NULL); break;
        case IDT_PREVIEW_UPDATE: update_preview(); break;
        case IDT_FILE_WATCH: check_external_file_change(); break;
        case IDT_SCROLL_SYNC: sync_preview_to_editor_scroll(); break;
        case IDT_SPELL_CHECK: perform_spell_check(); break;
        case IDT_AUTOSAVE: write_recovery_snapshot(); break;
        case IDT_OUTLINE_UPDATE:
            KillTimer(hwnd, IDT_OUTLINE_UPDATE);
            update_outline();
            break;
        case IDT_STATUS_UPDATE:
            KillTimer(hwnd, IDT_STATUS_UPDATE);
            update_status_bar();
            fire_plugin_event("text_changed", g_currentFile);
            break;
        case IDT_PRINT_PREVIEW: run_pending_print(); break;
        case IDT_DEBUG_LOCATION: poll_debug_location(); break;
        case IDT_EXPLORER_REFRESH:
            KillTimer(hwnd, IDT_EXPLORER_REFRESH);
            explorer_handle_change_message();
            break;
        case IDT_PLUGIN_STATUS:
            KillTimer(hwnd, IDT_PLUGIN_STATUS);
            update_status_position();
            break;
        case IDT_PLUGIN_SELECTION:
            KillTimer(hwnd, IDT_PLUGIN_SELECTION);
            fire_plugin_event("selection_changed", g_currentFile);
            break;
        default: return 0;
    }
    return 1;
}

static int set_splitter_cursor(HWND hwnd) {
    POINT point;
    GetCursorPos(&point);
    ScreenToClient(hwnd, &point);
    if (splitter_contains_x(point.x) || explorer_splitter_contains_x(point.x))
        SetCursor(LoadCursorA(NULL, (LPCSTR)IDC_SIZEWE));
    else if (split_editor_splitter_contains(point.x, point.y))
        SetCursor(LoadCursorA(NULL, (LPCSTR)(g_splitEditorVertical ? IDC_SIZEWE : IDC_SIZENS)));
    else
        return 0;
    return 1;
}

static void begin_splitter_drag(HWND hwnd, int x, int y) {
    if (splitter_contains_x(x))
        g_dragging = 1;
    else if (explorer_splitter_contains_x(x))
        g_draggingExplorer = 1;
    else if (split_editor_splitter_contains(x, y))
        g_draggingSplitEditor = 1;
    else
        return;
    SetCapture(hwnd);
}

static void update_splitter_drag(HWND hwnd, int x, int y) {
    int positionChanged = 0;
    int contentX = x - explorer_content_x(g_focusMode);
    if (g_dragging && contentX != g_splitterPos) {
        g_splitterPos = contentX;
        positionChanged = 1;
    } else if (g_draggingExplorer && x != g_explorerWidth) {
        g_explorerWidth = x;
        positionChanged = 1;
    } else if (g_draggingSplitEditor) {
        int position = g_splitEditorVertical ? x - g_splitEditorRegionOrigin.x
                                             : y - g_splitEditorRegionOrigin.y;
        if (position != g_splitEditorPos) {
            g_splitEditorPos = position;
            positionChanged = 1;
        }
    }
    if (positionChanged) {
        layout(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
    }
}

static void end_splitter_drag(void) {
    if (!g_dragging && !g_draggingExplorer && !g_draggingSplitEditor) return;
    g_dragging = g_draggingExplorer = g_draggingSplitEditor = 0;
    ReleaseCapture();
    save_layout_settings();
}

static void handle_main_notification(HWND hwnd, NMHDR *notification) {
    if (notification->hwndFrom == g_hTabs && notification->code == TCN_SELCHANGE) {
        switch_document(TabCtrl_GetCurSel(g_hTabs));
        return;
    }

    int isTooltip = notification->code == TTN_GETDISPINFOW ||
                    notification->code == TTN_GETDISPINFOA;
    if (isTooltip && g_hTabs && notification->hwndFrom == TabCtrl_GetToolTips(g_hTabs)) {
        int index = (int)notification->idFrom;
        const WCHAR *text = (index >= 0 && index < g_documentCount &&
            g_documents[index].path) ? g_documents[index].path : L"Unsaved document";
        if (notification->code == TTN_GETDISPINFOW) {
            ((NMTTDISPINFOW *)notification)->lpszText = (WCHAR *)text;
        } else {
            WideCharToMultiByte(CP_ACP, 0, text, -1, g_tabTooltipAnsi,
                sizeof(g_tabTooltipAnsi), NULL, NULL);
            ((NMTTDISPINFOA *)notification)->lpszText = g_tabTooltipAnsi;
        }
        return;
    }

    if (g_hExplorerTree && notification->hwndFrom == g_hExplorerTree)
        explorer_handle_notify(notification);
    else
        handle_editor_notification(hwnd, notification);
}

static void handle_main_window_command(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    if ((HWND)lParam == g_hOutline && HIWORD(wParam) == LBN_SELCHANGE) {
        int selected = (int)SendMessageW(g_hOutline, LB_GETCURSEL, 0, 0);
        if (selected >= 0 && selected < g_outlineCount) {
            SendMessageA(g_hEdit, SCI_GOTOLINE, (WPARAM)g_outlineLines[selected], 0);
            SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
            SetFocus(g_hEdit);
        }
        return;
    }
    handle_main_command(hwnd, LOWORD(wParam));
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
        case WM_APP_PLUGIN_DOCUMENT_ACTION: {
            PluginDocumentAction *action = (PluginDocumentAction *)lParam;
            if (!action) return 0;
            switch (action->kind) {
                case PLUGIN_DOCUMENT_ACTIVATE:
                    if (action->index >= 0 && action->index < g_documentCount)
                        switch_document(action->index);
                    break;
                case PLUGIN_DOCUMENT_NEW: create_document_tab(); break;
                case PLUGIN_DOCUMENT_OPEN:
                    if (action->path) open_file_in_tab(action->path);
                    break;
                case PLUGIN_DOCUMENT_SAVE: do_save(); break;
                case PLUGIN_DOCUMENT_SAVE_AS: do_saveas(); break;
                case PLUGIN_DOCUMENT_CLOSE:
                    if (action->index >= 0 && action->index < g_documentCount)
                        do_close_document_at(action->index);
                    break;
            }
            free(action->path); free(action);
            return 0;
        }
        case WM_CREATE:
            initialize_main_window(hwnd);
            return 0;
        /* Regaining focus repaints the non-client area (active vs. inactive
           caption colour) straight from DefWindowProc's own WM_NCACTIVATE
           handling, without a separate WM_NCPAINT reaching this WndProc -
           so the seam has to be re-painted after both, or it comes back
           every time the window is refocused. */
        case WM_NCPAINT:
        case WM_NCACTIVATE: {
            LRESULT result = DefWindowProcW(hwnd, msg, wParam, lParam);
            erase_menu_bottom_seam(hwnd);
            return result;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORLISTBOX: {
            LRESULT result;
            if (handle_control_color(msg, wParam, lParam, &result)) return result;
            break;
        }
        case WM_MEASUREITEM:
            if (handle_owner_measure(lParam)) return TRUE;
            break;
        case WM_DRAWITEM:
            if (handle_owner_draw(lParam)) return TRUE;
            break;
        /* g_hEdit/g_hPreview/the toolbar buttons cover the rest of the client
           area themselves, so this only ever shows through on the toolbar
           strip's margins and the splitter gap. */
        case WM_ERASEBKGND:
            return paint_main_background(hwnd, (HDC)wParam);
        case WM_TIMER:
            if (handle_main_timer(hwnd, (UINT_PTR)wParam)) return 0;
            break;
        case WM_SIZE:
            layout(hwnd);
            /* layout() recomputes g_splitterRect, but resizing doesn't
               automatically re-run WM_ERASEBKGND (which is what actually
               paints the splitter gap) for its new position/size. */
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        case WM_SETCURSOR:
            if (set_splitter_cursor(hwnd)) return TRUE;
            break;
        case WM_LBUTTONDOWN:
            begin_splitter_drag(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSEMOVE:
            update_splitter_drag(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONUP:
            end_splitter_drag();
            return 0;
        case WM_DROPFILES:
            handle_dropfiles((HDROP)wParam);
            return 0;
        case WM_SYSCOLORCHANGE:
            detect_high_contrast();
            return 0;
        case WM_SETTINGCHANGE:
            if (lParam && wcscmp((const WCHAR *)lParam, L"ImmersiveColorSet") == 0) detect_high_contrast();
            break;
        case WM_NOTIFY:
            handle_main_notification(hwnd, (NMHDR *)lParam);
            return 0;
        case WM_COMMAND:
            handle_main_window_command(hwnd, wParam, lParam);
            return 0;
        case WM_APP_CLOSE_TAB:
            do_close_document_at((int)wParam);
            return 0;
        case WM_APP_DEFERRED_INIT:
            run_deferred_startup_init();
            return 0;
        case WM_APP_PLUGIN_TAB_CHECK:
            check_plugin_active_document();
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

static WCHAR *command_line_file(void) {
    int argumentCount = 0;
    WCHAR **arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    WCHAR *file = (arguments && argumentCount > 1) ? wcsdup_heap(arguments[1]) : NULL;
    if (arguments) LocalFree(arguments);
    return file;
}

static void load_application_settings(const WCHAR *commandLineFile) {
    g_commandLinePreviewMode = commandLineFile != NULL;
    INITCOMMONCONTROLSEX commonControls = { sizeof(commonControls),
        ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&commonControls);
    load_font_settings();
    load_theme_settings();
    init_native_menu_theme_support();
    apply_menu_theme(NULL, g_editorDarkMode);
    load_layout_settings();
    load_find_settings();
    load_compiler_settings();
    if (commandLineFile && language_for_path(commandLineFile)->id == LANG_MARKDOWN) {
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
}

static void register_main_window_class(HINSTANCE instance, HICON largeIcon, HICON smallIcon) {
    WNDCLASSEXW windowClass = {0};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"MarkEditWndClass";
    windowClass.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); /* IDC_ARROW */
    /* WM_ERASEBKGND paints the themed background; a class brush would flash. */
    windowClass.hbrBackground = NULL;
    windowClass.hIcon = largeIcon;
    windowClass.hIconSm = smallIcon;
    RegisterClassExW(&windowClass);
}

static HWND create_main_window(HINSTANCE instance, HICON largeIcon, HICON smallIcon) {
    int x = g_haveSavedWindowRect ? g_savedWindowRect.left : CW_USEDEFAULT;
    int y = g_haveSavedWindowRect ? g_savedWindowRect.top : CW_USEDEFAULT;
    int width = g_haveSavedWindowRect
        ? g_savedWindowRect.right - g_savedWindowRect.left : 1100;
    int height = g_haveSavedWindowRect
        ? g_savedWindowRect.bottom - g_savedWindowRect.top : 750;
    HWND window = CreateWindowExW(WS_EX_ACCEPTFILES, L"MarkEditWndClass",
        L"Untitled.txt - MarkEdit", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, width, height, NULL, NULL, instance, NULL);
    if (largeIcon) SendMessageW(window, WM_SETICON, ICON_BIG, (LPARAM)largeIcon);
    if (smallIcon) SendMessageW(window, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
    return window;
}

static const ACCEL BUILTIN_ACCELERATORS[] = {
        { FVIRTKEY | FCONTROL, 'N', IDM_FILE_NEW },
        { FVIRTKEY | FCONTROL, 'O', IDM_FILE_OPEN },
        { FVIRTKEY | FCONTROL, 'K', IDM_FILE_QUICK_OPEN },
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
        { FVIRTKEY | FCONTROL, VK_OEM_5, IDM_VIEW_TOGGLESPLIT },
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

static HACCEL create_main_accelerators(void) {
    ACCEL accelerators[(sizeof(BUILTIN_ACCELERATORS) / sizeof(BUILTIN_ACCELERATORS[0])) +
                       PLUGIN_MAX_COMMANDS];
    int count = (int)(sizeof(BUILTIN_ACCELERATORS) / sizeof(BUILTIN_ACCELERATORS[0]));
    memcpy(accelerators, BUILTIN_ACCELERATORS, sizeof(BUILTIN_ACCELERATORS));
    for (int i = 0; i < plugin_host_command_count() && count < (int)(sizeof(accelerators) / sizeof(accelerators[0])); i++) {
        BYTE flags;
        WORD key;
        if (!plugin_host_command_accelerator(i, &flags, &key)) continue;
        ACCEL candidate = { flags, key, (WORD)(IDM_PLUGIN_COMMAND_0 + i) };
        int conflict = 0;
        for (int existing = 0; existing < count; existing++)
            if (accelerators[existing].fVirt == candidate.fVirt &&
                accelerators[existing].key == candidate.key) { conflict = 1; break; }
        if (!conflict) accelerators[count++] = candidate;
    }
    return CreateAcceleratorTableA(accelerators, count);
}

static void rebuild_main_accelerators(void) {
    HACCEL replacement = create_main_accelerators();
    if (!replacement) return;
    HACCEL old = g_hAccel;
    g_hAccel = replacement;
    if (old) DestroyAcceleratorTable(old);
}

static int run_message_loop(HWND mainWindow) {
    MSG message;
    int result;
    while ((result = GetMessage(&message, NULL, 0, 0)) > 0) {
        if (g_hFindDlg && IsDialogMessage(g_hFindDlg, &message)) continue;
        if (g_hAccel && TranslateAcceleratorA(mainWindow, g_hAccel, &message)) continue;
        if (IsDialogMessage(mainWindow, &message)) continue;
        TranslateMessage(&message);
        DispatchMessage(&message);
    }
    return result < 0 ? 1 : 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    WCHAR *commandLineFile = command_line_file();
    HMODULE scintillaModule = LoadLibraryW(L"Scintilla.dll");
    if (!scintillaModule) {
        MessageBoxW(NULL, L"Could not load Scintilla.dll (expected next to markedit.exe).",
                    L"MarkEdit", MB_OK | MB_ICONERROR);
        free(commandLineFile);
        return 1;
    }

    load_application_settings(commandLineFile);
    HRESULT comResult = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    g_comInitialized = SUCCEEDED(comResult);
    g_msgFindReplace = RegisterWindowMessageW(FINDMSGSTRINGW);

    HICON largeIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    HICON smallIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    register_main_window_class(hInstance, largeIcon, smallIcon);
    load_menus_and_plugins();
    HWND mainWindow = create_main_window(hInstance, largeIcon, smallIcon);
    g_pendingCliFile = commandLineFile;
    HACCEL accelerators = create_main_accelerators();
    g_hAccel = accelerators;

    ShowWindow(mainWindow, g_startMaximized ? SW_SHOWMAXIMIZED : nCmdShow);
    UpdateWindow(mainWindow);
    PostMessageW(mainWindow, WM_APP_DEFERRED_INIT, 0, 0);

    int exitCode = run_message_loop(mainWindow);
    if (g_hAccel) DestroyAcceleratorTable(g_hAccel);
    if (g_comInitialized) CoUninitialize();
    FreeLibrary(scintillaModule);
    return exitCode;
}
