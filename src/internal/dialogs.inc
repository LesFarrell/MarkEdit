/* One shared background brush for whichever of these popup dialogs
   (show_goto_line_dialog()/show_choice_dialog()/show_text_input()/the
   Find & Replace review list/the Settings dialog) are currently open. Most
   of them run their own modal message loop and never nest, but the Settings
   dialog's Compiler category and its spell-check language picker (Choose...)
   can open one of these ON TOP of another already-open one (Settings itself
   opening the Choice dialog) - g_popupThemeDepth counts how many are
   currently open so the brush is only actually torn down once the last one
   closes, rather than being pulled out from under a still-open outer dialog
   the moment a nested one exits. popup_dialog_theme_created() itself stays a
   plain "make sure the themed brush exists right now" call with no bearing
   on that count, since refresh_settings_dialog_theme() also calls it to
   recolor an already-open dialog's brush without opening a new one. */
static HBRUSH g_popupBkBrush = NULL;
static int g_popupThemeDepth = 0;

static void popup_dialog_theme_created(HWND hwnd) {
    apply_popup_titlebar_theme(hwnd, g_editorDarkMode);
    if (g_popupBkBrush) DeleteObject(g_popupBkBrush);
    g_popupBkBrush = CreateSolidBrush(theme_bg_color());
}

/* Call once per popup window actually opened (WM_CREATE), paired with
   popup_dialog_theme_destroyed() at that same window's WM_NCDESTROY. */
static void popup_dialog_theme_opened(HWND hwnd) {
    g_popupThemeDepth++;
    popup_dialog_theme_created(hwnd);
}

static void popup_dialog_theme_destroyed(void) {
    if (g_popupThemeDepth > 0) g_popupThemeDepth--;
    if (g_popupThemeDepth == 0 && g_popupBkBrush) {
        DeleteObject(g_popupBkBrush);
        g_popupBkBrush = NULL;
    }
}

/* Shared WM_ERASEBKGND/WM_CTLCOLORSTATIC/WM_CTLCOLORBTN/WM_CTLCOLOREDIT/
   WM_CTLCOLORLISTBOX handling for the popup dialogs below - factored out since they all need
   identical theming for their STATIC labels, EDIT fields, and checkbox/radio
   buttons. Returns the brush/TRUE the caller should return from its WndProc,
   or 0 if msg wasn't handled. */
static LRESULT popup_dialog_theme_message(HWND hwnd, UINT msg, WPARAM wParam, LRESULT *handled) {
    *handled = 1;
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, g_popupBkBrush ? g_popupBkBrush : (HBRUSH)(COLOR_BTNFACE + 1));
        return 1;
    }
    /* Checkboxes send WM_CTLCOLORSTATIC for their label; radio buttons send
       WM_CTLCOLORBTN instead - both need the same themed text colour, or the
       radio labels stay stuck at the system default (near-black) in dark mode. */
    if (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN) {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, theme_fg_color());
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)(g_popupBkBrush ? g_popupBkBrush : (HBRUSH)(COLOR_BTNFACE + 1));
    }
    /* Owner-drawn listboxes still ask for WM_CTLCOLORLISTBOX to paint
       whatever their items don't cover - the blank area below the last row,
       or the whole box when there are only a few rows - so without it that
       space stays the default white regardless of theme. */
    if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, theme_fg_color());
        SetBkColor(hdc, theme_bg_color());
        return (LRESULT)(g_popupBkBrush ? g_popupBkBrush : (HBRUSH)(COLOR_BTNFACE + 1));
    }
    *handled = 0;
    return 0;
}

/* Owner-drawn OK/Cancel-style push button matching the popup dialogs'
   theme - standard BUTTON controls are visual-styles-drawn regardless of
   WM_CTLCOLORBTN, so getting a themed fill/border needs BS_OWNERDRAW. */
static void draw_popup_button(const DRAWITEMSTRUCT *dis) {
    int pressed = (dis->itemState & ODS_SELECTED) != 0;
    /* The style can't say which button is the default: BS_OWNERDRAW (0xB)
       contains BS_DEFPUSHBUTTON's bit, so every button would match. Every
       popup's default is its IDOK button, or IDCANCEL when it has no OK. */
    int id = GetDlgCtrlID(dis->hwndItem);
    int isDefault = id == IDOK || (id == IDCANCEL && !GetDlgItem(GetParent(dis->hwndItem), IDOK));
    COLORREF fill = pressed ? theme_palette()->pressedBg : theme_palette()->subtleBg;
    fill_rect_color(dis->hDC, &dis->rcItem, fill);

    HPEN pen = CreatePen(PS_SOLID, isDefault ? 2 : 1, isDefault ? theme_accent_color() : theme_border_color());
    HPEN oldPen = (HPEN)SelectObject(dis->hDC, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
    Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
    SelectObject(dis->hDC, oldBrush);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(pen);

    WCHAR text[128];
    GetWindowTextW(dis->hwndItem, text, 128);
    SetTextColor(dis->hDC, theme_fg_color());
    SetBkMode(dis->hDC, TRANSPARENT);
    RECT textRect = dis->rcItem;
    DrawTextW(dis->hDC, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* The popups use the GUI font, as the debug panel does, rather than the
   bold System font a control gets when nobody sets one. */
static BOOL CALLBACK set_child_font(HWND child, LPARAM font) {
    SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

static void use_gui_font(HWND dialog) {
    EnumChildWindows(dialog, set_child_font, (LPARAM)GetStockObject(DEFAULT_GUI_FONT));
}

/* An LBS_OWNERDRAWFIXED list measures its rows once, as it's created, so
   after a font change its row height has to be set again. */
static void remeasure_listbox_rows(HWND list, UINT minimum) {
    MEASUREITEMSTRUCT mis = {0};
    measure_themed_listbox_item(list, &mis);
    SendMessageW(list, LB_SETITEMHEIGHT, 0, mis.itemHeight > minimum ? mis.itemHeight : minimum);
}

/* ---- themed frames for edit fields and lists ----
   WS_EX_CLIENTEDGE draws a sunken 3D frame in system colours whatever the
   theme. theme_edit_field() and theme_list_frame() replace it with a 1px
   border, the accent colour while the control has focus. Edit fields also
   get inner padding that centres the text vertically in whatever height the
   field is given; their background comes from the parent's WM_CTLCOLOREDIT
   as before. A list's frame is installed before themed_scrollbar_attach(),
   so the bar, which reserves its strip after the frame, sits inside it. */
enum { THEMED_FRAME_EDIT = 1, THEMED_FRAME_LIST = 2 };
static int themed_edit_text_height(HWND edit) {
    HFONT font = (HFONT)SendMessageW(edit, WM_GETFONT, 0, 0);
    HDC hdc = GetDC(edit);
    HFONT oldFont = font ? (HFONT)SelectObject(hdc, font) : NULL;
    TEXTMETRICW tm;
    GetTextMetricsW(hdc, &tm);
    if (oldFont) SelectObject(hdc, oldFont);
    ReleaseDC(edit, hdc);
    return tm.tmHeight;
}

/* Paints the 1px border, and for an edit field the padding inside it too.
   A list's other non-client area is its scrollbar, painted by its owner. */
static void paint_themed_frame(HWND control, int kind) {
    RECT window, client;
    GetWindowRect(control, &window);
    GetClientRect(control, &client);
    MapWindowPoints(control, NULL, (POINT *)&client, 2);
    OffsetRect(&client, -window.left, -window.top);
    OffsetRect(&window, -window.left, -window.top);
    HDC hdc = GetWindowDC(control);
    if (kind == THEMED_FRAME_EDIT) {
        ExcludeClipRect(hdc, client.left, client.top, client.right, client.bottom);
        fill_rect_color(hdc, &window, theme_bg_color());
    }
    COLORREF border = GetFocus() == control ? theme_accent_color() : theme_border_color();
    HBRUSH brush = CreateSolidBrush(border);
    FrameRect(hdc, &window, brush);
    DeleteObject(brush);
    ReleaseDC(control, hdc);
}

static LRESULT CALLBACK ThemedFrameSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                UINT_PTR subclassId, DWORD_PTR refData) {
    int kind = (int)refData;
    switch (msg) {
        case WM_NCCALCSIZE: {
            if (kind == THEMED_FRAME_LIST) {
                LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
                RECT *rc = wParam ? &((NCCALCSIZE_PARAMS *)lParam)->rgrc[0] : (RECT *)lParam;
                InflateRect(rc, -1, -1);
                return result;
            }
            if (!wParam) break;
            RECT *rc = &((NCCALCSIZE_PARAMS *)lParam)->rgrc[0];
            int padX = MulDiv(6, GetDpiForWindow(hwnd), 96);
            int padY = (rc->bottom - rc->top - themed_edit_text_height(hwnd)) / 2;
            if (padY < 1) padY = 1;
            rc->left += padX;
            rc->right -= padX;
            rc->top += padY;
            rc->bottom -= padY;
            if (rc->right < rc->left) rc->right = rc->left;
            if (rc->bottom < rc->top) rc->bottom = rc->top;
            return 0;
        }
        case WM_NCPAINT:
            if (kind == THEMED_FRAME_LIST) DefSubclassProc(hwnd, msg, wParam, lParam);
            paint_themed_frame(hwnd, kind);
            return 0;
        case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE: {
            LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
            RedrawWindow(hwnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
            return result;
        }
        /* The padding depends on the font, so recompute the frame. */
        case WM_SETFONT: {
            LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
            SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            return result;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, ThemedFrameSubclassProc, subclassId);
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void attach_themed_frame(HWND control, int kind) {
    if (!control) return;
    SetWindowSubclass(control, ThemedFrameSubclassProc, 1, (DWORD_PTR)kind);
    SetWindowPos(control, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

/* Call on an EDIT created without WS_EX_CLIENTEDGE. */
static void theme_edit_field(HWND edit) {
    attach_themed_frame(edit, THEMED_FRAME_EDIT);
}

/* Call on a LISTBOX created without WS_EX_CLIENTEDGE, before any
   themed_scrollbar_attach(). */
static void theme_list_frame(HWND list) {
    attach_themed_frame(list, THEMED_FRAME_LIST);
}


/* ---- small modal dialogs: Go to Line, choices, and text input ---- */

/* Control IDs for the popup dialogs below. IDs only need to be unique within
   one dialog, so the numbers overlap between dialogs. */
enum {
    ID_GOTO_LINE = 10,
    ID_CHOICE_FILTER = 10, ID_CHOICE_LIST = 11,
    ID_TEXT_INPUT = 10,
    ID_REPLACE_LIST = 11, ID_REPLACE_SELECT_ALL = 12, ID_REPLACE_SELECT_NONE = 13,
};

static HWND g_hGotoEdit = NULL;

static LRESULT CALLBACK GotoLineWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (msg) {
        case WM_CREATE: {
            CreateWindowExA(0, "STATIC", "Line number:", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                12, 11, 90, 24, hwnd, NULL, GetModuleHandleA(NULL), NULL);
            g_hGotoEdit = CreateWindowExA(0, "EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                104, 11, 105, 24, hwnd, (HMENU)ID_GOTO_LINE, GetModuleHandleA(NULL), NULL);
            theme_edit_field(g_hGotoEdit);
            CreateWindowExA(0, "BUTTON", "Go to",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
                48, 50, 76, 25, hwnd, (HMENU)IDOK, GetModuleHandleA(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                132, 50, 76, 25, hwnd, (HMENU)IDCANCEL, GetModuleHandleA(NULL), NULL);
            use_gui_font(hwnd);
            char current[32];
            Sci_Position line = (Sci_Position)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION,
                SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0), 0) + 1;
            snprintf(current, sizeof(current), "%lld", (long long)line);
            SetWindowTextA(g_hGotoEdit, current);
            SendMessageA(g_hGotoEdit, EM_SETSEL, 0, -1);
            SetFocus(g_hGotoEdit);
            popup_dialog_theme_opened(hwnd);
            return 0;
        }
        case WM_ERASEBKGND: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: {
            LRESULT handled = 0;
            LRESULT result = popup_dialog_theme_message(hwnd, msg, wParam, &handled);
            if (handled) return result;
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_BUTTON) { draw_popup_button(dis); return TRUE; }
            break;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                char value[32];
                GetWindowTextA(g_hGotoEdit, value, sizeof(value));
                long line = strtol(value, NULL, 10);
                Sci_Position lineCount = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
                if (line < 1 || line > lineCount) {
                    MessageBeep(MB_ICONWARNING);
                    SendMessageA(g_hGotoEdit, EM_SETSEL, 0, -1);
                    return 0;
                }
                SendMessageA(g_hEdit, SCI_GOTOLINE, (WPARAM)(line - 1), 0);
                SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            popup_dialog_theme_destroyed();
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* Registers the window class for one of the popup dialogs. Safe to call
   every time the dialog opens: a class that already exists counts as
   registered. Returns 0 only if the class can't be registered at all. */
static int register_popup_class(const WCHAR *className, WNDPROC windowProc) {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); /* IDC_ARROW */
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = className;
    return RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

/* Shows a popup dialog centred over the main window and runs its modal
   loop until it's destroyed, with the main window disabled meanwhile. A
   WM_QUIT that arrives during the loop is posted again for the main loop. */
static void run_modal_popup(HWND dialog) {
    RECT owner, rc;
    GetWindowRect(g_hMainWnd, &owner);
    GetWindowRect(dialog, &rc);
    SetWindowPos(dialog, HWND_TOP,
        owner.left + ((owner.right - owner.left) - (rc.right - rc.left)) / 2,
        owner.top + ((owner.bottom - owner.top) - (rc.bottom - rc.top)) / 2,
        0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    EnableWindow(g_hMainWnd, FALSE);
    MSG message = {0};
    int loopResult = -1;
    while (IsWindow(dialog) && (loopResult = GetMessageW(&message, NULL, 0, 0)) > 0) {
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(g_hMainWnd, TRUE);
    SetForegroundWindow(g_hMainWnd);
    if (loopResult == 0) PostQuitMessage((int)message.wParam);
}

static void show_goto_line_dialog(void) {
    if (!register_popup_class(L"MarkEditGotoLine", GotoLineWndProc)) return;
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MarkEditGotoLine", L"Go to Line",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 238, 118, g_hMainWnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!dialog) return;
    run_modal_popup(dialog);
    SetFocus(g_hEdit);
}

/* Reusable searchable picker used by Quick Open, folder search, the command
   palette and spelling-language selection. Item data keeps the
   original index while the visible list is filtered. */
typedef struct ChoiceDialogState {
    WCHAR **labels;
    int count;
    int result;
    const WCHAR *prompt;
    const WCHAR *okLabel;
    HWND edit;
    HWND list;
} ChoiceDialogState;

static void refill_choice_list(ChoiceDialogState *state) {
    WCHAR filter[256];
    GetWindowTextW(state->edit, filter, sizeof(filter) / sizeof(filter[0]));
    SendMessageW(state->list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < state->count; i++) {
        if (filter[0] && !StrStrIW(state->labels[i], filter)) continue;
        int row = (int)SendMessageW(state->list, LB_ADDSTRING, 0, (LPARAM)state->labels[i]);
        if (row >= 0) SendMessageW(state->list, LB_SETITEMDATA, (WPARAM)row, (LPARAM)i);
    }
    if (SendMessageW(state->list, LB_GETCOUNT, 0, 0) > 0)
        SendMessageW(state->list, LB_SETCURSEL, 0, 0);
}

static LRESULT CALLBACK ChoiceWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ChoiceDialogState *state = (ChoiceDialogState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        state = (ChoiceDialogState *)((CREATESTRUCTW *)lParam)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
        CreateWindowExW(0, L"STATIC", state->prompt, WS_CHILD | WS_VISIBLE,
            12, 10, 556, 20, hwnd, NULL, GetModuleHandleW(NULL), NULL);
        state->edit = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            12, 32, 556, 25, hwnd, (HMENU)ID_CHOICE_FILTER, GetModuleHandleW(NULL), NULL);
        theme_edit_field(state->edit);
        state->list = CreateWindowExW(0, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
            12, 64, 556, 286, hwnd, (HMENU)ID_CHOICE_LIST, GetModuleHandleW(NULL), NULL);
        theme_list_frame(state->list);
        CreateWindowExW(0, L"BUTTON", state->okLabel,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
            404, 360, 78, 26, hwnd, (HMENU)IDOK, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            490, 360, 78, 26, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(NULL), NULL);
        use_gui_font(hwnd);
        remeasure_listbox_rows(state->list, 0);
        themed_scrollbar_attach(state->list, THEMED_SCROLL_LISTBOX);
        refill_choice_list(state);
        SetFocus(state->edit);
        popup_dialog_theme_opened(hwnd);
        return 0;
    }
    if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
        case WM_ERASEBKGND: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
            LRESULT handled = 0;
            LRESULT result = popup_dialog_theme_message(hwnd, msg, wParam, &handled);
            if (handled) return result;
            break;
        }
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lParam;
            if (mis->CtlType == ODT_LISTBOX) { measure_themed_listbox_item(state->list, mis); return TRUE; }
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_BUTTON) { draw_popup_button(dis); return TRUE; }
            if (dis->CtlType == ODT_LISTBOX && dis->hwndItem == state->list) {
                draw_themed_listbox_item(state->list, dis);
                return TRUE;
            }
            break;
        }
        case WM_NCDESTROY:
            popup_dialog_theme_destroyed();
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_CHOICE_FILTER && HIWORD(wParam) == EN_CHANGE) {
                refill_choice_list(state);
                return 0;
            }
            if (LOWORD(wParam) == IDOK ||
                (LOWORD(wParam) == ID_CHOICE_LIST && HIWORD(wParam) == LBN_DBLCLK)) {
                int row = (int)SendMessageW(state->list, LB_GETCURSEL, 0, 0);
                if (row != LB_ERR)
                    state->result = (int)SendMessageW(state->list, LB_GETITEMDATA, (WPARAM)row, 0);
                if (state->result >= 0) DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
            break;
        case WM_SIZE: {
            int width = LOWORD(lParam), height = HIWORD(lParam);
            MoveWindow(state->edit, 12, 32, width - 24, 25, TRUE);
            MoveWindow(state->list, 12, 64, width - 24, height - 108, TRUE);
            return 0;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static int show_choice_dialog(const WCHAR *title, const WCHAR *prompt,
                              WCHAR **labels, int count, const WCHAR *okLabel) {
    if (!labels || count <= 0) return -1;
    if (!register_popup_class(L"MarkEditChoiceDialog", ChoiceWndProc)) return -1;
    ChoiceDialogState state = { labels, count, -1, prompt, okLabel, NULL, NULL };
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MarkEditChoiceDialog", title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 440, g_hMainWnd, NULL,
        GetModuleHandleW(NULL), &state);
    if (!dialog) return -1;
    run_modal_popup(dialog);
    return state.result;
}

typedef struct TextInputState {
    const WCHAR *prompt;
    const WCHAR *initial;
    WCHAR *output;
    int outputCapacity;
    int accepted;
    HWND edit;
} TextInputState;

static LRESULT CALLBACK TextInputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TextInputState *state = (TextInputState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        state = (TextInputState *)((CREATESTRUCTW *)lParam)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
        CreateWindowExW(0, L"STATIC", state->prompt, WS_CHILD | WS_VISIBLE,
            12, 14, 396, 20, hwnd, NULL, GetModuleHandleW(NULL), NULL);
        state->edit = CreateWindowExW(0, L"EDIT", state->initial,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            12, 38, 396, 25, hwnd, (HMENU)ID_TEXT_INPUT, GetModuleHandleW(NULL), NULL);
        theme_edit_field(state->edit);
        CreateWindowExW(0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
            244, 76, 78, 26, hwnd, (HMENU)IDOK, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            330, 76, 78, 26, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(NULL), NULL);
        use_gui_font(hwnd);
        SendMessageW(state->edit, EM_SETSEL, 0, -1);
        SetFocus(state->edit);
        popup_dialog_theme_opened(hwnd);
        return 0;
    }
    if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);
    if (msg == WM_ERASEBKGND || msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT) {
        LRESULT handled = 0;
        LRESULT result = popup_dialog_theme_message(hwnd, msg, wParam, &handled);
        if (handled) return result;
    }
    if (msg == WM_DRAWITEM) {
        const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
        if (dis->CtlType == ODT_BUTTON) { draw_popup_button(dis); return TRUE; }
    }
    if (msg == WM_NCDESTROY) popup_dialog_theme_destroyed();
    if (msg == WM_COMMAND && LOWORD(wParam) == IDOK) {
        GetWindowTextW(state->edit, state->output, state->outputCapacity);
        state->accepted = 1;
        DestroyWindow(hwnd);
        return 0;
    }
    if ((msg == WM_COMMAND && LOWORD(wParam) == IDCANCEL) || msg == WM_CLOSE) {
        DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static int show_text_input(const WCHAR *title, const WCHAR *prompt,
                           const WCHAR *initial, WCHAR *output, int outputCapacity) {
    if (!register_popup_class(L"MarkEditTextInput", TextInputWndProc)) return 0;
    TextInputState state = { prompt, initial ? initial : L"", output, outputCapacity, 0, NULL };
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MarkEditTextInput", title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 440, 145, g_hMainWnd, NULL,
        GetModuleHandleW(NULL), &state);
    if (!dialog) return 0;
    run_modal_popup(dialog);
    return state.accepted;
}

/* ---- themed check boxes and radio buttons ----
   Drawn by hand because the system's are white boxes in system colours on
   either theme. Shared by checklists and the Settings dialog. */

/* A square box: accent-filled with a tick when checked, outlined otherwise,
   greyed when disabled. `window` gives the DPI for the tick's width. */
static void draw_check_box(HDC hdc, HWND window, RECT box, int checked, int enabled) {
    COLORREF accent = enabled ? theme_accent_color() : theme_palette()->disabledFg;
    COLORREF outline = enabled ? theme_fg_color() : theme_palette()->disabledFg;
    HBRUSH fill = CreateSolidBrush(checked ? accent : theme_bg_color());
    HPEN border = CreatePen(PS_SOLID, 1, checked ? accent : outline);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, fill);
    HPEN oldPen = (HPEN)SelectObject(hdc, border);
    Rectangle(hdc, box.left, box.top, box.right, box.bottom);
    if (checked) {
        int size = box.right - box.left;
        HPEN tick = CreatePen(PS_SOLID, MulDiv(2, GetDpiForWindow(window), 96), theme_bg_color());
        SelectObject(hdc, tick);
        POINT points[3] = {
            { box.left + size * 3 / 14, box.top + size * 7 / 14 },
            { box.left + size * 6 / 14, box.top + size * 10 / 14 },
            { box.left + size * 11 / 14, box.top + size * 4 / 14 },
        };
        Polyline(hdc, points, 3);
        SelectObject(hdc, border);
        DeleteObject(tick);
    }
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(border);
    DeleteObject(fill);
}

/* A round radio button in `box`: an outlined ring, with an accent dot in
   an accent ring when selected. */
static void draw_radio_mark(HDC hdc, RECT box, int selected, int enabled) {
    COLORREF accent = enabled ? theme_accent_color() : theme_palette()->disabledFg;
    COLORREF outline = enabled ? theme_fg_color() : theme_palette()->disabledFg;
    HBRUSH fill = CreateSolidBrush(theme_bg_color());
    HPEN ring = CreatePen(PS_SOLID, 1, selected ? accent : outline);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, fill);
    HPEN oldPen = (HPEN)SelectObject(hdc, ring);
    Ellipse(hdc, box.left, box.top, box.right, box.bottom);
    if (selected) {
        int inset = (box.right - box.left) / 4;
        HBRUSH dot = CreateSolidBrush(accent);
        HPEN dotPen = CreatePen(PS_SOLID, 1, accent);
        SelectObject(hdc, dot);
        SelectObject(hdc, dotPen);
        Ellipse(hdc, box.left + inset, box.top + inset, box.right - inset, box.bottom - inset);
        SelectObject(hdc, ring);
        SelectObject(hdc, fill);
        DeleteObject(dotPen);
        DeleteObject(dot);
    }
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(ring);
    DeleteObject(fill);
}

/* An owner-drawn BUTTON acting as a check box or radio button: the mark
   at the left, vertically centred, then the button's text. The caller
   says whether it's on - an owner-drawn button keeps no check state. */
static void draw_toggle_button(const DRAWITEMSTRUCT *dis, int radio, int on) {
    int enabled = !(dis->itemState & ODS_DISABLED);
    fill_rect_color(dis->hDC, &dis->rcItem, theme_bg_color());
    int size = MulDiv(14, GetDpiForWindow(dis->hwndItem), 96);
    RECT box;
    box.left = dis->rcItem.left + 1;
    box.top = dis->rcItem.top + (dis->rcItem.bottom - dis->rcItem.top - size) / 2;
    box.right = box.left + size;
    box.bottom = box.top + size;
    if (radio) draw_radio_mark(dis->hDC, box, on, enabled);
    else draw_check_box(dis->hDC, dis->hwndItem, box, on, enabled);

    WCHAR text[128];
    GetWindowTextW(dis->hwndItem, text, 128);
    RECT textRect = dis->rcItem;
    textRect.left = box.right + MulDiv(8, GetDpiForWindow(dis->hwndItem), 96);
    SetTextColor(dis->hDC, enabled ? theme_fg_color() : theme_palette()->disabledFg);
    SetBkMode(dis->hDC, TRANSPARENT);
    RECT measured = textRect;
    DrawTextW(dis->hDC, text, -1, &measured, DT_LEFT | DT_SINGLELINE | DT_CALCRECT);
    DrawTextW(dis->hDC, text, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (dis->itemState & ODS_FOCUS) {
        RECT focus = { textRect.left - 2, textRect.top + 1,
                       textRect.left + (measured.right - measured.left) + 2, textRect.bottom - 1 };
        DrawFocusRect(dis->hDC, &focus);
    }
}

/* ---- checklists: an owner-drawn LISTBOX whose rows carry a checkbox ----
   Shared by the Replace in Folder review and Manage Plugins. Each row's item
   data is the caller's own index, and the caller keeps the checked state:
   it passes that state to draw_checklist_item() and flips it in `toggle`.
   Clicking a row's box, double-clicking the row, or pressing Space toggles
   it, the convention of Windows' own checkbox lists. */
typedef struct Checklist {
    HWND list;
    void (*toggle)(void *context, int index);
    void *context;
} Checklist;

static int checklist_scaled(HWND list, int pixels) {
    return MulDiv(pixels, GetDpiForWindow(list), 96);
}

/* The box occupies the row's left edge; clicks left of its right edge
   count as clicks on the box. */
static RECT checklist_box_rect(HWND list, const RECT *row) {
    int size = checklist_scaled(list, 14), margin = checklist_scaled(list, 6);
    RECT box;
    box.left = row->left + margin;
    box.top = row->top + (row->bottom - row->top - size) / 2;
    box.right = box.left + size;
    box.bottom = box.top + size;
    return box;
}

static void checklist_add(Checklist *checklist, const WCHAR *label, int index) {
    int row = (int)SendMessageW(checklist->list, LB_ADDSTRING, 0, (LPARAM)label);
    if (row >= 0) SendMessageW(checklist->list, LB_SETITEMDATA, (WPARAM)row, (LPARAM)index);
}

/* Draws one row: the box (accent-filled with a tick when checked), the
   row's text, and an optional dimmed `detail` right-aligned after it. */
static void draw_checklist_item(Checklist *checklist, const DRAWITEMSTRUCT *dis, int checked,
                                const WCHAR *detail) {
    if ((int)dis->itemID < 0) return;
    fill_themed_listbox_row(dis);

    RECT box = checklist_box_rect(checklist->list, &dis->rcItem);
    draw_check_box(dis->hDC, checklist->list, box, checked, 1);

    RECT textRect = dis->rcItem;
    textRect.left = box.right + checklist_scaled(checklist->list, 8);
    textRect.right -= checklist_scaled(checklist->list, 8);
    if (detail && *detail) {
        RECT detailRect = textRect;
        SetTextColor(dis->hDC, theme_palette()->lineNumber);
        SetBkMode(dis->hDC, TRANSPARENT);
        DrawTextW(dis->hDC, detail, -1, &detailRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SIZE extent;
        if (GetTextExtentPoint32W(dis->hDC, detail, (int)wcslen(detail), &extent))
            textRect.right -= extent.cx + checklist_scaled(checklist->list, 12);
    }
    WCHAR *label = listbox_item_text(checklist->list, (int)dis->itemID);
    if (label) draw_themed_listbox_text(dis, label, textRect);
    free(label);
}

static void checklist_toggle_row(Checklist *checklist, int row) {
    if (row < 0 || row >= (int)SendMessageW(checklist->list, LB_GETCOUNT, 0, 0)) return;
    int index = (int)SendMessageW(checklist->list, LB_GETITEMDATA, (WPARAM)row, 0);
    checklist->toggle(checklist->context, index);
    RECT rowRect;
    if (SendMessageW(checklist->list, LB_GETITEMRECT, (WPARAM)row, (LPARAM)&rowRect) != LB_ERR)
        InvalidateRect(checklist->list, &rowRect, FALSE);
}

/* The row under a client-area point, or -1 when the point is below the
   last row. */
static int checklist_row_at(HWND list, LPARAM point) {
    POINT pt = { GET_X_LPARAM(point), GET_Y_LPARAM(point) };
    DWORD hit = (DWORD)SendMessageW(list, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
    return HIWORD(hit) ? -1 : (int)LOWORD(hit);
}

static LRESULT CALLBACK ChecklistSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                              UINT_PTR subclassId, DWORD_PTR refData) {
    Checklist *checklist = (Checklist *)refData;
    switch (msg) {
        case WM_KEYDOWN:
            if (wParam == VK_SPACE) {
                checklist_toggle_row(checklist, (int)SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
                return 0;
            }
            break;
        /* Otherwise the listbox treats Space as a type-ahead search. */
        case WM_CHAR:
            if (wParam == L' ') return 0;
            break;
        case WM_LBUTTONDOWN: {
            LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
            int row = checklist_row_at(hwnd, lParam);
            RECT rowRect;
            if (row >= 0 && SendMessageW(hwnd, LB_GETITEMRECT, (WPARAM)row, (LPARAM)&rowRect) != LB_ERR) {
                RECT box = checklist_box_rect(hwnd, &rowRect);
                if (GET_X_LPARAM(lParam) < box.right + checklist_scaled(hwnd, 4))
                    checklist_toggle_row(checklist, row);
            }
            return result;
        }
        /* The second click of a double-click on the box arrives as this
           message, so toggling here keeps every click on the box a toggle. */
        case WM_LBUTTONDBLCLK: {
            LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);
            checklist_toggle_row(checklist, checklist_row_at(hwnd, lParam));
            return result;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, ChecklistSubclassProc, subclassId);
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

/* Creates the checklist's LISTBOX as control `id` of `parent`, in the GUI
   font, with rows tall enough for the box. */
static HWND create_checklist(Checklist *checklist, HWND parent, int id) {
    checklist->list = CreateWindowExW(0, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
        LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    if (!checklist->list) return NULL;
    theme_list_frame(checklist->list);
    SendMessageW(checklist->list, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), FALSE);
    remeasure_listbox_rows(checklist->list, (UINT)checklist_scaled(checklist->list, 24));
    themed_scrollbar_attach(checklist->list, THEMED_SCROLL_LISTBOX);
    SetWindowSubclass(checklist->list, ChecklistSubclassProc, 1, (DWORD_PTR)checklist);
    return checklist->list;
}

/* One line matched by a workspace-wide Find/Replace search - see
   do_replace_in_workspace() below. `label` is the "path:line — snippet"
   text shown for the match; `accepted` is whether the review dialog's
   checkbox is ticked. */
typedef struct ReplaceMatch {
    WCHAR *path;
    WCHAR *label;
    int line; /* 1-based */
    int accepted;
} ReplaceMatch;

typedef struct ReplaceReviewState {
    ReplaceMatch *matches;
    int count;
    Checklist checklist;
    HWND countLabel;
    int confirmed;
} ReplaceReviewState;

static void update_replace_count_label(ReplaceReviewState *state) {
    if (!state->countLabel) return;
    int checked = 0;
    for (int i = 0; i < state->count; i++) if (state->matches[i].accepted) checked++;
    WCHAR text[64];
    swprintf(text, 64, L"%d of %d matches selected", checked, state->count);
    SetWindowTextW(state->countLabel, text);
}

static void toggle_replace_match(void *context, int index) {
    ReplaceReviewState *state = (ReplaceReviewState *)context;
    if (index < 0 || index >= state->count) return;
    state->matches[index].accepted = !state->matches[index].accepted;
    update_replace_count_label(state);
}

static void set_all_replace_matches(ReplaceReviewState *state, int accepted) {
    for (int i = 0; i < state->count; i++) state->matches[i].accepted = accepted;
    InvalidateRect(state->checklist.list, NULL, FALSE);
    update_replace_count_label(state);
}

static LRESULT CALLBACK ReplaceReviewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ReplaceReviewState *state = (ReplaceReviewState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        state = (ReplaceReviewState *)((CREATESTRUCTW *)lParam)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
        CreateWindowExW(0, L"STATIC", L"Uncheck any matches you don't want replaced:",
            WS_CHILD | WS_VISIBLE, 12, 10, 440, 20, hwnd, NULL, GetModuleHandleW(NULL), NULL);
        state->countLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
            456, 10, 112, 20, hwnd, NULL, GetModuleHandleW(NULL), NULL);
        state->checklist.toggle = toggle_replace_match;
        state->checklist.context = state;
        create_checklist(&state->checklist, hwnd, ID_REPLACE_LIST);
        MoveWindow(state->checklist.list, 12, 34, 556, 316, FALSE);
        CreateWindowExW(0, L"BUTTON", L"Select All", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            12, 360, 96, 26, hwnd, (HMENU)ID_REPLACE_SELECT_ALL, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Select None", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            114, 360, 96, 26, hwnd, (HMENU)ID_REPLACE_SELECT_NONE, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Replace",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
            412, 360, 78, 26, hwnd, (HMENU)IDOK, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            496, 360, 78, 26, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(NULL), NULL);
        use_gui_font(hwnd);
        for (int i = 0; i < state->count; i++) checklist_add(&state->checklist, state->matches[i].label, i);
        SendMessageW(state->checklist.list, LB_SETCURSEL, 0, 0);
        update_replace_count_label(state);
        SetFocus(state->checklist.list);
        popup_dialog_theme_opened(hwnd);
        return 0;
    }
    if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
        case WM_ERASEBKGND: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
            LRESULT handled = 0;
            LRESULT result = popup_dialog_theme_message(hwnd, msg, wParam, &handled);
            if (handled) return result;
            break;
        }
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lParam;
            if (mis->CtlType == ODT_LISTBOX) { measure_themed_listbox_item(state->checklist.list, mis); return TRUE; }
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_BUTTON) { draw_popup_button(dis); return TRUE; }
            if (dis->CtlType == ODT_LISTBOX && dis->hwndItem == state->checklist.list) {
                int index = (int)dis->itemData;
                draw_checklist_item(&state->checklist, dis,
                                    index >= 0 && index < state->count && state->matches[index].accepted, NULL);
                return TRUE;
            }
            break;
        }
        case WM_NCDESTROY:
            popup_dialog_theme_destroyed();
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_REPLACE_SELECT_ALL) { set_all_replace_matches(state, 1); return 0; }
            if (LOWORD(wParam) == ID_REPLACE_SELECT_NONE) { set_all_replace_matches(state, 0); return 0; }
            if (LOWORD(wParam) == IDOK) {
                state->confirmed = 1;
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
            break;
        case WM_SIZE: {
            int width = LOWORD(lParam), height = HIWORD(lParam);
            MoveWindow(state->countLabel, width - 124, 10, 112, 20, TRUE);
            MoveWindow(state->checklist.list, 12, 34, width - 24, height - 78, TRUE);
            return 0;
        }
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* Shows the checkbox review list for a workspace-wide replace; `matches`
   is edited in place (accepted flags) and the function returns 1 if the
   user clicked Replace, 0 if they cancelled. */
static int show_replace_review_dialog(ReplaceMatch *matches, int count) {
    if (!register_popup_class(L"MarkEditReplaceDialog", ReplaceReviewWndProc)) return 0;
    ReplaceReviewState state = { matches, count, {0}, NULL, 0 };
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MarkEditReplaceDialog", L"Replace in Folder",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 440, g_hMainWnd, NULL,
        GetModuleHandleW(NULL), &state);
    if (!dialog) return 0;
    run_modal_popup(dialog);
    return state.confirmed;
}

/* ---- Find / Replace (operates on the markdown source in g_hEdit) ---- */

static void find_in_edit(const char *needle, int matchCase, int down) {
    if (!needle || !needle[0]) return;
    Sci_Position len = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    Sci_Position selStart = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    int needleLen = (int)strlen(needle);
    Sci_Position foundAt = -1;
    SendMessageA(g_hEdit, SCI_SETSEARCHFLAGS, matchCase ? SCFIND_MATCHCASE : 0, 0);

    if (down) {
        SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)selEnd, 0);
        SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)len, 0);
        foundAt = (Sci_Position)SendMessageA(g_hEdit, SCI_SEARCHINTARGET, (WPARAM)needleLen, (LPARAM)needle);
        if (foundAt < 0) {
            SendMessageA(g_hEdit, SCI_SETTARGETSTART, 0, 0);
            SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)selStart, 0);
            foundAt = (Sci_Position)SendMessageA(g_hEdit, SCI_SEARCHINTARGET, (WPARAM)needleLen, (LPARAM)needle);
        }
    } else {
        SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)selStart, 0);
        SendMessageA(g_hEdit, SCI_SETTARGETEND, 0, 0);
        foundAt = (Sci_Position)SendMessageA(g_hEdit, SCI_SEARCHINTARGET, (WPARAM)needleLen, (LPARAM)needle);
        if (foundAt < 0) {
            SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)len, 0);
            SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)selEnd, 0);
            foundAt = (Sci_Position)SendMessageA(g_hEdit, SCI_SEARCHINTARGET, (WPARAM)needleLen, (LPARAM)needle);
        }
    }

    if (foundAt >= 0) {
        Sci_Position foundEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETTARGETEND, 0, 0);
        SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)foundAt, (LPARAM)foundEnd);
        SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
        SetFocus(g_hEdit);
    } else {
        MessageBeep(MB_ICONASTERISK);
    }
}

static void replace_selection_and_find(const char *findWhat, const char *replaceWith, int matchCase) {
    Sci_Position selStart = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position selEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (selEnd > selStart) {
        int findLen = (int)strlen(findWhat);
        SendMessageA(g_hEdit, SCI_SETSEARCHFLAGS, matchCase ? SCFIND_MATCHCASE : 0, 0);
        SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)selStart, 0);
        SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)selEnd, 0);
        Sci_Position found = (Sci_Position)SendMessageA(g_hEdit, SCI_SEARCHINTARGET, (WPARAM)findLen, (LPARAM)findWhat);
        Sci_Position foundEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETTARGETEND, 0, 0);
        if (found == selStart && foundEnd == selEnd) {
            SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)replaceWith);
        }
    }
    find_in_edit(findWhat, matchCase, 1);
}

static void show_find_dialog(int replace) {
    if (g_hFindDlg) {
        SetFocus(g_hFindDlg);
        return;
    }
    ZeroMemory(&g_fr, sizeof(g_fr));
    g_fr.lStructSize = sizeof(FINDREPLACEW);
    g_fr.hwndOwner = g_hMainWnd;
    g_fr.lpstrFindWhat = g_findWhat;
    g_fr.wFindWhatLen = sizeof(g_findWhat) / sizeof(g_findWhat[0]);
    g_fr.lpstrReplaceWith = g_replaceWith;
    g_fr.wReplaceWithLen = sizeof(g_replaceWith) / sizeof(g_replaceWith[0]);
    g_fr.Flags = g_findFlags;

    if (replace) {
        g_hFindDlg = ReplaceTextW(&g_fr);
    } else {
        g_hFindDlg = FindTextW(&g_fr);
    }
}

static void handle_findmsg(FINDREPLACEW *fr) {
    /* Every message carries the current checkbox/radio state, including the
       one sent when the dialog closes, so capture it before acting. */
    g_findFlags = fr->Flags & (FR_MATCHCASE | FR_DOWN);
    save_find_settings();

    if (fr->Flags & FR_DIALOGTERM) {
        g_hFindDlg = NULL;
        return;
    }
    char *findUtf8 = to_utf8(fr->lpstrFindWhat);
    char *replaceUtf8 = to_utf8(fr->lpstrReplaceWith);
    if (!findUtf8 || !replaceUtf8) {
        free(findUtf8);
        free(replaceUtf8);
        return;
    }
    int matchCase = (fr->Flags & FR_MATCHCASE) ? 1 : 0;
    int down = (fr->Flags & FR_DOWN) ? 1 : 0;
    if (fr->Flags & FR_FINDNEXT) {
        find_in_edit(findUtf8, matchCase, down);
    } else if (fr->Flags & FR_REPLACE) {
        replace_selection_and_find(findUtf8, replaceUtf8, matchCase);
    } else if (fr->Flags & FR_REPLACEALL) {
        size_t count = 0;
        int findLen = (int)strlen(findUtf8);
        int replaceLen = (int)strlen(replaceUtf8);
        Sci_Position position = 0;
        Sci_Position documentLength = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
        SendMessageA(g_hEdit, SCI_SETSEARCHFLAGS, matchCase ? SCFIND_MATCHCASE : 0, 0);
        SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
        while (findLen > 0 && position <= documentLength) {
            SendMessageA(g_hEdit, SCI_SETTARGETSTART, (WPARAM)position, 0);
            SendMessageA(g_hEdit, SCI_SETTARGETEND, (WPARAM)documentLength, 0);
            Sci_Position found = (Sci_Position)SendMessageA(g_hEdit, SCI_SEARCHINTARGET,
                                           (WPARAM)findLen, (LPARAM)findUtf8);
            if (found < 0) break;
            Sci_Position oldEnd = (Sci_Position)SendMessageA(g_hEdit, SCI_GETTARGETEND, 0, 0);
            SendMessageA(g_hEdit, SCI_REPLACETARGET, (WPARAM)replaceLen, (LPARAM)replaceUtf8);
            documentLength += replaceLen - (oldEnd - found);
            position = found + replaceLen;
            count++;
        }
        SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
        WCHAR msg[64];
        swprintf(msg, sizeof(msg) / sizeof(msg[0]), L"Replaced %zu occurrence(s).", count);
        MessageBoxW(g_hMainWnd, msg, L"MarkEdit", MB_OK | MB_ICONINFORMATION);
    }
    free(findUtf8);
    free(replaceUtf8);
}

/* ChooseFontW hands the chosen font back through LOGFONTW.lfFaceName, a
   fixed WCHAR[LF_FACESIZE] (32) field - a hard limit of that struct, not of
   anything downstream. A face name at or past that length (e.g. "Google
   Sans Code Medium Monospace", 33 characters) comes back silently truncated
   to 31 characters plus NUL, which then doesn't match any installed font:
   Scintilla falls back to a default and the setting "resets" itself on
   every restart. Reading the dialog's own font-name combo (control ID
   cmb1, from <dlgs.h> - the font dialog's standard layout is cmb1 name/
   cmb2 style/cmb3 size, with cmb4 for the color combo that only exists
   when CF_EFFECTS is set, which this dialog doesn't use) via this hook
   captures the real, untruncated name before ChooseFontW's return path
   discards the extra characters. */
static WCHAR g_choseFontFaceFull[MAX_FONT_FACE_NAME];
static int g_choseFontFaceFullValid = 0;

static UINT_PTR CALLBACK choose_font_hook(HWND hdlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    if (msg == WM_INITDIALOG) {
        /* CF_INITTOLOGFONTSTRUCT seeded the dialog from lf.lfFaceName, a
           WCHAR[32] LOGFONTW field truncated at do_choose_font()'s call
           site - so a saved name at or past that length shows up here
           mangled, looking like the previous choice was forgotten even
           though g_fontFaceName itself is intact. The default dialog
           procedure has already populated the combo by the time the hook
           sees WM_INITDIALOG, so overwrite it with the untruncated name. */
        SetDlgItemTextW(hdlg, cmb1, g_fontFaceName);
    } else if (msg == WM_COMMAND && LOWORD(wParam) == IDOK) {
        int len = GetDlgItemTextW(hdlg, cmb1, g_choseFontFaceFull,
                                   MAX_FONT_FACE_NAME);
        g_choseFontFaceFullValid = len > 0;
    }
    return 0; /* let the dialog's own procedure handle every message as usual */
}

static void do_choose_font(void) {
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    wcsncpy(lf.lfFaceName, g_fontFaceName, LF_FACESIZE - 1);
    lf.lfWeight = g_fontWeight;
    lf.lfItalic = (BYTE)g_fontItalic;
    HDC hdc = GetDC(g_hEdit);
    lf.lfHeight = -MulDiv(g_fontPointSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(g_hEdit, hdc);

    CHOOSEFONTW cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = g_hMainWnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOSCRIPTSEL | CF_ENABLEHOOK;
    cf.lpfnHook = choose_font_hook;

    g_choseFontFaceFullValid = 0;
    if (ChooseFontW(&cf)) {
        if (g_choseFontFaceFullValid) {
            wcsncpy(g_fontFaceName, g_choseFontFaceFull, MAX_FONT_FACE_NAME - 1);
        } else {
            wcsncpy(g_fontFaceName, lf.lfFaceName, LF_FACESIZE - 1);
        }
        g_fontFaceName[MAX_FONT_FACE_NAME - 1] = 0;
        g_fontPointSize = cf.iPointSize / 10;
        if (g_fontPointSize < 1) g_fontPointSize = 1;
        g_fontWeight = lf.lfWeight ? lf.lfWeight : FW_NORMAL;
        g_fontItalic = lf.lfItalic ? 1 : 0;
        apply_editor_font();
        save_font_settings();
    }
}

static void do_choose_custom_css(void) {
    DWORD cap = LONG_PATH_CAPACITY;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    if (g_customCssPath) wcsncpy(path, g_customCssPath, cap - 1);
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"CSS Stylesheets (*.css)\0*.css\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrDefExt = L"css";
    if (GetOpenFileNameW(&ofn)) {
        char *css = read_utf8_text_file(path, CUSTOM_CSS_MAX_BYTES);
        if (!css) {
            show_error(L"The stylesheet must be a readable UTF-8 CSS file smaller than 4 MB.");
        } else {
            free(css);
            WCHAR *full = absolute_path(path);
            replace_wstring(&g_customCssPath, full ? full : path);
            free(full);
            save_theme_settings();
            apply_custom_preview_css(1);
            refresh_menu(g_hMainWnd);
        }
    }
    free(path);
}

static void clear_custom_css(void) {
    free(g_customCssPath);
    g_customCssPath = NULL;
    save_theme_settings();
    apply_custom_preview_css(0);
    refresh_menu(g_hMainWnd);
}
