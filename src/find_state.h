#ifndef MARKEDIT_FIND_STATE_H
#define MARKEDIT_FIND_STATE_H

#include <windows.h>
#include <commdlg.h> /* FR_MATCHCASE, FR_DOWN */

/* Transient Find/Replace dialog state the dialogs read and write directly.
   Kept out of settings.h so it is clear these are dialog-owned values rather
   than persisted preferences (load_find_settings()/save_find_settings() in
   settings.c persist them). */
extern WCHAR g_findWhat[256];
extern WCHAR g_replaceWith[256];
extern DWORD g_findFlags;

#endif
