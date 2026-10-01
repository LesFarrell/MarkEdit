/* Win32 file I/O shared by recovery, exports, workspace tools, and debugging. */
#include "file_io.h"

#include <limits.h>
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
