/* ---- autosave recovery -------------------------------------------------
   Recovery snapshots live in LocalAppData rather than beside the executable,
   so running the portable distribution never dirties its own folder. Only
   modified documents are written; normal saves and a clean exit remove them. */

static WCHAR *get_recovery_root(void) {
    return get_local_app_directory(L"Recovery");
}

/* This instance's session folder, opened on first use; NULL on failure. */
static const WCHAR *get_recovery_directory(void) {
    if (!g_recoverySession.directory) {
        WCHAR *root = get_recovery_root();
        if (root) recovery_session_open(root, &g_recoverySession);
        free(root);
    }
    return g_recoverySession.directory;
}

/* A clean exit: nothing of this instance's is left for the next launch. */
static void close_recovery_session(void) {
    recovery_session_close(&g_recoverySession, 1);
}

static char *read_recovery_editor_text(void *context, int index, size_t *outLength) {
    (void)context;
    Sci_Position length = 0;
    char *text = get_document_text(index, &length);
    *outLength = (size_t)length;
    return text;
}

static void write_recovery_snapshot(void) {
    if (!g_hEdit || g_commandLinePreviewMode || !g_recoverySnapshotStale) return;
    RecoveryDocument documents[RECOVERY_MAX_DOCUMENTS];
    int count = 0;
    for (int i = 0; i < g_documentStore.count; i++) {
        Document *doc = &g_documentStore.documents[i];
        if (!doc->dirty || doc->largeFile) continue;
        documents[count++] = (RecoveryDocument){ doc->path, doc->untitledNumber,
            doc->hasBom, (TextEncoding)doc->encoding, i };
    }
    /* An instance that has never written a snapshot has none to clear, so it
       creates no session folder until a document is first modified. */
    const WCHAR *directory = count || g_recoverySession.directory ? get_recovery_directory() : NULL;
    if (!count && !directory) g_recoverySnapshotStale = 0;
    else if (directory && recovery_write(directory, documents, count, read_recovery_editor_text, NULL))
        g_recoverySnapshotStale = 0;
    update_status_bar();
}

/* Opens one session's documents in tabs. Returns 1 when every one of them
   was restored, so the session can be discarded. */
static int restore_recovery_entries(const WCHAR *directory, int *restored) {
    int count = recovery_count(directory), complete = count >= 0;
    for (int i = 0; i < count; i++) {
        RecoveryEntry entry;
        if (!recovery_read(directory, i, &entry)) { complete = 0; continue; }
        char *text = entry.text;
        int length = entry.length;
        int reuse = *restored == 0 && active_document_is_reusable();
        if (!reuse && !create_document_tab()) { recovery_entry_free(&entry); return 0; }
        g_documentStore.switching = 1;
        set_edit_text(g_hEdit, text, length);
        reset_undo_history(g_hEdit);
        g_documentStore.switching = 0;
        replace_wstring(&g_currentFile, entry.path[0] ? entry.path : NULL);
        g_documentStore.documents[g_documentStore.active].untitledNumber = entry.untitledNumber;
        g_hasBom = entry.hasBom;
        g_encoding = entry.encoding;
        recovery_entry_free(&entry);
        apply_document_language(g_currentFile);
        /* Matches finish_file_load(): a recovered tab keeps the breakpoints that
           were persisted for the file it was snapshotted from. */
        restore_persisted_breakpoints(g_currentFile);
        set_dirty(1);
        (*restored)++;
    }
    return complete;
}

/* Offers the snapshots of instances that did not exit cleanly. Sessions of
   instances still running are locked, so they are never offered or cleared. */
static int restore_recovery_snapshot(void) {
    WCHAR *root = get_recovery_root();
    RecoverySession orphans[RECOVERY_MAX_SESSIONS];
    int orphanCount = root ? recovery_session_claim_orphans(root, orphans, RECOVERY_MAX_SESSIONS) : 0;
    free(root);
    if (!orphanCount) return 0;
    int recover = MessageBoxW(g_hMainWnd,
        L"MarkEdit found autosaved documents from an interrupted session. Recover them?",
        L"Recover Documents", MB_YESNO | MB_ICONQUESTION) == IDYES;

    int restored = 0, complete[RECOVERY_MAX_SESSIONS];
    for (int i = 0; i < orphanCount; i++)
        complete[i] = !recover || restore_recovery_entries(orphans[i].directory, &restored);
    if (restored) {
        update_document_mapping();
        update_preview();
        write_recovery_snapshot();
    }
    /* A recovered session is discarded only once its documents are safe in
       this instance's own snapshot; otherwise the next launch offers it again. */
    int saved = !recover || !g_recoverySnapshotStale;
    for (int i = 0; i < orphanCount; i++)
        recovery_session_close(&orphans[i], complete[i] && saved);
    return restored > 0;
}
