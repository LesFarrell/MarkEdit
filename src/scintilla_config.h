#ifndef TINKEREDIT_SCINTILLA_CONFIG_H
#define TINKEREDIT_SCINTILLA_CONFIG_H

#include <windows.h>

/* Marker and indicator IDs assigned when a Scintilla view is configured.
   Defined here because the editor, bookmark, debugger and spelling code all
   refer to them by number; the two are set up in configure_scintilla_margins()
   and marked from the rest of the shell. */
#define MARKER_BOOKMARK 1
#define MARKER_BREAKPOINT 2
#define MARKER_DEBUG_LINE 3
#define MARKER_DEBUG_ARROW 4
#define FOLD_MARGIN_WIDTH 14
#define BOOKMARK_MARGIN_WIDTH 14
#define CHANGE_MARGIN_WIDTH 4
#define INDICATOR_SPELLING 20

/* Everything this module needs from the application shell that is not a shared
   persisted setting. The settings themselves are read from settings_values.h;
   only the shell-owned window and document state is passed in. */
typedef struct ScintillaConfigHost {
    /* &AppWindows.splitEdit: the split view has its own line-number-digit
       cache, so the margin code needs the handle to tell the views apart. */
    const HWND *splitEdit;
    /* Nonzero while the active document is in large-file mode; word wrap is
       forced off for it (see apply_word_wrap). */
    int (*largeFileActive)(void);
} ScintillaConfigHost;

void scintilla_config_init(const ScintillaConfigHost *host);

/* Line-number gutter sizing, shared with the caller after fonts or line
   counts change. */
void update_linenumber_margin_width(HWND hEdit);
void remeasure_linenumber_margin(HWND hEdit);

/* Individual view settings, reused by the View menu and language switches. */
void apply_word_wrap(HWND hEdit);
void apply_show_whitespace(HWND hEdit);
void apply_indent_guides(HWND hEdit);
void apply_indentation(HWND hEdit);
void apply_change_markers(HWND hEdit);

/* Empties the undo buffer of the view's document and restarts its change
   history, so lines count as changed from now on. Use it in place of
   SCI_EMPTYUNDOBUFFER whenever a document gets new contents: change history
   can only be switched on while there's nothing to undo. */
void reset_undo_history(HWND hEdit);

/* The setup steps configure_scintilla() runs; feedback and the entry point
   stay in tinkeredit.c because they also apply the language and theme. */
void configure_scintilla_editing(HWND hEdit);
void configure_scintilla_view(HWND hEdit);
void configure_scintilla_margins(HWND hEdit);

#endif
