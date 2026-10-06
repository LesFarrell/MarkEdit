#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../json_formatter.h"
#include "../text_codec.h"
#include "../file_io.h"
#include "../path_utils.h"
#include "../workspace_text.h"

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

static void test_json(void) {
    const char *invalid[] = { "[1 2]", "{\"a\" 1}", "{\"a\":}", "[truefalse]",
        "[01]", "[1.]", "[1e+]", "[+1]", "[NaN]", "[undefined]", "[1,]", "[,1]",
        "{1:2}", "{\"a\":1 \"b\":2}", "[\"\\q\"]", "[\"\\u12xz\"]", "[\"a\nb\"]",
        "{}{}", "[1}", "true false", "", "[\"unterminated]" };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        for (int pretty = 0; pretty < 2; pretty++) {
            char *out = json_reformat(invalid[i], (int)strlen(invalid[i]), "\n", pretty);
            CHECK(out == NULL);
            free(out);
        }
    }
    const char *valid[] = { "null", "true", "false", "-0.25E+3", "\"hello\\u1234\"",
        "{}", "[]", "{\"a\":[1,true,null,{\"b\":\"\\\"\"}]}" };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        char *pretty = json_reformat(valid[i], (int)strlen(valid[i]), "\r\n", 1);
        CHECK(pretty != NULL);
        char *compact = pretty ? json_reformat(pretty, (int)strlen(pretty), "\n", 0) : NULL;
        CHECK(compact && strcmp(compact, valid[i]) == 0);
        free(pretty); free(compact);
    }
    char *out = json_reformat("[1\0]", 4, "\n", 0);
    CHECK(out == NULL); free(out);
    char deep[515];
    memset(deep, '[', 257); memset(deep + 257, ']', 257); deep[514] = 0;
    out = json_reformat(deep, 514, "\n", 0);
    CHECK(out == NULL); free(out);
}

static void test_codecs(void) {
    WCHAR *decoded = decode_json_string(L"\"line\\n\\u20ac\"");
    CHECK(decoded && wcscmp(decoded, L"line\n\u20ac") == 0);
    free(decoded);
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
    text = read_utf8_text_file(path, 7); CHECK(text == NULL); free(text);
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

static void test_workspace_text(void) {
    const char *unicode = "zero CAF\xC3\x89 \xCE\x91\xCE\x98\xCE\xA9\xCE\x9D\xCE\x91 end";
    CHECK(utf8_case_find(unicode, "caf\xC3\xA9") == unicode + 5);
    CHECK(utf8_case_find(unicode, "\xCE\xB1\xCE\xB8\xCF\x89\xCE\xBD\xCE\xB1") != NULL);
    CHECK(utf8_case_find(unicode, "missing") == NULL);

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
}

int main(void) {
    test_json(); test_codecs(); test_lossless_progress(); test_files(); test_workspace_text();
    if (failures) return 1;
    puts("All core tests passed.");
    return 0;
}
