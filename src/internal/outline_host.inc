/* ---- outline/minimap host wiring --------------------------------------
   The outline pane and minimap live in outline_minimap.c and reach the
   shell's window handles, view state, and a few static helpers through this
   table (see outline_minimap.h). It points at the globals by address, so the
   module always observes their current values once the windows exist. */
static int outline_large_file_active(void) {
    return g_largeFile;
}

static void init_outline_minimap(void) {
    static OutlineMinimapHost host;
    host.edit = &g_windows.edit;
    host.outline = &g_windows.outline;
    host.minimap = &g_windows.minimap;
    host.splitEdit = &g_windows.splitEdit;
    host.lastEditView = &g_windows.lastEditView;
    host.outlineCount = &g_outlineCount;
    host.outlineLines = g_outlineLines;
    host.activeLanguage = &g_activeLanguage;
    host.minimapDragging = &g_minimapDragging;
    host.minimapViewTop = &g_minimapViewTop;
    host.minimapViewHeight = &g_minimapViewHeight;
    host.splitEditorPos = &g_splitEditorPos;
    host.largeFile = outline_large_file_active;
    host.trackedEditView = tracked_edit_view;
    host.syncSplitDocument = sync_split_editor_document;
    host.relayoutAndSave = relayout_and_save;
    host.layout = layout;
    outline_minimap_init(&host);
}
