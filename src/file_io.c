/* Win32 file I/O shared by recovery, exports, workspace tools, and debugging. */
#include "file_io.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "path_utils.h"

/* Keeps long operations responsive to cancellation while still amortizing
   syscall and conversion setup costs. */
enum { FILE_IO_CHUNK_SIZE = 1024 * 1024 };

static int write_all(HANDLE file, const char *bytes, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        size_t remaining = length - offset;
        DWORD chunk = (DWORD)(remaining > FILE_IO_CHUNK_SIZE
            ? FILE_IO_CHUNK_SIZE : remaining);
        DWORD written = 0;
        if (!WriteFile(file, bytes + offset, chunk, &written, NULL) || written == 0)
            return 0;
        offset += written;
    }
    return 1;
}

static int read_all(HANDLE file, char *buffer, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        size_t remaining = length - offset;
        DWORD chunk = (DWORD)(remaining > FILE_IO_CHUNK_SIZE
            ? FILE_IO_CHUNK_SIZE : remaining);
        DWORD read = 0;
        if (!ReadFile(file, buffer + offset, chunk, &read, NULL) || read == 0)
            return 0;
        offset += read;
    }
    return 1;
}

int write_bytes_file(const WCHAR *path, const char *bytes, size_t length) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    int ok = write_all(file, bytes, length);
    if (!CloseHandle(file)) ok = 0;
    return ok;
}

char *read_bytes_file(const WCHAR *path, int *outLength) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > INT_MAX) {
        CloseHandle(file);
        return NULL;
    }
    int length = (int)size.QuadPart;
    char *text = (char *)malloc((size_t)length + 1);
    int ok = text && read_all(file, text, (size_t)length);
    CloseHandle(file);
    if (!ok) { free(text); return NULL; }
    text[length] = 0;
    if (outLength) *outLength = length;
    return text;
}

/* Reads a text file the way load_file() interprets one - detecting its
   encoding (see detect_text_encoding()) - as NUL-terminated UTF-8, for
   features that read files without opening them in a tab. */
char *read_text_file(const WCHAR *path, int *outLength, int *outEncoding, int *outHasBom) {
    int length = 0;
    char *bytes = read_bytes_file(path, &length);
    if (!bytes) return NULL;
    size_t bom = 0;
    TextEncoding encoding = detect_text_encoding(bytes, (size_t)length, 1, &bom);
    size_t decodedLength = 0;
    char *text = text_decode_all(encoding, bytes + bom, (size_t)length - bom, &decodedLength);
    free(bytes);
    if (!text || decodedLength > INT_MAX) { free(text); return NULL; }
    if (outLength) *outLength = (int)decodedLength;
    if (outEncoding) *outEncoding = encoding;
    if (outHasBom) *outHasBom = bom > 0;
    return text;
}

/* Read a reasonably-sized UTF-8 text file. Custom preview stylesheets are
   deliberately injected as text instead of linked directly, which keeps the
   preview's restrictive CSP intact and also makes exported HTML self-contained. */
char *read_utf8_text_file_length(const WCHAR *path, size_t maximumSize, size_t *outLength) {
    if (outLength) *outLength = 0;
    if (!path) return _strdup("");
    WCHAR *ioPath = extended_path(path);
    HANDLE file = CreateFileW(ioPath ? ioPath : path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    free(ioPath);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > INT_MAX ||
        (unsigned long long)size.QuadPart > maximumSize) {
        CloseHandle(file);
        return NULL;
    }
    size_t length = (size_t)size.QuadPart;
    char *buffer = (char *)malloc(length + 1);
    int ok = buffer && read_all(file, buffer, length);
    CloseHandle(file);
    if (!ok) { free(buffer); return NULL; }
    buffer[length] = 0;
    size_t offset = text_bom_length(buffer, length, TEXT_ENCODING_UTF8);
    if (length > offset && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            buffer + offset, (int)(length - offset), NULL, 0) <= 0) {
        free(buffer);
        return NULL;
    }
    if (offset) memmove(buffer, buffer + offset, length - offset + 1);
    if (outLength) *outLength = length - offset;
    return buffer;
}

char *read_utf8_text_file(const WCHAR *path, size_t maximumSize) {
    return read_utf8_text_file_length(path, maximumSize, NULL);
}

/* Crash-safe write: content goes to a sibling temp file first, which then
   replaces `target` in one filesystem operation (ReplaceFileW, or a plain
   move for a file that doesn't exist yet), so a failure partway through
   never leaves a half-written document on disk. `text` is UTF-8 and is
   converted to `encoding` on the way out. `target` must already be an
   I/O-ready (extended-length) path. Shared by save_to_path() and
   workspace-wide replace. */
int write_file_atomic_progress(const WCHAR *target, const char *text, size_t len,
                               TextEncoding encoding, int hasBom,
                               TextProgressCallback progress, void *context) {
    size_t pathLen = wcslen(target);
    if (pathLen > SIZE_MAX / sizeof(WCHAR) - 64) return 0;
    size_t temporaryCapacity = pathLen + 64;
    WCHAR *temporary = (WCHAR *)malloc(temporaryCapacity * sizeof(WCHAR));
    HANDLE hFile = INVALID_HANDLE_VALUE;
    if (temporary) {
        for (unsigned attempt = 0; attempt < 100 && hFile == INVALID_HANDLE_VALUE; attempt++) {
            swprintf(temporary, temporaryCapacity, L"%ls.tinkeredit-%lu-%u.tmp",
                     target, GetCurrentProcessId(), attempt);
            hFile = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
            if (hFile == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) break;
        }
    }
    if (hFile == INVALID_HANDLE_VALUE) {
        free(temporary);
        return 0;
    }
    size_t bomLength = 0;
    const char *bom = text_encoding_bom(encoding, &bomLength);
    size_t total = 0;
    BOOL ok = TRUE;
    if (hasBom && bomLength && !write_all(hFile, bom, bomLength)) ok = FALSE;
    /* `text` is UTF-8; anything else is converted a chunk at a time. */
    TextCodecState codec = { encoding, {0}, 0 };
    size_t encodedCapacity = text_encode_capacity(FILE_IO_CHUNK_SIZE);
    char *encoded = encoding == TEXT_ENCODING_UTF8 ? NULL : (char *)malloc(encodedCapacity);
    if (encoding != TEXT_ENCODING_UTF8 && !encoded) ok = FALSE;
    while (ok && total < len) {
        size_t piece = (len - total) > FILE_IO_CHUNK_SIZE
            ? FILE_IO_CHUNK_SIZE : (len - total);
        const char *bytes = text + total;
        size_t byteCount = piece;
        if (encoded) {
            if (!text_encode(&codec, text + total, piece, total + piece == len, encoded,
                             encodedCapacity, &byteCount, NULL)) { ok = FALSE; break; }
            bytes = encoded;
        }
        if (!write_all(hFile, bytes, byteCount)) {
            ok = FALSE;
            break;
        }
        total += piece;
        if (progress && !progress(total, len, context)) {
            ok = FALSE;
            break;
        }
    }
    free(encoded);
    /* One flush after the complete sequential write provides the durability
       needed before the atomic replace. FILE_FLAG_WRITE_THROUGH here would
       redundantly force every chunk to storage and makes large saves much
       slower without strengthening the final published state. */
    if (ok) ok = FlushFileBuffers(hFile);
    if (!CloseHandle(hFile)) ok = FALSE;
    if (ok) {
        DWORD attrs = GetFileAttributesW(target);
        if (attrs != INVALID_FILE_ATTRIBUTES)
            ok = ReplaceFileW(target, temporary, NULL, REPLACEFILE_WRITE_THROUGH, NULL, NULL);
        else
            ok = MoveFileExW(temporary, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    if (!ok) DeleteFileW(temporary);
    free(temporary);
    return ok;
}

int write_file_atomic(const WCHAR *target, const char *text, size_t len,
                      TextEncoding encoding, int hasBom) {
    return write_file_atomic_progress(target, text, len, encoding, hasBom, NULL, NULL);
}
