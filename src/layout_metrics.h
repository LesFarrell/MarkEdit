#ifndef MARKEDIT_LAYOUT_METRICS_H
#define MARKEDIT_LAYOUT_METRICS_H

#include <windows.h>

/* Geometry of the file-explorer panel, kept as plain values with no window
   handles or UI entry points so the persistence code can read and write them
   without depending on the explorer's translation unit. The panel's own
   module (file_explorer.c) still owns everything else about the tree. */

/* Persisted flag: whether the explorer panel is shown. */
extern int g_explorerVisible;
/* Live pixel width of the panel; the -1 sentinel means "derive from the ratio". */
extern int g_explorerWidth;
/* Persisted permille of the client width, like g_splitterRatio. */
extern int g_explorerWidthRatio;

/* The pixel x-offset everything right of the explorer panel is shifted by -
   the panel's width plus its splitter, or 0 when the panel isn't currently
   occupying space (hidden, suppressed by focus mode, or not created yet).
   layout() derives this inline; other call sites that need the same value
   once it has settled (the main splitter's hit-testing/drag math, and its
   persisted ratio in settings.c) call this instead of re-deriving it, so
   they cannot drift out of sync with where the panel actually is. */
int explorer_content_x(int focusModeActive);

#endif
