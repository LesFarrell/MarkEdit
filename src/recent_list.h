#ifndef RECENT_LIST_H
#define RECENT_LIST_H

#include <windows.h>

/* The caller provides an array with max slots and a count, initially zero.
   Entries are owned heap strings, ordered most-recent-first. The descriptor
   borrows the array, count, and INI section/key names for each operation. */
typedef struct {
    WCHAR **items;
    int *count;
    int max;
    const WCHAR *section;   /* e.g. L"RecentFiles" */
    const WCHAR *keyPrefix; /* e.g. L"File" -> File0, File1, ... */
} RecentList;

/* Reloads from the application's settings INI, replacing existing entries. */
void recent_list_load(const RecentList *list);
/* Mutations also persist the list. Paths are matched case-insensitively. */
void recent_list_add(const RecentList *list, const WCHAR *path);
void recent_list_clear(const RecentList *list);
void recent_list_remove(const RecentList *list, const WCHAR *path);

#endif
