#define COBJMACROS

#include "progress_dialog.h"

#include <shlobj.h>
#include <shobjidl.h>

/* IProgressDialog numbers its lines from one. */
#define PROGRESS_DIALOG_LINE 1
/* SetProgress64 takes unsigned counts; while the total is unknown, report it
   as one so the bar never divides by zero. */
#define PROGRESS_DIALOG_UNKNOWN_TOTAL 1

static const ProgressDialogHost *g_host;

void progress_dialog_init(const ProgressDialogHost *host) {
    g_host = host;
}

static IProgressDialog *dialog_of(const LargeOperationProgress *progress) {
    return (IProgressDialog *)progress->dialog;
}

void large_operation_progress_begin(LargeOperationProgress *progress,
                                    const WCHAR *title, const WCHAR *line) {
    IProgressDialog *dialog = NULL;
    progress->dialog = NULL;
    progress->cancelled = 0;
    if (!g_host || !g_host->comInitialized || !*g_host->comInitialized) return;
    if (FAILED(CoCreateInstance(&CLSID_ProgressDialog, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IProgressDialog, (void **)&dialog)) || !dialog)
        return;
    dialog->lpVtbl->SetTitle(dialog, title);
    dialog->lpVtbl->SetLine(dialog, PROGRESS_DIALOG_LINE, line, FALSE, NULL);
    dialog->lpVtbl->SetCancelMsg(dialog, L"Cancelling after the current block...", NULL);
    if (FAILED(dialog->lpVtbl->StartProgressDialog(dialog, g_host->owner, NULL,
            PROGDLG_MODAL | PROGDLG_AUTOTIME | PROGDLG_NOMINIMIZE, NULL))) {
        IUnknown_Release((IUnknown *)dialog);
        return;
    }
    progress->dialog = dialog;
}

void large_operation_progress_set_line(LargeOperationProgress *progress, const WCHAR *line) {
    IProgressDialog *dialog = dialog_of(progress);
    if (!dialog) return;
    dialog->lpVtbl->SetLine(dialog, PROGRESS_DIALOG_LINE, line, FALSE, NULL);
    dialog->lpVtbl->SetProgress64(dialog, 0, PROGRESS_DIALOG_UNKNOWN_TOTAL);
}

int large_operation_progress_update(LargeOperationProgress *progress, size_t completed, size_t total) {
    IProgressDialog *dialog = dialog_of(progress);
    if (!dialog) return 1;
    dialog->lpVtbl->SetProgress64(dialog, (ULONGLONG)completed,
        (ULONGLONG)(total ? total : PROGRESS_DIALOG_UNKNOWN_TOTAL));
    if (dialog->lpVtbl->HasUserCancelled(dialog)) {
        progress->cancelled = 1;
        return 0;
    }
    return 1;
}

int large_operation_progress_callback(size_t completed, size_t total, void *context) {
    return large_operation_progress_update((LargeOperationProgress *)context, completed, total);
}

void large_operation_progress_end(LargeOperationProgress *progress) {
    IProgressDialog *dialog = dialog_of(progress);
    if (!dialog) return;
    dialog->lpVtbl->StopProgressDialog(dialog);
    IUnknown_Release((IUnknown *)dialog);
    progress->dialog = NULL;
}
