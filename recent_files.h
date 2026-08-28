#ifndef RECENT_FILES_H
#define RECENT_FILES_H

#include <windows.h>

#define MAX_RECENT_FILES 10

void recent_files_load(const WCHAR *settingsPath);
void recent_files_add(const WCHAR *path, const WCHAR *settingsPath);
void recent_files_clear(const WCHAR *settingsPath);
void recent_files_remove(const WCHAR *path, const WCHAR *settingsPath);
void recent_files_prune(const WCHAR *settingsPath);
void recent_files_dispose(void);
int recent_files_count(void);
const WCHAR *recent_files_get(int index);

#endif
