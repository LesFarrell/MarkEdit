#include "recovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include "file_io.h"
#include "path_utils.h"

/* An exclusive file handle serializes cleanup and publication across processes.
   A competing operation fails promptly so autosave can retry on its next tick. */
static HANDLE lock_directory(const WCHAR *directory) {
    WCHAR *path = directory ? path_join(directory, L"recovery.lock") : NULL;
    HANDLE lock = path ? CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    free(path);
    return lock;
}

static int digits(const WCHAR *start, const WCHAR *end) {
    if (start == end) return 0;
    for (; start < end; start++) if (*start < L'0' || *start > L'9') return 0;
    return 1;
}

static int generation_name(const WCHAR *name, size_t length) {
    if (length != 35) return 0;
    for (size_t i = 0; i < length; i++) {
        if (i == 8 || i == 13 || i == 18) {
            if (name[i] != L'-') return 0;
        } else if (!iswxdigit(name[i])) return 0;
    }
    return 1;
}

/* Recognize our legacy names, GUID generations and atomic-write leftovers,
   without treating arbitrary user files in this directory as disposable. */
static int owned_name(const WCHAR *name) {
    if (wcsncmp(name, L"document-", 9)) return 0;
    const WCHAR *body = name + 9, *dot = wcschr(body, L'.');
    if (!dot) return 0;
    size_t length = (size_t)(dot - body);
    int content = digits(body, dot) || (length > 36 && generation_name(body, 35) &&
        body[35] == L'-' && digits(body + 36, dot));
    const WCHAR *suffix;
    if (content && !wcsncmp(dot, L".txt", 4)) suffix = dot + 4;
    else if (generation_name(body, length) && !wcsncmp(dot, L".ini", 4)) suffix = dot + 4;
    else return 0;
    if (!*suffix) return 1;
    if (wcsncmp(suffix, L".markedit-", 10)) return 0;
    const WCHAR *pid = suffix + 10, *dash = wcschr(pid, L'-');
    const WCHAR *ext = dash ? wcschr(dash + 1, L'.') : NULL;
    return dash && ext && digits(pid, dash) && digits(dash + 1, ext) && !wcscmp(ext, L".tmp");
}

static int manifest_count(const WCHAR *manifest) {
    DWORD attrs = GetFileAttributesW(manifest);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    }
    HANDLE file = CreateFileW(manifest, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return -1;
    CloseHandle(file);
    int count = (int)GetPrivateProfileIntW(L"Recovery", L"Count", (UINT)-1, manifest);
    return count >= 0 && count <= RECOVERY_MAX_DOCUMENTS ? count : -1;
}

static int cleanup_unlocked(const WCHAR *directory) {
    WCHAR *manifest = path_join(directory, L"manifest.ini");
    if (!manifest) return 0;
    int count = manifest_count(manifest), ok = count >= 0;
    WCHAR names[RECOVERY_MAX_DOCUMENTS][128];
    for (int i = 0; ok && i < count; i++) {
        WCHAR key[32];
        swprintf(key, 32, L"File%d", i);
        DWORD length = GetPrivateProfileStringW(L"Recovery", key, L"", names[i], 128, manifest);
        if (!length || length >= 127 || !owned_name(names[i])) ok = 0;
    }
    free(manifest);
    if (!ok) return 0;
    WCHAR *pattern = path_join(directory, L"document-*");
    if (!pattern) return 0;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || !owned_name(data.cFileName)) continue;
        int active = 0;
        for (int i = 0; i < count; i++)
            if (!_wcsicmp(names[i], data.cFileName)) { active = 1; break; }
        if (active) continue;
        WCHAR *path = path_join(directory, data.cFileName);
        if (!path || (!DeleteFileW(path) && GetLastError() != ERROR_FILE_NOT_FOUND)) ok = 0;
        free(path);
    } while (FindNextFileW(find, &data));
    if (GetLastError() != ERROR_NO_MORE_FILES) ok = 0;
    FindClose(find);
    return ok;
}

static int clear_unlocked(const WCHAR *directory) {
    WCHAR *manifest = path_join(directory, L"manifest.ini");
    if (!manifest) return 0;
    int ok = DeleteFileW(manifest) || GetLastError() == ERROR_FILE_NOT_FOUND;
    free(manifest);
    /* Failure to remove an orphan does not make the discarded snapshot active. */
    if (ok) cleanup_unlocked(directory);
    return ok;
}

static int write_unlocked(const WCHAR *directory, const RecoveryDocument *documents,
                          int count, RecoveryReadText readText, void *context) {
    if (!count) return clear_unlocked(directory);
    WCHAR *manifest = path_join(directory, L"manifest.ini");
    GUID generation;
    WCHAR generationName[80];
    if (!directory || !manifest || FAILED(CoCreateGuid(&generation))) {
        free(manifest); return 0;
    }
    swprintf(generationName, 80,
        L"document-%08lx-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x",
        (unsigned long)generation.Data1, generation.Data2, generation.Data3,
        generation.Data4[0], generation.Data4[1], generation.Data4[2], generation.Data4[3],
        generation.Data4[4], generation.Data4[5], generation.Data4[6], generation.Data4[7]);
    WCHAR stagedName[128];
    swprintf(stagedName, 128, L"%ls.ini", generationName);
    WCHAR *stagedManifest = path_join(directory, stagedName);
    if (!stagedManifest) { free(manifest); return 0; }
    /* Each generation owns its content files. Publish its manifest only once
       every document is durable; a failure leaves the old snapshot intact. */
    /* A Unicode INI preserves non-ASCII document paths on every code page. */
    int ok = write_bytes_file(stagedManifest, "\xFF\xFE", 2);
    int writtenCount = 0;
    for (int i = 0; ok && i < count; i++) {
        const RecoveryDocument *doc = &documents[i];
        size_t length = 0;
        char *text = readText(context, doc->sourceIndex, &length);
        WCHAR fileName[128], key[32], value[32];
        swprintf(fileName, 128, L"%ls-%d.txt", generationName, writtenCount);
        WCHAR *contentPath = path_join(directory, fileName);
        if (!text || !contentPath || !write_file_atomic(contentPath, text, (size_t)length,
                                                        TEXT_ENCODING_UTF8, 0)) {
            free(text); free(contentPath); ok = 0; break;
        }
        swprintf(key, sizeof(key) / sizeof(key[0]), L"File%d", writtenCount);
        ok &= WritePrivateProfileStringW(L"Recovery", key, fileName, stagedManifest);
        swprintf(key, sizeof(key) / sizeof(key[0]), L"Path%d", writtenCount);
        ok &= WritePrivateProfileStringW(L"Recovery", key, doc->path ? doc->path : L"", stagedManifest);
        swprintf(key, sizeof(key) / sizeof(key[0]), L"Untitled%d", writtenCount);
        swprintf(value, sizeof(value) / sizeof(value[0]), L"%u", doc->untitledNumber);
        ok &= WritePrivateProfileStringW(L"Recovery", key, value, stagedManifest);
        swprintf(key, sizeof(key) / sizeof(key[0]), L"Bom%d", writtenCount);
        ok &= WritePrivateProfileStringW(L"Recovery", key, doc->hasBom ? L"1" : L"0", stagedManifest);
        swprintf(key, sizeof(key) / sizeof(key[0]), L"Encoding%d", writtenCount);
        swprintf(value, sizeof(value) / sizeof(value[0]), L"%d", doc->encoding);
        ok &= WritePrivateProfileStringW(L"Recovery", key, value, stagedManifest);
        writtenCount++;
        free(text);
        free(contentPath);
    }
    WCHAR countValue[32];
    swprintf(countValue, 32, L"%d", writtenCount);
    ok &= WritePrivateProfileStringW(L"Recovery", L"Count", countValue, stagedManifest);
    if (ok) {
        HANDLE file = CreateFileW(stagedManifest, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        ok = file != INVALID_HANDLE_VALUE;
        if (ok) {
            ok = FlushFileBuffers(file);
            if (!CloseHandle(file)) ok = 0;
        }
    }
    if (ok) ok = MoveFileExW(stagedManifest, manifest,
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (ok) cleanup_unlocked(directory);
    else for (int i = 0; i < count; i++) {
        WCHAR name[128];
        swprintf(name, 128, L"%ls-%d.txt", generationName, i);
        WCHAR *path = path_join(directory, name);
        if (path) { DeleteFileW(path); free(path); }
    }
    if (!ok) DeleteFileW(stagedManifest);
    free(stagedManifest);
    free(manifest);
    return ok;
}

int recovery_write(const WCHAR *directory, const RecoveryDocument *documents,
                   int count, RecoveryReadText readText, void *context) {
    if (count < 0 || count > RECOVERY_MAX_DOCUMENTS || (count && (!documents || !readText))) return 0;
    HANDLE lock = lock_directory(directory);
    if (lock == INVALID_HANDLE_VALUE) return 0;
    int ok = write_unlocked(directory, documents, count, readText, context);
    CloseHandle(lock);
    return ok;
}

int recovery_cleanup(const WCHAR *directory) {
    HANDLE lock = lock_directory(directory);
    if (lock == INVALID_HANDLE_VALUE) return 0;
    int ok = cleanup_unlocked(directory);
    CloseHandle(lock);
    return ok;
}

int recovery_clear(const WCHAR *directory) {
    HANDLE lock = lock_directory(directory);
    if (lock == INVALID_HANDLE_VALUE) return 0;
    int ok = clear_unlocked(directory);
    CloseHandle(lock);
    return ok;
}

int recovery_count(const WCHAR *directory) {
    WCHAR *manifest = directory ? path_join(directory, L"manifest.ini") : NULL;
    int count = manifest ? manifest_count(manifest) : -1;
    free(manifest);
    return count;
}

static int read_unlocked(const WCHAR *directory, int index, RecoveryEntry *entry) {
    WCHAR *manifest = path_join(directory, L"manifest.ini");
    if (!manifest) return 0;
    int count = manifest_count(manifest);
    if (index < 0 || index >= count) { free(manifest); return 0; }
    WCHAR key[32], name[128];
    swprintf(key, 32, L"File%d", index);
    DWORD length = GetPrivateProfileStringW(L"Recovery", key, L"", name, 128, manifest);
    if (!length || length >= 127 || !owned_name(name)) { free(manifest); return 0; }
    WCHAR *path = path_join(directory, name);
    entry->text = path ? read_bytes_file(path, &entry->length) : NULL;
    free(path);
    if (!entry->text) { free(manifest); return 0; }
    swprintf(key, 32, L"Path%d", index);
    GetPrivateProfileStringW(L"Recovery", key, L"", entry->path,
                             (DWORD)(sizeof(entry->path) / sizeof(entry->path[0])), manifest);
    swprintf(key, 32, L"Untitled%d", index);
    entry->untitledNumber = GetPrivateProfileIntW(L"Recovery", key, 0, manifest);
    swprintf(key, 32, L"Bom%d", index);
    entry->hasBom = GetPrivateProfileIntW(L"Recovery", key, 0, manifest) != 0;
    swprintf(key, 32, L"Encoding%d", index);
    int encoding = (int)GetPrivateProfileIntW(L"Recovery", key, TEXT_ENCODING_UTF8, manifest);
    entry->encoding = encoding >= TEXT_ENCODING_UTF8 && encoding <= TEXT_ENCODING_ANSI
        ? (TextEncoding)encoding : TEXT_ENCODING_UTF8;
    free(manifest);
    return 1;
}

int recovery_read(const WCHAR *directory, int index, RecoveryEntry *entry) {
    if (!entry) return 0;
    memset(entry, 0, sizeof(*entry));
    HANDLE lock = lock_directory(directory);
    if (lock == INVALID_HANDLE_VALUE) return 0;
    int ok = read_unlocked(directory, index, entry);
    CloseHandle(lock);
    return ok;
}

void recovery_entry_free(RecoveryEntry *entry) {
    free(entry->text);
    entry->text = NULL;
}
