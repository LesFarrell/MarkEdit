#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../src/text_codec.h"
#include "../src/file_io.h"
#include "../src/path_utils.h"
#include "../src/plugin_settings.h"
#include "../src/workspace_text.h"
#include "../src/markdown_links.h"

static int failures;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); failures++; } } while (0)

typedef struct LinkProbe {
    MarkdownLinkKind kinds[16];
    int lines[16];
    char targets[16][128];
    int count;
} LinkProbe;

static void collect_link(MarkdownLinkKind kind, int line, const char *target,
                         size_t targetLength, void *context) {
    LinkProbe *probe = (LinkProbe *)context;
    if (probe->count >= 16) return;
    int index = probe->count++;
    probe->kinds[index] = kind;
    probe->lines[index] = line;
    if (targetLength >= sizeof(probe->targets[index]))
        targetLength = sizeof(probe->targets[index]) - 1;
    memcpy(probe->targets[index], target, targetLength);
    probe->targets[index][targetLength] = 0;
}

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
    size_t escapedLength = 0;
    char *escaped = json_escape_length("quote ' slash \\ line\n\xE2\x80\xA8", &escapedLength);
    CHECK(escaped && strcmp(escaped, "quote \\' slash \\\\ line\\n\\u2028") == 0);
    CHECK(escapedLength == strlen(escaped));
    free(escaped);
    CHECK(json_escape(NULL) == NULL);
    escapedLength = 99;
    CHECK(json_escape_length(NULL, &escapedLength) == NULL && escapedLength == 0);

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
                char decoded_chunk[128]; size_t a = 0, b = 0;
                CHECK(text_decode(&decoder, encoded, cut, 0, decoded_chunk, sizeof(decoded_chunk), &a));
                CHECK(text_decode(&decoder, encoded + cut, bytes - cut, 1,
                                  decoded_chunk + a, sizeof(decoded_chunk) - a, &b));
                CHECK(a + b == length && memcmp(decoded_chunk, text, length) == 0);
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
    CHECK(!path_has_skipped_component(NULL));
    CHECK(path_has_skipped_component(L"src\\.git\\objects\\ab"));
    CHECK(path_has_skipped_component(L"src/node_modules/package/index.js"));
    CHECK(path_has_skipped_component(L"C:\\work\\BUILD\\object.o"));
    CHECK(!path_has_skipped_component(L"src\\build-tools\\file.c"));
    CHECK(!path_has_skipped_component(L"src\\output.txt"));

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

    PluginSettingEntry *listed = NULL;
    int listedCount = 0;
    CHECK(plugin_settings_list(path, "one", &listed, &listedCount));
    CHECK(listedCount == 2);
    CHECK(listed && strcmp(listed[0].section, "one.lua") == 0 &&
          strcmp(listed[0].key, "enabled") == 0 && strcmp(listed[0].value, "1") == 0);
    CHECK(listed && strcmp(listed[1].section, "one.lua") == 0 &&
          strcmp(listed[1].key, "message") == 0);
    plugin_settings_free_entries(listed, listedCount);

    listed = NULL;
    listedCount = -1;
    CHECK(plugin_settings_list(path, "Plugin:", &listed, &listedCount));
    CHECK(listed == NULL && listedCount == 0);

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

static void test_markdown_links(void) {
    const char *text = "# First heading\n"
                       "[page](docs/page.md#details) ![plot](<images/plot one.png>)\n"
                       "[[Notes|read this]] and `[[not a link]]`\n"
                       "[guide]: guide.md \"Guide\"\n"
                       "```markdown\n[ignored](missing.md)\n[[Ignored]]\n```\n"
                       "## First heading\n"
                       "Details\n-------\n";
    LinkProbe probe = {0};
    markdown_links_visit(text, collect_link, &probe);
    CHECK(probe.count == 4);
    CHECK(probe.kinds[0] == MARKDOWN_LINK_FILE && probe.lines[0] == 2 &&
          strcmp(probe.targets[0], "docs/page.md#details") == 0);
    CHECK(probe.kinds[1] == MARKDOWN_LINK_IMAGE &&
          strcmp(probe.targets[1], "images/plot one.png") == 0);
    CHECK(probe.kinds[2] == MARKDOWN_LINK_WIKI && probe.lines[2] == 3 &&
          strcmp(probe.targets[2], "Notes") == 0);
    CHECK(probe.kinds[3] == MARKDOWN_LINK_FILE && probe.lines[3] == 4 &&
          strcmp(probe.targets[3], "guide.md") == 0);
    CHECK(markdown_heading_anchor_exists(text, "first-heading"));
    CHECK(markdown_heading_anchor_exists(text, "first-heading-1"));
    CHECK(markdown_heading_anchor_exists(text, "details"));
    CHECK(!markdown_heading_anchor_exists(text, "missing"));

    probe = (LinkProbe){0};
    markdown_links_visit("unmatched ` [still](linked.md)\n", collect_link, &probe);
    CHECK(probe.count == 1 && strcmp(probe.targets[0], "linked.md") == 0);
}

static int same_path(const WCHAR *left, const WCHAR *right) {
    return left && right && _wcsicmp(left, right) == 0;
}

/* portable.mode beside this test executable must route both the per-user
   settings root and the Local AppData folders into portable-data. */
static void test_portable_paths(void) {
    WCHAR *directory = module_directory();
    CHECK(directory != NULL);
    if (!directory) return;
    WCHAR *marker = path_join(directory, L"portable.mode");
    WCHAR *portableData = path_join(directory, L"portable-data");
    WCHAR *recovery = path_join(portableData, L"Recovery");
    CHECK(marker && portableData && recovery);
    if (!marker || !portableData || !recovery) {
        free(marker); free(portableData); free(recovery); free(directory);
        return;
    }

    DeleteFileW(marker);
    RemoveDirectoryW(recovery);
    RemoveDirectoryW(portableData);
    CHECK(!portable_mode_enabled());

    int created = write_bytes_file(marker, "", 0);
    CHECK(created);
    if (created) {
        CHECK(portable_mode_enabled());
        WCHAR *config = get_user_config_directory();
        CHECK(same_path(config, portableData));
        WCHAR *local = get_local_app_directory(L"Recovery");
        CHECK(same_path(local, recovery));
        free(local);
        free(config);
    }

    DeleteFileW(marker);
    RemoveDirectoryW(recovery);
    RemoveDirectoryW(portableData);
    CHECK(!portable_mode_enabled());
    free(marker);
    free(portableData);
    free(recovery);
    free(directory);
}

int main(void) {
    test_codecs(); test_lossless_progress(); test_files(); test_plugin_settings(); test_workspace_text();
    test_markdown_links(); test_portable_paths();
    if (failures) return 1;
    puts("All core tests passed.");
    return 0;
}
