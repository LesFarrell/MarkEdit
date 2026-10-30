#include <stdio.h>
#include <string.h>
#include "../src/terminal_grid.h"

static int s_ok = 1;

static void expect(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", what);
        s_ok = 0;
    }
}

static void feed(TerminalGrid *grid, const char *text) {
    terminal_grid_feed(grid, text, strlen(text), NULL, NULL);
}

static void expect_line(TerminalGrid *grid, int lineIndex, const char *expected, const char *what) {
    char actual[256];
    terminal_grid_line_text(grid, lineIndex, actual, sizeof(actual));
    if (strcmp(actual, expected) != 0)
        fprintf(stderr, "%s: got \"%s\", expected \"%s\"\n", what, actual, expected);
    expect(strcmp(actual, expected) == 0, what);
}

static void test_plain_text_and_wrapping(void) {
    TerminalGrid *grid = terminal_grid_create(5, 3);
    feed(grid, "hi\r\nbye");
    expect_line(grid, 0, "hi", "a plain line is written at the cursor");
    expect_line(grid, 1, "bye", "CRLF moves to the next line");
    terminal_grid_free(grid);

    grid = terminal_grid_create(3, 3);
    feed(grid, "abcdef");
    expect_line(grid, 0, "abc", "a line wraps at the right edge");
    expect_line(grid, 1, "def", "wrapped text continues on the next line");
    terminal_grid_free(grid);
}

static void test_cursor_addressing(void) {
    TerminalGrid *grid = terminal_grid_create(10, 3);
    feed(grid, "\x1b[2;3Hhi");
    expect_line(grid, 1, "  hi", "CUP moves the cursor before writing");
    feed(grid, "\x1b[1;1H\x1b[Kcleared");
    expect_line(grid, 0, "cleared", "EL erases from the cursor to the end of the line");
    terminal_grid_free(grid);
}

static void test_scroll_and_scrollback(void) {
    TerminalGrid *grid = terminal_grid_create(5, 2);
    feed(grid, "one\r\ntwo\r\nthree");
    expect(terminal_grid_scrollback_count(grid) == 1, "a line pushed off the top becomes scrollback");
    expect_line(grid, 0, "one", "the pushed-off line is kept, oldest first");
    expect_line(grid, 1, "two", "the screen's first row after a scroll");
    expect_line(grid, 2, "three", "the screen's second row after a scroll");
    terminal_grid_clear(grid);
    expect(terminal_grid_scrollback_count(grid) == 0,
           "clearing the terminal drops its scrollback");
    expect_line(grid, 0, "", "clearing the terminal blanks its screen");
    terminal_grid_free(grid);
}

static void test_scrollback_limit_keeps_order(void) {
    TerminalGrid *grid = terminal_grid_create(16, 2);
    char line[32];
    for (int i = 0; i < 2005; i++) {
        snprintf(line, sizeof(line), "L%04d\r\n", i);
        feed(grid, line);
    }
    expect(terminal_grid_scrollback_count(grid) == 2000,
           "scrollback stays at its configured limit");
    expect_line(grid, 0, "L0004", "the ring drops only the oldest scrollback line");
    expect_line(grid, 1999, "L2003", "the newest scrollback line remains last");
    expect_line(grid, 2000, "L2004", "screen rows follow scrollback in logical order");
    terminal_grid_free(grid);
}

static void test_sgr_colours(void) {
    TerminalGrid *grid = terminal_grid_create(10, 2);
    feed(grid, "\x1b[31mred\x1b[0m plain");
    int width = 0;
    const TerminalCell *line = terminal_grid_line_at(grid, terminal_grid_total_lines(grid) - 2, &width);
    expect(line[0].fg == 1 && line[2].fg == 1, "SGR 31 colours the following cells ANSI red (index 1)");
    expect(line[4].fg == TERM_COLOR_DEFAULT, "SGR 0 resets back to the default colour");
    terminal_grid_free(grid);
}

static void test_alternate_screen(void) {
    TerminalGrid *grid = terminal_grid_create(10, 2);
    feed(grid, "primary");
    feed(grid, "\x1b[?1049h");
    feed(grid, "alt");
    expect_line(grid, terminal_grid_total_lines(grid) - 2, "alt", "the alternate screen starts blank and takes new output");
    feed(grid, "\x1b[?1049l");
    expect_line(grid, terminal_grid_total_lines(grid) - 2, "primary", "leaving the alternate screen restores the primary one");
    terminal_grid_free(grid);
}

typedef struct { char *buf; size_t *len; } ReplyCapture;

static void capture_reply(void *context, const char *bytes, size_t length) {
    ReplyCapture *capture = (ReplyCapture *)context;
    memcpy(capture->buf, bytes, length);
    capture->buf[length] = 0;
    *capture->len = length;
}

static void test_device_attributes_reply(void) {
    TerminalGrid *grid = terminal_grid_create(80, 24);
    terminal_grid_feed(grid, "\x1b[c", 3, NULL, NULL); /* no reply sink: must not crash */
    char reply[64];
    size_t replyLength = 0;
    ReplyCapture capture = { reply, &replyLength };
    terminal_grid_feed(grid, "\x1b[c", 3, capture_reply, &capture);
    expect(replyLength > 0 && reply[0] == 0x1b, "a device-attributes query gets a reply");
    terminal_grid_free(grid);
}

static void test_cursor_position_report(void) {
    TerminalGrid *grid = terminal_grid_create(80, 24);
    feed(grid, "\x1b[5;10H");
    char reply[64];
    size_t replyLength = 0;
    ReplyCapture capture = { reply, &replyLength };
    terminal_grid_feed(grid, "\x1b[6n", 4, capture_reply, &capture);
    expect(replyLength > 0 && strcmp(reply, "\x1b[5;10R") == 0, "CPR reports the current cursor position");
    terminal_grid_free(grid);
}

int main(void) {
    test_plain_text_and_wrapping();
    test_cursor_addressing();
    test_scroll_and_scrollback();
    test_scrollback_limit_keeps_order();
    test_sgr_colours();
    test_alternate_screen();
    test_device_attributes_reply();
    test_cursor_position_report();
    if (s_ok) puts("All terminal grid tests passed.");
    return s_ok ? 0 : 1;
}
