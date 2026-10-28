#include "git_status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *copy_text(const char *text, size_t length) {
    char *copy = (char *)malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length);
    copy[length] = 0;
    return copy;
}

static int replace_text(char **destination, const char *text, size_t length) {
    char *copy = copy_text(text, length);
    if (!copy) return 0;
    free(*destination);
    *destination = copy;
    return 1;
}

static const char *after_spaces(const char *record, size_t length, int count) {
    const char *at = record;
    const char *end = record + length;
    for (int i = 0; i < count; i++) {
        at = (const char *)memchr(at, ' ', (size_t)(end - at));
        if (!at) return NULL;
        at++;
    }
    return at;
}

static int append_entry(GitStatusResult *result, const char *path, size_t pathLength,
                        char indexStatus, char worktreeStatus) {
    GitStatusEntry *grown = (GitStatusEntry *)realloc(
        result->entries, (result->entryCount + 1) * sizeof(GitStatusEntry));
    if (!grown) return 0;
    result->entries = grown;
    char *copy = copy_text(path, pathLength);
    if (!copy) return 0;
    if (indexStatus == '.' || indexStatus == '-') indexStatus = ' ';
    if (worktreeStatus == '.' || worktreeStatus == '-') worktreeStatus = ' ';
    result->entries[result->entryCount++] =
        (GitStatusEntry){ copy, indexStatus, worktreeStatus };
    return 1;
}

void git_status_result_free(GitStatusResult *result) {
    if (!result) return;
    free(result->head);
    free(result->upstream);
    for (size_t i = 0; i < result->entryCount; i++) free(result->entries[i].path);
    free(result->entries);
    memset(result, 0, sizeof(*result));
}

int git_status_parse_v2_z(const char *data, size_t length, GitStatusResult *result) {
    if (!result || (!data && length)) return 0;
    memset(result, 0, sizeof(*result));
    size_t position = 0;
    while (position < length) {
        const char *record = data + position;
        const char *nul = (const char *)memchr(record, 0, length - position);
        if (!nul) goto invalid;
        size_t recordLength = (size_t)(nul - record);
        position += recordLength + 1;
        if (!recordLength) continue;

        static const char HEAD[] = "# branch.head ";
        static const char OID[] = "# branch.oid ";
        static const char UPSTREAM[] = "# branch.upstream ";
        static const char AHEAD_BEHIND[] = "# branch.ab ";
        if (recordLength >= sizeof(HEAD) - 1 &&
            memcmp(record, HEAD, sizeof(HEAD) - 1) == 0) {
            const char *value = record + sizeof(HEAD) - 1;
            size_t valueLength = recordLength - (sizeof(HEAD) - 1);
            if (!replace_text(&result->head, value, valueLength)) goto invalid;
            result->detached = valueLength == 10 && memcmp(value, "(detached)", 10) == 0;
            continue;
        }
        if (recordLength >= sizeof(OID) - 1 &&
            memcmp(record, OID, sizeof(OID) - 1) == 0) {
            const char *value = record + sizeof(OID) - 1;
            size_t valueLength = recordLength - (sizeof(OID) - 1);
            result->unborn = valueLength == 9 && memcmp(value, "(initial)", 9) == 0;
            continue;
        }
        if (recordLength >= sizeof(UPSTREAM) - 1 &&
            memcmp(record, UPSTREAM, sizeof(UPSTREAM) - 1) == 0) {
            const char *value = record + sizeof(UPSTREAM) - 1;
            if (!replace_text(&result->upstream, value,
                              recordLength - (sizeof(UPSTREAM) - 1))) goto invalid;
            continue;
        }
        if (recordLength >= sizeof(AHEAD_BEHIND) - 1 &&
            memcmp(record, AHEAD_BEHIND, sizeof(AHEAD_BEHIND) - 1) == 0) {
            int ahead = 0, behind = 0;
            if (sscanf(record + sizeof(AHEAD_BEHIND) - 1, "+%d -%d", &ahead, &behind) == 2) {
                result->ahead = ahead;
                result->behind = behind;
            }
            continue;
        }

        const char *path = NULL;
        char indexStatus = ' ', worktreeStatus = ' ';
        if (record[0] == '1' && recordLength >= 4) {
            path = after_spaces(record, recordLength, 8);
            indexStatus = record[2];
            worktreeStatus = record[3];
        } else if (record[0] == '2' && recordLength >= 4) {
            path = after_spaces(record, recordLength, 9);
            indexStatus = record[2];
            worktreeStatus = record[3];
            /* A rename/copy record is followed by the original path. The
               first path is the destination, which is what panel actions use. */
            if (position >= length) goto invalid;
            const char *original = data + position;
            const char *originalNul = (const char *)memchr(original, 0, length - position);
            if (!originalNul) goto invalid;
            position += (size_t)(originalNul - original) + 1;
        } else if (record[0] == 'u' && recordLength >= 4) {
            path = after_spaces(record, recordLength, 10);
            indexStatus = record[2];
            worktreeStatus = record[3];
        } else if (record[0] == '?' && recordLength >= 3 && record[1] == ' ') {
            path = record + 2;
            indexStatus = '?';
            worktreeStatus = '?';
        }
        if (path && !append_entry(result, path, (size_t)((record + recordLength) - path),
                                  indexStatus, worktreeStatus)) goto invalid;
    }
    return 1;

invalid:
    git_status_result_free(result);
    return 0;
}
