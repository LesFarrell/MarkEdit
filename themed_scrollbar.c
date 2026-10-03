#include "themed_scrollbar.h"

#include <windowsx.h>
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>

#include "theme.h"
#include "third_party/scintilla/include/Scintilla.h"

/* Scintilla and list-box bars live in the target window's non-client area.
   TreeView must retain its native bars to maintain both real scroll ranges,
   so sibling overlay windows cover those allocations instead; Windows can
   animate/repaint underneath them without ever becoming visible. Scroll
   state is read back after control messages because these controls do not
   consistently notify their parent when they scroll themselves. */

#define SUBCLASS_ID    0x5342
#define BAR_VERTICAL   0
#define BAR_HORIZONTAL 1
#define BAR_COUNT      2
#define OVERLAY_ID_BASE 0x5340
#define OVERLAY_CLASS   L"MarkEditThemedScrollbarOverlay"

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
    HWND target;
    HWND overlay[BAR_COUNT];
    HWND cornerOverlay;
    int clipWidth;
    int clipHeight;
    int clipVertical;
    int clipHorizontal;
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

static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void position_tree_overlays(HWND hwnd, BarState *state);

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

static void query_treeview(HWND hwnd, BarState *state, int want[BAR_COUNT]) {
    RECT client;
    GetClientRect(hwnd, &client);
    int itemHeight = (int)TreeView_GetItemHeight(hwnd);
    if (itemHeight <= 0) itemHeight = 1;

    /* TreeView_GetCount includes descendants of collapsed folders. Walking
       TVGN_NEXTVISIBLE gives the range represented by the vertical bar. */
    ScrollAxis *v = &state->axis[BAR_VERTICAL];
    HTREEITEM first = TreeView_GetFirstVisible(hwnd);
    HTREEITEM item = TreeView_GetRoot(hwnd);
    int firstIndex = 0;
    int count = 0;
    while (item) {
        if (item == first) firstIndex = count;
        count++;
        item = TreeView_GetNextVisible(hwnd, item);
    }
    v->total = count;
    v->page = (client.bottom - client.top) / itemHeight;
    if (v->page < 1) v->page = 1;
    v->pos = firstIndex;
    want[BAR_VERTICAL] = v->total > v->page;

    /* The control keeps its logical range after its system-drawn bar is
       hidden, allowing the themed horizontal bar to mirror it. */
    ScrollAxis *h = &state->axis[BAR_HORIZONTAL];
    SCROLLINFO info = {0};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    if (GetScrollInfo(hwnd, SB_HORZ, &info)) {
        h->total = info.nMax >= info.nMin ? info.nMax - info.nMin + 1 : 0;
        h->page = (int)info.nPage;
        h->pos = info.nPos - info.nMin;
    } else {
        *h = (ScrollAxis){0, 0, 0};
    }
    want[BAR_HORIZONTAL] = h->page > 0 && h->total > h->page;
}

static void query_axes(HWND hwnd, BarState *state, int want[BAR_COUNT]) {
    if (state->kind == THEMED_SCROLL_SCINTILLA) query_scintilla(hwnd, state, want);
    else if (state->kind == THEMED_SCROLL_LISTBOX) query_listbox(hwnd, state, want);
    else query_treeview(hwnd, state, want);
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
    int left = origin.x - windowRect.left;
    int top = origin.y - windowRect.top;
    int width = client.right - client.left;
    int height = client.bottom - client.top;

    if (state->kind == THEMED_SCROLL_TREEVIEW) {
        /* Paint over the TreeView's existing native scrollbar allocation.
           This keeps its real vertical and horizontal ranges while avoiding
           a second non-client strip beside the system one. */
        int windowWidth = windowRect.right - windowRect.left;
        int windowHeight = windowRect.bottom - windowRect.top;
        SetRect(&bar[BAR_VERTICAL], left + width, top, windowWidth - left, top + height);
        SetRect(&bar[BAR_HORIZONTAL], left, top + height, left + width, windowHeight - top);
        SetRect(corner, left + width, top + height, windowWidth - left, windowHeight - top);
    } else {
        int thick = bar_thickness(hwnd);
        SetRect(&bar[BAR_VERTICAL], left + width, top, left + width + thick, top + height);
        SetRect(&bar[BAR_HORIZONTAL], left, top + height, left + width, top + height + thick);
        SetRect(corner, left + width, top + height, left + width + thick, top + height + thick);
    }
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

static void paint_overlay(HWND overlay, BarState *state, int bar) {
    PAINTSTRUCT paint;
    HDC hdc = BeginPaint(overlay, &paint);
    RECT client;
    GetClientRect(overlay, &client);
    fill_rect_color(hdc, &client, theme_bg_color());
    if (bar >= 0) paint_bar(hdc, state->target, state, bar, &client);
    EndPaint(overlay, &paint);
}

static void paint_bars(HWND hwnd, const BarState *state) {
    if (!state->visible[BAR_VERTICAL] && !state->visible[BAR_HORIZONTAL]) return;
    if (state->kind == THEMED_SCROLL_TREEVIEW) {
        for (int i = 0; i < BAR_COUNT; i++) {
            if (state->overlay[i]) {
                InvalidateRect(state->overlay[i], NULL, TRUE);
                UpdateWindow(state->overlay[i]);
            }
        }
        if (state->cornerOverlay) {
            InvalidateRect(state->cornerOverlay, NULL, TRUE);
            UpdateWindow(state->cornerOverlay);
        }
        return;
    }
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
    /* A former non-client bar can become client area when the frame changes.
       Invalidate both regions so its old thumb/track cannot remain as a
       second, stale-looking scrollbar until some later full repaint. */
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
}

static int strip_native_bars(HWND hwnd) {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (!(style & (WS_VSCROLL | WS_HSCROLL))) return 0;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~(LONG_PTR)(WS_VSCROLL | WS_HSCROLL));
    return 1;
}

static int set_tree_bar_styles(HWND hwnd, const int want[BAR_COUNT]) {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR wanted = style & ~(LONG_PTR)(WS_VSCROLL | WS_HSCROLL);
    if (want[BAR_VERTICAL]) wanted |= WS_VSCROLL;
    if (want[BAR_HORIZONTAL]) wanted |= WS_HSCROLL;
    if (wanted == style) return 0;
    SetWindowLongPtrW(hwnd, GWL_STYLE, wanted);
    return 1;
}

static void set_overlay_position(HWND overlay, int show, int x, int y, int width, int height) {
    if (!overlay) return;
    if (!show || width <= 0 || height <= 0) {
        ShowWindow(overlay, SW_HIDE);
        return;
    }
    SetWindowPos(overlay, HWND_TOP, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void position_tree_overlays(HWND hwnd, BarState *state) {
    if (!state->overlay[0] || !state->overlay[1]) return;
    HWND parent = GetParent(hwnd);
    if (!parent || !IsWindowVisible(hwnd)) {
        ShowWindow(state->overlay[0], SW_HIDE);
        ShowWindow(state->overlay[1], SW_HIDE);
        if (state->cornerOverlay) ShowWindow(state->cornerOverlay, SW_HIDE);
        return;
    }

    RECT windowRect, client;
    POINT clientOrigin = {0, 0};
    GetWindowRect(hwnd, &windowRect);
    GetClientRect(hwnd, &client);
    ClientToScreen(hwnd, &clientOrigin);
    POINT windowOrigin = {windowRect.left, windowRect.top};
    ScreenToClient(parent, &windowOrigin);
    ScreenToClient(parent, &clientOrigin);

    int leftBorder = clientOrigin.x - windowOrigin.x;
    int topBorder = clientOrigin.y - windowOrigin.y;
    int innerRight = windowOrigin.x + (windowRect.right - windowRect.left) - leftBorder;
    int innerBottom = windowOrigin.y + (windowRect.bottom - windowRect.top) - topBorder;
    int clientRight = clientOrigin.x + client.right;
    int clientBottom = clientOrigin.y + client.bottom;
    int vWidth = innerRight - clientRight;
    int hHeight = innerBottom - clientBottom;

    /* A sibling cannot cover another window's non-client pixels reliably:
       DWM may composite an animated native scrollbar above it. Remove those
       pixels from the TreeView's window region so only the sibling overlays
       occupy the scrollbar strips. Scroll ranges remain fully native. */
    int windowWidth = windowRect.right - windowRect.left;
    int windowHeight = windowRect.bottom - windowRect.top;
    int clipWidth = state->visible[BAR_VERTICAL]
        ? clientRight - windowOrigin.x : windowWidth;
    int clipHeight = state->visible[BAR_HORIZONTAL]
        ? clientBottom - windowOrigin.y : windowHeight;
    if (clipWidth != state->clipWidth || clipHeight != state->clipHeight ||
        state->visible[BAR_VERTICAL] != state->clipVertical ||
        state->visible[BAR_HORIZONTAL] != state->clipHorizontal) {
        if (state->visible[BAR_VERTICAL] || state->visible[BAR_HORIZONTAL]) {
            HRGN clip = CreateRectRgn(0, 0, clipWidth, clipHeight);
            if (!SetWindowRgn(hwnd, clip, TRUE)) DeleteObject(clip);
        } else {
            SetWindowRgn(hwnd, NULL, TRUE);
        }
        state->clipWidth = clipWidth;
        state->clipHeight = clipHeight;
        state->clipVertical = state->visible[BAR_VERTICAL];
        state->clipHorizontal = state->visible[BAR_HORIZONTAL];
    }

    set_overlay_position(state->overlay[BAR_VERTICAL], state->visible[BAR_VERTICAL],
                         clientRight, clientOrigin.y, vWidth, client.bottom);
    set_overlay_position(state->overlay[BAR_HORIZONTAL], state->visible[BAR_HORIZONTAL],
                         clientOrigin.x, clientBottom, client.right, hHeight);
    set_overlay_position(state->cornerOverlay,
                         state->visible[BAR_VERTICAL] && state->visible[BAR_HORIZONTAL],
                         clientRight, clientBottom, vWidth, hHeight);
}

static void sync(HWND hwnd, BarState *state) {
    if (state->syncing) return;
    state->syncing = 1;

    if (state->kind != THEMED_SCROLL_TREEVIEW && strip_native_bars(hwnd)) reframe(hwnd);

    ScrollAxis before[BAR_COUNT] = {state->axis[0], state->axis[1]};
    int want[BAR_COUNT];
    query_axes(hwnd, state, want);

    int changed = memcmp(before, state->axis, sizeof(before)) != 0;
    int visibilityChanged = want[BAR_VERTICAL] != state->visible[BAR_VERTICAL] ||
                            want[BAR_HORIZONTAL] != state->visible[BAR_HORIZONTAL];
    if (state->kind == THEMED_SCROLL_TREEVIEW) {
        state->visible[BAR_VERTICAL] = want[BAR_VERTICAL];
        state->visible[BAR_HORIZONTAL] = want[BAR_HORIZONTAL];
        if (set_tree_bar_styles(hwnd, want)) {
            reframe(hwnd);
            query_axes(hwnd, state, want);
            state->visible[BAR_VERTICAL] = want[BAR_VERTICAL];
            state->visible[BAR_HORIZONTAL] = want[BAR_HORIZONTAL];
            changed = 1;
        }
        if (visibilityChanged) changed = 1;
    } else if (visibilityChanged) {
        state->visible[BAR_VERTICAL] = want[BAR_VERTICAL];
        state->visible[BAR_HORIZONTAL] = want[BAR_HORIZONTAL];
        reframe(hwnd); /* the strip is reserved in WM_NCCALCSIZE, so the client area resizes */
        query_axes(hwnd, state, want); /* page sizes changed with the client area */
        changed = 1;
    }
    if (state->kind == THEMED_SCROLL_TREEVIEW) position_tree_overlays(hwnd, state);
    if (changed) paint_bars(hwnd, state);
    state->syncing = 0;
}

static void scroll_to(HWND hwnd, BarState *state, int bar, int pos) {
    pos = clamp_scroll_pos(&state->axis[bar], pos);
    if (state->kind == THEMED_SCROLL_SCINTILLA) {
        if (bar == BAR_VERTICAL) SendMessageA(hwnd, SCI_SETFIRSTVISIBLELINE, (WPARAM)pos, 0);
        else SendMessageA(hwnd, SCI_SETXOFFSET, (WPARAM)pos, 0);
    } else if (state->kind == THEMED_SCROLL_LISTBOX) {
        SendMessageW(hwnd, LB_SETTOPINDEX, (WPARAM)pos, 0);
    } else if (bar == BAR_VERTICAL) {
        HTREEITEM item = TreeView_GetRoot(hwnd);
        for (int index = 0; item && index < pos; index++)
            item = TreeView_GetNextVisible(hwnd, item);
        if (item) SendMessageW(hwnd, TVM_SELECTITEM, TVGN_FIRSTVISIBLE, (LPARAM)item);
    } else {
        SendMessageW(hwnd, WM_HSCROLL, MAKEWPARAM(SB_THUMBPOSITION, pos), 0);
    }
    sync(hwnd, state);
}

static void ensure_tree_selection_visible(HWND hwnd, BarState *state) {
    HTREEITEM selected = TreeView_GetSelection(hwnd);
    HTREEITEM first = TreeView_GetFirstVisible(hwnd);
    if (!selected || !first) return;

    int selectedIndex = -1;
    int firstIndex = -1;
    int index = 0;
    for (HTREEITEM item = TreeView_GetRoot(hwnd); item;
         item = TreeView_GetNextVisible(hwnd, item), index++) {
        if (item == selected) selectedIndex = index;
        if (item == first) firstIndex = index;
    }
    if (selectedIndex < 0 || firstIndex < 0) return;
    int page = state->axis[BAR_VERTICAL].page;
    if (page < 1) page = 1;
    if (selectedIndex < firstIndex)
        scroll_to(hwnd, state, BAR_VERTICAL, selectedIndex);
    else if (selectedIndex >= firstIndex + page)
        scroll_to(hwnd, state, BAR_VERTICAL, selectedIndex - page + 1);
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

static void wheel_vertical(HWND hwnd, BarState *state, int delta) {
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

static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    BarState *state = (BarState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lParam;
        state = (BarState *)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
    }
    if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);

    int bar = GetDlgCtrlID(hwnd) - OVERLAY_ID_BASE;
    if (bar < 0 || bar >= BAR_COUNT) bar = -1; /* the corner overlay */
    switch (msg) {
        case WM_PAINT:
            paint_overlay(hwnd, state, bar);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32512)));
            return TRUE;
        case WM_MOUSEMOVE:
            if (bar >= 0) {
                RECT client;
                POINT cursor;
                GetClientRect(hwnd, &client);
                GetCursorPos(&cursor);
                ScreenToClient(hwnd, &cursor);
                RECT thumb = thumb_rect(state->target, state, bar, &client);
                set_hot(state->target, state, PtInRect(&thumb, cursor) ? bar : -1);
                TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
            }
            return 0;
        case WM_MOUSELEAVE:
            if (state->drag < 0) set_hot(state->target, state, -1);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            if (bar >= 0) begin_track(state->target, state, bar);
            return 0;
        case WM_MOUSEWHEEL:
            wheel_vertical(state->target, state, GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static int ensure_overlay_class(HINSTANCE instance) {
    WNDCLASSW existing;
    if (GetClassInfoW(instance, OVERLAY_CLASS, &existing)) return 1;
    WNDCLASSW cls = {0};
    cls.lpfnWndProc = overlay_proc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    cls.lpszClassName = OVERLAY_CLASS;
    return RegisterClassW(&cls) != 0;
}

static int create_tree_overlays(BarState *state) {
    HWND parent = GetParent(state->target);
    HINSTANCE instance = (HINSTANCE)GetWindowLongPtrW(state->target, GWLP_HINSTANCE);
    if (!parent || !ensure_overlay_class(instance)) return 0;
    for (int i = 0; i < BAR_COUNT; i++) {
        state->overlay[i] = CreateWindowExW(0, OVERLAY_CLASS, L"",
            WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
            (HMENU)(UINT_PTR)(OVERLAY_ID_BASE + i), instance, state);
        if (!state->overlay[i]) return 0;
    }
    state->cornerOverlay = CreateWindowExW(0, OVERLAY_CLASS, L"",
        WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
        (HMENU)(UINT_PTR)(OVERLAY_ID_BASE + BAR_COUNT), instance, state);
    return state->cornerOverlay != NULL;
}

static void destroy_tree_overlays(BarState *state) {
    for (int i = 0; i < BAR_COUNT; i++) {
        if (state->overlay[i]) DestroyWindow(state->overlay[i]);
        state->overlay[i] = NULL;
    }
    if (state->cornerOverlay) DestroyWindow(state->cornerOverlay);
    state->cornerOverlay = NULL;
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
            if (state->kind != THEMED_SCROLL_TREEVIEW) {
                RECT *rc = wParam ? &((NCCALCSIZE_PARAMS *)lParam)->rgrc[0] : (RECT *)lParam;
                int thick = bar_thickness(hwnd);
                if (state->visible[BAR_VERTICAL]) rc->right -= thick;
                if (state->visible[BAR_HORIZONTAL]) rc->bottom -= thick;
            }
            return result;
        }
        case WM_NCPAINT: {
            LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
            paint_bars(hwnd, state);
            return result;
        }
        case WM_NCHITTEST: {
            RECT bar[BAR_COUNT];
            POINT screen = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int hit = hit_bar(state, hwnd, screen, bar);
            if (hit == BAR_VERTICAL) return HTVSCROLL;
            if (hit == BAR_HORIZONTAL) return HTHSCROLL;
            return DefSubclassProc(hwnd, msg, wParam, lParam);
        }
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTVSCROLL || LOWORD(lParam) == HTHSCROLL) {
                SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32512)));
                return TRUE;
            }
            break;
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
            /* The native TreeView handler starts its own scrollbar hover
               painting here. Our bars occupy that same allocation, so do
               not let the system repaint over them after set_hot(). */
            return 0;
        case WM_NCMOUSELEAVE:
            state->tracking = 0;
            if (state->drag < 0) set_hot(hwnd, state, -1);
            return 0;
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
            if (state->kind != THEMED_SCROLL_SCINTILLA) {
                wheel_vertical(hwnd, state, GET_WHEEL_DELTA_WPARAM(wParam));
                return 0;
            }
            break;
        case WM_NCDESTROY: {
            destroy_tree_overlays(state);
            RemoveWindowSubclass(hwnd, bar_subclass_proc, id);
            LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
            free(state);
            return result;
        }
    }

    LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (msg == WM_SIZE)
        RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
    if (state->kind == THEMED_SCROLL_TREEVIEW && msg == WM_KEYDOWN)
        ensure_tree_selection_visible(hwnd, state);
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
    state->target = hwnd;
    state->hot = -1;
    state->drag = -1;

    if (kind == THEMED_SCROLL_SCINTILLA) {
        state->widthSeen = (int)SendMessageA(hwnd, SCI_GETSCROLLWIDTH, 0, 0);
        SendMessageA(hwnd, SCI_SETVSCROLLBAR, FALSE, 0);
        SendMessageA(hwnd, SCI_SETHSCROLLBAR, FALSE, 0);
    }
    if (!SetWindowSubclass(hwnd, bar_subclass_proc, SUBCLASS_ID, (DWORD_PTR)state)) {
        free(state);
        return;
    }
    if (kind == THEMED_SCROLL_TREEVIEW && !create_tree_overlays(state)) {
        destroy_tree_overlays(state);
        RemoveWindowSubclass(hwnd, bar_subclass_proc, SUBCLASS_ID);
        free(state);
        return;
    }
    /* sync() removes any native bar and reframes through the subclass before
       it measures and reserves space for the themed replacement. */
    sync(hwnd, state);
}

void themed_scrollbar_refresh(HWND hwnd) {
    DWORD_PTR data;
    if (!hwnd || !GetWindowSubclass(hwnd, bar_subclass_proc, SUBCLASS_ID, &data)) return;
    paint_bars(hwnd, (BarState *)data);
}
