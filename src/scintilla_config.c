/* Scintilla view setup: marker/indicator IDs, line-number gutter sizing and
   the per-view settings (word wrap, whitespace, indent guides, tabs, margins)
   that were previously private to markedit.c. Shell-owned state arrives
   through ScintillaConfigHost; persisted settings come straight from
   settings_values.h. */
#include "scintilla_config.h"

#include "editor_commands.h" /* MARGIN_LINENUMBERS/BOOKMARKS/FOLD */
#include "settings_values.h"
#include "themed_scrollbar.h"

#include <stdio.h>

#include "../third_party/scintilla/include/Scintilla.h"

static const ScintillaConfigHost *g_host;

void scintilla_config_init(const ScintillaConfigHost *host) {
    g_host = host;
}

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
    return hEdit == *g_host->splitEdit ? &g_splitLineNumberDigits : &g_lineNumberDigits;
}

void update_linenumber_margin_width(HWND hEdit) {
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
void remeasure_linenumber_margin(HWND hEdit) {
    *linenumber_digit_cache(hEdit) = 0;
    update_linenumber_margin_width(hEdit);
}

/* Wrapped lines continue with a hanging indent so a wrapped list item or
   blockquote stays visually attached to its marker. Wrap mode belongs to
   the view, not the document, so this is reapplied from
   apply_document_language() whenever the active document changes:
   wrapping a large-file document would lay out every line of it up front. */
void apply_word_wrap(HWND hEdit) {
    int wrap = g_wordWrap && !g_host->largeFileActive();
    SendMessageA(hEdit, SCI_SETWRAPMODE, wrap ? SC_WRAP_WORD : SC_WRAP_NONE, 0);
    if (!wrap) return;
    SendMessageA(hEdit, SCI_SETWRAPINDENTMODE, SC_WRAPINDENT_SAME, 0);
    SendMessageA(hEdit, SCI_SETWRAPVISUALFLAGS, SC_WRAPVISUALFLAG_END, 0);
}

void apply_show_whitespace(HWND hEdit) {
    SendMessageA(hEdit, SCI_SETVIEWWS, g_showWhitespace ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE, 0);
}

void apply_indent_guides(HWND hEdit) {
    SendMessageA(hEdit, SCI_SETINDENTATIONGUIDES, g_indentGuides ? SC_IV_LOOKBOTH : SC_IV_NONE, 0);
}

void apply_indentation(HWND hEdit) {
    SendMessageA(hEdit, SCI_SETUSETABS, g_indentUseSpaces ? FALSE : TRUE, 0);
    SendMessageA(hEdit, SCI_SETTABWIDTH, g_tabWidth, 0);
}

/* The change bars are always tracked; the setting only shows them. Hiding
   the margin alone isn't enough: Scintilla draws a marker with no visible
   margin as a background across its whole line, so the markers become
   empty symbols too. */
void apply_change_markers(HWND hEdit) {
    static const int markers[] = { SC_MARKNUM_HISTORY_SAVED, SC_MARKNUM_HISTORY_MODIFIED,
                                   SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED };
    for (int i = 0; i < 3; i++)
        SendMessageA(hEdit, SCI_MARKERDEFINE, (WPARAM)markers[i], g_markChangedLines ? SC_MARK_FULLRECT : SC_MARK_EMPTY);
    SendMessageA(hEdit, SCI_SETMARGINWIDTHN, MARGIN_CHANGES, g_markChangedLines ? CHANGE_MARGIN_WIDTH : 0);
}

void reset_undo_history(HWND hEdit) {
    SendMessageA(hEdit, SCI_SETCHANGEHISTORY, SC_CHANGE_HISTORY_DISABLED, 0);
    SendMessageA(hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
    SendMessageA(hEdit, SCI_SETCHANGEHISTORY, SC_CHANGE_HISTORY_ENABLED | SC_CHANGE_HISTORY_MARKERS, 0);
}

void configure_scintilla_editing(HWND hEdit) {
    SendMessageA(hEdit, SCI_SETCODEPAGE, SC_CP_UTF8, 0);
    SendMessageA(hEdit, SCI_SETEOLMODE, SC_EOL_CRLF, 0);
    /* Bound lexer work performed while painting after an edit. Scintilla
       styles in short foreground chunks and finishes the visible range while
       idle, keeping long multiline constructs from blocking key handling. */
    SendMessageA(hEdit, SCI_SETIDLESTYLING, SC_IDLESTYLING_TOVISIBLE, 0);
    apply_indentation(hEdit);
    SendMessageA(hEdit, SCI_SETINDENT, 0, 0); /* 0 = indent width follows tab width */
    SendMessageA(hEdit, SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT, 0);

    /* Additional carets edit in step with the main one; the smart-input
       handlers in EditSubclassProc still act on the main caret alone. */
    SendMessageA(hEdit, SCI_SETMULTIPLESELECTION, TRUE, 0);
    SendMessageA(hEdit, SCI_SETADDITIONALSELECTIONTYPING, TRUE, 0);
    SendMessageA(hEdit, SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH, 0);
}

void configure_scintilla_view(HWND hEdit) {
    themed_scrollbar_attach(hEdit, THEMED_SCROLL_SCINTILLA);
    SendMessageA(hEdit, SCI_SETSCROLLWIDTHTRACKING, TRUE, 0);
    SendMessageA(hEdit, SCI_SETSCROLLWIDTH, 1, 0);
    apply_word_wrap(hEdit);
    apply_show_whitespace(hEdit);
    apply_indent_guides(hEdit);
    SendMessageA(hEdit, SCI_SETZOOM, (WPARAM)g_editorZoom, 0);
}

void configure_scintilla_margins(HWND hEdit) {
    /* Line numbers: a dedicated margin, sized to fit the current line count. */
    SendMessageA(hEdit, SCI_SETMARGINTYPEN, MARGIN_LINENUMBERS, SC_MARGIN_NUMBER);
    update_linenumber_margin_width(hEdit);

    /* Bookmark and fold margins. Marker definitions are per-view, so they are
       set once here and survive the SCI_SETDOCPOINTER swaps done by tabs. */
    SendMessageA(hEdit, SCI_SETMARGINTYPEN, MARGIN_BOOKMARKS, SC_MARGIN_SYMBOL);
    SendMessageA(hEdit, SCI_SETMARGINMASKN, MARGIN_BOOKMARKS,
                 (LPARAM)((1 << MARKER_BOOKMARK) | (1 << MARKER_BREAKPOINT) |
                          (1 << MARKER_DEBUG_ARROW)));
    SendMessageA(hEdit, SCI_SETMARGINWIDTHN, MARGIN_BOOKMARKS, BOOKMARK_MARGIN_WIDTH);
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

    /* Change history: a bar beside each line changed since the file was
       loaded - one colour until it's saved, another after. A line edited and
       then undone back to how it was gets no bar, and one undone to an
       earlier unsaved change counts as changed. */
    SendMessageA(hEdit, SCI_SETMARGINTYPEN, MARGIN_CHANGES, SC_MARGIN_SYMBOL);
    SendMessageA(hEdit, SCI_SETMARGINMASKN, MARGIN_CHANGES,
                 (LPARAM)((1 << SC_MARKNUM_HISTORY_SAVED) | (1 << SC_MARKNUM_HISTORY_MODIFIED) |
                          (1 << SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED)));
    apply_change_markers(hEdit);
}
