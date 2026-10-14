/* ---- autosave recovery -------------------------------------------------
   Recovery snapshots live in LocalAppData rather than beside the executable,
   so running the portable distribution never dirties its own folder. Only
   modified documents are written; normal saves and a clean exit remove them. */

static WCHAR *get_recovery_directory(void) {
    return get_local_app_directory(L"Recovery");
}

static int clear_recovery_snapshot(void) {
    WCHAR *directory = get_recovery_directory();
    int ok = directory && recovery_clear(directory);
    free(directory);
    return ok;
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
    WCHAR *directory = get_recovery_directory();
    if (directory && recovery_write(directory, documents, count, read_recovery_editor_text, NULL))
        g_recoverySnapshotStale = 0;
    free(directory);
    update_status_bar();
}

static int restore_recovery_snapshot(void) {
    WCHAR *directory = get_recovery_directory();
    if (!directory) return 0;
    recovery_cleanup(directory);
    int count = recovery_count(directory);
    if (count <= 0 || count > MAX_DOCUMENTS) {
        free(directory); return 0;
    }
    int answer = MessageBoxW(g_hMainWnd,
        L"MarkEdit found autosaved documents from an interrupted session. Recover them?",
        L"Recover Documents", MB_YESNO | MB_ICONQUESTION);
    if (answer != IDYES) {
        free(directory);
        clear_recovery_snapshot();
        return 0;
    }

    int restored = 0;
    for (int i = 0; i < count; i++) {
        RecoveryEntry entry;
        if (!recovery_read(directory, i, &entry)) continue;
        char *text = entry.text;
        int length = entry.length;
        if (restored > 0 && !create_document_tab()) { recovery_entry_free(&entry); break; }
        if (restored == 0 && !active_document_is_reusable() && !create_document_tab()) {
            recovery_entry_free(&entry); break;
        }
        g_documentStore.switching = 1;
        set_edit_text(g_hEdit, text, length);
        SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
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
        restored++;
    }
    free(directory);

    if (restored) {
        update_document_mapping();
        update_preview();
        write_recovery_snapshot();
    }
    return restored > 0;
}
