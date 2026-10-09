/* Shared persistence and ordering for recent files and workspace folders. */
#include "recent_list.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>

#include "path_utils.h"
#include "settings.h"

static int recent_list_find(const RecentList *list, const WCHAR *path) {
    for (int i = 0; i < *list->count; i++)
        if (_wcsicmp(list->items[i], path) == 0) return i;
    return -1;
}

static void recent_list_release_items(const RecentList *list) {
    for (int i = 0; i < *list->count; i++) {
        free(list->items[i]);
        list->items[i] = NULL;
    }
    *list->count = 0;
}

void recent_list_load(const RecentList *list) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    recent_list_release_items(list);
    for (int i = 0; i < list->max; i++) {
        WCHAR key[32];
        swprintf(key, 32, L"%ls%d", list->keyPrefix, i);
        WCHAR *value = read_profile_string(list->section, key, path);
        if (!value) break;
        list->items[(*list->count)++] = value;
    }
    free(path);
}

/* WritePrivateProfileSectionW replaces the whole section in one file
   read-modify-write pass, rather than the N+1 passes that clearing the
   section and then calling WritePrivateProfileStringW once per entry would
   cost - this runs on every single file open, so it isn't a one-off. */
static void recent_list_save(const RecentList *list) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    size_t prefixLen = wcslen(list->keyPrefix);
    size_t cap = 1;
    for (int i = 0; i < *list->count; i++) {
        size_t itemLength = wcslen(list->items[i]);
        if (itemLength > SIZE_MAX - cap - prefixLen - 16) {
            free(path);
            return;
        }
        cap += prefixLen + 16 + itemLength;
    }
    if (cap > SIZE_MAX / sizeof(WCHAR)) {
        free(path);
        return;
    }
    WCHAR *section = (WCHAR *)malloc(cap * sizeof(WCHAR));
    if (section) {
        size_t pos = 0;
        for (int i = 0; i < *list->count; i++) {
            int written = swprintf(section + pos, cap - pos, L"%ls%d=%ls", list->keyPrefix, i, list->items[i]);
            if (written < 0) {
                free(section);
                free(path);
                return;
            }
            pos += (size_t)written + 1; /* step past the entry's own NUL */
        }
        section[pos] = 0; /* second, block-terminating NUL */
        WritePrivateProfileSectionW(list->section, *list->count ? section : NULL, path);
        free(section);
    }
    free(path);
}

/* Move (or insert) path to the front of the list, deduping
   case-insensitively (Windows paths), capping at list->max. */
void recent_list_add(const RecentList *list, const WCHAR *path) {
    if (!path || list->max <= 0) return;
    WCHAR *copy = wcsdup_heap(path);
    if (!copy) return;
    int count = *list->count;
    int existing = recent_list_find(list, path);
    int start = (existing >= 0) ? existing : (count < list->max ? count : list->max - 1);
    if (existing >= 0) free(list->items[existing]);
    else if (count == list->max) free(list->items[list->max - 1]);
    for (int i = start; i > 0; i--) {
        list->items[i] = list->items[i - 1];
    }
    list->items[0] = copy;
    if (existing < 0 && count < list->max) (*list->count)++;
    recent_list_save(list);
}

void recent_list_clear(const RecentList *list) {
    recent_list_release_items(list);
    recent_list_save(list);
}

/* Remove path from the list (e.g. because it no longer exists on disk) and
   persist the change. */
void recent_list_remove(const RecentList *list, const WCHAR *path) {
    if (!path) return;
    int idx = recent_list_find(list, path);
    if (idx < 0) return;
    free(list->items[idx]);
    for (int i = idx; i < *list->count - 1; i++) list->items[i] = list->items[i + 1];
    (*list->count)--;
    list->items[*list->count] = NULL;
    recent_list_save(list);
}
