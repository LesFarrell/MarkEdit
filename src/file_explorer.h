#ifndef MARKEDIT_FILE_EXPLORER_H
#define MARKEDIT_FILE_EXPLORER_H

#include <windows.h>

/* Posted (with no payload - the receiver drains file_explorer.c's own
   pending-directories queue) by the watcher thread whenever it observes a
   change under the current root. markedit.c's WndProc handles this by
   (re)starting a short debounce timer rather than refreshing immediately,
   so a burst of changes (e.g. a git operation) collapses into one refresh. */
#define MARKEDIT_WM_EXPLORER_CHANGED (WM_APP + 3)

/* Called with the absolute path of a file the user double-clicked/pressed
   Enter on/chose "Open" for in the tree. markedit.c wires this to
   open_file_in_tab(), which is static to markedit.c's own translation unit
   and so cannot be called by name from here. */
typedef void (*ExplorerOpenFileFn)(const WCHAR *path);

/* Matches show_text_input()'s exact signature (also static to markedit.c) so
   markedit.c can hand explorer_init() that function's address directly, with
   no wrapper glue, for the New File/New Folder/Rename name prompts. */
typedef int (*ExplorerPromptTextFn)(const WCHAR *title, const WCHAR *prompt,
                                    const WCHAR *initial, WCHAR *output, int outputCapacity);

/* Shows the given path selected in Windows Explorer - reused from
   markedit.c's own "Show in Explorer" command (reveal_path_in_explorer())
   rather than file_explorer.c reimplementing the same ShellExecuteW call. */
typedef void (*ExplorerRevealFn)(const WCHAR *path);

/* Creates the explorer's TreeView child control (initially hidden) and
   records the callbacks used to reach markedit.c's static functions. Call
   once, from initialize_main_window(). */
void explorer_init(HWND hParent, ExplorerOpenFileFn openFile, ExplorerPromptTextFn promptText,
                   ExplorerRevealFn revealPath);

/* Re-applies dark/light native theming (scrollbars, background/text colour)
   to the tree control. Call whenever g_previewDarkMode changes. */
void explorer_apply_theme(void);

/* (Re)points the tree at a new root folder, repopulating it and restarting
   the live file-system watcher. Safe to call whether or not a watcher is
   already running for a previous root. */
void explorer_set_root(const WCHAR *rootPath);

/* Stops the background watcher thread (if any) without discarding the tree's
   current contents/expansion state. Call when the panel is hidden. */
void explorer_stop_watch(void);

/* Full teardown: stops+joins the watcher thread and frees all tree item
   data. Call once from destroy_main_window(). */
void explorer_shutdown(void);

/* Routes a WM_NOTIFY from the tree control. markedit.c calls this from its
   own WM_NOTIFY handler whenever notification->hwndFrom == g_hExplorerTree. */
void explorer_handle_notify(HWND hwnd, NMHDR *notification);

/* Drains the pending-changed-directories queue built up by the watcher
   thread and refreshes just the affected, currently-loaded tree nodes.
   markedit.c calls this from its MARKEDIT_WM_EXPLORER_CHANGED debounce
   timer (IDT_EXPLORER_REFRESH). */
void explorer_handle_change_message(void);

/* The pixel x-offset everything right of the explorer panel is shifted by -
   the panel's width plus its splitter, or 0 when the panel isn't currently
   occupying space (hidden, or suppressed by focus mode). layout() derives
   this inline (it also has to clamp/normalize g_explorerWidth); other call
   sites that need the same value once that's already settled (the main
   splitter's hit-testing/drag math, and its persisted ratio in settings.c)
   call this instead of re-deriving it, so they can't drift out of sync with
   where the panel actually is. */
int explorer_content_x(int focusModeActive);

extern HWND g_hExplorerTree;
extern RECT g_explorerSplitterRect;
extern int g_explorerVisible;
extern int g_explorerWidth;      /* live pixel width; -1 sentinel means "derive from ratio" */
extern int g_explorerWidthRatio; /* persisted permille of client width, like g_splitterRatio */
extern int g_draggingExplorer;

#endif
