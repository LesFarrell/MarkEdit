#ifndef THEME_H
#define THEME_H

#include <windows.h>
#include <uxtheme.h>

#include "settings_values.h" /* g_editorDarkMode, g_previewDarkMode */

/* GitHub light/dark palette shared by every natively drawn surface (editor,
   menus, tabs, status bar, popups, file explorer). The preview's CSS
   (preview_shell.html, github-markdown.css, preview_fallback.c)
   carries the same values and must be kept in step by hand. */
#define THEME_DARK_BG      RGB(0x16, 0x1B, 0x22)
#define THEME_DARK_FG      RGB(0xE6, 0xED, 0xF3)
#define THEME_DARK_BORDER  RGB(0x30, 0x36, 0x3D)
#define THEME_DARK_ACCENT  RGB(0x38, 0x8B, 0xFD)
#define THEME_LIGHT_BG     RGB(0xFF, 0xFF, 0xFF)
#define THEME_LIGHT_FG     RGB(0x1F, 0x23, 0x28)
#define THEME_LIGHT_BORDER RGB(0xD0, 0xD7, 0xDE)
#define THEME_LIGHT_ACCENT RGB(0x09, 0x69, 0xDA)

#define THEME_DARK_SCROLL_THUMB        RGB(0x48, 0x4F, 0x58)
#define THEME_DARK_SCROLL_THUMB_HOT    RGB(0x6E, 0x76, 0x81)
#define THEME_DARK_SCROLL_THUMB_DRAG   RGB(0x8B, 0x94, 0x9E)
#define THEME_LIGHT_SCROLL_THUMB       RGB(0xC8, 0xCD, 0xD3)
#define THEME_LIGHT_SCROLL_THUMB_HOT   RGB(0xA8, 0xB0, 0xB9)
#define THEME_LIGHT_SCROLL_THUMB_DRAG  RGB(0x8C, 0x95, 0x9F)

/* Editor-side chrome follows g_editorDarkMode; the preview pane host
   follows g_previewDarkMode. */
static inline COLORREF theme_bg_color(void) { return g_editorDarkMode ? THEME_DARK_BG : THEME_LIGHT_BG; }
static inline COLORREF theme_fg_color(void) { return g_editorDarkMode ? THEME_DARK_FG : THEME_LIGHT_FG; }
static inline COLORREF theme_border_color(void) { return g_editorDarkMode ? THEME_DARK_BORDER : THEME_LIGHT_BORDER; }
static inline COLORREF theme_accent_color(void) { return g_editorDarkMode ? THEME_DARK_ACCENT : THEME_LIGHT_ACCENT; }
static inline COLORREF theme_scroll_thumb_color(void) { return g_editorDarkMode ? THEME_DARK_SCROLL_THUMB : THEME_LIGHT_SCROLL_THUMB; }
static inline COLORREF theme_scroll_thumb_hot_color(void) { return g_editorDarkMode ? THEME_DARK_SCROLL_THUMB_HOT : THEME_LIGHT_SCROLL_THUMB_HOT; }
static inline COLORREF theme_scroll_thumb_drag_color(void) { return g_editorDarkMode ? THEME_DARK_SCROLL_THUMB_DRAG : THEME_LIGHT_SCROLL_THUMB_DRAG; }
static inline COLORREF preview_bg_color(void) { return g_previewDarkMode ? THEME_DARK_BG : THEME_LIGHT_BG; }

/* Opts a control's system-drawn parts (scrollbars, tree/list chrome) into
   the explorer theme matching g_editorDarkMode. */
static inline void apply_explorer_theme(HWND hwnd) {
    SetWindowTheme(hwnd, g_editorDarkMode ? L"DarkMode_Explorer" : L"Explorer", NULL);
}

/* Solid fill without creating a GDI brush: the stock DC brush takes its
   colour from the DC, so paint paths allocate nothing. */
static inline void fill_rect_color(HDC hdc, const RECT *rc, COLORREF color) {
    COLORREF previous = SetDCBrushColor(hdc, color);
    FillRect(hdc, rc, (HBRUSH)GetStockObject(DC_BRUSH));
    SetDCBrushColor(hdc, previous);
}

#endif
