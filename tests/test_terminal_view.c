#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "../src/terminal_view.h"
#include "../src/terminal_process.h"

static int s_ok = 1;

static void expect(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", what);
        s_ok = 0;
    }
}

/* ---- driving the control without touching real keyboard/input state -----
   terminal_view_pretranslate_message() dispatches the MSG it is given to
   this window directly (DispatchMessage, or TranslateMessage+DispatchMessage
   for WM_(SYS)KEYDOWN); neither depends on which window has focus or on
   SendInput/keybd_event, so posting crafted messages to our own hidden
   window cannot leak into the real desktop the way simulating hardware
   input would. Ctrl/Shift-gated actions (Ctrl+C, paste) are not exercised
   here for that reason: they read real modifier key state (GetKeyState),
   which these tests deliberately avoid touching. */

typedef struct KeyEvent { UINT message; WPARAM wParam; } KeyEvent;
#define K_CHAR(c) { WM_CHAR, (WPARAM)(c) }
#define K_KEY(vk) { WM_KEYDOWN, (WPARAM)(vk) }

typedef struct TypeJob {
    HWND hwnd;
    const KeyEvent *events;
    int count;
} TypeJob;

static DWORD WINAPI type_thread(LPVOID param) {
    TypeJob *job = (TypeJob *)param;
    for (int i = 0; i < job->count; i++) {
        PostMessageW(job->hwnd, job->events[i].message, job->events[i].wParam, 1);
        /* A real keystroke's WM_KEYDOWN is fully handled - including the
           TranslateMessage-queued WM_(SYS)CHAR it produces, if any, being
           drained inline by terminal_view_pretranslate_message() - before a
           human could possibly press the next key. Posting this thread's
           events back to back with no gap can occasionally beat that handling
           and land a later event in the queue while a special key is still
           being processed; a short pause keeps this test's synthetic input
           ordered the way real keyboard input always is. */
        Sleep(5);
    }
    return 0;
}

/* Runs terminal_view_read_line() while a worker thread posts events into
   this thread's message queue - the same queue terminal_view_read_line()'s
   own GetMessageW loop pumps, so the events arrive exactly as a real
   keyboard-driven call to it would see them. Events go to the control's own
   child window (viewHwnd), not the owner passed to read_line. */
static char *type_line(TerminalView *view, HWND owner, HWND viewHwnd, const KeyEvent *events,
                       int count, int *interrupted) {
    TypeJob job = { viewHwnd, events, count };
    HANDLE thread = CreateThread(NULL, 0, type_thread, &job, 0, NULL);
    char *result = terminal_view_read_line(view, owner, NULL, interrupted);
    if (thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
    return result;
}

static void test_first_input(TerminalView *view, HWND owner, HWND viewHwnd) {
    KeyEvent events[] = { K_CHAR('h'), K_CHAR('i'), K_KEY(VK_RETURN) };
    char *result = type_line(view, owner, viewHwnd, events, sizeof(events) / sizeof(events[0]), NULL);
    expect(result && strcmp(result, "hi") == 0, "a plain typed line is read back");
    free(result);
}

static void test_utf8_editing(TerminalView *view, HWND owner, HWND viewHwnd) {
    /* U+00E9 (e acute) encodes as two UTF-8 bytes; Backspace must remove the
       whole codepoint, not leave a dangling continuation byte behind. */
    KeyEvent events[] = {
        K_CHAR('a'), K_CHAR('b'), K_CHAR((WCHAR)0x00E9), K_KEY(VK_BACK), K_KEY(VK_RETURN)
    };
    char *result = type_line(view, owner, viewHwnd, events, sizeof(events) / sizeof(events[0]), NULL);
    expect(result && strcmp(result, "ab") == 0,
           "backspace over a multi-byte character removes it whole");
    free(result);

    /* The character stays, deleted from the middle: Home, Right, then the
       accented character inserted between 'a' and 'b'. */
    KeyEvent events2[] = {
        K_CHAR('a'), K_CHAR('b'), K_KEY(VK_HOME), K_KEY(VK_RIGHT),
        K_CHAR((WCHAR)0x00E9), K_KEY(VK_RETURN)
    };
    result = type_line(view, owner, viewHwnd, events2, sizeof(events2) / sizeof(events2[0]), NULL);
    WCHAR wide[] = { 'a', 0x00E9, 'b', 0 };
    char expected[8];
    int length = WideCharToMultiByte(CP_UTF8, 0, wide, 3, expected, sizeof(expected), NULL, NULL);
    expected[length] = 0;
    expect(result && strcmp(result, expected) == 0,
           "a multi-byte character inserted mid-line lands at the cursor");
    free(result);
}

static void test_long_wrapped_input(TerminalView *view, HWND owner, HWND viewHwnd) {
    /* Longer than the default 80-column width, so the single-row input
       viewport (redraw_session_input's horizontal window) must scroll
       instead of corrupting the buffer or wrapping onto another row. */
    enum { LENGTH = 200 };
    KeyEvent events[LENGTH + 1];
    for (int i = 0; i < LENGTH; i++) events[i] = (KeyEvent)K_CHAR('x');
    events[LENGTH] = (KeyEvent)K_KEY(VK_RETURN);
    char *result = type_line(view, owner, viewHwnd, events, LENGTH + 1, NULL);
    int allX = result && strlen(result) == LENGTH;
    if (allX) for (int i = 0; i < LENGTH; i++) if (result[i] != 'x') allX = 0;
    expect(allX, "a line longer than the terminal width is captured in full");
    free(result);
}

static void test_history(TerminalView *view, HWND owner, HWND viewHwnd) {
    KeyEvent first[] = { K_CHAR('f'), K_CHAR('i'), K_CHAR('r'), K_CHAR('s'), K_CHAR('t'),
                        K_KEY(VK_RETURN) };
    char *result = type_line(view, owner, viewHwnd, first, sizeof(first) / sizeof(first[0]), NULL);
    expect(result && strcmp(result, "first") == 0, "history: the first line is read back");
    free(result);

    /* Up recalls "first"; Down returns to the (empty) draft, into which
       "second" is then typed - confirming Up/Down navigate instead of
       permanently replacing what's being typed. */
    KeyEvent second[] = { K_KEY(VK_UP), K_KEY(VK_DOWN),
                         K_CHAR('s'), K_CHAR('e'), K_CHAR('c'), K_CHAR('o'), K_CHAR('n'), K_CHAR('d'),
                         K_KEY(VK_RETURN) };
    result = type_line(view, owner, viewHwnd, second, sizeof(second) / sizeof(second[0]), NULL);
    expect(result && strcmp(result, "second") == 0,
           "history: Up recalls and Down restores the in-progress draft");
    free(result);
}

static void test_cancellation(TerminalView *view, HWND owner, HWND viewHwnd) {
    /* Escape cancels without the "interrupted" flag - that's reserved for
       Ctrl+C, which the Lua runner needs to distinguish from an ordinary
       cancelled read (not exercised here: it reads real modifier key state,
       which these tests deliberately avoid touching). */
    KeyEvent events[] = { K_CHAR('n'), K_CHAR('o'), K_KEY(VK_ESCAPE) };
    int interrupted = -1;
    char *result = type_line(view, owner, viewHwnd, events, sizeof(events) / sizeof(events[0]),
                             &interrupted);
    expect(result == NULL, "Escape cancels the read instead of returning the partial line");
    expect(interrupted == 0, "Escape is not reported as the Ctrl+C interrupt");
    free(result);
}

static void test_session_rendering(TerminalView *view) {
    expect(terminal_view_begin_session(view), "a session can begin without a live shell");
    terminal_view_write(view, "line one\r\nline two\r\n", strlen("line one\r\nline two\r\n"));
    int lines = terminal_view_total_lines(view);
    expect(lines >= 2, "written lines appear on the rendered screen");
    char text[256];
    int found = 0;
    for (int i = 0; i < lines; i++) {
        terminal_view_line_text(view, i, text, sizeof(text));
        if (strcmp(text, "line one") == 0) found = 1;
    }
    expect(found, "written text is rendered on the expected row");
    terminal_view_end_session(view);
}

/* ---- a real shell underneath an in-process session ----------------------
   Only process I/O, never simulated keyboard/modifier state, so these are as
   safe as test_terminal_process.c's own use of a real ConPTY shell. */

static int grid_contains(TerminalView *view, const char *needle) {
    int lines = terminal_view_total_lines(view);
    char text[256];
    for (int i = 0; i < lines; i++) {
        terminal_view_line_text(view, i, text, sizeof(text));
        if (strstr(text, needle)) return 1;
    }
    return 0;
}

static void pump_messages(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

static void pump_for(DWORD milliseconds) {
    DWORD started = GetTickCount();
    do { pump_messages(); Sleep(10); } while (GetTickCount() - started < milliseconds);
}

static void pump_until_contains(TerminalView *view, DWORD timeoutMs, const char *needle) {
    DWORD started = GetTickCount();
    do {
        pump_messages();
        if (grid_contains(view, needle)) return;
        Sleep(10);
    } while (GetTickCount() - started < timeoutMs);
}

static void test_shell_preservation(HWND owner) {
    if (!terminal_process_available()) {
        fprintf(stderr, "SKIP: shell preservation (ConPTY unavailable on this Windows)\n");
        return;
    }
    TerminalView *view = terminal_view_create(owner, 2);
    expect(view != NULL, "a second control is created for the live shell");
    if (!view) return;
    /* terminal_view_start() sizes the grid from the control's actual client
       rect; left at its as-created 0x0, every row would collapse to a single
       column and wrap each character onto its own line. */
    MoveWindow(terminal_view_window(view), 0, 0, 800, 400, TRUE);

    WCHAR command[] = L"cmd.exe /q /c \"echo marker1 & ping -n 2 127.0.0.1 >nul & echo marker2\"";
    expect(terminal_view_start(view, command, NULL), "a real shell starts");

    /* Catch it right after marker1, well before marker2 (~1s later, the
       ping's delay) - a session needs to begin while there's more to come. */
    pump_until_contains(view, 5000, "marker1");
    expect(grid_contains(view, "marker1"), "the live shell's first output arrives");

    expect(terminal_view_begin_session(view), "a Lua session can begin while a shell is running");
    expect(!grid_contains(view, "marker1"),
           "the session's own screen starts blank instead of showing the shell's history");

    terminal_view_write(view, "lua output\r\n", strlen("lua output\r\n"));
    pump_for(2500); /* long enough for marker2 to arrive on the hidden shell grid */
    expect(!grid_contains(view, "marker2"),
           "shell output that arrives while a session is active doesn't leak into it");
    expect(grid_contains(view, "lua output"), "the session's own output is still visible");

    terminal_view_end_session(view);

    /* Any plain key returns from a finished transcript to the live shell
       (Ctrl+C/Ctrl+V are not used here - see the file header comment). */
    PostMessageW(terminal_view_window(view), WM_KEYDOWN, (WPARAM)'A', 1);
    pump_for(300);
    expect(grid_contains(view, "marker1") && grid_contains(view, "marker2"),
           "the live shell's output survived the session untouched, including what arrived during it");

    terminal_view_free(view);
}

int main(void) {
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    expect(hwnd != NULL, "a hidden host window is created");

    TerminalView *view = terminal_view_create(hwnd, 1);
    expect(view != NULL, "the control is created");
    if (view) {
        HWND viewHwnd = terminal_view_window(view);
        expect(terminal_view_begin_session(view), "an in-process session begins");
        test_first_input(view, hwnd, viewHwnd);
        test_utf8_editing(view, hwnd, viewHwnd);
        test_long_wrapped_input(view, hwnd, viewHwnd);
        test_history(view, hwnd, viewHwnd);
        test_cancellation(view, hwnd, viewHwnd);
        terminal_view_end_session(view);

        test_session_rendering(view);

        terminal_view_free(view);
    }

    test_shell_preservation(hwnd);

    if (hwnd) DestroyWindow(hwnd);

    if (s_ok) puts("All terminal view tests passed.");
    return s_ok ? 0 : 1;
}
