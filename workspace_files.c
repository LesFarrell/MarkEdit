#define COBJMACROS

#include "workspace_files.h"

#include <shlwapi.h>
#include <shobjidl.h>
#include <stdlib.h>
#include <string.h>

#include "path_utils.h"

#define MAX_WORKSPACE_FILES 5000
#define MAX_WORKSPACE_DEPTH 24

void workspace_files_free(WorkspaceFiles *files) {
    for (int i = 0; i < files->count; i++) {
        free(files->paths[i]);
        free(files->labels[i]);
    }
    free(files->paths);
    free(files->labels);
    free(files->lines);
    free(files->root);
    memset(files, 0, sizeof(*files));
}

int workspace_files_add(WorkspaceFiles *files, const WCHAR *path,
                        const WCHAR *label, int line) {
    if (files->count >= files->capacity) {
        int next = files->capacity ? files->capacity * 2 : 128;
        WCHAR **paths = (WCHAR **)realloc(files->paths, (size_t)next * sizeof(WCHAR *));
        if (!paths) return 0;
        files->paths = paths;
        WCHAR **labels = (WCHAR **)realloc(files->labels, (size_t)next * sizeof(WCHAR *));
        if (!labels) return 0;
        files->labels = labels;
        int *lines = (int *)realloc(files->lines, (size_t)next * sizeof(int));
        if (!lines) return 0;
        files->lines = lines;
        files->capacity = next;
    }
    files->paths[files->count] = wcsdup_heap(path);
    files->labels[files->count] = wcsdup_heap(label);
    files->lines[files->count] = line;
    if (!files->paths[files->count] || !files->labels[files->count]) {
        free(files->paths[files->count]);
        free(files->labels[files->count]);
        return 0;
    }
    files->count++;
    return 1;
}

static WCHAR *choose_workspace_folder(HWND owner) {
    IFileOpenDialog *dialog = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IFileOpenDialog, (void **)&dialog)) || !dialog) return NULL;
    FILEOPENDIALOGOPTIONS options = 0;
    IFileOpenDialog_GetOptions(dialog, &options);
    IFileOpenDialog_SetOptions(dialog, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                               FOS_PATHMUSTEXIST);
    WCHAR *resultPath = NULL;
    if (SUCCEEDED(IFileOpenDialog_Show(dialog, owner))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(dialog, &item)) && item) {
            PWSTR selected = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &selected)) && selected) {
                resultPath = wcsdup_heap(selected);
                CoTaskMemFree(selected);
            }
            IShellItem_Release(item);
        }
    }
    IFileOpenDialog_Release(dialog);
    return resultPath;
}

static int workspace_file_extension(const WCHAR *name) {
    const WCHAR *extension = PathFindExtensionW(name);
    static const WCHAR *allowed[] = {
        L".md", L".markdown", L".mdown", L".mkd", L".txt", L".json", L".yaml", L".yml",
        L".html", L".htm", L".xml", L".c", L".h", L".cpp", L".hpp", L".js", L".ts",
        L".py", L".sh", L".bash", L".ps1"
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
        if (_wcsicmp(extension, allowed[i]) == 0) return 1;
    return 0;
}

static int skipped_workspace_directory(const WCHAR *name) {
    static const WCHAR *skipped[] = {
        L".git", L".svn", L".hg", L"node_modules", L"build", L"dist", L"Output",
        L"webview2_data", L".vs", L".vscode"
    };
    for (size_t i = 0; i < sizeof(skipped) / sizeof(skipped[0]); i++)
        if (_wcsicmp(name, skipped[i]) == 0) return 1;
    return 0;
}

static void enumerate_workspace_files(WorkspaceFiles *files, const WCHAR *directory, int depth) {
    if (depth > MAX_WORKSPACE_DEPTH || files->count >= MAX_WORKSPACE_FILES) return;
    WCHAR *pattern = path_join(directory, L"*");
    if (!pattern) return;
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(pattern, &data);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        WCHAR *full = path_join(directory, data.cFileName);
        if (!full) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                !skipped_workspace_directory(data.cFileName))
                enumerate_workspace_files(files, full, depth + 1);
        } else if (!(data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) &&
                   workspace_file_extension(data.cFileName)) {
            const WCHAR *relative = full;
            size_t rootLength = wcslen(files->root);
            if (_wcsnicmp(full, files->root, rootLength) == 0) {
                relative = full + rootLength;
                if (*relative == L'\\' || *relative == L'/') relative++;
            }
            workspace_files_add(files, full, relative, 0);
        }
        free(full);
    } while (files->count < MAX_WORKSPACE_FILES && FindNextFileW(search, &data));
    FindClose(search);
}

int workspace_files_collect(WorkspaceFiles *files, HWND owner) {
    memset(files, 0, sizeof(*files));
    files->root = choose_workspace_folder(owner);
    if (!files->root) return 0;
    enumerate_workspace_files(files, files->root, 0);
    return 1;
}
