#ifndef TINKEREDIT_GIT_STATUS_H
#define TINKEREDIT_GIT_STATUS_H

#include <stddef.h>

typedef struct GitStatusEntry {
    char *path;
    char indexStatus;
    char worktreeStatus;
} GitStatusEntry;

typedef struct GitStatusResult {
    char *head;
    char *upstream;
    int ahead;
    int behind;
    int detached;
    int unborn;
    GitStatusEntry *entries;
    size_t entryCount;
} GitStatusResult;

/* Parses `git status --porcelain=v2 --branch -z`. Paths in the result are
   the exact UTF-8 bytes Git emitted, without C-style quoting. */
int git_status_parse_v2_z(const char *data, size_t length, GitStatusResult *result);
void git_status_result_free(GitStatusResult *result);
int git_status_is_conflict(char indexStatus, char worktreeStatus);

#endif
