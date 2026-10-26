#include "terminal_view.h"

#include <commctrl.h> /* SetWindowSubclass */
#include <windowsx.h> /* GET_X_LPARAM/GET_Y_LPARAM */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "terminal_grid.h"
#include "terminal_input.h"
#include "terminal_process.h"

/* Private message the process's reader thread posts chunks with; scoped to
   one control's own window, unlike output_panel.inc's shared WM_APP_OUTPUT. */
#define WM_TERMINAL_CHUNK (WM_APP + 100)

struct TerminalView {
    HWND hwnd;
    TerminalProcess *process;
    TerminalGrid *grid;
    int session;
    int running;
    DWORD exitCode;

    int cols, rows;
    int cellWidth, cellHeight;
    HFONT font;
    TerminalPalette palette;

    int scrollOffset; /* lines scrolled up from the live bottom; 0 = pinned to it */
    int wheelDeltaRemainder; /* preserves partial high-resolution wheel turns */

    /* a plain, non-rectangular (stream) selection, in total-line coordinates */
    int selecting;
    int selecting_any;
    int anchorLine, anchorCol, pointLine, pointCol;

    unsigned int consumeCharCount; /* queued WM_(SYS)CHAR messages already handled as keys */
    int suppressTranslatedChar; /* set while the current pretranslated key is dispatched */
};

static int next_session = 1;

/* ---- layout ---- */

static void measure_font(TerminalView *view) {
    HDC hdc = GetDC(view->hwnd);
    HFONT previous = (HFONT)SelectObject(hdc, view->font);
    TEXTMETRICW metrics;
    GetTextMetricsW(hdc, &metrics);
    SIZE size;
    GetTextExtentPoint32W(hdc, L"M", 1, &size);
    view->cellWidth = size.cx > 0 ? size.cx : 1;
    view->cellHeight = metrics.tmHeight > 0 ? metrics.tmHeight : 1;
    SelectObject(hdc, previous);
    ReleaseDC(view->hwnd, hdc);
}

static void resize_to_client(TerminalView *view) {
    RECT client;
    GetClientRect(view->hwnd, &client);
    int cols = (client.right - client.left) / view->cellWidth;
    int rows = (client.bottom - client.top) / view->cellHeight;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols == view->cols && rows == view->rows) return;
    view->cols = cols;
    view->rows = rows;
    terminal_grid_resize(view->grid, cols, rows);
    terminal_process_resize(view->process, cols, rows);
    view->scrollOffset = 0;
}

static int max_scroll_offset(const TerminalView *view) {
    return terminal_grid_scrollback_count(view->grid);
}

/* Positive lines move into older scrollback; negative lines move back toward
   the live tail. Keeping this direction explicit avoids confusing wheel-up's
   positive Windows delta with screen coordinates, which grow downward. */
static void scroll_view(TerminalView *view, long long lines) {
    long long offset = (long long)view->scrollOffset + lines;
    int maximum = max_scroll_offset(view);
    if (offset < 0) offset = 0;
    if (offset > maximum) offset = maximum;
    if ((int)offset == view->scrollOffset) return;
    view->scrollOffset = (int)offset;
    InvalidateRect(view->hwnd, NULL, FALSE);
}

/* ---- selection ---- */

static void clear_selection(TerminalView *view) {
    view->selecting_any = 0;
    view->selecting = 0;
}

static void normalized_selection(const TerminalView *view, int *startLine, int *startCol,
                                 int *endLine, int *endCol) {
    if (view->anchorLine < view->pointLine ||
        (view->anchorLine == view->pointLine && view->anchorCol <= view->pointCol)) {
        *startLine = view->anchorLine; *startCol = view->anchorCol;
        *endLine = view->pointLine; *endCol = view->pointCol;
    } else {
        *startLine = view->pointLine; *startCol = view->pointCol;
        *endLine = view->anchorLine; *endCol = view->anchorCol;
    }
}

static int cell_selected(const TerminalView *view, int lineIndex, int col) {
    if (!view->selecting_any) return 0;
    int startLine, startCol, endLine, endCol;
    normalized_selection(view, &startLine, &startCol, &endLine, &endCol);
    if (lineIndex < startLine || lineIndex > endLine) return 0;
    if (lineIndex == startLine && col < startCol) return 0;
    if (lineIndex == endLine && col > endCol) return 0;
    return 1;
}

/* The total-line index and column under a client point, clamped to the grid. */
static void point_to_cell(const TerminalView *view, int x, int y, int *lineIndex, int *col) {
    int totalLines = terminal_grid_total_lines(view->grid);
    int firstLine = totalLines - view->rows - view->scrollOffset;
    if (firstLine < 0) firstLine = 0;
    int row = y / view->cellHeight;
    if (row < 0) row = 0;
    if (row >= view->rows) row = view->rows - 1;
    *lineIndex = firstLine + row;
    if (*lineIndex >= totalLines) *lineIndex = totalLines - 1;
    int c = x / view->cellWidth;
    if (c < 0) c = 0;
    *col = c;
}

static void copy_selection(TerminalView *view) {
    if (!view->selecting_any) return;
    int startLine, startCol, endLine, endCol;
    normalized_selection(view, &startLine, &startCol, &endLine, &endCol);
    size_t capacity = 256;
    WCHAR *text = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!text) return;
    size_t used = 0;
    for (int line = startLine; line <= endLine; line++) {
        int width = 0;
        const TerminalCell *cells = terminal_grid_line_at(view->grid, line, &width);
        int from = line == startLine ? startCol : 0;
        int to = line == endLine ? endCol : width - 1;
        for (int c = from; cells && c <= to && c < width; c++) {
            if (used + 2 >= capacity) {
                capacity *= 2;
                WCHAR *grown = (WCHAR *)realloc(text, capacity * sizeof(WCHAR));
                if (!grown) { free(text); return; }
                text = grown;
            }
            text[used++] = cells[c].ch;
        }
        if (line != endLine) {
            if (used + 2 >= capacity) {
                capacity *= 2;
                WCHAR *grown = (WCHAR *)realloc(text, capacity * sizeof(WCHAR));
                if (!grown) { free(text); return; }
                text = grown;
            }
            text[used++] = L'\r';
            text[used++] = L'\n';
        }
    }
    text[used] = 0;
    if (OpenClipboard(view->hwnd)) {
        EmptyClipboard();
        HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, (used + 1) * sizeof(WCHAR));
        if (block) {
            WCHAR *dest = (WCHAR *)GlobalLock(block);
            if (dest) { memcpy(dest, text, (used + 1) * sizeof(WCHAR)); GlobalUnlock(block); }
            SetClipboardData(CF_UNICODETEXT, block);
        }
        CloseClipboard();
    }
    free(text);
}

static void paste_clipboard(TerminalView *view) {
    if (!view->running || !OpenClipboard(view->hwnd)) return;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data) {
        const WCHAR *wide = (const WCHAR *)GlobalLock(data);
        if (wide) {
            int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
            if (utf8Length > 1) {
                char *utf8 = (char *)malloc((size_t)utf8Length);
                if (utf8) {
                    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, utf8Length, NULL, NULL);
                    terminal_process_write(view->process, utf8, (size_t)utf8Length - 1);
                    free(utf8);
                }
            }
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
}

/* ---- painting ---- */

static void paint_view(TerminalView *view, HDC hdc, const RECT *client) {
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP bitmap = CreateCompatibleBitmap(hdc, client->right, client->bottom);
    HBITMAP previousBitmap = (HBITMAP)SelectObject(memDC, bitmap);
    HFONT previousFont = (HFONT)SelectObject(memDC, view->font);
    SetBkMode(memDC, OPAQUE);

    int totalLines = terminal_grid_total_lines(view->grid);
    int firstLine = totalLines - view->rows - view->scrollOffset;
    if (firstLine < 0) firstLine = 0;
    int cursorCol = 0, cursorRow = 0, cursorVisible = 0;
    terminal_grid_cursor(view->grid, &cursorCol, &cursorRow, &cursorVisible);

    for (int screenRow = 0; screenRow < view->rows; screenRow++) {
        int lineIndex = firstLine + screenRow;
        int width = 0;
        const TerminalCell *cells = lineIndex < totalLines
            ? terminal_grid_line_at(view->grid, lineIndex, &width) : NULL;
        int y = screenRow * view->cellHeight;
        int x = 0;
        int runStart = 0;
        int isCursorRow = view->scrollOffset == 0 && cursorVisible && lineIndex == firstLine + cursorRow;

        for (int c = 0; c <= view->cols; c++) {
            int atRunEnd = c == view->cols || c >= width;
            int breaksRun = atRunEnd;
            if (!atRunEnd && c > runStart) {
                int sameAsPrev = cells[c].fg == cells[c - 1].fg && cells[c].bg == cells[c - 1].bg &&
                                 cells[c].underline == cells[c - 1].underline &&
                                 cell_selected(view, lineIndex, c) == cell_selected(view, lineIndex, c - 1) &&
                                 (!isCursorRow || (c != cursorCol && c - 1 != cursorCol));
                breaksRun = !sameAsPrev;
            }
            if (breaksRun && c > runStart) {
                int runLength = c - runStart;
                WCHAR text[512];
                int textLength = runLength < 512 ? runLength : 511;
                for (int i = 0; i < textLength; i++) text[i] = cells[runStart + i].ch;
                int selected = cell_selected(view, lineIndex, runStart);
                int isCursorCell = isCursorRow && runStart == cursorCol && runLength == 1;
                COLORREF fg = terminal_grid_resolve_colour(cells[runStart].fg, 0, &view->palette);
                COLORREF bg = terminal_grid_resolve_colour(cells[runStart].bg, 1, &view->palette);
                if (selected || isCursorCell) { COLORREF t = fg; fg = bg; bg = t; }
                SetTextColor(memDC, fg);
                SetBkColor(memDC, bg);
                RECT runRect = { x, y, x + runLength * view->cellWidth, y + view->cellHeight };
                ExtTextOutW(memDC, x, y, ETO_OPAQUE, &runRect, text, (UINT)textLength, NULL);
                if (cells[runStart].underline) {
                    HPEN pen = CreatePen(PS_SOLID, 1, fg);
                    HPEN previousPen = (HPEN)SelectObject(memDC, pen);
                    int lineY = y + view->cellHeight - 1;
                    MoveToEx(memDC, x, lineY, NULL);
                    LineTo(memDC, x + runLength * view->cellWidth, lineY);
                    SelectObject(memDC, previousPen);
                    DeleteObject(pen);
                }
                x += runLength * view->cellWidth;
                runStart = c;
            }
        }
        if (width <= 0 || !cells) {
            RECT blankRect = { 0, y, client->right, y + view->cellHeight };
            SetBkColor(memDC, view->palette.defaultBg);
            ExtTextOutW(memDC, 0, y, ETO_OPAQUE, &blankRect, L"", 0, NULL);
        } else if (x < client->right) {
            RECT tailRect = { x, y, client->right, y + view->cellHeight };
            SetBkColor(memDC, view->palette.defaultBg);
            ExtTextOutW(memDC, x, y, ETO_OPAQUE, &tailRect, L"", 0, NULL);
        }
    }
    int paintedBottom = view->rows * view->cellHeight;
    if (paintedBottom < client->bottom) {
        RECT fillRect = { 0, paintedBottom, client->right, client->bottom };
        SetBkColor(memDC, view->palette.defaultBg);
        ExtTextOutW(memDC, 0, paintedBottom, ETO_OPAQUE, &fillRect, L"", 0, NULL);
    }

    BitBlt(hdc, 0, 0, client->right, client->bottom, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, previousFont);
    SelectObject(memDC, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memDC);
}

/* ---- keyboard ---- */

static void send_bytes(TerminalView *view, const char *bytes, size_t length) {
    if (view->running) terminal_process_write(view->process, bytes, length);
}

static unsigned int key_modifiers(void) {
    unsigned int modifiers = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= TERMINAL_MOD_SHIFT;
    if (GetKeyState(VK_MENU) & 0x8000) modifiers |= TERMINAL_MOD_ALT;
    if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= TERMINAL_MOD_CONTROL;
    return modifiers;
}

/* Virtual keys that need VT sequences instead of their WM_CHAR translation.
   Printable keys and control characters still use TranslateMessage(), which
   preserves the active keyboard layout and dead-key composition. */
static int handle_special_key(TerminalView *view, WPARAM key) {
    unsigned int modifiers = key_modifiers();
    if (key == VK_INSERT && (modifiers & TERMINAL_MOD_SHIFT)) {
        paste_clipboard(view);
        return 1;
    }
    char sequence[24];
    size_t length = terminal_input_sequence((unsigned int)key, modifiers,
                                            terminal_grid_app_cursor_keys(view->grid),
                                            sequence, sizeof(sequence));
    if (!length) return 0;
    send_bytes(view, sequence, length);
    return 1;
}

static void send_character(TerminalView *view, WCHAR ch, int prefixEscape) {
    char utf8[8];
    int offset = prefixEscape ? 1 : 0;
    if (prefixEscape) utf8[0] = '\x1b';
    int length = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, utf8 + offset,
                                     (int)sizeof(utf8) - offset, NULL, NULL);
    if (length > 0) send_bytes(view, utf8, (size_t)(length + offset));
}

/* ---- the process's output ---- */

static void grid_reply(void *context, const char *bytes, size_t length) {
    TerminalView *view = (TerminalView *)context;
    if (view->running) terminal_process_write(view->process, bytes, length);
}

static void send_notification(TerminalView *view, UINT code) {
    NMHDR notification = { view->hwnd, (UINT_PTR)GetDlgCtrlID(view->hwnd), code };
    SendMessageW(GetParent(view->hwnd), WM_NOTIFY, (WPARAM)notification.idFrom, (LPARAM)&notification);
}

static void handle_chunk(TerminalView *view, TerminalChunk *chunk) {
    if (chunk->session == view->session) {
        int oldScrollback = terminal_grid_scrollback_count(view->grid);
        if (chunk->length) terminal_grid_feed(view->grid, chunk->data, chunk->length, grid_reply, view);
        if (chunk->finished) {
            view->running = 0;
            view->exitCode = chunk->exitCode;
            char note[128];
            snprintf(note, sizeof(note),
                    "\r\n\x1b[90m[The shell exited with code %lu. Press any key to start it again.]\x1b[0m\r\n",
                    (unsigned long)chunk->exitCode);
            terminal_grid_feed(view->grid, note, strlen(note), NULL, NULL);
            send_notification(view, TVN_EXITED);
        }
        /* New output normally keeps the live tail pinned. If the user has
           scrolled back, compensate for newly archived rows so the text
           currently under the pointer stays in place instead of drifting. */
        if (view->scrollOffset > 0) {
            int added = terminal_grid_scrollback_count(view->grid) - oldScrollback;
            if (added > 0) {
                long long offset = (long long)view->scrollOffset + added;
                int maximum = max_scroll_offset(view);
                view->scrollOffset = offset > maximum ? maximum : (int)offset;
            }
        }
        if (terminal_grid_take_dirty(view->grid)) InvalidateRect(view->hwnd, NULL, FALSE);
    }
    free(chunk);
}

/* ---- window procedure ---- */

static LRESULT CALLBACK TerminalViewSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                 UINT_PTR id, DWORD_PTR data) {
    (void)id;
    TerminalView *view = (TerminalView *)data;
    switch (msg) {
        case WM_TERMINAL_CHUNK:
            handle_chunk(view, (TerminalChunk *)lParam);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            HDC hdc = BeginPaint(hwnd, &paint);
            RECT client;
            GetClientRect(hwnd, &client);
            paint_view(view, hdc, &client);
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_SIZE:
            resize_to_client(view);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_SETFOCUS: case WM_KILLFOCUS:
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_GETDLGCODE:
            /* The pretranslation path normally bypasses IsDialogMessage(),
               but these flags also protect keys delivered directly or by a
               future dialog host: the terminal, not dialog navigation, owns
               Enter, Escape, Tab, characters, and cursor keys. */
            return DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB | DLGC_WANTALLKEYS;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            SetCapture(hwnd);
            view->selecting = 1;
            point_to_cell(view, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &view->anchorLine, &view->anchorCol);
            view->pointLine = view->anchorLine;
            view->pointCol = view->anchorCol;
            view->selecting_any = 1;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (view->selecting) {
                point_to_cell(view, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &view->pointLine, &view->pointCol);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (view->selecting) {
                view->selecting = 0;
                ReleaseCapture();
                /* A plain click, without dragging, selects nothing. */
                if (view->anchorLine == view->pointLine && view->anchorCol == view->pointCol) {
                    clear_selection(view);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;
        case WM_RBUTTONUP:
            paste_clipboard(view);
            return 0;
        case WM_MOUSEWHEEL: {
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            view->wheelDeltaRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
            int notches = view->wheelDeltaRemainder / WHEEL_DELTA;
            view->wheelDeltaRemainder -= notches * WHEEL_DELTA;
            if (notches) {
                long long amount = lines == WHEEL_PAGESCROLL
                    ? (long long)notches * view->rows
                    : (long long)notches * lines;
                scroll_view(view, amount);
            }
            return 0;
        }
        case WM_KEYDOWN: case WM_SYSKEYDOWN: {
            int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrl && shift && (wParam == 'C')) {
                copy_selection(view);
                view->suppressTranslatedChar = 1;
                return 0;
            }
            if (ctrl && shift && (wParam == 'V')) {
                paste_clipboard(view);
                view->suppressTranslatedChar = 1;
                return 0;
            }
            if (!view->running) {
                if (wParam != VK_SHIFT && wParam != VK_CONTROL && wParam != VK_MENU) {
                    view->suppressTranslatedChar = 1;
                    send_notification(view, TVN_RESTART_REQUESTED);
                }
                return 0;
            }
            if (view->scrollOffset != 0 && wParam != VK_SHIFT && wParam != VK_CONTROL && wParam != VK_MENU)
                view->scrollOffset = 0; /* typing returns the view to the live tail */
            if (handle_special_key(view, wParam)) {
                view->suppressTranslatedChar = 1;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            return 0;
        }
        case WM_CHAR: case WM_SYSCHAR: {
            if (view->consumeCharCount) { view->consumeCharCount--; return 0; }
            if (!view->running) return 0;
            /* WM_SYSCHAR represents Alt/Meta and gets an Escape prefix. Right
               Alt plus Control is AltGr, so it remains a layout character. */
            int altGr = (GetKeyState(VK_RMENU) & 0x8000) &&
                        (GetKeyState(VK_CONTROL) & 0x8000);
            send_character(view, (WCHAR)wParam, msg == WM_SYSCHAR && !altGr);
            return 0;
        }
        case WM_DEADCHAR:
        case WM_SYSDEADCHAR:
        case WM_SYSKEYUP:
            return 0; /* keep Alt input from activating the application menu */
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, TerminalViewSubclassProc, 1);
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

int terminal_view_pretranslate_message(TerminalView *view, const MSG *message) {
    if (!view || !view->hwnd || !message ||
        (message->hwnd != view->hwnd && !IsChild(view->hwnd, message->hwnd)))
        return 0;
    switch (message->message) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            /* Translate first so layout-aware WM_(SYS)CHAR is queued, then let
               the key handler emit any non-text VT sequence. If it did, count
               the queued character so repeats cannot leak or swallow input. */
            view->suppressTranslatedChar = 0;
            int translated = TranslateMessage(message);
            DispatchMessage(message);
            if (translated && view->suppressTranslatedChar && view->consumeCharCount < UINT_MAX)
                view->consumeCharCount++;
            return 1;
        case WM_KEYUP: case WM_SYSKEYUP:
        case WM_CHAR: case WM_SYSCHAR:
        case WM_DEADCHAR: case WM_SYSDEADCHAR:
            DispatchMessage(message);
            return 1;
        default:
            return 0;
    }
}

/* ---- lifecycle ---- */

TerminalView *terminal_view_create(HWND parent, int id) {
    TerminalView *view = (TerminalView *)calloc(1, sizeof(*view));
    if (!view) return NULL;
    view->hwnd = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP,
                                 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    if (!view->hwnd) { free(view); return NULL; }
    view->font = (HFONT)GetStockObject(SYSTEM_FIXED_FONT);
    view->cols = 80;
    view->rows = 24;
    view->grid = terminal_grid_create(view->cols, view->rows);
    if (!view->grid) { DestroyWindow(view->hwnd); free(view); return NULL; }
    measure_font(view);
    SetWindowSubclass(view->hwnd, TerminalViewSubclassProc, 1, (DWORD_PTR)view);
    return view;
}

HWND terminal_view_window(const TerminalView *view) { return view ? view->hwnd : NULL; }

void terminal_view_free(TerminalView *view) {
    if (!view) return;
    terminal_process_free(view->process);
    terminal_grid_free(view->grid);
    if (view->hwnd) DestroyWindow(view->hwnd);
    free(view);
}

int terminal_view_start(TerminalView *view, const WCHAR *commandLine, const WCHAR *directory) {
    terminal_process_free(view->process);
    view->process = NULL;
    view->running = 0;
    clear_selection(view);
    view->scrollOffset = 0;
    terminal_grid_reset(view->grid);
    resize_to_client(view);
    view->session = next_session++;
    view->process = terminal_process_start(commandLine, directory, view->cols, view->rows,
                                           view->hwnd, WM_TERMINAL_CHUNK, view->session);
    view->running = view->process != NULL;
    InvalidateRect(view->hwnd, NULL, FALSE);
    return view->running;
}

void terminal_view_show_message(TerminalView *view, const WCHAR *message) {
    int utf8Length = WideCharToMultiByte(CP_UTF8, 0, message, -1, NULL, 0, NULL, NULL);
    if (utf8Length <= 0) return;
    static const char prefix[] = "\x1b[90m", suffix[] = "\x1b[0m\r\n";
    size_t capacity = sizeof(prefix) - 1 + (size_t)utf8Length - 1 + sizeof(suffix) - 1;
    char *text = (char *)malloc(capacity);
    if (!text) return;
    memcpy(text, prefix, sizeof(prefix) - 1);
    WideCharToMultiByte(CP_UTF8, 0, message, -1, text + sizeof(prefix) - 1, utf8Length, NULL, NULL);
    memcpy(text + sizeof(prefix) - 1 + utf8Length - 1, suffix, sizeof(suffix) - 1);
    terminal_grid_feed(view->grid, text, capacity, NULL, NULL);
    free(text);
    InvalidateRect(view->hwnd, NULL, FALSE);
}

int terminal_view_running(const TerminalView *view) { return view->running; }

void terminal_view_stop(TerminalView *view) {
    if (view->process) terminal_process_stop(view->process);
}

void terminal_view_clear_scrollback(TerminalView *view) {
    terminal_grid_reset(view->grid);
    clear_selection(view);
    view->scrollOffset = 0;
    InvalidateRect(view->hwnd, NULL, FALSE);
}

void terminal_view_apply_theme(TerminalView *view, const COLORREF ansi[16],
                               COLORREF defaultFg, COLORREF defaultBg) {
    memcpy(view->palette.ansi, ansi, sizeof(view->palette.ansi));
    view->palette.defaultFg = defaultFg;
    view->palette.defaultBg = defaultBg;
    InvalidateRect(view->hwnd, NULL, FALSE);
}

void terminal_view_apply_font(TerminalView *view, const char *faceNameUtf8, int pointSize, int weight) {
    WCHAR faceName[64];
    if (MultiByteToWideChar(CP_UTF8, 0, faceNameUtf8, -1, faceName, 64) == 0)
        wcscpy(faceName, L"Consolas");
    HDC hdc = GetDC(view->hwnd);
    int height = -MulDiv(pointSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(view->hwnd, hdc);
    HFONT font = CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, faceName);
    if (!font) return;
    if (view->font && view->font != (HFONT)GetStockObject(SYSTEM_FIXED_FONT)) DeleteObject(view->font);
    view->font = font;
    measure_font(view);
    resize_to_client(view);
    InvalidateRect(view->hwnd, NULL, FALSE);
}
