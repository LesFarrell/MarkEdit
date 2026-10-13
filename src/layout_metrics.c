#include "layout_metrics.h"

#include "settings_values.h" /* SPLITTER_WIDTH */

/* Defined in file_explorer.c. Used only so explorer_content_x() reports zero
   before the panel's tree control has been created; the value itself carries
   no explorer behaviour. */
extern HWND g_hExplorerTree;

int g_explorerVisible = 0;
int g_explorerWidth = -1;
int g_explorerWidthRatio = 220;

int explorer_content_x(int focusModeActive)
{
    if (!g_hExplorerTree || !g_explorerVisible || focusModeActive)
        return 0;
    return (g_explorerWidth >= 0 ? g_explorerWidth : 0) + SPLITTER_WIDTH;
}
