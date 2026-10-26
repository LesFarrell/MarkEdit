#ifndef MARKEDIT_PROGRESS_DIALOG_H
#define MARKEDIT_PROGRESS_DIALOG_H

#include <windows.h>

/* A long file operation (large-file load or save) reports its progress through
   the shell's IProgressDialog. The caller owns the struct on its stack and
   drives it with the functions below. It is inert - every function becomes a
   no-op that reports "keep going" - when the dialog couldn't be opened. */
typedef struct {
    void *dialog; /* the IProgressDialog, or NULL; treat as opaque */
    int cancelled;
} LargeOperationProgress;

/* The shell's live state the module needs: the window that owns the dialog and
   whether COM is initialised (CoCreateInstance requires it). */
typedef struct {
    HWND owner;
    const int *comInitialized;
} ProgressDialogHost;

/* Points the module at the shell's state; call once during startup. */
void progress_dialog_init(const ProgressDialogHost *host);

/* Opens the dialog. Safe to call even when it can't be shown. */
void large_operation_progress_begin(LargeOperationProgress *progress,
                                    const WCHAR *title, const WCHAR *line);
/* Replaces the dialog's status line and restarts its progress bar at zero. */
void large_operation_progress_set_line(LargeOperationProgress *progress, const WCHAR *line);
/* Returns nonzero to continue, zero after the user cancels. `total` may be 0
   while the size is still unknown. */
int large_operation_progress_update(LargeOperationProgress *progress, size_t completed, size_t total);
/* The update function as a TextProgressCallback, ready to hand to the codec
   and recovery APIs. */
int large_operation_progress_callback(size_t completed, size_t total, void *context);
void large_operation_progress_end(LargeOperationProgress *progress);

#endif
