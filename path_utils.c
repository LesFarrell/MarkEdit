#include "path_utils.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

WCHAR *wcsdup_heap(const WCHAR *value) {
    if (!value) return NULL;
    size_t bytes = (wcslen(value) + 1) * sizeof(WCHAR);
    WCHAR *copy = (WCHAR *)malloc(bytes);
    if (copy) memcpy(copy, value, bytes);
    return copy;
}

void replace_wstring(WCHAR **target, const WCHAR *value) {
    WCHAR *copy = value ? wcsdup_heap(value) : NULL;
    if (value && !copy) return;
    free(*target);
    *target = copy;
}

WCHAR *get_module_path(void) {
    DWORD capacity = 512;
    for (;;) {
        WCHAR *path = (WCHAR *)malloc((size_t)capacity * sizeof(WCHAR));
        if (!path) return NULL;
        DWORD length = GetModuleFileNameW(NULL, path, capacity);
        if (length > 0 && length < capacity - 1) return path;
        free(path);
        if (capacity >= 32768) return NULL;
        capacity *= 2;
    }
}

WCHAR *path_directory(const WCHAR *path) {
    WCHAR *directory = wcsdup_heap(path);
    if (!directory) return NULL;
    WCHAR *slash = wcsrchr(directory, L'\\');
    WCHAR *forward = wcsrchr(directory, L'/');
    if (forward && (!slash || forward > slash)) slash = forward;
    if (slash) {
        if (slash == directory + 2 && directory[1] == L':') slash[1] = 0;
        else *slash = 0;
    } else {
        free(directory);
        return wcsdup_heap(L".");
    }
    return directory;
}

WCHAR *path_join(const WCHAR *directory, const WCHAR *name) {
    if (!directory || !name) return NULL;
    size_t directoryLength = wcslen(directory);
    size_t nameLength = wcslen(name);
    if (nameLength > SIZE_MAX - 2 || directoryLength > SIZE_MAX - nameLength - 2)
        return NULL;
    int addSlash = directoryLength > 0 && directory[directoryLength - 1] != L'\\' &&
                   directory[directoryLength - 1] != L'/';
    WCHAR *joined = (WCHAR *)malloc(
        (directoryLength + (size_t)addSlash + nameLength + 1) * sizeof(WCHAR));
    if (!joined) return NULL;
    memcpy(joined, directory, directoryLength * sizeof(WCHAR));
    if (addSlash) joined[directoryLength++] = L'\\';
    memcpy(joined + directoryLength, name, (nameLength + 1) * sizeof(WCHAR));
    return joined;
}

WCHAR *absolute_path(const WCHAR *path) {
    if (!path) return NULL;
    DWORD capacity = GetFullPathNameW(path, 0, NULL, NULL);
    if (!capacity) return wcsdup_heap(path);
    WCHAR *full = (WCHAR *)malloc((size_t)capacity * sizeof(WCHAR));
    if (!full) return NULL;
    if (!GetFullPathNameW(path, capacity, full, NULL)) {
        free(full);
        return NULL;
    }
    return full;
}

WCHAR *extended_path(const WCHAR *path) {
    WCHAR *full = absolute_path(path);
    if (!full || wcsncmp(full, L"\\\\?\\", 4) == 0) return full;
    const WCHAR *prefix = wcsncmp(full, L"\\\\", 2) == 0 ? L"\\\\?\\UNC\\" : L"\\\\?\\";
    const WCHAR *tail = wcsncmp(full, L"\\\\", 2) == 0 ? full + 2 : full;
    size_t length = wcslen(prefix) + wcslen(tail) + 1;
    WCHAR *extended = (WCHAR *)malloc(length * sizeof(WCHAR));
    if (extended) swprintf(extended, length, L"%ls%ls", prefix, tail);
    free(full);
    return extended;
}

/* Shared by the workspace-scoped file enumeration (Quick Open/Find-in-Folder,
   in markedit.c) and the file explorer's live watcher (file_explorer.c), so
   both agree on which directories are noisy/uninteresting to recurse into. */
int skipped_workspace_directory(const WCHAR *name) {
    if (!name) return 0;
    static const WCHAR *skipped[] = {
        L".git", L".svn", L".hg", L"node_modules", L"build", L"dist", L"Output",
        L"webview2_data", L".vs", L".vscode"
    };
    for (size_t i = 0; i < sizeof(skipped) / sizeof(skipped[0]); i++)
        if (_wcsicmp(name, skipped[i]) == 0) return 1;
    return 0;
}

/* True if any path component (not just the last one) is a skipped
   directory name - e.g. ".git\objects\ab\cdef" needs this to be caught by
   the ".git" check even though the file's own immediate parent is "ab",
   not ".git". Used by the file explorer's watcher to filter change
   notifications from deep inside a skipped directory, where checking only
   the immediate parent would miss almost everything git itself writes. */
int path_has_skipped_component(const WCHAR *path) {
    WCHAR *copy = wcsdup_heap(path);
    if (!copy) return 0;
    int found = 0;
    WCHAR *context = NULL;
    for (WCHAR *token = wcstok_s(copy, L"\\/", &context); token;
         token = wcstok_s(NULL, L"\\/", &context)) {
        if (skipped_workspace_directory(token)) { found = 1; break; }
    }
    free(copy);
    return found;
}
