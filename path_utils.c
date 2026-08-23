#include "path_utils.h"

#include <stdio.h>
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
    size_t directoryLength = wcslen(directory);
    size_t nameLength = wcslen(name);
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
