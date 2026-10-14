#ifndef THEME_H
#define THEME_H

#include <windows.h>
#include <uxtheme.h>

#include "settings_values.h" /* g_editorDarkMode, g_previewDarkMode */

/* The light and dark palettes shared by every natively drawn surface
   (editor, menus, tabs, status bar, popups, file explorer). The built-in
   values live in theme.c; a user's themes.lua can override any of them.
   The preview's CSS (preview_shell.html, github-markdown.css,
   preview_fallback.c) carries the built-in values too and must be kept in
   step by hand. */

/* Every colour the native UI draws with, by role, for one theme. Code picks
   a field from theme_palette() rather than choosing between two literal
   colours itself. Each field needs a row in theme.c's THEME_COLOURS, which
   gives its themes.lua name and built-in values. */
typedef struct ThemePalette {
    /* Window chrome */
    COLORREF bg, fg, border, accent;
    COLORREF subtleBg;        /* gutters, unpressed buttons, the tab strip */
    COLORREF pressedBg;       /* pressed buttons */
    COLORREF highlightBg;     /* the caret line, the hot menu-bar item */
    COLORREF selectionBg, selectionFg;
    COLORREF disabledFg;      /* disabled debug-panel buttons */
    COLORREF menuDisabledFg;  /* disabled menu-bar items */
    COLORREF scrollThumb, scrollThumbHot, scrollThumbDrag;
    COLORREF tabBg, tabSelectedBg, tabFg, tabSelectedFg;
    COLORREF changedMarker;   /* the dot on a tab whose file changed on disk */
    COLORREF minimapFrame;    /* the visible-region box on the minimap */
    COLORREF previewTextFg;   /* the preview host's text before WebView2 paints */
    /* Editor */
    COLORREF lineNumber;
    COLORREF faint;           /* indent guides, visible whitespace */
    COLORREF braceBg;
    COLORREF error;           /* unmatched braces, spelling squiggles */
    COLORREF bookmarkFore, bookmarkBack;
    COLORREF breakpointFore, breakpointBack;
    COLORREF debugLineBack, debugArrowBack;
    /* Markdown styles */
    COLORREF heading, link, code, quote, list;
    /* Code syntax styles, shared by every non-Markdown lexer */
    COLORREF comment, keyword, string, number, functionName, variable, tag, attribute, operatorColour;
} ThemePalette;

/* The palette for the light (0) or dark (non-zero) theme. */
const ThemePalette *theme_palette_for(int dark);
/* Puts every colour back to its built-in value. */
void theme_reset_palettes(void);
/* Sets one colour by its themes.lua name; 0 if there is no such colour. */
int theme_set_colour(int dark, const char *name, COLORREF colour);
/* Reads "#RRGGBB"; 0 if `text` isn't in that form. */
int theme_parse_colour(const char *text, COLORREF *colour);
/* A themes.lua listing every colour with its built-in value and what it
   paints, as a heap string (NULL when out of memory). */
char *theme_template_lua(void);

/* Editor-side chrome follows g_editorDarkMode; the preview pane host
   follows g_previewDarkMode. */
static inline const ThemePalette *theme_palette(void) { return theme_palette_for(g_editorDarkMode); }
static inline const ThemePalette *preview_palette(void) { return theme_palette_for(g_previewDarkMode); }

static inline COLORREF theme_bg_color(void) { return theme_palette()->bg; }
static inline COLORREF theme_fg_color(void) { return theme_palette()->fg; }
static inline COLORREF theme_border_color(void) { return theme_palette()->border; }
static inline COLORREF theme_accent_color(void) { return theme_palette()->accent; }
static inline COLORREF theme_scroll_thumb_color(void) { return theme_palette()->scrollThumb; }
static inline COLORREF theme_scroll_thumb_hot_color(void) { return theme_palette()->scrollThumbHot; }
static inline COLORREF theme_scroll_thumb_drag_color(void) { return theme_palette()->scrollThumbDrag; }
static inline COLORREF preview_bg_color(void) { return preview_palette()->bg; }

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
