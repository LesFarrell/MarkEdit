#ifndef RECOVERY_H
#define RECOVERY_H

#include <stddef.h>
#include <windows.h>
#include "text_codec.h"

#define RECOVERY_MAX_DOCUMENTS 32

typedef struct {
    const WCHAR *path;
    unsigned untitledNumber;
    int hasBom;
    TextEncoding encoding;
    int sourceIndex;
} RecoveryDocument;

typedef struct {
    WCHAR path[32768];
    unsigned untitledNumber;
    int hasBom;
    TextEncoding encoding;
    char *text;
    int length;
} RecoveryEntry;

/* Callback returns heap-owned UTF-8; persistence frees it after each write. */
typedef char *(*RecoveryReadText)(void *context, int sourceIndex, size_t *length);
/* directory must exist. Writes publish all documents atomically, or retain
   the previous snapshot. Zero documents clears it. Failure returns zero. */
int recovery_write(const WCHAR *directory, const RecoveryDocument *documents,
                   int count, RecoveryReadText readText, void *context);
int recovery_count(const WCHAR *directory);
int recovery_read(const WCHAR *directory, int index, RecoveryEntry *entry);
void recovery_entry_free(RecoveryEntry *entry);
int recovery_clear(const WCHAR *directory);
/* Deletes only recognized recovery artifacts absent from the active manifest.
   A corrupt/unreadable manifest prevents cleanup; concurrent writes are locked. */
int recovery_cleanup(const WCHAR *directory);

#endif
