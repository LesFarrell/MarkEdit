#include "terminal_grid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Scrollback rows beyond this are dropped, oldest first - the same role as
   output_panel.inc's OUTPUT_MAX_LENGTH for the Run tab's log. */
#define TERMINAL_SCROLLBACK_MAX_ROWS 2000
#define TERM_MAX_CSI_PARAMS 16

typedef struct {
    TerminalCell *cells;
    int width;
} TerminalScrollRow;

typedef enum {
    TG_GROUND, TG_ESCAPE, TG_ESCAPE_CHARSET, TG_CSI, TG_OSC, TG_OSC_ESC
} TerminalGridState;

struct TerminalGrid {
    int cols, rows;
    TerminalCell *screen;    /* cols*rows, the primary screen */
    TerminalCell *altScreen; /* cols*rows, shown by vim/less/etc (DECSET 1049) */
    int usingAlt;

    int cursorCol, cursorRow; /* 0-based */
    int cursorVisible;
    int savedCol, savedRow; /* DECSC/DECRC */
    int savedFg, savedBg;
    unsigned char savedUnderline;
    int altSavedCol, altSavedRow; /* the primary screen's cursor, while the alternate screen is active */

    int scrollTop, scrollBottom; /* inclusive, 0-based, DECSTBM */
    int autowrapMode;            /* DECAWM, default on */
    int autowrapPending;         /* a wrap deferred until the next printable char */
    int appCursorKeys;           /* DECCKM; read by terminal_view.c's key handling */

    int curFg, curBg; /* pending SGR colours for the next cell written */
    unsigned char curUnderline;
    unsigned char reverseVideo; /* SGR 7; applied to cells as they're written */

    TerminalScrollRow *scrollback;
    int scrollbackCount, scrollbackCapacity;

    int dirty;

    /* ---- UTF-8 decoding ---- */
    unsigned char pending[4];
    int pendingLength, pendingNeeded;

    /* ---- escape/CSI/OSC parsing ---- */
    TerminalGridState state;
    int params[TERM_MAX_CSI_PARAMS];
    int paramCount;
    int paramPrivate; /* a '?' prefix, e.g. CSI ?25h */
};

static int effective_fg(const TerminalGrid *g) { return g->reverseVideo ? g->curBg : g->curFg; }
static int effective_bg(const TerminalGrid *g) { return g->reverseVideo ? g->curFg : g->curBg; }
static TerminalCell *active_buffer(TerminalGrid *g) { return g->usingAlt ? g->altScreen : g->screen; }
static const TerminalCell *active_buffer_c(const TerminalGrid *g) { return g->usingAlt ? g->altScreen : g->screen; }

COLORREF terminal_grid_resolve_colour(int colour, int background, const TerminalPalette *palette) {
    if (colour == TERM_COLOR_DEFAULT) return background ? palette->defaultBg : palette->defaultFg;
    if (colour & TERM_COLOR_TRUECOLOR_BIT) return (COLORREF)(colour & 0xFFFFFF);
    if (colour >= 0 && colour < 16) return palette->ansi[colour];
    if (colour >= 16 && colour <= 231) {
        static const int levels[6] = {0, 95, 135, 175, 215, 255};
        int n = colour - 16;
        int r = levels[(n / 36) % 6], g = levels[(n / 6) % 6], b = levels[n % 6];
        return RGB(r, g, b);
    }
    if (colour >= 232 && colour <= 255) {
        int v = 8 + (colour - 232) * 10;
        return RGB(v, v, v);
    }
    return background ? palette->defaultBg : palette->defaultFg;
}

/* ---- scrollback ---- */

static void push_scrollback(TerminalGrid *g, const TerminalCell *row, int width) {
    TerminalCell *copy = (TerminalCell *)malloc((size_t)width * sizeof(TerminalCell));
    if (!copy) return;
    memcpy(copy, row, (size_t)width * sizeof(TerminalCell));
    if (g->scrollbackCount == g->scrollbackCapacity) {
        if (g->scrollbackCapacity >= TERMINAL_SCROLLBACK_MAX_ROWS) {
            free(g->scrollback[0].cells);
            memmove(g->scrollback, g->scrollback + 1,
                   (size_t)(g->scrollbackCount - 1) * sizeof(TerminalScrollRow));
            g->scrollbackCount--;
        } else {
            int capacity = g->scrollbackCapacity ? g->scrollbackCapacity * 2 : 256;
            if (capacity > TERMINAL_SCROLLBACK_MAX_ROWS) capacity = TERMINAL_SCROLLBACK_MAX_ROWS;
            TerminalScrollRow *grown = (TerminalScrollRow *)realloc(
                g->scrollback, (size_t)capacity * sizeof(TerminalScrollRow));
            if (!grown) { free(copy); return; }
            g->scrollback = grown;
            g->scrollbackCapacity = capacity;
        }
    }
    g->scrollback[g->scrollbackCount].cells = copy;
    g->scrollback[g->scrollbackCount].width = width;
    g->scrollbackCount++;
}

/* ---- scrolling the active buffer's region ----
   toScrollback is true only for a natural top-of-screen overflow (a line
   feed past the bottom margin, or an explicit "scroll up" with the region
   covering the whole screen): xterm doesn't add scrollback for the IL/DL/ECH
   family of edit operations even when they happen to touch row 0. */

static void scroll_up(TerminalGrid *g, int lines, int toScrollback) {
    int top = g->scrollTop < 0 ? 0 : g->scrollTop;
    int bottom = g->scrollBottom >= g->rows ? g->rows - 1 : g->scrollBottom;
    if (top > bottom || lines <= 0) return;
    int height = bottom - top + 1;
    if (lines > height) lines = height;
    TerminalCell *buf = active_buffer(g);
    if (toScrollback && top == 0 && !g->usingAlt)
        for (int i = 0; i < lines; i++) push_scrollback(g, buf + (size_t)i * g->cols, g->cols);
    if (lines < height)
        memmove(buf + (size_t)top * g->cols, buf + (size_t)(top + lines) * g->cols,
               (size_t)(height - lines) * g->cols * sizeof(TerminalCell));
    TerminalCell blank = { L' ', effective_fg(g), effective_bg(g), 0 };
    for (int r = bottom - lines + 1; r <= bottom; r++)
        for (int c = 0; c < g->cols; c++) buf[(size_t)r * g->cols + c] = blank;
    g->dirty = 1;
}

static void scroll_down(TerminalGrid *g, int lines) {
    int top = g->scrollTop < 0 ? 0 : g->scrollTop;
    int bottom = g->scrollBottom >= g->rows ? g->rows - 1 : g->scrollBottom;
    if (top > bottom || lines <= 0) return;
    int height = bottom - top + 1;
    if (lines > height) lines = height;
    TerminalCell *buf = active_buffer(g);
    if (lines < height)
        memmove(buf + (size_t)(top + lines) * g->cols, buf + (size_t)top * g->cols,
               (size_t)(height - lines) * g->cols * sizeof(TerminalCell));
    TerminalCell blank = { L' ', effective_fg(g), effective_bg(g), 0 };
    for (int r = top; r < top + lines; r++)
        for (int c = 0; c < g->cols; c++) buf[(size_t)r * g->cols + c] = blank;
    g->dirty = 1;
}

/* ---- cursor movement ---- */

static void clamp_cursor(TerminalGrid *g) {
    if (g->cursorCol < 0) g->cursorCol = 0;
    if (g->cursorCol >= g->cols) g->cursorCol = g->cols - 1;
    if (g->cursorRow < 0) g->cursorRow = 0;
    if (g->cursorRow >= g->rows) g->cursorRow = g->rows - 1;
    g->autowrapPending = 0;
}

static void line_feed(TerminalGrid *g) {
    g->autowrapPending = 0;
    if (g->cursorRow == g->scrollBottom) scroll_up(g, 1, 1);
    else if (g->cursorRow < g->rows - 1) g->cursorRow++;
}

static void reverse_line_feed(TerminalGrid *g) {
    g->autowrapPending = 0;
    if (g->cursorRow == g->scrollTop) scroll_down(g, 1);
    else if (g->cursorRow > 0) g->cursorRow--;
}

static void put_char(TerminalGrid *g, WCHAR ch) {
    if (g->autowrapPending) {
        g->cursorCol = 0;
        line_feed(g);
    }
    TerminalCell *cell = &active_buffer(g)[(size_t)g->cursorRow * g->cols + g->cursorCol];
    cell->ch = ch;
    cell->fg = effective_fg(g);
    cell->bg = effective_bg(g);
    cell->underline = g->curUnderline;
    g->dirty = 1;
    if (g->cursorCol + 1 < g->cols) g->cursorCol++;
    else if (g->autowrapMode) g->autowrapPending = 1;
}

/* ---- erasing and editing ---- */

static void erase_cells(TerminalGrid *g, int row, int fromCol, int toCol) {
    TerminalCell blank = { L' ', effective_fg(g), effective_bg(g), 0 };
    TerminalCell *buf = active_buffer(g);
    for (int c = fromCol; c <= toCol && c < g->cols; c++) buf[(size_t)row * g->cols + c] = blank;
    g->dirty = 1;
}

static void erase_in_display(TerminalGrid *g, int mode) {
    if (mode == 1) {
        for (int r = 0; r < g->cursorRow; r++) erase_cells(g, r, 0, g->cols - 1);
        erase_cells(g, g->cursorRow, 0, g->cursorCol);
    } else if (mode == 0) {
        erase_cells(g, g->cursorRow, g->cursorCol, g->cols - 1);
        for (int r = g->cursorRow + 1; r < g->rows; r++) erase_cells(g, r, 0, g->cols - 1);
    } else { /* 2: whole screen; 3: whole screen (scrollback is kept, unlike xterm) */
        for (int r = 0; r < g->rows; r++) erase_cells(g, r, 0, g->cols - 1);
    }
}

static void erase_in_line(TerminalGrid *g, int mode) {
    if (mode == 1) erase_cells(g, g->cursorRow, 0, g->cursorCol);
    else if (mode == 2) erase_cells(g, g->cursorRow, 0, g->cols - 1);
    else erase_cells(g, g->cursorRow, g->cursorCol, g->cols - 1);
}

static void insert_lines(TerminalGrid *g, int n) {
    if (g->cursorRow < g->scrollTop || g->cursorRow > g->scrollBottom) return;
    int savedTop = g->scrollTop;
    g->scrollTop = g->cursorRow;
    scroll_down(g, n);
    g->scrollTop = savedTop;
}

static void delete_lines(TerminalGrid *g, int n) {
    if (g->cursorRow < g->scrollTop || g->cursorRow > g->scrollBottom) return;
    int savedTop = g->scrollTop;
    g->scrollTop = g->cursorRow;
    scroll_up(g, n, 0);
    g->scrollTop = savedTop;
}

static void delete_chars(TerminalGrid *g, int n) {
    TerminalCell *buf = active_buffer(g);
    int row = g->cursorRow, col = g->cursorCol;
    int remaining = g->cols - col;
    if (n > remaining) n = remaining;
    if (n < remaining)
        memmove(buf + (size_t)row * g->cols + col, buf + (size_t)row * g->cols + col + n,
               (size_t)(remaining - n) * sizeof(TerminalCell));
    erase_cells(g, row, g->cols - n, g->cols - 1);
}

static void insert_chars(TerminalGrid *g, int n) {
    TerminalCell *buf = active_buffer(g);
    int row = g->cursorRow, col = g->cursorCol;
    int remaining = g->cols - col;
    if (n > remaining) n = remaining;
    if (n < remaining)
        memmove(buf + (size_t)row * g->cols + col + n, buf + (size_t)row * g->cols + col,
               (size_t)(remaining - n) * sizeof(TerminalCell));
    erase_cells(g, row, col, col + n - 1);
}

static void erase_chars(TerminalGrid *g, int n) {
    int end = g->cursorCol + n - 1;
    if (end >= g->cols) end = g->cols - 1;
    erase_cells(g, g->cursorRow, g->cursorCol, end);
}

/* ---- SGR (colours and attributes) ---- */

static void apply_sgr(TerminalGrid *g) {
    if (g->paramCount == 0) g->params[g->paramCount++] = 0; /* bare "\x1b[m" means reset */
    for (int i = 0; i < g->paramCount; i++) {
        int p = g->params[i];
        if (p == 0) {
            g->curFg = g->curBg = TERM_COLOR_DEFAULT;
            g->curUnderline = 0;
            g->reverseVideo = 0;
        } else if (p == 1) {
            if (g->curFg >= 0 && g->curFg < 8) g->curFg += 8; /* bold brightens, as output_text.c does */
        } else if (p == 4) g->curUnderline = 1;
        else if (p == 7) g->reverseVideo = 1;
        else if (p == 22) { if (g->curFg >= 8 && g->curFg < 16) g->curFg -= 8; }
        else if (p == 24) g->curUnderline = 0;
        else if (p == 27) g->reverseVideo = 0;
        else if (p == 39) g->curFg = TERM_COLOR_DEFAULT;
        else if (p == 49) g->curBg = TERM_COLOR_DEFAULT;
        else if (p >= 30 && p <= 37) g->curFg = p - 30;
        else if (p >= 40 && p <= 47) g->curBg = p - 40;
        else if (p >= 90 && p <= 97) g->curFg = p - 90 + 8;
        else if (p >= 100 && p <= 107) g->curBg = p - 100 + 8;
        else if ((p == 38 || p == 48) && i + 1 < g->paramCount) {
            int *target = p == 38 ? &g->curFg : &g->curBg;
            if (g->params[i + 1] == 5 && i + 2 < g->paramCount) {
                *target = g->params[i + 2] & 0xFF;
                i += 2;
            } else if (g->params[i + 1] == 2 && i + 4 < g->paramCount) {
                int r = g->params[i + 2] & 0xFF, gr = g->params[i + 3] & 0xFF, b = g->params[i + 4] & 0xFF;
                *target = (int)(TERM_COLOR_TRUECOLOR_BIT | RGB(r, gr, b));
                i += 4;
            }
        }
        /* 2 (dim), 3 (italic), 5 (blink), 9 (strike) and their resets are
           read and ignored - cosmetic attributes outside this control's scope. */
    }
}

/* ---- the CSI final byte, once its parameters are collected ---- */

static int param_or(const TerminalGrid *g, int index, int fallback) {
    if (index >= g->paramCount || g->params[index] == 0) return fallback;
    return g->params[index];
}

static void send_reply(TerminalGridReply reply, void *context, const char *text) {
    if (reply) reply(context, text, strlen(text));
}

static void dispatch_csi(TerminalGrid *g, char final, TerminalGridReply reply, void *context) {
    int n = param_or(g, 0, 1);
    switch (final) {
        case 'H': case 'f':
            g->cursorRow = param_or(g, 0, 1) - 1;
            g->cursorCol = param_or(g, 1, 1) - 1;
            clamp_cursor(g);
            break;
        case 'A': g->cursorRow -= n; clamp_cursor(g); break;
        case 'B': g->cursorRow += n; clamp_cursor(g); break;
        case 'C': g->cursorCol += n; clamp_cursor(g); break;
        case 'D': g->cursorCol -= n; clamp_cursor(g); break;
        case 'E': g->cursorRow += n; g->cursorCol = 0; clamp_cursor(g); break;
        case 'F': g->cursorRow -= n; g->cursorCol = 0; clamp_cursor(g); break;
        case 'G': g->cursorCol = n - 1; clamp_cursor(g); break;
        case 'd': g->cursorRow = n - 1; clamp_cursor(g); break;
        case 'J': erase_in_display(g, param_or(g, 0, 0)); break;
        case 'K': erase_in_line(g, param_or(g, 0, 0)); break;
        case 'S': scroll_up(g, n, g->scrollTop == 0 && g->scrollBottom == g->rows - 1); break;
        case 'T': scroll_down(g, n); break;
        case 'L': insert_lines(g, n); break;
        case 'M': delete_lines(g, n); break;
        case 'P': delete_chars(g, n); break;
        case '@': insert_chars(g, n); break;
        case 'X': erase_chars(g, n); break;
        case 'm': apply_sgr(g); break;
        case 'r':
            if (!g->paramPrivate) {
                g->scrollTop = param_or(g, 0, 1) - 1;
                g->scrollBottom = param_or(g, 1, g->rows) - 1;
                if (g->scrollTop < 0) g->scrollTop = 0;
                if (g->scrollBottom >= g->rows) g->scrollBottom = g->rows - 1;
                if (g->scrollTop > g->scrollBottom) { g->scrollTop = 0; g->scrollBottom = g->rows - 1; }
                g->cursorRow = g->scrollTop;
                g->cursorCol = 0;
            }
            break;
        case 's': g->savedCol = g->cursorCol; g->savedRow = g->cursorRow; break;
        case 'u': g->cursorCol = g->savedCol; g->cursorRow = g->savedRow; clamp_cursor(g); break;
        case 'c': if (!g->paramPrivate) send_reply(reply, context, "\x1b[?1;2c"); break;
        case 'n':
            if (param_or(g, 0, 0) == 6) {
                char report[32];
                snprintf(report, sizeof(report), "\x1b[%d;%dR", g->cursorRow + 1, g->cursorCol + 1);
                send_reply(reply, context, report);
            }
            break;
        case 'h': case 'l': {
            int set = final == 'h';
            if (!g->paramPrivate) break;
            for (int i = 0; i < g->paramCount; i++) {
                int mode = g->params[i];
                if (mode == 1) g->appCursorKeys = set;
                else if (mode == 7) g->autowrapMode = set;
                else if (mode == 25) g->cursorVisible = set;
                else if (mode == 1049 || mode == 47 || mode == 1047) {
                    if (set != g->usingAlt) {
                        if (set) {
                            g->altSavedCol = g->cursorCol;
                            g->altSavedRow = g->cursorRow;
                            g->usingAlt = 1;
                            for (int r = 0; r < g->rows; r++) erase_cells(g, r, 0, g->cols - 1);
                            g->cursorCol = g->cursorRow = 0;
                        } else {
                            g->usingAlt = 0;
                            g->cursorCol = g->altSavedCol;
                            g->cursorRow = g->altSavedRow;
                            clamp_cursor(g);
                        }
                    }
                }
                /* Bracketed paste (2004) and mouse reporting (1000/1002/1003)
                   are read and ignored - see the Terminal tab's non-goals. */
            }
            break;
        }
        default: break; /* an unsupported sequence is simply not acted on */
    }
    g->dirty = 1;
}

/* ---- feeding bytes ---- */

static void begin_csi(TerminalGrid *g) {
    g->state = TG_CSI;
    g->paramCount = 0;
    g->paramPrivate = 0;
    g->params[0] = 0;
}

static void csi_byte(TerminalGrid *g, unsigned char byte, TerminalGridReply reply, void *context) {
    if (byte == '?' && g->paramCount == 0) { g->paramPrivate = 1; return; }
    if (byte >= '0' && byte <= '9') {
        if (g->paramCount == 0) g->paramCount = 1;
        int *p = &g->params[g->paramCount - 1];
        *p = *p * 10 + (byte - '0');
        return;
    }
    if (byte == ';') {
        if (g->paramCount < TERM_MAX_CSI_PARAMS) { g->params[g->paramCount] = 0; g->paramCount++; }
        return;
    }
    if (byte >= 0x40 && byte <= 0x7e) {
        if (g->paramCount == 0) g->paramCount = 1;
        dispatch_csi(g, (char)byte, reply, context);
        g->state = TG_GROUND;
        return;
    }
    /* Intermediate bytes (0x20-0x2f, e.g. a cursor-style request) are read
       and otherwise ignored. */
}

static void process_control(TerminalGrid *g, unsigned char byte) {
    switch (byte) {
        case 0x07: break; /* BEL: no bell to ring */
        case 0x08: if (g->cursorCol > 0) g->cursorCol--; g->autowrapPending = 0; break;
        case 0x09: g->cursorCol = (g->cursorCol / 8 + 1) * 8; if (g->cursorCol >= g->cols) g->cursorCol = g->cols - 1; break;
        case 0x0a: case 0x0b: case 0x0c: line_feed(g); break;
        case 0x0d: g->cursorCol = 0; g->autowrapPending = 0; break;
        default: break;
    }
}

static void process_codepoint(TerminalGrid *g, unsigned int codepoint, TerminalGridReply reply, void *context) {
    if (codepoint > 0xFFFF) codepoint = 0xFFFD; /* astral characters: see the header's known limitations */
    if (codepoint == 0x1b) { g->state = TG_ESCAPE; return; }
    if (codepoint < 0x20) { process_control(g, (unsigned char)codepoint); return; }
    if (g->state == TG_GROUND) { put_char(g, (WCHAR)codepoint); return; }
    (void)reply; (void)context;
}

/* Decodes one UTF-8 byte; calls process_codepoint() once a character (or the
   replacement character, for invalid bytes) is complete. ConPTY's output is
   documented as always UTF-8, so unlike output_text.c there's no OEM-code-
   page fallback to consider here. */
static void feed_byte(TerminalGrid *g, unsigned char byte, TerminalGridReply reply, void *context) {
    if (g->state == TG_ESCAPE) {
        g->state = TG_GROUND;
        if (byte == '[') { begin_csi(g); return; }
        if (byte == ']') { g->state = TG_OSC; return; }
        if (byte == '(' || byte == ')') { g->state = TG_ESCAPE_CHARSET; return; }
        if (byte == '7') { g->savedCol = g->cursorCol; g->savedRow = g->cursorRow;
                           g->savedFg = g->curFg; g->savedBg = g->curBg; g->savedUnderline = g->curUnderline; return; }
        if (byte == '8') { g->cursorCol = g->savedCol; g->cursorRow = g->savedRow;
                           g->curFg = g->savedFg; g->curBg = g->savedBg; g->curUnderline = g->savedUnderline;
                           clamp_cursor(g); return; }
        if (byte == 'D') { line_feed(g); return; }
        if (byte == 'M') { reverse_line_feed(g); return; }
        if (byte == 'E') { g->cursorCol = 0; line_feed(g); return; }
        if (byte == 'c') { terminal_grid_reset(g); return; }
        return; /* an unsupported escape is simply not acted on */
    }
    if (g->state == TG_ESCAPE_CHARSET) { g->state = TG_GROUND; return; } /* the byte names a charset; ignored */
    if (g->state == TG_CSI) { csi_byte(g, byte, reply, context); return; }
    if (g->state == TG_OSC) {
        if (byte == 0x07) g->state = TG_GROUND;
        else if (byte == 0x1b) g->state = TG_OSC_ESC;
        return;
    }
    if (g->state == TG_OSC_ESC) { g->state = TG_GROUND; return; } /* ST (ESC \); any other byte also ends it */

    /* TG_GROUND: decode UTF-8, one byte at a time, same shape as
       OutputDecoder's pending[]/pendingLength in output_text.c. */
    if (g->pendingLength == 0) {
        if (byte < 0x80) { process_codepoint(g, byte, reply, context); return; }
        int needed = (byte & 0xe0) == 0xc0 ? 2 : (byte & 0xf0) == 0xe0 ? 3 : (byte & 0xf8) == 0xf0 ? 4 : 0;
        if (needed == 0) { process_codepoint(g, 0xFFFD, reply, context); return; }
        g->pending[0] = byte;
        g->pendingLength = 1;
        g->pendingNeeded = needed;
        return;
    }
    if ((byte & 0xc0) != 0x80) { /* not a continuation byte: the pending sequence was invalid */
        g->pendingLength = 0;
        process_codepoint(g, 0xFFFD, reply, context);
        feed_byte(g, byte, reply, context); /* byte may start a new character of its own */
        return;
    }
    g->pending[g->pendingLength++] = byte;
    if (g->pendingLength < g->pendingNeeded) return;
    unsigned int codepoint;
    if (g->pendingNeeded == 2) codepoint = ((g->pending[0] & 0x1f) << 6) | (g->pending[1] & 0x3f);
    else if (g->pendingNeeded == 3)
        codepoint = ((g->pending[0] & 0x0f) << 12) | ((g->pending[1] & 0x3f) << 6) | (g->pending[2] & 0x3f);
    else
        codepoint = ((g->pending[0] & 0x07) << 18) | ((g->pending[1] & 0x3f) << 12) |
                   ((g->pending[2] & 0x3f) << 6) | (g->pending[3] & 0x3f);
    g->pendingLength = 0;
    process_codepoint(g, codepoint, reply, context);
}

void terminal_grid_feed(TerminalGrid *grid, const char *bytes, size_t length,
                        TerminalGridReply reply, void *context) {
    for (size_t i = 0; i < length; i++) feed_byte(grid, (unsigned char)bytes[i], reply, context);
}

/* ---- lifecycle ---- */

TerminalGrid *terminal_grid_create(int cols, int rows) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    TerminalGrid *g = (TerminalGrid *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->cols = cols;
    g->rows = rows;
    g->screen = (TerminalCell *)calloc((size_t)cols * rows, sizeof(TerminalCell));
    g->altScreen = (TerminalCell *)calloc((size_t)cols * rows, sizeof(TerminalCell));
    if (!g->screen || !g->altScreen) { terminal_grid_free(g); return NULL; }
    terminal_grid_reset(g);
    return g;
}

void terminal_grid_free(TerminalGrid *grid) {
    if (!grid) return;
    for (int i = 0; i < grid->scrollbackCount; i++) free(grid->scrollback[i].cells);
    free(grid->scrollback);
    free(grid->screen);
    free(grid->altScreen);
    free(grid);
}

void terminal_grid_reset(TerminalGrid *grid) {
    if (!grid) return;
    grid->usingAlt = 0;
    grid->cursorCol = grid->cursorRow = 0;
    grid->cursorVisible = 1;
    grid->savedCol = grid->savedRow = 0;
    grid->savedFg = grid->savedBg = TERM_COLOR_DEFAULT;
    grid->savedUnderline = 0;
    grid->altSavedCol = grid->altSavedRow = 0;
    grid->scrollTop = 0;
    grid->scrollBottom = grid->rows - 1;
    grid->autowrapMode = 1;
    grid->autowrapPending = 0;
    grid->appCursorKeys = 0;
    grid->curFg = TERM_COLOR_DEFAULT;
    grid->curBg = TERM_COLOR_DEFAULT;
    grid->curUnderline = 0;
    grid->reverseVideo = 0;
    grid->state = TG_GROUND;
    grid->paramCount = 0;
    grid->paramPrivate = 0;
    grid->pendingLength = 0;
    grid->pendingNeeded = 0;
    TerminalCell blank = { L' ', TERM_COLOR_DEFAULT, TERM_COLOR_DEFAULT, 0 };
    for (int i = 0; i < grid->cols * grid->rows; i++) { grid->screen[i] = blank; grid->altScreen[i] = blank; }
    grid->dirty = 1;
}

void terminal_grid_resize(TerminalGrid *grid, int cols, int rows) {
    if (!grid || cols < 1 || rows < 1) return;
    if (cols == grid->cols && rows == grid->rows) return;
    TerminalCell blank = { L' ', TERM_COLOR_DEFAULT, TERM_COLOR_DEFAULT, 0 };
    TerminalCell *newScreen = (TerminalCell *)malloc((size_t)cols * rows * sizeof(TerminalCell));
    TerminalCell *newAlt = (TerminalCell *)malloc((size_t)cols * rows * sizeof(TerminalCell));
    if (!newScreen || !newAlt) { free(newScreen); free(newAlt); return; }
    for (int i = 0; i < cols * rows; i++) { newScreen[i] = blank; newAlt[i] = blank; }
    int copyRows = rows < grid->rows ? rows : grid->rows;
    int copyCols = cols < grid->cols ? cols : grid->cols;
    for (int r = 0; r < copyRows; r++) {
        memcpy(newScreen + (size_t)r * cols, grid->screen + (size_t)r * grid->cols,
              (size_t)copyCols * sizeof(TerminalCell));
        memcpy(newAlt + (size_t)r * cols, grid->altScreen + (size_t)r * grid->cols,
              (size_t)copyCols * sizeof(TerminalCell));
    }
    free(grid->screen);
    free(grid->altScreen);
    grid->screen = newScreen;
    grid->altScreen = newAlt;
    grid->cols = cols;
    grid->rows = rows;
    grid->scrollTop = 0;
    grid->scrollBottom = rows - 1;
    clamp_cursor(grid);
    grid->dirty = 1;
}

void terminal_grid_cursor(const TerminalGrid *grid, int *col, int *row, int *visible) {
    if (col) *col = grid->cursorCol;
    if (row) *row = grid->cursorRow;
    if (visible) *visible = grid->cursorVisible;
}

int terminal_grid_app_cursor_keys(const TerminalGrid *grid) { return grid->appCursorKeys; }

int terminal_grid_scrollback_count(const TerminalGrid *grid) { return grid->scrollbackCount; }
int terminal_grid_total_lines(const TerminalGrid *grid) { return grid->scrollbackCount + grid->rows; }

const TerminalCell *terminal_grid_line_at(const TerminalGrid *grid, int lineIndex, int *width) {
    if (lineIndex < 0 || lineIndex >= terminal_grid_total_lines(grid)) return NULL;
    if (lineIndex < grid->scrollbackCount) {
        if (width) *width = grid->scrollback[lineIndex].width;
        return grid->scrollback[lineIndex].cells;
    }
    int row = lineIndex - grid->scrollbackCount;
    if (width) *width = grid->cols;
    return active_buffer_c(grid) + (size_t)row * grid->cols;
}

int terminal_grid_take_dirty(TerminalGrid *grid) {
    int was = grid->dirty;
    grid->dirty = 0;
    return was;
}

void terminal_grid_line_text(const TerminalGrid *grid, int lineIndex, char *utf8, size_t capacity) {
    if (capacity == 0) return;
    utf8[0] = 0;
    int width = 0;
    const TerminalCell *line = terminal_grid_line_at(grid, lineIndex, &width);
    if (!line) return;
    size_t used = 0;
    int lastNonBlank = -1;
    for (int c = 0; c < width; c++) if (line[c].ch != L' ') lastNonBlank = c;
    for (int c = 0; c <= lastNonBlank && used + 4 < capacity; c++) {
        unsigned int ch = line[c].ch;
        if (ch < 0x80) utf8[used++] = (char)ch;
        else if (ch < 0x800) {
            utf8[used++] = (char)(0xc0 | (ch >> 6));
            utf8[used++] = (char)(0x80 | (ch & 0x3f));
        } else {
            utf8[used++] = (char)(0xe0 | (ch >> 12));
            utf8[used++] = (char)(0x80 | ((ch >> 6) & 0x3f));
            utf8[used++] = (char)(0x80 | (ch & 0x3f));
        }
    }
    utf8[used] = 0;
}
