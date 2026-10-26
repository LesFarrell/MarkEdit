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
        CW_USEDEFAULT, CW_USEDEFAULT, 258, 138, g_hMainWnd, NULL, GetModuleHandleW(NULL), NULL);
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
            MoveWindow(GetDlgItem(hwnd, IDOK), width - 180, height - 41, 78, 26, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDCANCEL), width - 94, height - 41, 78, 26, TRUE);
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
        CW_USEDEFAULT, CW_USEDEFAULT, 620, 460, g_hMainWnd, NULL,
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
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 165, g_hMainWnd, NULL,
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
            MoveWindow(GetDlgItem(hwnd, ID_REPLACE_SELECT_ALL), 12, height - 41, 96, 26, TRUE);
            MoveWindow(GetDlgItem(hwnd, ID_REPLACE_SELECT_NONE), 114, height - 41, 96, 26, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDOK), width - 172, height - 41, 78, 26, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDCANCEL), width - 88, height - 41, 78, 26, TRUE);
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
        CW_USEDEFAULT, CW_USEDEFAULT, 620, 460, g_hMainWnd, NULL,
        GetModuleHandleW(NULL), &state);
    if (!dialog) return 0;
    run_modal_popup(dialog);
    return state.confirmed;
}

/* ---- Find / Replace (operates on the markdown source in g_hEdit) ----
   A modeless window: Find (Ctrl+F) shows the search options, Replace
   (Ctrl+H) adds the replacement row and buttons. The search itself is
   text_search.c's. "In selection" limits Count and Replace All, since
   finding a match moves the selection. */

enum {
    ID_FIND_WHAT = 10, ID_FIND_REPLACE_LABEL = 11, ID_FIND_REPLACE_WITH = 12,
    ID_FIND_MATCH_CASE = 13, ID_FIND_WHOLE_WORD = 14, ID_FIND_REGEX = 15, ID_FIND_IN_SELECTION = 16,
    ID_FIND_STATUS = 17, ID_FIND_PREVIOUS = 18, ID_FIND_COUNT = 19, ID_FIND_REPLACE = 20,
    ID_FIND_REPLACE_ALL = 21,
};

static int g_findReplaceMode = 0;

static void show_find_dialog(int replace);

static void set_find_status(const WCHAR *text) {
    if (g_hFindDlg) SetDlgItemTextW(g_hFindDlg, ID_FIND_STATUS, text);
}

/* The Find window's status line, or a beep and a message box when it's
   closed, as for F3 from the editor. */
static void report_find_result(int result) {
    const WCHAR *text = result == SEARCH_BAD_PATTERN ? L"That regular expression isn't valid."
                      : result == SEARCH_NOT_FOUND ? L"Not found."
                      : result == SEARCH_WRAPPED ? L"Found, after wrapping round the document."
                      : L"";
    if (g_hFindDlg) { set_find_status(text); return; }
    if (result == SEARCH_BAD_PATTERN) MessageBoxW(g_hMainWnd, text, L"Find", MB_OK | MB_ICONWARNING);
    else if (result == SEARCH_NOT_FOUND) MessageBeep(MB_ICONASTERISK);
}

/* Finds the next (or previous) match of the current Find text: Find Next,
   Find Previous, F3 and Shift+F3. Opens the Find window when there's no
   text to find yet. */
static void find_again(int forward) {
    if (!g_findWhat[0]) { show_find_dialog(0); return; }
    char *needle = to_utf8(g_findWhat);
    if (!needle) return;
    report_find_result(search_find(g_hEdit, needle, g_findOptions, forward));
    free(needle);
}

/* The range Count and Replace All work in: the selection with In selection
   on, or else the whole document. Returns 0 (after saying why) when In
   selection is on but nothing is selected. */
static int find_scope(Sci_Position *start, Sci_Position *end) {
    if (!g_findInSelection) {
        *start = 0;
        *end = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
        return 1;
    }
    *start = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    *end = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (*end > *start) return 1;
    set_find_status(L"Select the text to search in first, or turn off In selection.");
    return 0;
}

static void count_matches(void) {
    Sci_Position start, end;
    char *needle = to_utf8(g_findWhat);
    if (needle && find_scope(&start, &end)) {
        long count = search_count(g_hEdit, needle, g_findOptions, start, end);
        WCHAR text[96];
        if (count == SEARCH_BAD_PATTERN) wcscpy(text, L"That regular expression isn't valid.");
        else swprintf(text, 96, L"%ld match%ls%ls.", count, count == 1 ? L"" : L"es",
                      g_findInSelection ? L" in the selection" : L"");
        set_find_status(text);
    }
    free(needle);
}

static void replace_and_find(void) {
    char *needle = to_utf8(g_findWhat);
    char *replacement = to_utf8(g_replaceWith);
    if (needle && replacement) {
        int replaced = search_replace_selection(g_hEdit, needle, replacement, g_findOptions);
        if (replaced == SEARCH_BAD_PATTERN) report_find_result(SEARCH_BAD_PATTERN);
        else report_find_result(search_find(g_hEdit, needle, g_findOptions, 1));
    }
    free(needle);
    free(replacement);
}

static void replace_all_matches(void) {
    Sci_Position start, end;
    char *needle = to_utf8(g_findWhat);
    char *replacement = to_utf8(g_replaceWith);
    if (needle && replacement && find_scope(&start, &end)) {
        long count = search_replace_all(g_hEdit, needle, replacement, g_findOptions, start, &end);
        WCHAR text[96];
        if (count == SEARCH_BAD_PATTERN) {
            wcscpy(text, L"That regular expression isn't valid.");
        } else {
            swprintf(text, 96, L"Replaced %ld match%ls%ls.", count, count == 1 ? L"" : L"es",
                     g_findInSelection ? L" in the selection" : L"");
            /* The selection grows or shrinks with the text replaced in it. */
            if (g_findInSelection) SendMessageA(g_hEdit, SCI_SETSEL, (WPARAM)start, (LPARAM)end);
        }
        set_find_status(text);
    }
    free(needle);
    free(replacement);
}

static int find_option_is_on(int id) {
    switch (id) {
        case ID_FIND_MATCH_CASE: return (g_findOptions & SEARCH_MATCH_CASE) != 0;
        case ID_FIND_WHOLE_WORD: return (g_findOptions & SEARCH_WHOLE_WORD) != 0;
        case ID_FIND_REGEX: return (g_findOptions & SEARCH_REGEX) != 0;
        case ID_FIND_IN_SELECTION: return g_findInSelection;
    }
    return 0;
}

static void toggle_find_option(HWND hwnd, int id) {
    switch (id) {
        case ID_FIND_MATCH_CASE: g_findOptions ^= SEARCH_MATCH_CASE; break;
        case ID_FIND_WHOLE_WORD: g_findOptions ^= SEARCH_WHOLE_WORD; break;
        case ID_FIND_REGEX: g_findOptions ^= SEARCH_REGEX; break;
        case ID_FIND_IN_SELECTION: g_findInSelection = !g_findInSelection; break;
    }
    InvalidateRect(GetDlgItem(hwnd, id), NULL, FALSE);
    save_find_settings();
}

/* Shows or hides the Replace row and buttons, moving the options and
   buttons to suit, and sizes the window to fit. */
static void layout_find_dialog(HWND hwnd) {
    static const int buttons[] = { IDOK, ID_FIND_PREVIOUS, ID_FIND_COUNT, ID_FIND_REPLACE,
                                   ID_FIND_REPLACE_ALL, IDCANCEL };
    int replace = g_findReplaceMode;
    ShowWindow(GetDlgItem(hwnd, ID_FIND_REPLACE_LABEL), replace ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, ID_FIND_REPLACE_WITH), replace ? SW_SHOW : SW_HIDE);
    int top = replace ? 76 : 44;
    static const int options[] = { ID_FIND_MATCH_CASE, ID_FIND_WHOLE_WORD, ID_FIND_REGEX, ID_FIND_IN_SELECTION };
    for (int i = 0; i < 4; i++)
        MoveWindow(GetDlgItem(hwnd, options[i]), 12, top + i * 24, 330, 22, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_FIND_STATUS), 12, top + 100, 330, 20, TRUE);
    int y = 12;
    for (int i = 0; i < 6; i++) {
        HWND button = GetDlgItem(hwnd, buttons[i]);
        int shown = replace || (buttons[i] != ID_FIND_REPLACE && buttons[i] != ID_FIND_REPLACE_ALL);
        ShowWindow(button, shown ? SW_SHOW : SW_HIDE);
        if (!shown) continue;
        MoveWindow(button, 356, y, 104, 25, TRUE);
        y += 30;
    }
    int height = top + 132 > y + 7 ? top + 132 : y + 7;
    RECT rc = { 0, 0, 472, height };
    AdjustWindowRectEx(&rc, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE,
                       (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    SetWindowPos(hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowTextW(hwnd, replace ? L"Replace" : L"Find");
}

static HWND create_find_control(HWND parent, const WCHAR *className, const WCHAR *text, DWORD style, int id) {
    return CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                           parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static LRESULT CALLBACK FindWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            CreateWindowExW(0, L"STATIC", L"Find what:", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                12, 12, 84, 24, hwnd, NULL, GetModuleHandleW(NULL), NULL);
            HWND what = CreateWindowExW(0, L"EDIT", g_findWhat, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                100, 12, 242, 24, hwnd, (HMENU)ID_FIND_WHAT, GetModuleHandleW(NULL), NULL);
            CreateWindowExW(0, L"STATIC", L"Replace with:", WS_CHILD | SS_CENTERIMAGE,
                12, 44, 84, 24, hwnd, (HMENU)ID_FIND_REPLACE_LABEL, GetModuleHandleW(NULL), NULL);
            HWND with = CreateWindowExW(0, L"EDIT", g_replaceWith, WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
                100, 44, 242, 24, hwnd, (HMENU)ID_FIND_REPLACE_WITH, GetModuleHandleW(NULL), NULL);
            SendMessageW(what, EM_LIMITTEXT, sizeof(g_findWhat) / sizeof(g_findWhat[0]) - 1, 0);
            SendMessageW(with, EM_LIMITTEXT, sizeof(g_replaceWith) / sizeof(g_replaceWith[0]) - 1, 0);
            theme_edit_field(what);
            theme_edit_field(with);
            create_find_control(hwnd, L"BUTTON", L"Match case", WS_TABSTOP | BS_OWNERDRAW, ID_FIND_MATCH_CASE);
            create_find_control(hwnd, L"BUTTON", L"Whole word", WS_TABSTOP | BS_OWNERDRAW, ID_FIND_WHOLE_WORD);
            create_find_control(hwnd, L"BUTTON", L"Regular expression (replace with \\1 for groups)",
                                WS_TABSTOP | BS_OWNERDRAW, ID_FIND_REGEX);
            create_find_control(hwnd, L"BUTTON", L"In selection (Count and Replace All)",
                                WS_TABSTOP | BS_OWNERDRAW, ID_FIND_IN_SELECTION);
            create_find_control(hwnd, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, ID_FIND_STATUS);
            create_find_control(hwnd, L"BUTTON", L"Find Next", WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW, IDOK);
            create_find_control(hwnd, L"BUTTON", L"Find Previous", WS_TABSTOP | BS_OWNERDRAW, ID_FIND_PREVIOUS);
            create_find_control(hwnd, L"BUTTON", L"Count", WS_TABSTOP | BS_OWNERDRAW, ID_FIND_COUNT);
            create_find_control(hwnd, L"BUTTON", L"Replace", WS_TABSTOP | BS_OWNERDRAW, ID_FIND_REPLACE);
            create_find_control(hwnd, L"BUTTON", L"Replace All", WS_TABSTOP | BS_OWNERDRAW, ID_FIND_REPLACE_ALL);
            create_find_control(hwnd, L"BUTTON", L"Close", WS_TABSTOP | BS_OWNERDRAW, IDCANCEL);
            use_gui_font(hwnd);
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
            if (dis->CtlType != ODT_BUTTON) break;
            if (dis->CtlID >= ID_FIND_MATCH_CASE && dis->CtlID <= ID_FIND_IN_SELECTION)
                draw_toggle_button(dis, 0, find_option_is_on((int)dis->CtlID));
            else
                draw_popup_button(dis);
            return TRUE;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (HIWORD(wParam) == EN_CHANGE) {
                if (id == ID_FIND_WHAT) GetDlgItemTextW(hwnd, id, g_findWhat, sizeof(g_findWhat) / sizeof(g_findWhat[0]));
                if (id == ID_FIND_REPLACE_WITH)
                    GetDlgItemTextW(hwnd, id, g_replaceWith, sizeof(g_replaceWith) / sizeof(g_replaceWith[0]));
                set_find_status(L"");
                return 0;
            }
            switch (id) {
                case IDOK: find_again(1); break;
                case ID_FIND_PREVIOUS: find_again(0); break;
                case ID_FIND_COUNT: count_matches(); break;
                case ID_FIND_REPLACE: replace_and_find(); break;
                case ID_FIND_REPLACE_ALL: replace_all_matches(); break;
                case IDCANCEL: DestroyWindow(hwnd); return 0;
                case ID_FIND_MATCH_CASE: case ID_FIND_WHOLE_WORD: case ID_FIND_REGEX: case ID_FIND_IN_SELECTION:
                    toggle_find_option(hwnd, id);
                    return 0;
                default: return 0;
            }
            save_find_settings();
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            g_hFindDlg = NULL;
            save_find_settings();
            popup_dialog_theme_destroyed();
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* Lets the Find window keep the editor's F3/Shift+F3, and Ctrl+F/Ctrl+H to
   switch between Find and Replace, while one of its controls has the
   keyboard. Returns 1 when the message was one of those keys. */
static int find_dialog_shortcut(const MSG *message) {
    if (!g_hFindDlg || message->message != WM_KEYDOWN ||
        (message->hwnd != g_hFindDlg && !IsChild(g_hFindDlg, message->hwnd))) return 0;
    int control = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (message->wParam == VK_F3 && !control) {
        find_again(!shift);
        return 1;
    }
    if (control && !shift && (message->wParam == 'F' || message->wParam == 'H')) {
        show_find_dialog(message->wParam == 'H');
        return 1;
    }
    return 0;
}

/* The editor's selection as the text to find, when it's a short piece of
   one line: what the user most likely wants to search for. */
static void find_text_from_selection(void) {
    Sci_Position start = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONSTART, 0, 0);
    Sci_Position end = (Sci_Position)SendMessageA(g_hEdit, SCI_GETSELECTIONEND, 0, 0);
    if (end <= start || end - start >= (Sci_Position)(sizeof(g_findWhat) / sizeof(g_findWhat[0]))) return;
    char text[sizeof(g_findWhat) / sizeof(g_findWhat[0])];
    struct Sci_TextRangeFull range = { { start, end }, text };
    SendMessageA(g_hEdit, SCI_GETTEXTRANGEFULL, 0, (LPARAM)&range);
    if (strpbrk(text, "\r\n")) return;
    WCHAR *wide = to_wide(text);
    if (wide && wcslen(wide) < sizeof(g_findWhat) / sizeof(g_findWhat[0])) wcscpy(g_findWhat, wide);
    free(wide);
}

static void show_find_dialog(int replace) {
    g_findReplaceMode = replace;
    /* In selection searches the selection, so it isn't the text to find. */
    if (!g_findInSelection) find_text_from_selection();
    if (!g_hFindDlg) {
        if (!register_popup_class(L"MarkEditFindDialog", FindWndProc)) return;
        g_hFindDlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"MarkEditFindDialog", L"Find",
            WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, 480, 220, g_hMainWnd, NULL, GetModuleHandleW(NULL), NULL);
        if (!g_hFindDlg) return;
        layout_find_dialog(g_hFindDlg);
        /* Near the top right of the main window, clear of most of the text. */
        RECT owner, rc;
        GetWindowRect(g_hMainWnd, &owner);
        GetWindowRect(g_hFindDlg, &rc);
        SetWindowPos(g_hFindDlg, NULL, owner.right - (rc.right - rc.left) - 32, owner.top + 96, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER);
        ShowWindow(g_hFindDlg, SW_SHOW);
    } else {
        layout_find_dialog(g_hFindDlg);
    }
    HWND what = GetDlgItem(g_hFindDlg, ID_FIND_WHAT);
    SetWindowTextW(what, g_findWhat);
    set_find_status(L"");
    SetActiveWindow(g_hFindDlg);
    SetFocus(replace && g_findWhat[0] ? GetDlgItem(g_hFindDlg, ID_FIND_REPLACE_WITH) : what);
    SendMessageW(GetFocus(), EM_SETSEL, 0, -1);
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
