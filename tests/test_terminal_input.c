#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "../src/terminal_input.h"

static int s_ok = 1;

static void expect_sequence(unsigned int key, unsigned int modifiers, int application,
                            const char *expected, const char *what) {
    char actual[24];
    size_t length = terminal_input_sequence(key, modifiers, application,
                                            actual, sizeof(actual));
    size_t expectedLength = strlen(expected);
    if (length != expectedLength || memcmp(actual, expected, expectedLength) != 0) {
        fprintf(stderr, "FAILED: %s\n", what);
        s_ok = 0;
    }
}

static void test_cursor_keys(void) {
    expect_sequence(VK_UP, 0, 0, "\x1b[A", "Up uses the normal cursor sequence");
    expect_sequence(VK_LEFT, 0, 1, "\x1bOD", "application cursor mode changes arrows");
    expect_sequence(VK_RIGHT, TERMINAL_MOD_CONTROL, 1, "\x1b[1;5C",
                    "Ctrl+Right includes its xterm modifier");
    expect_sequence(VK_LEFT, TERMINAL_MOD_ALT, 0, "\x1b[1;3D",
                    "Alt+Left reaches the terminal instead of editor history");
    expect_sequence(VK_UP, TERMINAL_MOD_SHIFT | TERMINAL_MOD_ALT |
                           TERMINAL_MOD_CONTROL, 0, "\x1b[1;8A",
                    "combined cursor modifiers are preserved");
    expect_sequence(VK_HOME, TERMINAL_MOD_SHIFT, 0, "\x1b[1;2H",
                    "modified Home uses CSI 1 modifier form");
}

static void test_editing_and_function_keys(void) {
    expect_sequence(VK_INSERT, 0, 0, "\x1b[2~", "Insert");
    expect_sequence(VK_DELETE, TERMINAL_MOD_CONTROL, 0, "\x1b[3;5~", "Ctrl+Delete");
    expect_sequence(VK_PRIOR, TERMINAL_MOD_ALT, 0, "\x1b[5;3~", "Alt+PageUp");
    expect_sequence(VK_F1, 0, 0, "\x1bOP", "F1 uses SS3");
    expect_sequence(VK_F4, TERMINAL_MOD_SHIFT, 0, "\x1b[1;2S", "Shift+F4");
    expect_sequence(VK_F5, 0, 0, "\x1b[15~", "F5");
    expect_sequence(VK_F12, TERMINAL_MOD_CONTROL, 0, "\x1b[24;5~", "Ctrl+F12");
}

static void test_control_keys(void) {
    expect_sequence(VK_BACK, 0, 0, "\x7f", "Backspace sends DEL");
    expect_sequence(VK_BACK, TERMINAL_MOD_CONTROL, 0, "\x17",
                    "Ctrl+Backspace deletes the previous word");
    expect_sequence(VK_BACK, TERMINAL_MOD_ALT, 0, "\x1b\x7f", "Alt+Backspace");
    expect_sequence(VK_TAB, TERMINAL_MOD_SHIFT, 0, "\x1b[Z", "Shift+Tab sends backtab");
    expect_sequence(VK_TAB, TERMINAL_MOD_SHIFT | TERMINAL_MOD_CONTROL, 0,
                    "\x1b[1;6Z", "Ctrl+Shift+Tab preserves both modifiers");
    expect_sequence(VK_CANCEL, TERMINAL_MOD_CONTROL, 0, "\x03", "Ctrl+Break interrupts");
}

static void test_text_is_not_stolen(void) {
    char output[24];
    size_t length = terminal_input_sequence('F', TERMINAL_MOD_ALT, 0,
                                            output, sizeof(output));
    if (length != 0) {
        fprintf(stderr, "FAILED: Alt+printable must be translated from WM_SYSCHAR\n");
        s_ok = 0;
    }
    length = terminal_input_sequence('F', TERMINAL_MOD_CONTROL, 0,
                                     output, sizeof(output));
    if (length != 0) {
        fprintf(stderr, "FAILED: Ctrl+letter must be translated from WM_CHAR\n");
        s_ok = 0;
    }
}

int main(void) {
    test_cursor_keys();
    test_editing_and_function_keys();
    test_control_keys();
    test_text_is_not_stolen();
    if (!s_ok) return 1;
    puts("terminal input tests passed");
    return 0;
}
