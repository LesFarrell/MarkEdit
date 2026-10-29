#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "../src/output_text.h"
#include "../src/output_process.h"

static int s_ok = 1;

static void expect(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", what);
        s_ok = 0;
    }
}

/* ---- the decoder, writing what it emits as text: a style change shows as
   {n}, a lone carriage return as <CR> ---- */

typedef struct {
    char text[8192];
    size_t length;
    int style;
} Captured;

static void append(Captured *captured, const char *text, size_t length) {
    if (captured->length + length < sizeof(captured->text)) {
        memcpy(captured->text + captured->length, text, length);
        captured->length += length;
        captured->text[captured->length] = 0;
    }
}

static void capture_text(void *context, int style, const char *utf8, size_t length) {
    Captured *captured = (Captured *)context;
    if (style != captured->style) {
        char marker[8];
        snprintf(marker, sizeof(marker), "{%d}", style);
        append(captured, marker, strlen(marker));
        captured->style = style;
    }
    append(captured, utf8, length);
}

static void capture_carriage_return(void *context) {
    append((Captured *)context, "<CR>", 4);
}

static const OutputSink CAPTURE = { capture_text, capture_carriage_return };

/* Feeds `bytes` in pieces of `step` bytes and returns what came out. */
static const char *decode(const char *bytes, size_t step) {
    static Captured captured;
    memset(&captured, 0, sizeof(captured));
    OutputDecoder decoder;
    output_decoder_init(&decoder);
    size_t length = strlen(bytes);
    for (size_t at = 0; at < length; at += step) {
        size_t piece = length - at < step ? length - at : step;
        output_decoder_feed(&decoder, bytes + at, piece, &CAPTURE, &captured);
    }
    output_decoder_finish(&decoder, &CAPTURE, &captured);
    return captured.text;
}

static void expect_decoded(const char *bytes, const char *expected, const char *what) {
    for (size_t step = 1; step <= 8; step += 7) {
        const char *actual = decode(bytes, step);
        if (strcmp(actual, expected) != 0)
            fprintf(stderr, "%s (fed %u at a time): got \"%s\", expected \"%s\"\n",
                    what, (unsigned)step, actual, expected);
        expect(strcmp(actual, expected) == 0, what);
    }
}

static void test_decoder(void) {
    expect_decoded("plain\r\ntext\n", "plain\ntext\n", "CRLF becomes LF");
    expect_decoded("\x1b[31mred\x1b[0m done", "{2}red{0} done", "colours become styles");
    expect_decoded("\x1b[1;32mbold green\x1b[39m", "{11}bold green", "bold makes a colour bright");
    expect_decoded("\x1b[94mbright\x1b[m", "{13}bright", "bright colours and an empty reset");
    expect_decoded("\x1b[38;5;3m256\x1b[38;2;1;2;3mrgb", "{4}256rgb", "256-colour basics apply, true colour is read past");
    expect_decoded("10%\r50%\r100%\n", "10%<CR>50%<CR>100%\n", "a lone CR overwrites the line");
    expect_decoded("a\x1b[2Kb\x1b]0;title\x07" "c\x1b]8;;x\x1b\\d", "abcd", "other escape sequences are dropped");
    expect_decoded("caf\xc3\xa9 \xe2\x82\xac", "caf\xc3\xa9 \xe2\x82\xac", "UTF-8 survives being split");
    /* 0x82 is é in code page 437/850, the usual OEM code pages. */
    char oem[8] = "x\x82y";
    const char *decoded = decode(oem, 8);
    expect(decoded[0] == 'x' && (unsigned char)decoded[1] >= 0xc2 && decoded[strlen(decoded) - 1] == 'y',
           "bytes that aren't UTF-8 are read in the OEM code page");
    expect_decoded("bell\x07\x08", "bell", "other control characters are dropped");

    char plain[4097];
    memset(plain, 'x', sizeof(plain) - 1);
    plain[sizeof(plain) - 1] = 0;
    const char *decodedPlain = decode(plain, sizeof(plain) - 1);
    expect(strlen(decodedPlain) == sizeof(plain) - 1 &&
           memcmp(decodedPlain, plain, sizeof(plain) - 1) == 0,
           "large plain runs are emitted whole");
}

/* ---- file locations ---- */

static void expect_location(const char *line, const char *path, int number, int column) {
    OutputLocation location;
    int found = output_find_location(line, strlen(line), &location);
    char actual[256] = "";
    if (found) snprintf(actual, sizeof(actual), "%.*s", (int)location.pathLength, line + location.pathStart);
    int ok = found && strcmp(actual, path) == 0 && location.line == number && location.column == column;
    if (!ok) fprintf(stderr, "%s: got %s:%d:%d\n", line, actual, found ? location.line : 0,
                     found ? location.column : 0);
    expect(ok, "file locations are found in compiler and interpreter output");
}

static void test_locations(void) {
    expect_location("C:\\src\\main.c:12:5: error: expected ';'", "C:\\src\\main.c", 12, 5);
    expect_location("main.c:3: warning", "main.c", 3, 0);
    expect_location("lua: script.lua:7: attempt to call a nil value", "script.lua", 7, 0);
    expect_location("C:\\src\\x.cpp(42): error C2065: 'y': undeclared", "C:\\src\\x.cpp", 42, 0);
    expect_location("x.c(10,4): warning", "x.c", 10, 4);
    expect_location("  File \"C:\\p\\t.py\", line 9, in <module>", "C:\\p\\t.py", 9, 0);
    expect_location("At C:\\s\\a.ps1:3 char:5", "At C:\\s\\a.ps1", 3, 0);
    OutputLocation location;
    const char *none = "Process exited with code 3";
    expect(!output_find_location(none, strlen(none), &location), "plain text has no location");

    const char *included = "In file included from x.c:3:";
    expect(output_find_location(included, strlen(included), &location) &&
           output_location_next_candidate(included, &location) &&
           output_location_next_candidate(included, &location) &&
           output_location_next_candidate(included, &location) &&
           output_location_next_candidate(included, &location) &&
           location.pathLength == 3 && strncmp(included + location.pathStart, "x.c", 3) == 0 &&
           !output_location_next_candidate(included, &location),
           "leading words can be dropped from a location's path");

    const char *diff =
        "diff --git a/src/file name.c b/src/file name.c\n"
        "--- a/src/file name.c\n"
        "+++ b/src/file name.c\n"
        "@@ -10,4 +20,5 @@ static void example(void) {\n"
        " context\n"
        "-old\n"
        "+new\n"
        "+another\n";
    const char *added = strstr(diff, "+another");
    expect(output_find_diff_location(diff, strlen(diff), (size_t)(added - diff), &location) &&
           location.pathLength == strlen("src/file name.c") &&
           strncmp(diff + location.pathStart, "src/file name.c", location.pathLength) == 0 &&
           location.line == 22,
           "a diff row resolves to its working-tree file and new line");
    const char *removed = strstr(diff, "-old");
    expect(output_find_diff_location(diff, strlen(diff), (size_t)(removed - diff), &location) &&
           location.line == 21,
           "a removed diff row resolves to the following working-tree line");
    const char *hunk = strstr(diff, "@@ -10");
    expect(output_find_diff_location(diff, strlen(diff), (size_t)(hunk - diff), &location) &&
           location.line == 20,
           "a diff hunk header resolves to its first new line");
    const char deleted[] = "+++ /dev/null\n@@ -1 +0,0 @@\n-old\n";
    expect(!output_find_diff_location(deleted, sizeof(deleted) - 1,
                                      sizeof(deleted) - 2, &location),
           "a deleted file diff is not offered as a working-tree location");
}

/* ---- a real program on pipes ---- */

#define WM_TEST_OUTPUT (WM_APP + 1)

/* Collects the chunks for session until it finishes, or ten seconds pass. */
static int collect(HWND window, int session, char *output, size_t capacity, DWORD *exitCode) {
    size_t used = 0;
    DWORD started = GetTickCount();
    output[0] = 0;
    while (GetTickCount() - started < 10000) {
        MSG msg;
        if (!PeekMessageW(&msg, window, WM_TEST_OUTPUT, WM_TEST_OUTPUT, PM_REMOVE)) {
            Sleep(10);
            continue;
        }
        OutputChunk *chunk = (OutputChunk *)msg.lParam;
        int finished = chunk->session == session && chunk->finished;
        if (chunk->session == session && used + chunk->length < capacity) {
            memcpy(output + used, chunk->data, chunk->length);
            used += chunk->length;
            output[used] = 0;
        }
        if (finished) *exitCode = chunk->exitCode;
        free(chunk);
        if (finished) return 1;
    }
    return 0;
}

static void test_process(void) {
    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    WCHAR command[] = L"cmd.exe /q /c \"echo out & echo %MARKEDIT_TEST% & set /p line= & "
                      L"call echo got %line% & echo err 1>&2 & exit /b 7\"";
    OutputProcess *process = output_process_start(command, NULL, L"MARKEDIT_TEST=added\0",
                                                  window, WM_TEST_OUTPUT, 1);
    expect(process != NULL, "a program starts on pipes");
    if (!process) return;
    expect(output_process_write(process, "typed\r\n", 7), "input reaches the program");
    char output[1024];
    DWORD exitCode = 0;
    expect(collect(window, 1, output, sizeof(output), &exitCode), "the program finishes");
    expect(strstr(output, "out") && strstr(output, "added") && strstr(output, "got typed") &&
           strstr(output, "err") && exitCode == 7,
           "output, error, added environment, input and the exit code all arrive");
    output_process_free(process);

    WCHAR forever[] = L"cmd.exe /q /c \"echo waiting & ping -n 30 127.0.0.1 >nul\"";
    process = output_process_start(forever, NULL, NULL, window, WM_TEST_OUTPUT, 2);
    expect(process != NULL, "a long-running program starts");
    if (process) {
        Sleep(300);
        output_process_stop(process);
        expect(collect(window, 2, output, sizeof(output), &exitCode) && strstr(output, "waiting"),
               "stopping ends the program and everything it started");
        output_process_free(process);
    }
    WCHAR missing[] = L"markedit-no-such-program.exe";
    expect(output_process_start(missing, NULL, NULL, window, WM_TEST_OUTPUT, 3) == NULL,
           "a program that doesn't exist doesn't start");
    DestroyWindow(window);
}

int main(void) {
    test_decoder();
    test_locations();
    test_process();
    if (s_ok) puts("All output panel tests passed.");
    return s_ok ? 0 : 1;
}
