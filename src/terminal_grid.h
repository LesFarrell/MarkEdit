#ifndef MARKEDIT_TERMINAL_GRID_H
#define MARKEDIT_TERMINAL_GRID_H

#include <stddef.h>
#include <windows.h>

/* A VT100/ECMA-48 character-cell terminal: a cursor-addressed screen buffer,
   SGR colours, scroll regions and the alternate screen buffer (used by
   full-screen programs such as vim and less), fed the raw UTF-8 byte stream
   a ConPTY writes (src/terminal_process.h) and read by the rendering control
   (src/terminal_view.h). ConPTY's output is documented to always be UTF-8
   VT/xterm sequences regardless of the child's own code page, so unlike
   output_text.c's decoder there is no OEM-code-page fallback here; bytes
   that aren't valid UTF-8 become the replacement character.

   Colours are stored as small logical values, not resolved RGB, so a theme
   change recolours everything already on screen or in scrollback the next
   time it's painted; see terminal_grid_resolve_colour(). */

/* The default foreground/background, an ANSI index (0..15), an xterm
   256-colour index (16..255, fixed regardless of theme), or a 24-bit colour
   from SGR 38/48;2, encoded with TERM_COLOR_TRUECOLOR_BIT set. */
#define TERM_COLOR_DEFAULT (-1)
#define TERM_COLOR_TRUECOLOR_BIT 0x1000000

typedef struct {
    WCHAR ch;
    int fg, bg;
    unsigned char underline;
} TerminalCell;

/* The 16 ANSI colours for the current theme (see output_panel.inc's
   OUTPUT_ANSI_LIGHT/DARK, which a caller reuses here) plus the default pair
   shown where nothing has set a colour. */
typedef struct {
    COLORREF ansi[16];
    COLORREF defaultFg, defaultBg;
} TerminalPalette;

/* Resolves a TerminalCell's fg or bg (background nonzero) against palette. */
COLORREF terminal_grid_resolve_colour(int colour, int background, const TerminalPalette *palette);

typedef struct TerminalGrid TerminalGrid;

/* Bytes to write back to the pseudoconsole in response to a query it sent
   (a cursor-position report, a device-attributes handshake); ConPTY expects
   at least the latter answered. context is terminal_grid_feed's own. */
typedef void (*TerminalGridReply)(void *context, const char *bytes, size_t length);

TerminalGrid *terminal_grid_create(int cols, int rows);
void terminal_grid_free(TerminalGrid *grid);

/* Clips or pads the screen in place; scrollback and its rows are untouched
   (no reflow - see the Terminal tab's stated non-goals). */
void terminal_grid_resize(TerminalGrid *grid, int cols, int rows);

/* Parses bytes as UTF-8 VT/xterm output, updating the screen, cursor and
   scrollback. May call reply (possibly more than once) with bytes to send
   back to the program, e.g. a cursor-position report. */
void terminal_grid_feed(TerminalGrid *grid, const char *bytes, size_t length,
                        TerminalGridReply reply, void *context);

/* Back to one blank screen, default cursor and attributes; scrollback is
   kept. For restarting the shell in the same control. */
void terminal_grid_reset(TerminalGrid *grid);
/* Drops scrollback as well as resetting to a blank screen. */
void terminal_grid_clear(TerminalGrid *grid);

/* 0-based column/row and whether it should be drawn; row is in screen
   coordinates (add terminal_grid_scrollback_count() for a line index). */
void terminal_grid_cursor(const TerminalGrid *grid, int *col, int *row, int *visible);
/* Whether arrow keys should be sent as "ESC O letter" (DECCKM, CSI ?1h) or
   the default "ESC [ letter" - read by terminal_view.c's key handling. */
int terminal_grid_app_cursor_keys(const TerminalGrid *grid);

/* How many scrollback lines precede the screen; never grows while the
   alternate screen (vim, less, ...) is active. */
int terminal_grid_scrollback_count(const TerminalGrid *grid);
/* Screen rows plus scrollback lines - the full range terminal_grid_line_at() accepts. */
int terminal_grid_total_lines(const TerminalGrid *grid);
/* The line at lineIndex (0 is the oldest scrollback line; scrollback_count()
   and up are the screen rows), and how many cells it holds - a scrollback
   line keeps the width it was pushed off the screen with, which may differ
   from the screen's current width. NULL when lineIndex is out of range. */
const TerminalCell *terminal_grid_line_at(const TerminalGrid *grid, int lineIndex, int *width);

/* Whether anything has changed since the last call, which also clears the
   flag. One flag for the whole grid rather than a per-row diff: a terminal
   panel is small enough that the view can just repaint what's visible. */
int terminal_grid_take_dirty(TerminalGrid *grid);

/* lineIndex's text, UTF-8, trimmed of trailing spaces, NUL-terminated within
   capacity. For tests and for a future "copy line"; rendering uses
   terminal_grid_line_at() directly so it keeps per-cell colour. */
void terminal_grid_line_text(const TerminalGrid *grid, int lineIndex, char *utf8, size_t capacity);

#endif
