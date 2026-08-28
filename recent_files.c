#include "recent_files.h"

#include <stdlib.h>
#include <wchar.h>

#include "path_utils.h"

static WCHAR *g_files[MAX_RECENT_FILES];
static int g_count;

static void save(const WCHAR *settingsPath) {
    if (!settingsPath) return;
    WritePrivateProfileStringW(L"RecentFiles", NULL, NULL, settingsPath);
    for (int i = 0; i < g_count; i++) {
        WCHAR key[16];
        swprintf(key, 16, L"File%d", i);
        WritePrivateProfileStringW(L"RecentFiles", key, g_files[i], settingsPath);
    }
}

void recent_files_dispose(void) {
    for (int i = 0; i < g_count; i++) {
        free(g_files[i]);
        g_files[i] = NULL;
    }
    g_count = 0;
}

void recent_files_load(const WCHAR *settingsPath) {
    recent_files_dispose();
    if (!settingsPath) return;
    for (int i = 0; i < MAX_RECENT_FILES; i++) {
        WCHAR key[16];
        swprintf(key, 16, L"File%d", i);
        DWORD capacity = 512;
        WCHAR *value = NULL;
        DWORD length = 0;
        do {
            free(value);
            value = (WCHAR *)malloc((size_t)capacity * sizeof(WCHAR));
            if (!value) break;
            length = GetPrivateProfileStringW(L"RecentFiles", key, L"", value,
                                              capacity, settingsPath);
            if (length < capacity - 1) break;
            capacity *= 2;
        } while (capacity <= 32768);
        if (!length) {
            free(value);
            break;
        }
        g_files[g_count++] = value;
    }
}

void recent_files_add(const WCHAR *path, const WCHAR *settingsPath) {
    WCHAR *copy = wcsdup_heap(path);
    if (!copy) return;
    int existing = -1;
    for (int i = 0; i < g_count; i++) {
        if (_wcsicmp(g_files[i], path) == 0) {
            existing = i;
            break;
        }
    }
    int start = existing >= 0 ? existing :
        (g_count < MAX_RECENT_FILES ? g_count : MAX_RECENT_FILES - 1);
    if (existing >= 0) free(g_files[existing]);
    else if (g_count == MAX_RECENT_FILES) free(g_files[MAX_RECENT_FILES - 1]);
    for (int i = start; i > 0; i--) g_files[i] = g_files[i - 1];
    g_files[0] = copy;
    if (existing < 0 && g_count < MAX_RECENT_FILES) g_count++;
    save(settingsPath);
}

void recent_files_clear(const WCHAR *settingsPath) {
    recent_files_dispose();
    save(settingsPath);
}

void recent_files_remove(const WCHAR *path, const WCHAR *settingsPath) {
    int index = -1;
    for (int i = 0; i < g_count; i++) {
        if (_wcsicmp(g_files[i], path) == 0) {
            index = i;
            break;
        }
    }
    if (index < 0) return;
    free(g_files[index]);
    for (int i = index; i < g_count - 1; i++) g_files[i] = g_files[i + 1];
    g_files[--g_count] = NULL;
    save(settingsPath);
}

void recent_files_prune(const WCHAR *settingsPath) {
    int i = 0;
    while (i < g_count) {
        WCHAR *ioPath = extended_path(g_files[i]);
        DWORD attributes = ioPath ? GetFileAttributesW(ioPath) : INVALID_FILE_ATTRIBUTES;
        free(ioPath);
        if (attributes == INVALID_FILE_ATTRIBUTES) recent_files_remove(g_files[i], settingsPath);
        else i++;
    }
}

int recent_files_count(void) {
    return g_count;
}

const WCHAR *recent_files_get(int index) {
    return index >= 0 && index < g_count ? g_files[index] : NULL;
}
