#ifndef WORKSPACE_FILES_H
#define WORKSPACE_FILES_H

#include <windows.h>

typedef struct WorkspaceFiles {
    WCHAR **paths;
    WCHAR **labels;
    int *lines;
    int count;
    int capacity;
    WCHAR *root;
} WorkspaceFiles;

void workspace_files_free(WorkspaceFiles *files);
int workspace_files_add(WorkspaceFiles *files, const WCHAR *path,
                        const WCHAR *label, int line);
int workspace_files_collect(WorkspaceFiles *files, HWND owner);

#endif
