#include "themed_scrollbar.h"

#include <windowsx.h>
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>

#include "theme.h"
#include "../third_party/scintilla/include/Scintilla.h"

/* The bars live in the target window's non-client area, exactly where native
   ones would, so nothing about the surrounding layout changes: WM_NCCALCSIZE
   reserves the strip, WM_NCPAINT paints it, and WM_NCHITTEST/WM_NCLBUTTONDOWN
   drive it. The target must not carry WS_VSCROLL/WS_HSCROLL (Scintilla's own
   bars are switched off in attach) or the system would draw its own on top.
   Scroll state is read back from the control after every message (see
   should_sync()) because neither Scintilla nor LISTBOX tells us when it
   scrolls on its own, and moved with the control's own scroll messages. */

#define SUBCLASS_ID    0x5342
#define BAR_VERTICAL   0
#define BAR_HORIZONTAL 1
#define BAR_COUNT      2

#define BAR_THICKNESS_DIP  14
#define THUMB_INSET_DIP    3
#define THUMB_MIN_DIP      24

typedef struct {
    int total; /* content extent, in the control's own scroll units */
    int page;  /* how much of it is visible at once */
    int pos;   /* first visible unit */
} ScrollAxis;

typedef struct {
    ThemedScrollKind kind;
    ScrollAxis axis[BAR_COUNT];
    int visible[BAR_COUNT];
    int hot;         /* bar whose thumb the mouse is over, or -1 */
    int drag;        /* bar whose thumb is being dragged, or -1 */
    int dragGrab;    /* cursor offset from the thumb's leading edge when the drag began */
    int tracking;    /* a WM_NCMOUSELEAVE is pending */
    int syncing;     /* guards sync() against re-entry from its own queries/frame change */
    int wheelRemainder;
    int widthSeen;   /* widest line painted so far; see track_line_widths() */
    int scanWidths;  /* set around WM_PAINT so sync() re-measures the visible lines */
} BarState;

static int max_scroll_pos(const ScrollAxis *axis) {
    int range = axis->total - axis->page;
    return range > 0 ? range : 0;
}

static int clamp_scroll_pos(const ScrollAxis *axis, int pos) {
    int maxPos = max_scroll_pos(axis);
    if (pos < 0) return 0;
    return pos > maxPos ? maxPos : pos;
}

/* ---- reading the control's scroll state ---- */

static int scintilla_text_width(HWND hwnd, int clientWidth) {
    int width = clientWidth;
    for (int margin = 0; margin <= SC_MAX_MARGIN; margin++)
        width -= (int)SendMessageA(hwnd, SCI_GETMARGINWIDTHN, (WPARAM)margin, 0);
    return width;
}

/* Scintilla only grows its scroll width ("tracking") while its own horizontal
   bar is switched on, and ours replaces that bar - so the widest line seen
   while painting is tracked here instead, the same way: it never shrinks, and
   resets when the app sets the scroll width (see bar_subclass_proc). */
static void track_line_widths(HWND hwnd, BarState *state, int clientWidth, int textWidth) {
    Sci_Position lineCount = (Sci_Position)SendMessageA(hwnd, SCI_GETLINECOUNT, 0, 0);
    int firstVisible = (int)SendMessageA(hwnd, SCI_GETFIRSTVISIBLELINE, 0, 0);
    int visibleLines = (int)SendMessageA(hwnd, SCI_LINESONSCREEN, 0, 0);
    int xOffset = (int)SendMessageA(hwnd, SCI_GETXOFFSET, 0, 0);
    int marginWidth = clientWidth - textWidth;
    for (int i = 0; i <= visibleLines; i++) { /* <= : the partly visible last line counts too */
        Sci_Position docLine = (Sci_Position)SendMessageA(hwnd, SCI_DOCLINEFROMVISIBLE, (WPARAM)(firstVisible + i), 0);
        if (docLine >= lineCount) break;
        Sci_Position lineEnd = (Sci_Position)SendMessageA(hwnd, SCI_GETLINEENDPOSITION, (WPARAM)docLine, 0);
        int width = (int)SendMessageA(hwnd, SCI_POINTXFROMPOSITION, 0, (LPARAM)lineEnd) - marginWidth + xOffset;
        if (width > state->widthSeen) state->widthSeen = width;
    }
}

/* Fills state->axis and returns which bars are wanted. */
static void query_scintilla(HWND hwnd, BarState *state, int want[BAR_COUNT]) {
    RECT client;
    GetClientRect(hwnd, &client);

    ScrollAxis *v = &state->axis[BAR_VERTICAL];
    Sci_Position lineCount = (Sci_Position)SendMessageA(hwnd, SCI_GETLINECOUNT, 0, 0);
    v->total = (int)SendMessageA(hwnd, SCI_VISIBLEFROMDOCLINE, (WPARAM)lineCount, 0);
    v->page = (int)SendMessageA(hwnd, SCI_LINESONSCREEN, 0, 0);
    v->pos = (int)SendMessageA(hwnd, SCI_GETFIRSTVISIBLELINE, 0, 0);
    want[BAR_VERTICAL] = v->page > 0 && v->total > v->page;

    ScrollAxis *h = &state->axis[BAR_HORIZONTAL];
    int unwrapped = SendMessageA(hwnd, SCI_GETWRAPMODE, 0, 0) == SC_WRAP_NONE;
    h->page = scintilla_text_width(hwnd, client.right - client.left);
    if (unwrapped && state->scanWidths && h->page > 0)
        track_line_widths(hwnd, state, client.right - client.left, h->page);
    h->total = state->widthSeen;
    h->pos = (int)SendMessageA(hwnd, SCI_GETXOFFSET, 0, 0);
    want[BAR_HORIZONTAL] = unwrapped && h->page > 0 && h->total > h->page;
}

static void query_listbox(HWND hwnd, BarState *state, int want[BAR_COUNT]) {
    RECT client;
    GetClientRect(hwnd, &client);
    int itemHeight = (int)SendMessageW(hwnd, LB_GETITEMHEIGHT, 0, 0);
    if (itemHeight <= 0) itemHeight = 1;

    ScrollAxis *v = &state->axis[BAR_VERTICAL];
    v->total = (int)SendMessageW(hwnd, LB_GETCOUNT, 0, 0);
    v->page = (client.bottom - client.top) / itemHeight;
    if (v->page < 1) v->page = 1;
    v->pos = (int)SendMessageW(hwnd, LB_GETTOPINDEX, 0, 0);
    want[BAR_VERTICAL] = v->total > v->page;
    want[BAR_HORIZONTAL] = 0;
    state->axis[BAR_HORIZONTAL] = (ScrollAxis){0, 0, 0};
}

static void query_axes(HWND hwnd, BarState *state, int want[BAR_COUNT]) {
    if (state->kind == THEMED_SCROLL_SCINTILLA) query_scintilla(hwnd, state, want);
    else query_listbox(hwnd, state, want);
}

/* ---- geometry (all in window coordinates, i.e. what GetWindowDC paints in) ---- */

static int bar_thickness(HWND hwnd) {
    return MulDiv(BAR_THICKNESS_DIP, (int)GetDpiForWindow(hwnd), 96);
}

static void get_layout(HWND hwnd, const BarState *state, RECT bar[BAR_COUNT], RECT *corner) {
    RECT windowRect, client;
    POINT origin = {0, 0};
    GetWindowRect(hwnd, &windowRect);
    ClientToScreen(hwnd, &origin);
    GetClientRect(hwnd, &client);
    int thick = bar_thickness(hwnd);
    int left = origin.x - windowRect.left;
    int top = origin.y - windowRect.top;
    int width = client.right - client.left;
    int height = client.bottom - client.top;

    SetRect(&bar[BAR_VERTICAL], left + width, top, left + width + thick, top + height);
    SetRect(&bar[BAR_HORIZONTAL], left, top + height, left + width, top + height + thick);
    SetRect(corner, left + width, top + height, left + width + thick, top + height + thick);
    if (!state->visible[BAR_VERTICAL]) SetRectEmpty(&bar[BAR_VERTICAL]);
    if (!state->visible[BAR_HORIZONTAL]) SetRectEmpty(&bar[BAR_HORIZONTAL]);
    if (!(state->visible[BAR_VERTICAL] && state->visible[BAR_HORIZONTAL])) SetRectEmpty(corner);
}

static int bar_track_length(const RECT *rc, int bar) {
    return bar == BAR_VERTICAL ? rc->bottom - rc->top : rc->right - rc->left;
}

/* Thumb's offset from, and length along, the bar's leading edge. */
static void thumb_metrics(HWND hwnd, const BarState *state, int bar, int trackLength,
                          int *thumbStart, int *thumbLength) {
    const ScrollAxis *axis = &state->axis[bar];
    int minThumb = MulDiv(THUMB_MIN_DIP, (int)GetDpiForWindow(hwnd), 96);
    int length = axis->total > 0
        ? (int)((long long)trackLength * axis->page / axis->total)
        : trackLength;
    if (length < minThumb) length = minThumb;
    if (length > trackLength) length = trackLength;
    int range = trackLength - length;
    int maxPos = max_scroll_pos(axis);
    int start = maxPos > 0 ? (int)((long long)range * clamp_scroll_pos(axis, axis->pos) / maxPos) : 0;
    *thumbStart = start;
    *thumbLength = length;
}

static RECT thumb_rect(HWND hwnd, const BarState *state, int bar, const RECT *rcBar) {
    int thumbStart, thumbLength;
    thumb_metrics(hwnd, state, bar, bar_track_length(rcBar, bar), &thumbStart, &thumbLength);
    int inset = MulDiv(THUMB_INSET_DIP, (int)GetDpiForWindow(hwnd), 96);
    RECT thumb = *rcBar;
    if (bar == BAR_VERTICAL) {
        thumb.top = rcBar->top + thumbStart;
        thumb.bottom = thumb.top + thumbLength;
        thumb.left += inset;
        thumb.right -= inset;
    } else {
        thumb.left = rcBar->left + thumbStart;
        thumb.right = thumb.left + thumbLength;
        thumb.top += inset;
        thumb.bottom -= inset;
    }
    return thumb;
}

/* ---- painting ---- */

static void paint_bar(HDC hdc, HWND hwnd, const BarState *state, int bar, const RECT *rcBar) {
    if (IsRectEmpty(rcBar)) return;
    fill_rect_color(hdc, rcBar, theme_bg_color());

    RECT thumb = thumb_rect(hwnd, state, bar, rcBar);
    COLORREF colour = state->drag == bar ? theme_scroll_thumb_drag_color()
                    : state->hot == bar ? theme_scroll_thumb_hot_color()
                    : theme_scroll_thumb_color();
    int radius = (bar == BAR_VERTICAL ? thumb.right - thumb.left : thumb.bottom - thumb.top);
    HRGN region = CreateRoundRectRgn(thumb.left, thumb.top, thumb.right + 1, thumb.bottom + 1, radius, radius);
    COLORREF previous = SetDCBrushColor(hdc, colour);
    FillRgn(hdc, region, (HBRUSH)GetStockObject(DC_BRUSH));
    SetDCBrushColor(hdc, previous);
    DeleteObject(region);
}

static void paint_bars(HWND hwnd, const BarState *state) {
    if (!state->visible[BAR_VERTICAL] && !state->visible[BAR_HORIZONTAL]) return;
    HDC hdc = GetDCEx(hwnd, NULL, DCX_WINDOW | DCX_CACHE | DCX_CLIPSIBLINGS);
    if (!hdc) return;
    RECT bar[BAR_COUNT], corner;
    get_layout(hwnd, state, bar, &corner);
    for (int i = 0; i < BAR_COUNT; i++) paint_bar(hdc, hwnd, state, i, &bar[i]);
    if (!IsRectEmpty(&corner)) fill_rect_color(hdc, &corner, theme_bg_color());
    ReleaseDC(hwnd, hdc);
}

/* ---- keeping the bars in step with the control ---- */

static void reframe(HWND hwnd) {
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static void sync(HWND hwnd, BarState *state) {
    if (state->syncing) return;
    state->syncing = 1;

    ScrollAxis before[BAR_COUNT] = {state->axis[0], state->axis[1]};
    int want[BAR_COUNT];
    query_axes(hwnd, state, want);

    int changed = memcmp(before, state->axis, sizeof(before)) != 0;
    if (want[BAR_VERTICAL] != state->visible[BAR_VERTICAL] ||
        want[BAR_HORIZONTAL] != state->visible[BAR_HORIZONTAL]) {
        state->visible[BAR_VERTICAL] = want[BAR_VERTICAL];
        state->visible[BAR_HORIZONTAL] = want[BAR_HORIZONTAL];
        reframe(hwnd); /* the strip is reserved in WM_NCCALCSIZE, so the client area resizes */
        query_axes(hwnd, state, want); /* page sizes changed with the client area */
        changed = 1;
    }
    if (changed) paint_bars(hwnd, state);
    state->syncing = 0;
}

static void scroll_to(HWND hwnd, BarState *state, int bar, int pos) {
    pos = clamp_scroll_pos(&state->axis[bar], pos);
    if (state->kind == THEMED_SCROLL_SCINTILLA) {
        if (bar == BAR_VERTICAL) SendMessageA(hwnd, SCI_SETFIRSTVISIBLELINE, (WPARAM)pos, 0);
        else SendMessageA(hwnd, SCI_SETXOFFSET, (WPARAM)pos, 0);
    } else {
        SendMessageW(hwnd, LB_SETTOPINDEX, (WPARAM)pos, 0);
    }
    sync(hwnd, state);
}

static int should_sync(UINT msg) {
    switch (msg) {
        case WM_NCPAINT: case WM_ERASEBKGND: /* WM_PAINT is not skipped: Scintilla grows its scroll width while painting */
        case WM_NCHITTEST: case WM_SETCURSOR:
        case WM_MOUSEMOVE: case WM_NCMOUSEMOVE: case WM_NCMOUSELEAVE: case WM_MOUSELEAVE:
        case WM_GETDLGCODE: case WM_GETFONT: case WM_GETTEXT: case WM_GETTEXTLENGTH:
            return 0;
        default:
            return 1;
    }
}

/* ---- mouse ---- */

static int hit_bar(const BarState *state, HWND hwnd, POINT screen, RECT bar[BAR_COUNT]) {
    RECT corner, windowRect;
    get_layout(hwnd, state, bar, &corner);
    GetWindowRect(hwnd, &windowRect);
    POINT local = {screen.x - windowRect.left, screen.y - windowRect.top};
    for (int i = 0; i < BAR_COUNT; i++)
        if (PtInRect(&bar[i], local)) return i;
    return -1;
}

static int thumb_under_cursor(HWND hwnd, const BarState *state, int bar, const RECT *rcBar, POINT screen) {
    RECT windowRect;
    GetWindowRect(hwnd, &windowRect);
    POINT local = {screen.x - windowRect.left, screen.y - windowRect.top};
    RECT thumb = thumb_rect(hwnd, state, bar, rcBar);
    return PtInRect(&thumb, local);
}

static void set_hot(HWND hwnd, BarState *state, int hot) {
    if (state->hot == hot) return;
    state->hot = hot;
    paint_bars(hwnd, state);
}

static void begin_track(HWND hwnd, BarState *state, int bar) {
    POINT cursor;
    GetCursorPos(&cursor);
    RECT barRects[BAR_COUNT], windowRect;
    hit_bar(state, hwnd, cursor, barRects);
    GetWindowRect(hwnd, &windowRect);
    POINT local = {cursor.x - windowRect.left, cursor.y - windowRect.top};
    RECT thumb = thumb_rect(hwnd, state, bar, &barRects[bar]);

    if (PtInRect(&thumb, local)) {
        state->drag = bar;
        state->dragGrab = bar == BAR_VERTICAL ? local.y - thumb.top : local.x - thumb.left;
        SetCapture(hwnd);
        paint_bars(hwnd, state);
        return;
    }
    int before = bar == BAR_VERTICAL ? local.y < thumb.top : local.x < thumb.left;
    const ScrollAxis *axis = &state->axis[bar];
    scroll_to(hwnd, state, bar, axis->pos + (before ? -axis->page : axis->page));
}

static void drag_thumb(HWND hwnd, BarState *state) {
    int bar = state->drag;
    POINT cursor;
    GetCursorPos(&cursor);
    RECT barRects[BAR_COUNT], corner, windowRect;
    get_layout(hwnd, state, barRects, &corner);
    GetWindowRect(hwnd, &windowRect);
    int along = bar == BAR_VERTICAL ? cursor.y - windowRect.top - barRects[bar].top
                                    : cursor.x - windowRect.left - barRects[bar].left;
    int trackLength = bar_track_length(&barRects[bar], bar);
    int thumbStart, thumbLength;
    thumb_metrics(hwnd, state, bar, trackLength, &thumbStart, &thumbLength);
    int range = trackLength - thumbLength;
    if (range <= 0) return;
    int start = along - state->dragGrab;
    if (start < 0) start = 0;
    if (start > range) start = range;
    scroll_to(hwnd, state, bar, (int)((long long)start * max_scroll_pos(&state->axis[bar]) / range));
}

static void wheel_listbox(HWND hwnd, BarState *state, int delta) {
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    state->wheelRemainder += delta;
    int notches = state->wheelRemainder / WHEEL_DELTA;
    state->wheelRemainder -= notches * WHEEL_DELTA;
    if (notches == 0) return;
    const ScrollAxis *axis = &state->axis[BAR_VERTICAL];
    int step = lines == WHEEL_PAGESCROLL ? axis->page : (int)lines;
    scroll_to(hwnd, state, BAR_VERTICAL, axis->pos - notches * step);
}

/* ---- window procedure ---- */

static LRESULT CALLBACK bar_subclass_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR id, DWORD_PTR data) {
    BarState *state = (BarState *)data;

    switch (msg) {
        case SCI_SETSCROLLWIDTH:
            if (state->kind == THEMED_SCROLL_SCINTILLA) state->widthSeen = (int)wParam;
            break;
        case WM_NCCALCSIZE: {
            LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
            RECT *rc = wParam ? &((NCCALCSIZE_PARAMS *)lParam)->rgrc[0] : (RECT *)lParam;
            int thick = bar_thickness(hwnd);
            if (state->visible[BAR_VERTICAL]) rc->right -= thick;
            if (state->visible[BAR_HORIZONTAL]) rc->bottom -= thick;
            return result;
        }
        case WM_NCPAINT: {
            LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
            paint_bars(hwnd, state);
            return result;
        }
        case WM_NCHITTEST: {
            LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
            RECT bar[BAR_COUNT];
            POINT screen = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int hit = hit_bar(state, hwnd, screen, bar);
            if (hit == BAR_VERTICAL) return HTVSCROLL;
            if (hit == BAR_HORIZONTAL) return HTHSCROLL;
            return result;
        }
        case WM_NCLBUTTONDOWN: case WM_NCLBUTTONDBLCLK:
            if (wParam == HTVSCROLL || wParam == HTHSCROLL) {
                begin_track(hwnd, state, wParam == HTVSCROLL ? BAR_VERTICAL : BAR_HORIZONTAL);
                return 0;
            }
            break;
        case WM_NCRBUTTONDOWN: case WM_NCRBUTTONUP: case WM_NCLBUTTONUP:
            if (wParam == HTVSCROLL || wParam == HTHSCROLL) return 0;
            break;
        case WM_NCMOUSEMOVE:
            if (wParam == HTVSCROLL || wParam == HTHSCROLL) {
                int bar = wParam == HTVSCROLL ? BAR_VERTICAL : BAR_HORIZONTAL;
                RECT barRects[BAR_COUNT];
                POINT cursor;
                GetCursorPos(&cursor);
                hit_bar(state, hwnd, cursor, barRects);
                set_hot(hwnd, state, thumb_under_cursor(hwnd, state, bar, &barRects[bar], cursor) ? bar : -1);
                if (!state->tracking) {
                    TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
                    state->tracking = TrackMouseEvent(&track) != 0;
                }
            } else {
                set_hot(hwnd, state, -1);
            }
            break;
        case WM_NCMOUSELEAVE:
            state->tracking = 0;
            if (state->drag < 0) set_hot(hwnd, state, -1);
            break;
        case WM_MOUSEMOVE:
            if (state->drag >= 0) { drag_thumb(hwnd, state); return 0; }
            break;
        case WM_LBUTTONUP:
            if (state->drag >= 0) {
                ReleaseCapture();
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (state->drag >= 0) {
                state->drag = -1;
                paint_bars(hwnd, state);
            }
            break;
        case WM_MOUSEWHEEL:
            if (state->kind == THEMED_SCROLL_LISTBOX) {
                wheel_listbox(hwnd, state, GET_WHEEL_DELTA_WPARAM(wParam));
                return 0;
            }
            break;
        case WM_NCDESTROY: {
            RemoveWindowSubclass(hwnd, bar_subclass_proc, id);
            LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
            free(state);
            return result;
        }
    }

    LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (!state->syncing && should_sync(msg)) { /* our own queries re-enter this proc */
        state->scanWidths = msg == WM_PAINT;
        sync(hwnd, state);
        state->scanWidths = 0;
    }
    return result;
}

void themed_scrollbar_attach(HWND hwnd, ThemedScrollKind kind) {
    if (!hwnd) return;
    BarState *state = calloc(1, sizeof(*state));
    if (!state) return;
    state->kind = kind;
    state->hot = -1;
    state->drag = -1;

    if (kind == THEMED_SCROLL_SCINTILLA) {
        state->widthSeen = (int)SendMessageA(hwnd, SCI_GETSCROLLWIDTH, 0, 0);
        SendMessageA(hwnd, SCI_SETVSCROLLBAR, FALSE, 0);
        SendMessageA(hwnd, SCI_SETHSCROLLBAR, FALSE, 0);
    }
    /* Any native bar left in the style would be drawn by the system as well. */
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (style & (WS_VSCROLL | WS_HSCROLL))
        SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~(LONG_PTR)(WS_VSCROLL | WS_HSCROLL));

    if (!SetWindowSubclass(hwnd, bar_subclass_proc, SUBCLASS_ID, (DWORD_PTR)state)) {
        free(state);
        return;
    }
    sync(hwnd, state);
}

void themed_scrollbar_refresh(HWND hwnd) {
    DWORD_PTR data;
    if (!hwnd || !GetWindowSubclass(hwnd, bar_subclass_proc, SUBCLASS_ID, &data)) return;
    paint_bars(hwnd, (BarState *)data);
}
