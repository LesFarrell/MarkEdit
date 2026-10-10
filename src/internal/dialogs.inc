/* ---- small modal dialogs: Go to Line, choices, and text input ---- */
static HWND g_hGotoEdit = NULL;

static LRESULT CALLBACK GotoLineWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    switch (msg) {
        case WM_CREATE: {
            CreateWindowExA(0, "STATIC", "Line number:", WS_CHILD | WS_VISIBLE,
                12, 14, 90, 20, hwnd, NULL, GetModuleHandleA(NULL), NULL);
            g_hGotoEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                104, 11, 105, 24, hwnd, (HMENU)10, GetModuleHandleA(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Go to",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
                48, 50, 76, 25, hwnd, (HMENU)IDOK, GetModuleHandleA(NULL), NULL);
            CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                132, 50, 76, 25, hwnd, (HMENU)IDCANCEL, GetModuleHandleA(NULL), NULL);
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
    return DefWindowProcA(hwnd, msg, wParam, lParam);
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
    static int classRegistered = 0;
    if (!classRegistered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = GotoLineWndProc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = "MarkEditGotoLine";
        if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        classRegistered = 1;
    }
    HWND dialog = CreateWindowExA(WS_EX_DLGMODALFRAME, "MarkEditGotoLine", "Go to Line",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 238, 118, g_hMainWnd, NULL, GetModuleHandleA(NULL), NULL);
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
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            12, 32, 556, 25, hwnd, (HMENU)10, GetModuleHandleW(NULL), NULL);
        state->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
            12, 64, 556, 286, hwnd, (HMENU)11, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", state->okLabel,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
            404, 360, 78, 26, hwnd, (HMENU)IDOK, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            490, 360, 78, 26, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(NULL), NULL);
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
            if (LOWORD(wParam) == 10 && HIWORD(wParam) == EN_CHANGE) {
                refill_choice_list(state);
                return 0;
            }
            if (LOWORD(wParam) == IDOK ||
                (LOWORD(wParam) == 11 && HIWORD(wParam) == LBN_DBLCLK)) {
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
    static int registered = 0;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = ChoiceWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MarkEditChoiceDialog";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return -1;
        registered = 1;
    }
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
        state->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", state->initial,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            12, 38, 396, 25, hwnd, (HMENU)10, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
            244, 76, 78, 26, hwnd, (HMENU)IDOK, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            330, 76, 78, 26, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(NULL), NULL);
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
    static int registered = 0;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = TextInputWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MarkEditTextInput";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 0;
        registered = 1;
    }
    TextInputState state = { prompt, initial ? initial : L"", output, outputCapacity, 0, NULL };
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MarkEditTextInput", title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 440, 145, g_hMainWnd, NULL,
        GetModuleHandleW(NULL), &state);
    if (!dialog) return 0;
    run_modal_popup(dialog);
    return state.accepted;
}

/* One line matched by a workspace-wide Find/Replace search - see
   do_replace_in_workspace() below. `label` is the "path:line — snippet"
   text shown for the match (without the checkbox prefix ReplaceReviewWndProc
   adds); `accepted` is whether the review dialog's checkbox is ticked. */
typedef struct ReplaceMatch {
    WCHAR *path;
    WCHAR *label;
    int line; /* 1-based */
    int accepted;
} ReplaceMatch;

typedef struct ReplaceReviewState {
    ReplaceMatch *matches;
    int count;
    HWND list;
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

/* "[x] label" or "[ ] label" for a row of the replace review list. Caller frees. */
static WCHAR *format_replace_row(const ReplaceMatch *match) {
    size_t capacity = wcslen(match->label) + 8;
    WCHAR *row = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (row) swprintf(row, capacity, L"[%lc] %ls", (wint_t)(match->accepted ? L'x' : L' '), match->label);
    return row;
}

static void refill_replace_list(ReplaceReviewState *state) {
    SendMessageW(state->list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < state->count; i++) {
        WCHAR *row = format_replace_row(&state->matches[i]);
        if (!row) continue;
        int idx = (int)SendMessageW(state->list, LB_ADDSTRING, 0, (LPARAM)row);
        if (idx >= 0) SendMessageW(state->list, LB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);
        free(row);
    }
    if (SendMessageW(state->list, LB_GETCOUNT, 0, 0) > 0)
        SendMessageW(state->list, LB_SETCURSEL, 0, 0);
    update_replace_count_label(state);
}

/* Flips the checked state of the match shown at listbox row `row` and
   rewrites just that row's text in place, preserving its selection. */
static void toggle_replace_row(ReplaceReviewState *state, int row) {
    if (row == LB_ERR) return;
    int matchIndex = (int)SendMessageW(state->list, LB_GETITEMDATA, (WPARAM)row, 0);
    if (matchIndex < 0 || matchIndex >= state->count) return;
    state->matches[matchIndex].accepted = !state->matches[matchIndex].accepted;
    WCHAR *text = format_replace_row(&state->matches[matchIndex]);
    if (text) {
        SendMessageW(state->list, LB_DELETESTRING, (WPARAM)row, 0);
        SendMessageW(state->list, LB_INSERTSTRING, (WPARAM)row, (LPARAM)text);
        SendMessageW(state->list, LB_SETITEMDATA, (WPARAM)row, (LPARAM)matchIndex);
        SendMessageW(state->list, LB_SETCURSEL, (WPARAM)row, 0);
        free(text);
    }
    update_replace_count_label(state);
}

/* Space toggles the selected row's checkbox, the same convention as a
   Windows Explorer checkbox listbox - listboxes don't offer this by
   themselves, so it's added via the same safe-subclassing API already used
   for g_hTabs/g_hEdit/g_hMinimap elsewhere in this file. */
static LRESULT CALLBACK ReplaceListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                 UINT_PTR subclassId, DWORD_PTR refData) {
    (void)subclassId;
    if (msg == WM_KEYDOWN && wParam == VK_SPACE) {
        toggle_replace_row((ReplaceReviewState *)refData, (int)SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ReplaceListSubclassProc, subclassId);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
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
        state->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
            12, 34, 556, 316, hwnd, (HMENU)11, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Select All", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            12, 360, 96, 26, hwnd, (HMENU)12, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Select None", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            114, 360, 96, 26, hwnd, (HMENU)13, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Replace",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
            412, 360, 78, 26, hwnd, (HMENU)IDOK, GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            496, 360, 78, 26, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(NULL), NULL);
        themed_scrollbar_attach(state->list, THEMED_SCROLL_LISTBOX);
        SetWindowSubclass(state->list, ReplaceListSubclassProc, 1, (DWORD_PTR)state);
        refill_replace_list(state);
        SetFocus(state->list);
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
            if (LOWORD(wParam) == 11 && HIWORD(wParam) == LBN_DBLCLK) {
                toggle_replace_row(state, (int)SendMessageW(state->list, LB_GETCURSEL, 0, 0));
                return 0;
            }
            if (LOWORD(wParam) == 12) { /* Select All */
                for (int i = 0; i < state->count; i++) state->matches[i].accepted = 1;
                refill_replace_list(state);
                return 0;
            }
            if (LOWORD(wParam) == 13) { /* Select None */
                for (int i = 0; i < state->count; i++) state->matches[i].accepted = 0;
                refill_replace_list(state);
                return 0;
            }
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
            MoveWindow(state->list, 12, 34, width - 24, height - 78, TRUE);
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
    static int registered = 0;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = ReplaceReviewWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MarkEditReplaceDialog";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 0;
        registered = 1;
    }
    ReplaceReviewState state = { matches, count, NULL, NULL, 0 };
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
    DWORD cap = 32768;
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
        char *css = read_utf8_text_file(path, 4 * 1024 * 1024);
        if (!css) {
            MessageBoxW(g_hMainWnd,
                L"The stylesheet must be a readable UTF-8 CSS file smaller than 4 MB.",
                L"MarkEdit", MB_OK | MB_ICONERROR);
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
