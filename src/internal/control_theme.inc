static RECT tab_close_rectangle(const RECT *tabRect) {
    RECT close = *tabRect;
    close.left = close.right - 23;
    close.right -= 5;
    close.top += 3;
    close.bottom -= 3;
    return close;
}

static void draw_tab_item(const DRAWITEMSTRUCT *dis) {
    int index = (int)dis->itemID;
    if (index < 0 || index >= g_documentStore.count) return;
    int selected = TabCtrl_GetCurSel(g_hTabs) == index;
    COLORREF bg = g_editorDarkMode ? THEME_DARK_BG
        : (selected ? THEME_LIGHT_BG : RGB(0xF0, 0xF3, 0xF6));
    COLORREF fg = g_editorDarkMode
        ? (selected ? RGB(0xF0, 0xF6, 0xFC) : RGB(0xB1, 0xBA, 0xC4))
        : THEME_LIGHT_FG;
    fill_rect_color(dis->hDC, &dis->rcItem, bg);
    HPEN borderPen = CreatePen(PS_SOLID, 1, theme_border_color());
    HPEN oldPen = (HPEN)SelectObject(dis->hDC, borderPen);
    MoveToEx(dis->hDC, dis->rcItem.right - 1, dis->rcItem.top + 2, NULL);
    LineTo(dis->hDC, dis->rcItem.right - 1, dis->rcItem.bottom - 2);

    RECT textRect = dis->rcItem;
    textRect.left += 9;
    textRect.right -= 33;
    WCHAR label[MAX_PATH + 32];
    format_document_label(&g_documentStore.documents[index], label, sizeof(label) / sizeof(label[0]));
    HFONT font = (HFONT)SendMessageW(g_hTabs, WM_GETFONT, 0, 0);
    HFONT oldFont = font ? (HFONT)SelectObject(dis->hDC, font) : NULL;
    SetTextColor(dis->hDC, fg);
    SetBkMode(dis->hDC, TRANSPARENT);
    DrawTextW(dis->hDC, label, -1, &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (oldFont) SelectObject(dis->hDC, oldFont);

    /* tab_close_rectangle() is wider than it is tall (a roomier click
       target), so inset-padding it directly for the X stretched the cross
       into a near-flat bowtie instead of a diagonal. Draw the glyph in a
       square centered on that rect instead, sized to its shorter side, so
       the strokes stay a proper 45-degree X regardless of the hit box's
       aspect ratio. */
    RECT close = tab_close_rectangle(&dis->rcItem);
    int side = (close.right - close.left < close.bottom - close.top)
        ? (close.right - close.left) : (close.bottom - close.top);
    int cx = (close.left + close.right) / 2, cy = (close.top + close.bottom) / 2;
    int pad = 3;
    if (pad * 2 >= side) pad = side / 2 - 1;
    if (pad < 1) pad = 1;
    RECT glyph = { cx - side / 2, cy - side / 2, cx + side / 2, cy + side / 2 };
    int padTop = pad + 1, padBottom = pad;
    HPEN closePen = CreatePen(PS_SOLID, 2, fg);
    SelectObject(dis->hDC, closePen);
    MoveToEx(dis->hDC, glyph.left + pad, glyph.top + padTop, NULL);
    LineTo(dis->hDC, glyph.right - pad, glyph.bottom - padBottom);
    MoveToEx(dis->hDC, glyph.right - pad, glyph.top + padTop, NULL);
    LineTo(dis->hDC, glyph.left + pad, glyph.bottom - padBottom);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(closePen);
    DeleteObject(borderPen);

    if (g_documentStore.documents[index].externalChanged) {
        COLORREF changedColor = RGB(0xD2, 0x99, 0x22);
        HBRUSH changed = CreateSolidBrush(changedColor);
        HPEN changedPen = CreatePen(PS_SOLID, 1, changedColor);
        HBRUSH oldBrush = (HBRUSH)SelectObject(dis->hDC, changed);
        HPEN changedOldPen = (HPEN)SelectObject(dis->hDC, changedPen);
        Ellipse(dis->hDC, close.left - 9, (close.top + close.bottom) / 2 - 3,
                close.left - 3, (close.top + close.bottom) / 2 + 3);
        SelectObject(dis->hDC, changedOldPen);
        SelectObject(dis->hDC, oldBrush);
        DeleteObject(changedPen);
        DeleteObject(changed);
    }
}

/* Shared owner-draw for the app's plain, single-column LISTBOX controls (the
   outline panel and the searchable picker's result list) so both follow the
   app's light/dark theme - a plain LISTBOX always paints in system colors
   regardless of SetWindowTheme. Parameterized by HWND since both listboxes
   need identical drawing. */
static void measure_themed_listbox_item(HWND listbox, MEASUREITEMSTRUCT *mis) {
    HFONT font = (HFONT)SendMessageW(listbox, WM_GETFONT, 0, 0);
    HDC hdc = GetDC(listbox);
    HFONT oldFont = font ? (HFONT)SelectObject(hdc, font) : NULL;
    TEXTMETRICW tm;
    GetTextMetricsW(hdc, &tm);
    if (oldFont) SelectObject(hdc, oldFont);
    ReleaseDC(listbox, hdc);
    mis->itemHeight = (UINT)(tm.tmHeight + tm.tmExternalLeading + MulDiv(6, GetDpiForWindow(g_hMainWnd), 96));
}

static void draw_themed_listbox_item(HWND listbox, const DRAWITEMSTRUCT *dis) {
    if ((int)dis->itemID < 0) return;
    int selected = (dis->itemState & ODS_SELECTED) != 0;
    COLORREF bg = g_editorDarkMode
        ? (selected ? RGB(0x26, 0x4F, 0x78) : THEME_DARK_BG)
        : (selected ? RGB(0xB6, 0xD7, 0xFF) : THEME_LIGHT_BG);
    COLORREF fg = theme_fg_color();
    fill_rect_color(dis->hDC, &dis->rcItem, bg);

    /* LB_GETTEXT overflows whatever buffer it's given rather than truncating
       to it, so the buffer has to be sized from LB_GETTEXTLEN first - a
       heading line (or a long file path in the picker) can run well past
       any fixed stack buffer. */
    int textLen = (int)SendMessageW(listbox, LB_GETTEXTLEN, dis->itemID, 0);
    if (textLen <= 0) return;
    WCHAR stackLabel[256];
    WCHAR *heapLabel = NULL;
    WCHAR *label = stackLabel;
    if (textLen >= (int)(sizeof(stackLabel) / sizeof(stackLabel[0]))) {
        heapLabel = (WCHAR *)malloc(((size_t)textLen + 1) * sizeof(WCHAR));
        if (!heapLabel) return;
        label = heapLabel;
    }
    int length = (int)SendMessageW(listbox, LB_GETTEXT, dis->itemID, (LPARAM)label);
    if (length > 0) {
        RECT textRect = dis->rcItem;
        textRect.left += 4;
        SetTextColor(dis->hDC, fg);
        SetBkMode(dis->hDC, TRANSPARENT);
        DrawTextW(dis->hDC, label, length, &textRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    free(heapLabel);
}
