/* Win32 file I/O shared by recovery, exports, workspace tools, and debugging. */
#include "file_io.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "path_utils.h"
#include "text_codec.h"

int write_bytes_file(const WCHAR *path, const char *bytes, size_t length) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    size_t total = 0;
    int ok = 1;
    while (total < length) {
        DWORD chunk = (DWORD)((length - total) > 1024 * 1024 ? 1024 * 1024 : length - total);
        DWORD written = 0;
        if (!WriteFile(file, bytes + total, chunk, &written, NULL) || written != chunk) {
            ok = 0;
            break;
        }
        total += written;
    }
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
    DWORD read = 0;
    int ok = text && ReadFile(file, text, (DWORD)length, &read, NULL) && read == (DWORD)length;
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
char *read_utf8_text_file(const WCHAR *path, size_t maximumSize) {
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
    size_t total = 0;
    while (buffer && total < length) {
        DWORD chunk = (DWORD)((length - total) > 1024 * 1024 ? 1024 * 1024 : length - total);
        DWORD got = 0;
        if (!ReadFile(file, buffer + total, chunk, &got, NULL) || got == 0) break;
        total += got;
    }
    CloseHandle(file);
    if (!buffer || total != length) { free(buffer); return NULL; }
    buffer[length] = 0;
    size_t offset = has_utf8_bom(buffer, (int)length) ? 3 : 0;
    if (length > offset && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            buffer + offset, (int)(length - offset), NULL, 0) <= 0) {
        free(buffer);
        return NULL;
    }
    if (offset) memmove(buffer, buffer + offset, length - offset + 1);
    return buffer;
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
    WCHAR *temporary = (WCHAR *)malloc((pathLen + 64) * sizeof(WCHAR));
    HANDLE hFile = INVALID_HANDLE_VALUE;
    if (temporary) {
        for (unsigned attempt = 0; attempt < 100 && hFile == INVALID_HANDLE_VALUE; attempt++) {
            swprintf(temporary, pathLen + 64, L"%ls.markedit-%lu-%u.tmp", target, GetCurrentProcessId(), attempt);
            hFile = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, NULL);
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
    if (hasBom && bomLength) {
        DWORD written = 0;
        if (!WriteFile(hFile, bom, (DWORD)bomLength, &written, NULL) || written != bomLength) ok = FALSE;
    }
    /* `text` is UTF-8; anything else is converted a chunk at a time. */
    TextCodecState codec = { encoding, {0}, 0 };
    size_t encodedCapacity = text_encode_capacity(1024 * 1024);
    char *encoded = encoding == TEXT_ENCODING_UTF8 ? NULL : (char *)malloc(encodedCapacity);
    if (encoding != TEXT_ENCODING_UTF8 && !encoded) ok = FALSE;
    while (ok && total < len) {
        size_t piece = (len - total) > 1024 * 1024 ? 1024 * 1024 : (len - total);
        const char *bytes = text + total;
        size_t byteCount = piece;
        if (encoded) {
            if (!text_encode(&codec, text + total, piece, total + piece == len, encoded,
                             encodedCapacity, &byteCount, NULL)) { ok = FALSE; break; }
            bytes = encoded;
        }
        DWORD written = 0;
        if (byteCount && (!WriteFile(hFile, bytes, (DWORD)byteCount, &written, NULL) || written != byteCount)) {
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
