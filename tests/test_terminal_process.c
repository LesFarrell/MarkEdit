#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "../src/terminal_process.h"

static int s_ok = 1;

static void expect(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", what);
        s_ok = 0;
    }
}

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
        TerminalChunk *chunk = (TerminalChunk *)msg.lParam;
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
    expect(terminal_process_available(), "ConPTY is available on the machine running the tests");
    if (!terminal_process_available()) return;

    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    WCHAR command[] = L"cmd.exe /q /c \"echo hello & exit /b 7\"";
    TerminalProcess *process = terminal_process_start(command, NULL, 80, 24, window, WM_TEST_OUTPUT, 1);
    expect(process != NULL, "a shell starts on a pseudoconsole");
    if (process) {
        char output[4096];
        DWORD exitCode = 0;
        expect(collect(window, 1, output, sizeof(output), &exitCode), "the shell finishes");
        expect(strstr(output, "hello") != NULL, "its output arrives");
        expect(exitCode == 7, "its exit code is reported");
        terminal_process_free(process);
    }

    WCHAR forever[] = L"cmd.exe /q /c \"echo waiting & ping -n 30 127.0.0.1 >nul\"";
    process = terminal_process_start(forever, NULL, 80, 24, window, WM_TEST_OUTPUT, 2);
    expect(process != NULL, "a long-running shell starts");
    if (process) {
        char output[4096];
        DWORD exitCode = 0;
        Sleep(300);
        terminal_process_stop(process);
        expect(collect(window, 2, output, sizeof(output), &exitCode) && strstr(output, "waiting"),
               "stopping ends the shell and everything it started");
        terminal_process_free(process);
    }

    WCHAR interactive[] = L"cmd.exe /q";
    process = terminal_process_start(interactive, NULL, 80, 24, window, WM_TEST_OUTPUT, 3);
    expect(process != NULL, "an interactive shell starts");
    if (process) {
        static const char input[] = "echo roundtrip\r\n";
        int queued = 1;
        for (size_t i = 0; i < sizeof(input) - 1; i++)
            queued = queued && terminal_process_write(process, input + i, 1);
        expect(queued, "individual keystrokes are queued without blocking the caller");
        char output[4096];
        DWORD started = GetTickCount();
        output[0] = 0;
        size_t used = 0;
        while (GetTickCount() - started < 5000 && !strstr(output, "roundtrip\r\nroundtrip")) {
            MSG msg;
            if (!PeekMessageW(&msg, window, WM_TEST_OUTPUT, WM_TEST_OUTPUT, PM_REMOVE)) { Sleep(10); continue; }
            TerminalChunk *chunk = (TerminalChunk *)msg.lParam;
            if (chunk->session == 3 && used + chunk->length < sizeof(output)) {
                memcpy(output + used, chunk->data, chunk->length);
                used += chunk->length;
                output[used] = 0;
            }
            free(chunk);
        }
        expect(strstr(output, "roundtrip") != NULL, "typed input is echoed back through the pseudoconsole");
        terminal_process_resize(process, 100, 30); /* exercised for crashes only; not independently observable here */
        terminal_process_free(process);
    }

    DestroyWindow(window);
}

int main(void) {
    test_process();
    if (s_ok) puts("All terminal process tests passed.");
    return s_ok ? 0 : 1;
}
