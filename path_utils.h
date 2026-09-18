#ifndef MARKEDIT_PATH_UTILS_H
#define MARKEDIT_PATH_UTILS_H

#include <windows.h>

WCHAR *wcsdup_heap(const WCHAR *value);
void replace_wstring(WCHAR **target, const WCHAR *value);
WCHAR *get_module_path(void);
WCHAR *path_directory(const WCHAR *path);
WCHAR *path_join(const WCHAR *directory, const WCHAR *name);
WCHAR *absolute_path(const WCHAR *path);
WCHAR *extended_path(const WCHAR *path);
int skipped_workspace_directory(const WCHAR *name);
int path_has_skipped_component(const WCHAR *path);
int compare_explorer_entries(int aIsDirectory, const WCHAR *aName,
                             int bIsDirectory, const WCHAR *bName);

#endif
