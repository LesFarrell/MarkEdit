#ifndef RECOVERY_H
#define RECOVERY_H

#include <stddef.h>
#include <windows.h>

#include "path_utils.h" /* LONG_PATH_CAPACITY */
#include "text_codec.h"

#define RECOVERY_MAX_DOCUMENTS 32
#define RECOVERY_MAX_SESSIONS 16

/* Every running instance snapshots into a session folder of its own under the
   recovery root, kept open through its session.lock until the instance exits.
   One instance can therefore never overwrite, clear or recover another's live
   snapshot, and a folder whose lock is free was left by a crash. */
typedef struct {
    WCHAR *directory;
    HANDLE lock; /* INVALID_HANDLE_VALUE for a snapshot written straight into root */
} RecoverySession;

typedef struct {
    const WCHAR *path;
    unsigned untitledNumber;
    int hasBom;
    TextEncoding encoding;
    int sourceIndex;
} RecoveryDocument;

typedef struct {
    WCHAR path[LONG_PATH_CAPACITY];
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

/* Creates and locks a new session folder under root. Failure returns zero. */
int recovery_session_open(const WCHAR *root, RecoverySession *session);
/* Claims up to max sessions left by instances that did not exit cleanly and
   still hold documents, including a snapshot older builds wrote straight into
   root. Abandoned empty sessions are removed; unreadable ones are left alone.
   Returns the number claimed; each must be passed to recovery_session_close. */
int recovery_session_claim_orphans(const WCHAR *root, RecoverySession *sessions, int max);
/* Releases a session. discard first clears its snapshot and removes its folder;
   a snapshot that cannot be cleared is kept for the next launch to offer. */
void recovery_session_close(RecoverySession *session, int discard);

#endif
