#ifndef TINKEREDIT_PATH_UTILS_H
#define TINKEREDIT_PATH_UTILS_H

#include <windows.h>

/* The longest path Windows accepts through the "\\?\" prefix, in WCHARs
   including the terminating NUL; also the size of a path buffer. */
#define LONG_PATH_CAPACITY 32768

WCHAR *wcsdup_heap(const WCHAR *value);
void replace_wstring(WCHAR **target, const WCHAR *value);
WCHAR *get_module_path(void);
WCHAR *module_directory(void); /* the folder holding tinkeredit.exe */
WCHAR *path_directory(const WCHAR *path);
WCHAR *path_join(const WCHAR *directory, const WCHAR *name);
WCHAR *absolute_path(const WCHAR *path);
WCHAR *extended_path(const WCHAR *path);
int skipped_workspace_directory(const WCHAR *name);
int path_has_skipped_component(const WCHAR *path);
/* %LOCALAPPDATA%\TinkerEditor\<name>, created if needed. Caller frees. */
WCHAR *get_local_app_directory(const WCHAR *name);
/* Whether `path` names an existing file (not a folder). NULL is never one. */
int file_exists(const WCHAR *path);

#endif
