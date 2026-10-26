#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/recovery.h"
#include "../src/file_io.h"
#include "../src/path_utils.h"

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

typedef struct { const char *text; int failIndex; const WCHAR *directory; int lockChecked; } Source;

static char *read_text(void *context, int index, size_t *length) {
    Source *source = context;
    if (!source->lockChecked) {
        /* Cleanup must not enter while a generation is still being written. */
        CHECK(!recovery_cleanup(source->directory));
        source->lockChecked = 1;
    }
    if (index == source->failIndex) return NULL;
    *length = strlen(source->text);
    char *text = malloc(*length + 1);
    if (text) memcpy(text, source->text, *length + 1);
    return text;
}

static void create_artifact(const WCHAR *directory, const WCHAR *name) {
    WCHAR *path = path_join(directory, name);
    CHECK(path && write_bytes_file(path, "orphan", 6));
    free(path);
}

static int exists(const WCHAR *directory, const WCHAR *name) {
    WCHAR *path = path_join(directory, name);
    int found = path && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    free(path);
    return found;
}

static void expect_text(const WCHAR *directory, int index, const char *expected) {
    RecoveryEntry entry;
    CHECK(recovery_read(directory, index, &entry));
    if (entry.text) CHECK(!strcmp(entry.text, expected));
    recovery_entry_free(&entry);
}

static int is_directory(const WCHAR *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/* Each instance's snapshot lives in its own locked session: a running instance
   is never offered or cleared by another, and a crashed one survives until a
   later launch claims it. */
static void test_sessions(const WCHAR *temp) {
    WCHAR root[MAX_PATH];
    CHECK(GetTempFileNameW(temp, L"mrs", 0, root));
    CHECK(DeleteFileW(root));
    CHECK(CreateDirectoryW(root, NULL));
    RecoveryDocument doc = {NULL, 1, 0, TEXT_ENCODING_UTF8, 0};
    Source source = {"crashed work", -1, NULL, 1};
    RecoverySession crashed, live, empty, claimed[RECOVERY_MAX_SESSIONS];
    CHECK(recovery_session_open(root, &crashed));
    CHECK(recovery_session_open(root, &live));
    CHECK(recovery_session_open(root, &empty));
    CHECK(wcscmp(crashed.directory, live.directory));
    CHECK(recovery_write(crashed.directory, &doc, 1, read_text, &source));
    source.text = "live work";
    CHECK(recovery_write(live.directory, &doc, 1, read_text, &source));

    /* Sessions whose instances still run are not claimed. */
    CHECK(recovery_session_claim_orphans(root, claimed, RECOVERY_MAX_SESSIONS) == 0);

    /* A crash releases the lock without discarding the snapshot; an empty
       abandoned session is removed rather than offered. */
    WCHAR crashedDirectory[MAX_PATH], emptyDirectory[MAX_PATH];
    wcscpy(crashedDirectory, crashed.directory);
    wcscpy(emptyDirectory, empty.directory);
    recovery_session_close(&crashed, 0);
    recovery_session_close(&empty, 0);
    CHECK(!crashed.directory);

    /* Another instance clearing its own empty snapshot leaves the crash alone. */
    CHECK(recovery_write(live.directory, NULL, 0, NULL, NULL));
    CHECK(recovery_count(crashedDirectory) == 1);

    int count = recovery_session_claim_orphans(root, claimed, RECOVERY_MAX_SESSIONS);
    CHECK(count == 1);
    CHECK(!is_directory(emptyDirectory));
    if (count == 1) {
        CHECK(!wcscmp(claimed[0].directory, crashedDirectory));
        expect_text(claimed[0].directory, 0, "crashed work");
        /* A claimed session is locked against a second claimant. */
        RecoverySession again[RECOVERY_MAX_SESSIONS];
        CHECK(recovery_session_claim_orphans(root, again, RECOVERY_MAX_SESSIONS) == 0);
        recovery_session_close(&claimed[0], 1);
    }
    CHECK(!is_directory(crashedDirectory));

    /* A snapshot older builds wrote straight into root is still offered. */
    source.text = "legacy work";
    CHECK(recovery_write(root, &doc, 1, read_text, &source));
    count = recovery_session_claim_orphans(root, claimed, RECOVERY_MAX_SESSIONS);
    CHECK(count == 1);
    if (count == 1) {
        CHECK(claimed[0].lock == INVALID_HANDLE_VALUE);
        expect_text(claimed[0].directory, 0, "legacy work");
        recovery_session_close(&claimed[0], 1);
    }
    CHECK(recovery_count(root) == 0);

    WCHAR liveDirectory[MAX_PATH];
    wcscpy(liveDirectory, live.directory);
    recovery_session_close(&live, 1);
    CHECK(!is_directory(liveDirectory));
    WCHAR *lock = path_join(root, L"recovery.lock");
    CHECK(lock && DeleteFileW(lock));
    free(lock);
    CHECK(RemoveDirectoryW(root));
}

int main(void) {
    WCHAR temp[MAX_PATH], directory[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH, temp));
    CHECK(GetTempFileNameW(temp, L"mre", 0, directory));
    CHECK(DeleteFileW(directory));
    CHECK(CreateDirectoryW(directory, NULL));
    RecoveryDocument docs[2] = {
        {L"C:\\notes\\\x03A9.md", 7, 1, TEXT_ENCODING_UTF16BE, 0},
        {NULL, 8, 0, TEXT_ENCODING_ANSI, 1}
    };
    Source source = {"first snapshot", -1, directory, 0};
    CHECK(recovery_write(directory, docs, 2, read_text, &source));
    CHECK(recovery_count(directory) == 2);
    RecoveryEntry entry;
    CHECK(recovery_read(directory, 0, &entry));
    CHECK(!wcscmp(entry.path, docs[0].path));
    CHECK(entry.untitledNumber == 7 && entry.hasBom && entry.encoding == TEXT_ENCODING_UTF16BE);
    recovery_entry_free(&entry);
    CHECK(recovery_read(directory, 1, &entry));
    CHECK(!entry.path[0] && entry.untitledNumber == 8 && !entry.hasBom && entry.encoding == TEXT_ENCODING_ANSI);
    recovery_entry_free(&entry);

    /* A second-document failure cannot publish a partial replacement. */
    source.text = "incomplete replacement";
    source.failIndex = 1;
    CHECK(!recovery_write(directory, docs, 2, read_text, &source));
    CHECK(recovery_count(directory) == 2);
    expect_text(directory, 0, "first snapshot");
    expect_text(directory, 1, "first snapshot");

    const WCHAR *orphan = L"document-00000000-0000-0000-0000000000000000-0.txt";
    const WCHAR *staged = L"document-00000000-0000-0000-0000000000000000.ini";
    const WCHAR *partial = L"document-00000000-0000-0000-0000000000000000-0.txt.markedit-123-0.tmp";
    create_artifact(directory, orphan);
    create_artifact(directory, staged);
    create_artifact(directory, partial);
    create_artifact(directory, L"document-31.txt");
    create_artifact(directory, L"document-notes.txt");
    CHECK(recovery_cleanup(directory));
    CHECK(!exists(directory, orphan) && !exists(directory, staged) && !exists(directory, partial));
    CHECK(!exists(directory, L"document-31.txt"));
    CHECK(exists(directory, L"document-notes.txt"));
    expect_text(directory, 0, "first snapshot");

    /* Publication and clear failures retain the active manifest and contents. */
    WCHAR *manifest = path_join(directory, L"manifest.ini");
    HANDLE held = CreateFileW(manifest, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(held != INVALID_HANDLE_VALUE);
    source.failIndex = -1;
    CHECK(!recovery_write(directory, docs, 2, read_text, &source));
    CHECK(!recovery_clear(directory));
    CloseHandle(held);
    expect_text(directory, 0, "first snapshot");
    source.text = "complete replacement";
    CHECK(recovery_write(directory, docs, 1, read_text, &source));
    CHECK(recovery_count(directory) == 1);
    expect_text(directory, 0, source.text);
    CHECK(!recovery_read(directory, 1, &entry));
    CHECK(!recovery_write(directory, docs, RECOVERY_MAX_DOCUMENTS + 1, read_text, &source));
    CHECK(recovery_write(directory, NULL, 0, NULL, NULL));
    CHECK(recovery_count(directory) == 0);
    CHECK(exists(directory, L"document-notes.txt"));

    /* Legacy UTF-8 snapshots remain readable and malformed manifests prevent
       cleanup from discarding potentially recoverable document contents. */
    create_artifact(directory, L"document-0.txt");
    CHECK(WritePrivateProfileStringW(L"Recovery", L"Count", L"1", manifest));
    CHECK(WritePrivateProfileStringW(L"Recovery", L"File0", L"document-0.txt", manifest));
    CHECK(recovery_read(directory, 0, &entry));
    CHECK(entry.encoding == TEXT_ENCODING_UTF8 && !strcmp(entry.text, "orphan"));
    recovery_entry_free(&entry);
    CHECK(WritePrivateProfileStringW(L"Recovery", L"Count", L"999", manifest));
    create_artifact(directory, orphan);
    CHECK(!recovery_cleanup(directory));
    CHECK(exists(directory, orphan));
    CHECK(recovery_clear(directory));
    CHECK(!exists(directory, orphan));
    free(manifest);

    test_sessions(temp);

    WCHAR *path = path_join(directory, L"document-notes.txt");
    CHECK(DeleteFileW(path)); free(path);
    path = path_join(directory, L"recovery.lock");
    CHECK(DeleteFileW(path)); free(path);
    CHECK(RemoveDirectoryW(directory));
    if (failures) return 1;
    puts("All recovery tests passed.");
    return 0;
}
