#ifndef FILE_IO_H
#define FILE_IO_H

#include <stddef.h>
#include <windows.h>
#include "text_codec.h"

enum { FILE_FOLLOW_TAIL_SIZE = 64 };

/* Exact raw-disk position represented by a followed document. The trailing
   signature distinguishes append-only growth from replacement or copytruncate
   rotation, while retryBlocked suppresses repeated resource-error dialogs for
   one unchanged on-disk revision. */
typedef struct FileFollowState {
    FILETIME writeTime;
    ULONGLONG size;
    DWORD volumeSerial, fileIndexHigh, fileIndexLow;
    unsigned char tail[FILE_FOLLOW_TAIL_SIZE];
    size_t tailLength;
    int hasTime, hasIdentity;
    FILETIME blockedWriteTime;
    ULONGLONG blockedSize;
    int retryBlocked;
} FileFollowState;

typedef enum FileFollowAction {
    FILE_FOLLOW_RETRY,
    FILE_FOLLOW_APPEND,
    FILE_FOLLOW_RELOAD
} FileFollowAction;

void file_follow_clear(FileFollowState *state);
/* Captures identity and the signature ending at representedSize, which may be
   smaller than the handle's current size if a writer appended during loading. */
int file_follow_capture(FileFollowState *state, HANDLE file, ULONGLONG representedSize);
FileFollowAction file_follow_check_growth(const FileFollowState *state, HANDLE file,
                                          const BY_HANDLE_FILE_INFORMATION *info,
                                          ULONGLONG size);
void file_follow_commit_bytes(FileFollowState *state, const char *bytes, size_t length);
void file_follow_complete(FileFollowState *state, const BY_HANDLE_FILE_INFORMATION *info);
void file_follow_block_retry(FileFollowState *state, ULONGLONG size, FILETIME writeTime);
/* Returns true only for the revision that already failed; a newer observation
   automatically releases the block. */
int file_follow_retry_blocked(FileFollowState *state, ULONGLONG size, FILETIME writeTime);

/* Creates or truncates path. Returns nonzero only when all bytes and the
   final handle close succeed; a failure may leave a partial file. */
int write_bytes_file(const WCHAR *path, const char *bytes, size_t length);

/* Readers return heap-allocated, NUL-terminated buffers (caller frees), or
   NULL on failure. Optional output arguments are set only on success. */
char *read_bytes_file(const WCHAR *path, int *outLength);
/* Detects the source encoding and returns UTF-8; outEncoding is TextEncoding. */
char *read_text_file(const WCHAR *path, int *outLength, int *outEncoding, int *outHasBom);
/* Validates UTF-8, strips its BOM, and rejects files larger than maximumSize.
   A NULL path returns an allocated empty string. Intended for small assets. */
char *read_utf8_text_file(const WCHAR *path, size_t maximumSize);
/* As above, also returning the byte length after a UTF-8 BOM is removed.
   Unlike strlen(), this preserves embedded NUL bytes so script loaders can
   pass the complete file to their parser and reject it as malformed. */
char *read_utf8_text_file_length(const WCHAR *path, size_t maximumSize, size_t *outLength);

/* Atomically publishes encoded UTF-8 input. target is an I/O-ready path. */
int write_file_atomic(const WCHAR *target, const char *text, size_t length,
                      TextEncoding encoding, int hasBom);
int write_file_atomic_progress(const WCHAR *target, const char *text, size_t length,
                               TextEncoding encoding, int hasBom,
                               TextProgressCallback progress, void *context);

#endif
