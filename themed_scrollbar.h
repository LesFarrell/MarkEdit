#ifndef THEMED_SCROLLBAR_H
#define THEMED_SCROLLBAR_H

#include <windows.h>

/* Native scrollbars are drawn by the system with a fixed track colour that
   can't be set, so it never quite matches the app's own background. These
   replace them with bars drawn in the non-client area in the theme colours. */
typedef enum {
    THEMED_SCROLL_SCINTILLA, /* vertical + horizontal; the control's own bars are turned off */
    THEMED_SCROLL_LISTBOX    /* vertical; create the LISTBOX without WS_VSCROLL */
} ThemedScrollKind;

/* Subclasses hwnd and takes over its scrollbars. Safe to call once per window;
   cleaned up when the window is destroyed. */
void themed_scrollbar_attach(HWND hwnd, ThemedScrollKind kind);

/* Repaints the bars after the light/dark theme changes. No-op if hwnd was
   never attached. */
void themed_scrollbar_refresh(HWND hwnd);

#endif
