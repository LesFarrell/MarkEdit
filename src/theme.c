/* The light and dark palettes, and the names a themes.lua file uses for
   their colours. The defaults below are the built-in GitHub palette; a
   user's themes.lua (see load_theme_colours()) overrides any of them. */
#include "theme.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One row per palette colour: its themes.lua name, where it lives in
   ThemePalette, what it paints, and its built-in light and dark values. */
typedef struct ThemeColour {
    const char *name;
    size_t offset;
    const char *role;
    COLORREF light, dark;
} ThemeColour;

#define COLOUR(field, role, light, dark) { #field, offsetof(ThemePalette, field), role, light, dark }

static const ThemeColour THEME_COLOURS[] = {
    COLOUR(bg, "window and editor background", RGB(0xFF, 0xFF, 0xFF), RGB(0x16, 0x1B, 0x22)),
    COLOUR(fg, "text", RGB(0x1F, 0x23, 0x28), RGB(0xE6, 0xED, 0xF3)),
    COLOUR(border, "borders and splitters", RGB(0xD0, 0xD7, 0xDE), RGB(0x30, 0x36, 0x3D)),
    COLOUR(accent, "the default button's outline", RGB(0x09, 0x69, 0xDA), RGB(0x38, 0x8B, 0xFD)),
    COLOUR(subtleBg, "gutters and unpressed buttons", RGB(0xF6, 0xF8, 0xFA), RGB(0x16, 0x1B, 0x22)),
    COLOUR(pressedBg, "pressed buttons", RGB(0xE1, 0xE7, 0xEC), RGB(0x30, 0x36, 0x3D)),
    COLOUR(highlightBg, "the caret line and the hot menu-bar item", RGB(0xE8, 0xEC, 0xF1), RGB(0x21, 0x26, 0x2D)),
    COLOUR(selectionBg, "selected text and list items", RGB(0xB6, 0xD7, 0xFF), RGB(0x26, 0x4F, 0x78)),
    COLOUR(selectionFg, "text inside a selection", RGB(0x1F, 0x23, 0x28), RGB(0xFF, 0xFF, 0xFF)),
    COLOUR(disabledFg, "disabled debug-panel buttons", RGB(0x8C, 0x95, 0x9F), RGB(0x7D, 0x85, 0x90)),
    COLOUR(menuDisabledFg, "disabled menu-bar items", RGB(0x65, 0x6D, 0x76), RGB(0x8B, 0x94, 0x9E)),
    COLOUR(scrollThumb, "scrollbar thumbs", RGB(0xC8, 0xCD, 0xD3), RGB(0x48, 0x4F, 0x58)),
    COLOUR(scrollThumbHot, "a scrollbar thumb under the mouse", RGB(0xA8, 0xB0, 0xB9), RGB(0x6E, 0x76, 0x81)),
    COLOUR(scrollThumbDrag, "a scrollbar thumb being dragged", RGB(0x8C, 0x95, 0x9F), RGB(0x8B, 0x94, 0x9E)),
    COLOUR(tabBg, "inactive document tabs", RGB(0xF0, 0xF3, 0xF6), RGB(0x16, 0x1B, 0x22)),
    COLOUR(tabSelectedBg, "the active document tab", RGB(0xFF, 0xFF, 0xFF), RGB(0x16, 0x1B, 0x22)),
    COLOUR(tabFg, "inactive tab labels", RGB(0x1F, 0x23, 0x28), RGB(0xB1, 0xBA, 0xC4)),
    COLOUR(tabSelectedFg, "the active tab's label", RGB(0x1F, 0x23, 0x28), RGB(0xF0, 0xF6, 0xFC)),
    COLOUR(changedMarker, "the dot on a tab whose file changed on disk", RGB(0xD2, 0x99, 0x22), RGB(0xD2, 0x99, 0x22)),
    COLOUR(minimapFrame, "the visible-region box on the minimap", RGB(0x8B, 0x94, 0x9E), RGB(0x5E, 0x80, 0x8C)),
    COLOUR(previewTextFg, "the preview's text before it has rendered", RGB(0x00, 0x00, 0x00), RGB(0xE6, 0xED, 0xF3)),
    COLOUR(lineNumber, "line numbers, and Markdown horizontal rules", RGB(0x65, 0x6D, 0x76), RGB(0x7D, 0x85, 0x90)),
    COLOUR(faint, "indent guides and visible whitespace", RGB(0xAF, 0xB8, 0xC1), RGB(0x6E, 0x76, 0x81)),
    COLOUR(braceBg, "matching braces", RGB(0xCF, 0xD8, 0xE3), RGB(0x3F, 0x46, 0x4F)),
    COLOUR(error, "unmatched braces and spelling squiggles", RGB(0xCF, 0x22, 0x2E), RGB(0xFF, 0x7B, 0x72)),
    COLOUR(bookmarkFore, "bookmark outlines", RGB(0x09, 0x69, 0xDA), RGB(0x1F, 0x6F, 0xEB)),
    COLOUR(bookmarkBack, "bookmark fills", RGB(0x21, 0x8B, 0xFF), RGB(0x38, 0x8B, 0xFD)),
    COLOUR(breakpointFore, "breakpoint outlines", RGB(0xFF, 0xFF, 0xFF), RGB(0xFF, 0xFF, 0xFF)),
    COLOUR(breakpointBack, "breakpoint fills", RGB(0xCF, 0x22, 0x2E), RGB(0xF8, 0x51, 0x49)),
    COLOUR(debugLineBack, "the current line while debugging", RGB(0xFF, 0xDF, 0x5D), RGB(0x9E, 0x6A, 0x03)),
    COLOUR(debugArrowBack, "the current-line arrow while debugging", RGB(0x9A, 0x67, 0x00), RGB(0xD2, 0x99, 0x22)),
    COLOUR(lineChanged, "the bar beside a line changed and not yet saved", RGB(0xBF, 0x87, 0x00), RGB(0xD2, 0x99, 0x22)),
    COLOUR(lineSaved, "the bar beside a line changed and saved since opening", RGB(0x1A, 0x7F, 0x37), RGB(0x3F, 0xB9, 0x50)),
    COLOUR(heading, "Markdown headings", RGB(0x09, 0x69, 0xDA), RGB(0x79, 0xC0, 0xFF)),
    COLOUR(link, "Markdown links", RGB(0x09, 0x69, 0xDA), RGB(0x58, 0xA6, 0xFF)),
    COLOUR(code, "Markdown code", RGB(0xB3, 0x53, 0x00), RGB(0xFF, 0xA6, 0x57)),
    COLOUR(quote, "Markdown block quotes and strikethrough", RGB(0x5A, 0x5A, 0x5A), RGB(0x8B, 0x94, 0x9E)),
    COLOUR(list, "Markdown list markers", RGB(0x82, 0x50, 0xDF), RGB(0xD2, 0xA8, 0xFF)),
    COLOUR(comment, "code comments", RGB(0x1F, 0x88, 0x3D), RGB(0x1F, 0x88, 0x3D)),
    COLOUR(keyword, "code keywords", RGB(0xCF, 0x22, 0x2E), RGB(0xFF, 0x7B, 0x72)),
    COLOUR(string, "code strings", RGB(0x0A, 0x30, 0x69), RGB(0xA5, 0xD6, 0xFF)),
    COLOUR(number, "code numbers", RGB(0x09, 0x69, 0xDA), RGB(0x79, 0xC0, 0xFF)),
    COLOUR(functionName, "function and class names", RGB(0x82, 0x50, 0xDF), RGB(0xD2, 0xA8, 0xFF)),
    COLOUR(variable, "variables, preprocessor lines and escapes", RGB(0x95, 0x38, 0x00), RGB(0xFF, 0xA6, 0x57)),
    COLOUR(tag, "HTML and XML tags", RGB(0x11, 0x63, 0x29), RGB(0x7E, 0xE7, 0x87)),
    COLOUR(attribute, "attributes, JSON keys and YAML keys", RGB(0x09, 0x69, 0xDA), RGB(0x79, 0xC0, 0xFF)),
    COLOUR(operatorColour, "operators", RGB(0x1F, 0x23, 0x28), RGB(0xE6, 0xED, 0xF3)),
};

#undef COLOUR

#define THEME_COLOUR_COUNT ((int)(sizeof(THEME_COLOURS) / sizeof(THEME_COLOURS[0])))

/* Every ThemePalette field is a COLORREF with a row above; this catches a
   field added to the struct without one. */
_Static_assert(sizeof(ThemePalette) == sizeof(THEME_COLOURS) / sizeof(THEME_COLOURS[0]) * sizeof(COLORREF),
               "every ThemePalette colour needs a THEME_COLOURS row");

/* [0] light, [1] dark; filled from THEME_COLOURS on first use. */
static ThemePalette g_palettes[2];
static int g_palettesReady = 0;

static COLORREF *palette_slot(ThemePalette *palette, int index) {
    return (COLORREF *)((char *)palette + THEME_COLOURS[index].offset);
}

void theme_reset_palettes(void) {
    for (int i = 0; i < THEME_COLOUR_COUNT; i++) {
        *palette_slot(&g_palettes[0], i) = THEME_COLOURS[i].light;
        *palette_slot(&g_palettes[1], i) = THEME_COLOURS[i].dark;
    }
    g_palettesReady = 1;
}

const ThemePalette *theme_palette_for(int dark) {
    if (!g_palettesReady) theme_reset_palettes();
    return &g_palettes[dark ? 1 : 0];
}

int theme_parse_colour(const char *text, COLORREF *colour) {
    if (!text || text[0] != '#' || strlen(text) != 7) return 0;
    for (int i = 1; i < 7; i++)
        if (!isxdigit((unsigned char)text[i])) return 0;
    unsigned long value = strtoul(text + 1, NULL, 16);
    *colour = RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
    return 1;
}

int theme_set_colour(int dark, const char *name, COLORREF colour) {
    if (!g_palettesReady) theme_reset_palettes();
    for (int i = 0; i < THEME_COLOUR_COUNT; i++) {
        if (strcmp(THEME_COLOURS[i].name, name) != 0) continue;
        *palette_slot(&g_palettes[dark ? 1 : 0], i) = colour;
        return 1;
    }
    return 0;
}

/* Appends to a growing heap string; returns 0 (and frees it) when out of memory. */
static int append(char **text, size_t *used, size_t *capacity, const char *piece) {
    size_t length = strlen(piece);
    if (*used + length + 1 > *capacity) {
        size_t grown = (*capacity + length + 1) * 2;
        char *bigger = (char *)realloc(*text, grown);
        if (!bigger) { free(*text); *text = NULL; return 0; }
        *text = bigger;
        *capacity = grown;
    }
    memcpy(*text + *used, piece, length + 1);
    *used += length;
    return 1;
}

char *theme_template_lua(void) {
    char *text = NULL;
    size_t used = 0, capacity = 0;
    if (!append(&text, &used, &capacity,
            "-- Tinker Editor theme colours. Each value is \"#RRGGBB\". Delete any line to\n"
            "-- keep the built-in colour; edit the rest, then use Plugins > Reload.\n"
            "-- This covers the editor and window; the preview is styled by CSS\n"
            "-- (View > Custom CSS). For a complete example that computes its colours,\n"
            "-- see plugins\\examples\\themes.lua in the Tinker Editor folder.\n"
            "return {\n")) return NULL;
    static const char *const themes[] = { "light", "dark" };
    for (int theme = 0; theme < 2; theme++) {
        char line[160];
        snprintf(line, sizeof(line), "  %s = {\n", themes[theme]);
        if (!append(&text, &used, &capacity, line)) return NULL;
        for (int i = 0; i < THEME_COLOUR_COUNT; i++) {
            COLORREF colour = theme ? THEME_COLOURS[i].dark : THEME_COLOURS[i].light;
            snprintf(line, sizeof(line), "    %-16s = \"#%02X%02X%02X\", -- %s\n",
                     THEME_COLOURS[i].name, GetRValue(colour), GetGValue(colour), GetBValue(colour),
                     THEME_COLOURS[i].role);
            if (!append(&text, &used, &capacity, line)) return NULL;
        }
        if (!append(&text, &used, &capacity, "  },\n")) return NULL;
    }
    if (!append(&text, &used, &capacity, "}\n")) return NULL;
    return text;
}
