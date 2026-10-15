#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../src/text_codec.h"
#include "../src/file_io.h"
#include "../src/path_utils.h"
#include "../src/plugin_settings.h"
#include "../src/workspace_text.h"

static int failures;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); failures++; } } while (0)

typedef struct ProgressProbe { int calls, cancelAfter; size_t completed, total; } ProgressProbe;

/* cancelAfter <= 0 runs the operation to completion; otherwise the callback
   cancels on that call, which is also the call it stops being counted from. */
static int probe_progress(size_t completed, size_t total, void *context) {
    ProgressProbe *probe = (ProgressProbe *)context;
    probe->calls++;
    probe->completed = completed;
    probe->total = total;
    return probe->cancelAfter <= 0 || probe->calls < probe->cancelAfter;
}

static void test_codecs(void) {
    char *escaped = json_escape("quote ' slash \\ line\n\xE2\x80\xA8");
    CHECK(escaped && strcmp(escaped, "quote \\' slash \\\\ line\\n\\u2028") == 0);
    free(escaped);
    CHECK(json_escape(NULL) == NULL);

    WCHAR *decoded = decode_json_string(L"\"line\\n\\u20ac\"");
    CHECK(decoded && wcscmp(decoded, L"line\n\u20ac") == 0);
    free(decoded);

    CHECK(text_decode_capacity(SIZE_MAX) == 0);
    CHECK(text_encode_capacity(SIZE_MAX) == 0);
    size_t emptyLength = 99;
    char *empty = text_decode_all(TEXT_ENCODING_UTF8, NULL, 0, &emptyLength);
    CHECK(empty && emptyLength == 0 && empty[0] == 0);
    free(empty);
    CHECK(text_decode_all(TEXT_ENCODING_UTF8, NULL, 1, NULL) == NULL);
    decoded = decode_json_string(L"\"ok\" trailing");
    CHECK(decoded == NULL);
    free(decoded);
    decoded = decode_json_string(L"\"raw\nnewline\"");
    CHECK(decoded == NULL);
    free(decoded);

    const char *text = "A\xC3\xA9\xF0\x9F\x98\x80Z";
    size_t length = strlen(text);
    for (int encoding = TEXT_ENCODING_UTF16LE; encoding <= TEXT_ENCODING_UTF16BE; encoding++) {
        for (size_t split = 0; split <= length; split++) {
            TextCodecState encoder = {(TextEncoding)encoding, {0}, 0};
            char encoded[128]; size_t first = 0, second = 0;
            CHECK(text_encode(&encoder, text, split, 0, encoded, sizeof(encoded), &first, NULL));
            CHECK(text_encode(&encoder, text + split, length - split, 1,
                              encoded + first, sizeof(encoded) - first, &second, NULL));
            size_t bytes = first + second;
            for (size_t cut = 0; cut <= bytes; cut++) {
                TextCodecState decoder = {(TextEncoding)encoding, {0}, 0};
                char decoded[128]; size_t a = 0, b = 0;
                CHECK(text_decode(&decoder, encoded, cut, 0, decoded, sizeof(decoded), &a));
                CHECK(text_decode(&decoder, encoded + cut, bytes - cut, 1,
                                  decoded + a, sizeof(decoded) - a, &b));
                CHECK(a + b == length && memcmp(decoded, text, length) == 0);
            }
        }
    }

    size_t largeLength = 2 * 1024 * 1024 + 17;
    char *large = (char *)malloc(largeLength);
    CHECK(large != NULL);
    if (large) {
        memset(large, 'a', largeLength);
        ProgressProbe probe = {0, 2, 0, 0};
        CHECK(text_encoding_is_lossless_progress(large, largeLength, TEXT_ENCODING_ANSI,
                                                 probe_progress, &probe) == -2);
        CHECK(probe.calls == 2 && probe.completed < probe.total && probe.total == largeLength);
        free(large);
    }
}

/* The UTF-16 branch of text_encoding_is_lossless_progress() validates in
   pieces, so a multi-byte character landing across a piece boundary is the
   case that would break if the validator's pending state weren't carried
   between feed() calls. The codec's internal piece is 1 MiB, so build a
   buffer spanning several pieces with a 3-byte character astride the first. */
static void test_lossless_progress(void) {
    enum { PIECE = 1024 * 1024, LENGTH = 2 * PIECE + 17 };
    char *buffer = (char *)malloc(LENGTH);
    CHECK(buffer != NULL);
    if (!buffer) return;
    memset(buffer, 'a', LENGTH);
    buffer[PIECE - 1] = (char)0xE2;   /* U+20AC EURO SIGN, split 1/2/1 */
    buffer[PIECE]     = (char)0x82;
    buffer[PIECE + 1] = (char)0xAC;

    /* UTF-16 holds every character: one callback per piece, ending at total. */
    for (int encoding = TEXT_ENCODING_UTF16LE; encoding <= TEXT_ENCODING_UTF16BE; encoding++) {
        ProgressProbe probe = {0, 0, 0, 0};
        CHECK(text_encoding_is_lossless_progress(buffer, LENGTH, (TextEncoding)encoding,
                                                 probe_progress, &probe) == 1);
        CHECK(probe.calls == 3 && probe.completed == LENGTH && probe.total == LENGTH);
        CHECK(text_encoding_is_lossless(buffer, LENGTH, (TextEncoding)encoding) == 1);
    }

    /* UTF-8 is the buffer's own bytes, so it needs no scan but still ticks. */
    ProgressProbe utf8 = {0, 0, 0, 0};
    CHECK(text_encoding_is_lossless_progress(buffer, LENGTH, TEXT_ENCODING_UTF8,
                                             probe_progress, &utf8) == 1);
    CHECK(utf8.calls == 1 && utf8.completed == LENGTH);

    /* Cancelling mid-scan stops each encoding that walks the buffer. */
    ProgressProbe cancel = {0, 2, 0, 0};
    CHECK(text_encoding_is_lossless_progress(buffer, LENGTH, TEXT_ENCODING_UTF16LE,
                                             probe_progress, &cancel) == -2);
    CHECK(cancel.calls == 2 && cancel.completed < cancel.total);
    cancel = (ProgressProbe){0, 1, 0, 0};
    CHECK(text_encoding_is_lossless_progress(buffer, LENGTH, TEXT_ENCODING_UTF8,
                                             probe_progress, &cancel) == -2);
    CHECK(cancel.calls == 1);
    cancel = (ProgressProbe){0, 2, 0, 0};
    CHECK(text_encoding_is_lossless_progress(buffer, LENGTH, TEXT_ENCODING_ANSI,
                                             probe_progress, &cancel) == -2);
    CHECK(cancel.calls == 2 && cancel.completed < cancel.total);

    /* Bytes that aren't valid UTF-8 can't be stored as UTF-16, whether or not
       the offending byte opens a piece or leaves a sequence pending at EOF. */
    buffer[PIECE * 2] = (char)0xFF;
    CHECK(text_encoding_is_lossless(buffer, LENGTH, TEXT_ENCODING_UTF16LE) == 0);
    buffer[PIECE * 2] = 'a';
    buffer[LENGTH - 1] = (char)0x82;
    CHECK(text_encoding_is_lossless(buffer, LENGTH, TEXT_ENCODING_UTF16LE) == 0);
    buffer[LENGTH - 1] = 'a';

    /* Empty input is lossless everywhere, with and without a callback. */
    for (int encoding = TEXT_ENCODING_UTF8; encoding <= TEXT_ENCODING_ANSI; encoding++) {
        ProgressProbe empty = {0, 0, 0, 0};
        CHECK(text_encoding_is_lossless_progress(buffer, 0, (TextEncoding)encoding, NULL, NULL) == 1);
        CHECK(text_encoding_is_lossless_progress(buffer, 0, (TextEncoding)encoding,
                                                 probe_progress, &empty) == 1);
        CHECK(empty.calls == 1 && empty.completed == 0);
    }
    free(buffer);
}

static void test_files(void) {
    CHECK(path_join(NULL, L"file.txt") == NULL);
    CHECK(path_join(L"C:\\folder", NULL) == NULL);
    CHECK(absolute_path(NULL) == NULL);
    CHECK(!skipped_workspace_directory(NULL));

    WCHAR directory[MAX_PATH], path[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, directory);
    CHECK(length && length < MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    UINT created = GetTempFileNameW(directory, L"mkt", 0, path);
    CHECK(created != 0);
    if (!created) return;
    CHECK(write_bytes_file(path, "\xEF\xBB\xBFhello", 8));
    char *text = read_utf8_text_file(path, 8);
    CHECK(text && strcmp(text, "hello") == 0); free(text);
    size_t utf8Length = 99;
    text = read_utf8_text_file_length(path, 8, &utf8Length);
    CHECK(text && utf8Length == 5 && memcmp(text, "hello", 5) == 0); free(text);
    text = read_utf8_text_file(path, 7); CHECK(text == NULL); free(text);
    CHECK(write_bytes_file(path, "a\0b", 3));
    text = read_utf8_text_file_length(path, 3, &utf8Length);
    CHECK(text && utf8Length == 3 && memcmp(text, "a\0b", 3) == 0); free(text);
    CHECK(write_bytes_file(path, "\xFF\xFEh\0i\0", 6));
    int encoding = -1, bom = 0, count = 0;
    text = read_text_file(path, &count, &encoding, &bom);
    CHECK(text && strcmp(text, "hi") == 0 && count == 2 && bom && encoding == TEXT_ENCODING_UTF16LE);
    free(text);
    text = read_utf8_text_file(path, 6); CHECK(text == NULL); free(text);
    CHECK(write_bytes_file(path, "", 0));
    text = read_utf8_text_file(path, 0); CHECK(text && !text[0]); free(text);

    size_t largeLength = 2 * 1024 * 1024;
    char *large = (char *)malloc(largeLength);
    CHECK(large != NULL);
    if (large) {
        memset(large, 'x', largeLength);
        CHECK(write_bytes_file(path, "original", 8));
        ProgressProbe probe = {0, 2, 0, 0};
        CHECK(!write_file_atomic_progress(path, large, largeLength, TEXT_ENCODING_UTF8, 0,
                                          probe_progress, &probe));
        int originalLength = 0;
        char *original = read_bytes_file(path, &originalLength);
        CHECK(original && originalLength == 8 && memcmp(original, "original", 8) == 0);
        free(original);
        free(large);
    }
    CHECK(DeleteFileW(path));
}

static HANDLE open_follow_test_file(const WCHAR *path, DWORD access, DWORD creation) {
    return CreateFileW(path, access,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void test_file_following(void) {
    WCHAR directory[MAX_PATH], path[MAX_PATH];
    DWORD pathLength = GetTempPathW(MAX_PATH, directory);
    CHECK(pathLength && pathLength < MAX_PATH);
    if (!pathLength || pathLength >= MAX_PATH) return;
    if (!GetTempFileNameW(directory, L"mff", 0, path)) {
        CHECK(0);
        return;
    }
    CHECK(write_bytes_file(path, "old", 3));

    HANDLE loaded = open_follow_test_file(path, GENERIC_READ, OPEN_EXISTING);
    CHECK(loaded != INVALID_HANDLE_VALUE);
    if (loaded == INVALID_HANDLE_VALUE) { DeleteFileW(path); return; }

    /* Simulate a live writer growing the log after the loader sampled three
       bytes but before it captured identity/signature metadata. */
    HANDLE writer = open_follow_test_file(path, FILE_APPEND_DATA, OPEN_EXISTING);
    DWORD written = 0;
    CHECK(writer != INVALID_HANDLE_VALUE &&
          WriteFile(writer, "new", 3, &written, NULL) && written == 3);
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);

    FileFollowState state;
    CHECK(file_follow_capture(&state, loaded, 3));
    CHECK(state.size == 3 && state.tailLength == 3 &&
          memcmp(state.tail, "old", 3) == 0);
    BY_HANDLE_FILE_INFORMATION info;
    CHECK(GetFileInformationByHandle(loaded, &info));
    ULONGLONG size = ((ULONGLONG)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    CHECK(size == 6);
    CHECK(file_follow_check_growth(&state, loaded, &info, size) == FILE_FOLLOW_APPEND);

    FileFollowState rolling = state;
    char growth[100];
    for (size_t i = 0; i < sizeof(growth); i++) growth[i] = (char)i;
    file_follow_commit_bytes(&rolling, growth, 37);
    file_follow_commit_bytes(&rolling, growth + 37, sizeof(growth) - 37);
    CHECK(rolling.size == state.size + sizeof(growth));
    CHECK(rolling.tailLength == FILE_FOLLOW_TAIL_SIZE);
    CHECK(memcmp(rolling.tail, growth + sizeof(growth) - FILE_FOLLOW_TAIL_SIZE,
                 FILE_FOLLOW_TAIL_SIZE) == 0);

    /* A same-file copytruncate/regrow can still be larger than the old offset;
       changing the signature must force a reload instead of an append. */
    writer = open_follow_test_file(path, GENERIC_WRITE, OPEN_EXISTING);
    CHECK(writer != INVALID_HANDLE_VALUE);
    if (writer != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER zero = {0};
        CHECK(SetFilePointerEx(writer, zero, NULL, FILE_BEGIN));
        CHECK(WriteFile(writer, "OLD", 3, &written, NULL) && written == 3);
        CloseHandle(writer);
    }
    CHECK(GetFileInformationByHandle(loaded, &info));
    CHECK(file_follow_check_growth(&state, loaded, &info, size) == FILE_FOLLOW_RELOAD);

    /* Truncation and replacement are reloads, never negative/duplicate appends. */
    writer = open_follow_test_file(path, GENERIC_WRITE, TRUNCATE_EXISTING);
    CHECK(writer != INVALID_HANDLE_VALUE);
    if (writer != INVALID_HANDLE_VALUE) {
        CHECK(WriteFile(writer, "x", 1, &written, NULL) && written == 1);
        CloseHandle(writer);
    }
    CHECK(GetFileInformationByHandle(loaded, &info));
    size = ((ULONGLONG)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    CHECK(file_follow_check_growth(&state, loaded, &info, size) == FILE_FOLLOW_RELOAD);
    CloseHandle(loaded);

    CHECK(DeleteFileW(path));
    CHECK(write_bytes_file(path, "replacement", 11));
    HANDLE replacement = open_follow_test_file(path, GENERIC_READ, OPEN_EXISTING);
    CHECK(replacement != INVALID_HANDLE_VALUE);
    if (replacement != INVALID_HANDLE_VALUE) {
        CHECK(GetFileInformationByHandle(replacement, &info));
        size = ((ULONGLONG)info.nFileSizeHigh << 32) | info.nFileSizeLow;
        CHECK(file_follow_check_growth(&state, replacement, &info, size) == FILE_FOLLOW_RELOAD);
        CloseHandle(replacement);
    }

    FILETIME failedTime = {17, 23};
    file_follow_block_retry(&state, 100, failedTime);
    CHECK(file_follow_retry_blocked(&state, 100, failedTime));
    failedTime.dwLowDateTime++;
    CHECK(!file_follow_retry_blocked(&state, 100, failedTime));
    CHECK(!state.retryBlocked);

    /* Follow decoding deliberately does not finalize each read: an incomplete
       UTF-16 code unit must remain pending until the next append arrives. */
    TextCodecState codec = {TEXT_ENCODING_UTF16LE, {0}, 0};
    char decoded[8]; size_t produced = 99;
    CHECK(text_decode(&codec, "A", 1, 0, decoded, sizeof(decoded), &produced));
    CHECK(produced == 0 && codec.carryLength == 1);
    CHECK(text_decode(&codec, "\0", 1, 0, decoded, sizeof(decoded), &produced));
    CHECK(produced == 1 && decoded[0] == 'A' && codec.carryLength == 0);
    CHECK(text_encoding_has_incomplete_suffix(TEXT_ENCODING_UTF16LE, "A", 1));
    CHECK(!text_encoding_has_incomplete_suffix(TEXT_ENCODING_UTF16LE, "A\0", 2));
    CHECK(text_encoding_has_incomplete_suffix(TEXT_ENCODING_UTF16LE, "\x3d\xd8", 2));

    CHECK(DeleteFileW(path));
}

static void test_plugin_settings(void) {
    WCHAR directory[MAX_PATH], path[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, directory);
    CHECK(length && length < MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    CHECK(GetTempFileNameW(directory, L"mps", 0, path) != 0);
    DeleteFileW(path); /* a missing store starts as an empty JSON object */

    CHECK(plugin_settings_get(path, "one.lua", "missing") == NULL);
    CHECK(plugin_settings_set(path, "one.lua", "enabled", "1"));
    CHECK(plugin_settings_set(path, "one.lua", "message", "quote \" slash \\ line\n\xE2\x82\xAC"));
    CHECK(plugin_settings_set(path, "two.lua", "width", "80"));

    char *value = plugin_settings_get(path, "one.lua", "enabled");
    CHECK(value && strcmp(value, "1") == 0); free(value);
    value = plugin_settings_get(path, "one.lua", "message");
    CHECK(value && strcmp(value, "quote \" slash \\ line\n\xE2\x82\xAC") == 0); free(value);
    value = plugin_settings_get(path, "two.lua", "width");
    CHECK(value && strcmp(value, "80") == 0); free(value);

    CHECK(plugin_settings_set(path, "one.lua", "enabled", "0"));
    CHECK(plugin_settings_set(path, "one.lua", "message", NULL));
    value = plugin_settings_get(path, "one.lua", "enabled");
    CHECK(value && strcmp(value, "0") == 0); free(value);
    CHECK(plugin_settings_get(path, "one.lua", "message") == NULL);

    int jsonLength = 0;
    char *json = read_bytes_file(path, &jsonLength);
    CHECK(json && jsonLength > 0 && strstr(json, "\"one.lua\": {") &&
          strstr(json, "\"two.lua\": {") && strstr(json, "\"width\": \"80\""));
    free(json);

    CHECK(plugin_settings_clear_section(path, "one.lua"));
    CHECK(plugin_settings_get(path, "one.lua", "enabled") == NULL);
    value = plugin_settings_get(path, "two.lua", "width");
    CHECK(value && strcmp(value, "80") == 0); free(value);

    const char escapedJson[] = "{\"Unicode\":{\"value\":\"\\u20ac \\ud83d\\ude00\"}}";
    CHECK(write_bytes_file(path, escapedJson, sizeof(escapedJson) - 1));
    value = plugin_settings_get(path, "Unicode", "value");
    CHECK(value && strcmp(value, "\xE2\x82\xAC \xF0\x9F\x98\x80") == 0); free(value);

    /* A malformed hand-edited store is never silently overwritten. */
    CHECK(write_bytes_file(path, "{ broken", 8));
    CHECK(!plugin_settings_set(path, "one.lua", "enabled", "1"));
    json = read_bytes_file(path, &jsonLength);
    CHECK(json && jsonLength == 8 && memcmp(json, "{ broken", 8) == 0); free(json);
    CHECK(DeleteFileW(path));
}

static void test_workspace_text(void) {
    const char *unicode = "zero CAF\xC3\x89 \xCE\x91\xCE\x98\xCE\xA9\xCE\x9D\xCE\x91 end";
    CHECK(utf8_case_find(unicode, "caf\xC3\xA9") == unicode + 5);
    CHECK(utf8_case_find(unicode, "\xCE\xB1\xCE\xB8\xCF\x89\xCE\xBD\xCE\xB1") != NULL);
    CHECK(utf8_case_find(unicode, "missing") == NULL);

    /* A linguistic match can differ in length from the needle: precomposed
       "café" (5 bytes) matches decomposed "cafe" + U+0301 (6 bytes). */
    size_t matched = 0;
    const char *decomposed = "a cafe\xCC\x81 b";
    CHECK(utf8_case_find_length(decomposed, "caf\xC3\xA9", &matched) == decomposed + 2 &&
          matched == 6);
    CHECK(utf8_case_find_length(decomposed, "CAFE", &matched) == decomposed + 2 && matched == 4);

    char lines[] = "one\rtwo\r\nthree\nfour";
    size_t terminator = 0;
    char *end = text_line_break(lines, &terminator);
    CHECK(end == lines + 3 && terminator == 1);
    end = text_line_break(end + terminator, &terminator);
    CHECK(end == lines + 7 && terminator == 2);
    end = text_line_break(end + terminator, &terminator);
    CHECK(end == lines + 14 && terminator == 1);

    const char *markdown = "[inline](docs/Old.md#part)\n"
                           "![image](<docs/Old.md>)\n"
                           "[ref]: docs/Old.md \"title\"\n"
                           "plain docs/Old.md\n";
    int changes = 0;
    char *rewritten = markdown_rewrite_link_targets(markdown, "docs/old.md",
                                                     "archive/New.md", &changes);
    CHECK(rewritten != NULL && changes == 3);
    CHECK(rewritten && strstr(rewritten, "[inline](archive/New.md#part)"));
    CHECK(rewritten && strstr(rewritten, "![image](<archive/New.md>)"));
    CHECK(rewritten && strstr(rewritten, "[ref]: archive/New.md \"title\""));
    CHECK(rewritten && strstr(rewritten, "plain docs/Old.md"));
    free(rewritten);

    changes = 0;
    rewritten = markdown_rewrite_link_targets("[dot](./docs/Old.md)\n", "./docs/old.md",
                                               "./archive/New.md", &changes);
    CHECK(rewritten && changes == 1 && strstr(rewritten, "(./archive/New.md)"));
    free(rewritten);

    /* A new name a bare destination can't hold moves to the <...> form,
       keeping the fragment; an already-angled destination stays as it is. */
    changes = 0;
    rewritten = markdown_rewrite_link_targets("[a](notes.md#top) [b](<notes.md>)\n", "notes.md",
                                               "my notes (old).md", &changes);
    CHECK(rewritten && changes == 2 &&
          strcmp(rewritten, "[a](<my notes (old).md#top>) [b](<my notes (old).md>)\n") == 0);
    free(rewritten);

    /* A renamed folder rewrites links into it, but not a folder that merely
       shares its name as a prefix. */
    changes = 0;
    rewritten = markdown_rewrite_link_directory(
        "[a](docs/intro.md) [b](Docs/sub/x.md#y) [c](docs) [d](docsextra/z.md)\n",
        "docs", "user guide", &changes);
    CHECK(rewritten && changes == 3 &&
          strcmp(rewritten, "[a](<user guide/intro.md>) [b](<user guide/sub/x.md#y>) "
                            "[c](<user guide>) [d](docsextra/z.md)\n") == 0);
    free(rewritten);

    /* Link examples are code, not workspace references, and must survive a
       rename verbatim. Real links around them are still updated. */
    const char *withCode = "[before](old.md) `code [link](old.md)`\n"
                           "```markdown\n[fenced](old.md)\n```\n"
                           "[after](old.md)\n";
    changes = 0;
    rewritten = markdown_rewrite_link_targets(withCode, "old.md", "new.md", &changes);
    CHECK(rewritten && changes == 2 &&
          strcmp(rewritten, "[before](new.md) `code [link](old.md)`\n"
                            "```markdown\n[fenced](old.md)\n```\n"
                            "[after](new.md)\n") == 0);
    free(rewritten);

    changes = 0;
    rewritten = markdown_rewrite_link_targets("unmatched ` [link](old.md)\n", "old.md",
                                               "new.md", &changes);
    CHECK(rewritten && changes == 1 &&
          strcmp(rewritten, "unmatched ` [link](new.md)\n") == 0);
    free(rewritten);
}

int main(void) {
    test_codecs(); test_lossless_progress(); test_files(); test_file_following();
    test_plugin_settings(); test_workspace_text();
    if (failures) return 1;
    puts("All core tests passed.");
    return 0;
}
