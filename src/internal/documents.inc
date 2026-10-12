/* ================= rest of the app (menus, file I/O, find/replace, font) ================= */

static const WCHAR *document_short_name(const Document *doc) {
    return doc->path ? PathFindFileNameW(doc->path) : NULL;
}

static void format_document_label(const Document *doc, WCHAR *label, size_t capacity) {
    const WCHAR *shortName = document_short_name(doc);
    if (doc->isReference && !shortName) {
        swprintf(label, capacity, L"%ls%ls", reference_title(doc->isReference), doc->dirty ? L" *" : L"");
    } else if (shortName && shortName[0]) {
        swprintf(label, capacity, L"%ls%ls", shortName, doc->dirty ? L" *" : L"");
    } else if (doc->untitledNumber == 0) {
        swprintf(label, capacity, L"Untitled.txt%ls", doc->dirty ? L" *" : L"");
    } else {
        swprintf(label, capacity, L"Untitled %u%ls",
                 doc->untitledNumber, doc->dirty ? L" *" : L"");
    }
}

static void update_tab_label(int index) {
    if (!g_hTabs || index < 0 || index >= g_documentCount) return;
    Document *doc = &g_documents[index];
    WCHAR label[MAX_PATH + 32];
    format_document_label(doc, label, sizeof(label) / sizeof(label[0]));
    TCITEMW item = {0};
    item.mask = TCIF_TEXT;
    item.pszText = label;
    SendMessageW(g_hTabs, TCM_SETITEMW, (WPARAM)index, (LPARAM)&item);
    InvalidateRect(g_hTabs, NULL, FALSE);
}

static void save_active_document_view(void) {
    if (!g_hEdit || g_documentCount <= 0) return;
    Document *doc = &g_documents[g_activeDocument];
    doc->caret = (Sci_Position)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    doc->anchor = (Sci_Position)SendMessageA(g_hEdit, SCI_GETANCHOR, 0, 0);
    doc->firstVisibleLine = (Sci_Position)SendMessageA(g_hEdit, SCI_GETFIRSTVISIBLELINE, 0, 0);
    doc->xOffset = (int)SendMessageA(g_hEdit, SCI_GETXOFFSET, 0, 0);
}

/* Puts a document's remembered selection and scroll position back on a view. */
static void restore_document_view(HWND hEdit, const Document *doc) {
    SendMessageA(hEdit, SCI_SETSEL, (WPARAM)doc->anchor, (LPARAM)doc->caret);
    SendMessageA(hEdit, SCI_SETFIRSTVISIBLELINE, (WPARAM)doc->firstVisibleLine, 0);
    SendMessageA(hEdit, SCI_SETXOFFSET, (WPARAM)doc->xOffset, 0);
}

static void record_document_file_time(int index) {
    if (index < 0 || index >= g_documentCount) return;
    Document *doc = &g_documents[index];
    doc->hasFileTime = 0;
    if (!doc->path) return;
    WCHAR *ioPath = extended_path(doc->path);
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (ioPath && GetFileAttributesExW(ioPath, GetFileExInfoStandard, &data)) {
        doc->fileTime = data.ftLastWriteTime;
        doc->hasFileTime = 1;
    }
    doc->externalChanged = 0;
    doc->externalDeleted = 0;
    if (g_hTabs) InvalidateRect(g_hTabs, NULL, FALSE);
    free(ioPath);
}

/* Swaps the shared document pointer for the main editor, its minimap, and
   its split pane (if any), keeping all of them in sync with whichever tab is
   active. Not used for the transient peeks in get_document_text() and
   clear_debug_execution_markers(), which restore the original pointer
   before returning and would otherwise flash the minimap/split pane at an
   unrelated document. */
static void set_active_editor_document(sptr_t doc) {
    SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)doc);
    if (g_hMinimap) SendMessageA(g_hMinimap, SCI_SETDOCPOINTER, 0, (LPARAM)doc);
    if (g_hSplitEdit) SendMessageA(g_hSplitEdit, SCI_SETDOCPOINTER, 0, (LPARAM)doc);
}

static void switch_document(int index) {
    if (!g_hEdit || index < 0 || index >= g_documentCount || index == g_activeDocument) return;
    save_active_document_view();
    g_switchingDocument = 1;
    g_activeDocument = index;
    set_active_editor_document(g_documents[index].sciDocument);
    apply_editor_font();
    apply_document_language(g_documents[index].path);
    restore_document_view(g_hEdit, &g_documents[index]);
    update_linenumber_margin_width(g_hEdit);
    if (g_hSplitEdit) {
        if (g_splitEditorVisible) {
            /* Starts wherever the main pane starts; the user can then scroll
               it independently - the split pane doesn't persist its own
               per-document view state (see set_active_editor_document()). */
            restore_document_view(g_hSplitEdit, &g_documents[index]);
            update_linenumber_margin_width(g_hSplitEdit);
        }
    }
    g_switchingDocument = 0;
    if (g_hTabs) TabCtrl_SetCurSel(g_hTabs, index);
    update_document_mapping();
    update_preview();
    update_title();
    update_status_bar();
    update_outline();
    prompt_reload_active_external_change();
    SetFocus(g_hEdit);
}

static int create_document_tab(void) {
    if (g_documentCount >= MAX_DOCUMENTS) {
        MessageBoxW(g_hMainWnd, L"MarkEdit can keep up to 32 documents open at once.", L"MarkEdit", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (g_documentCount > 0) save_active_document_view();
    sptr_t sciDoc = (sptr_t)SendMessageA(g_hEdit, SCI_CREATEDOCUMENT, 0, 0);
    if (!sciDoc) return 0;
    int index = g_documentCount++;
    memset(&g_documents[index], 0, sizeof(g_documents[index]));
    g_documents[index].sciDocument = sciDoc;
    g_documents[index].untitledNumber = g_nextUntitledNumber++;
    g_activeDocument = index;
    g_switchingDocument = 1;
    set_active_editor_document(sciDoc);
    configure_scintilla(g_hEdit);
    apply_editor_font();
    SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    g_switchingDocument = 0;
    TCITEMW item = {0};
    item.mask = TCIF_TEXT;
    item.pszText = L"";
    SendMessageW(g_hTabs, TCM_INSERTITEMW, (WPARAM)index, (LPARAM)&item);
    TabCtrl_SetCurSel(g_hTabs, index);
    update_tab_label(index);
    update_document_mapping();
    update_preview();
    update_title();
    return 1;
}

/* The document plugins were last told is active, and whether a check for a
   change is already queued. */
static sptr_t g_pluginActiveDocument = 0;
static int g_pluginTabCheckPosted = 0;

/* Queued, so a sweep through the tabs and back again (Save All, exit)
   reaches plugins as no change at all. */
static void queue_plugin_tab_check(void) {
    if (g_hMainWnd && !g_pluginTabCheckPosted &&
        g_documents[g_activeDocument].sciDocument != g_pluginActiveDocument) {
        g_pluginTabCheckPosted = 1;
        PostMessageW(g_hMainWnd, WM_APP_PLUGIN_TAB_CHECK, 0, 0);
    }
}

static void update_title(void) {
    /* Every change of active document (switching, a new tab, closing one)
       ends up here. */
    queue_plugin_tab_check();
    const Document *active = &g_documents[g_activeDocument];
    const WCHAR *name = g_currentFile ? g_currentFile
        : active->isReference ? reference_title(active->isReference)
        : (active->untitledNumber == 0 ? L"Untitled.txt" : L"Untitled");
    size_t cap = wcslen(name) + 32;
    WCHAR *title = (WCHAR *)malloc(cap * sizeof(WCHAR));
    if (!title) return;
    swprintf(title, cap, L"%ls%ls - MarkEdit", name, g_dirty ? L"*" : L"");
    SetWindowTextW(g_hMainWnd, title);
    free(title);
}

static void set_dirty(int dirty) {
    g_dirty = dirty;
    g_recoverySnapshotStale = 1;
    update_tab_label(g_activeDocument);
    update_title();
    update_status_position();
}

/* Swaps the active tab onto a different Scintilla document and releases the
   one it had. A document's storage options are fixed when it's created, so
   load_file() needs this whenever a file's large-file mode differs from the
   one the tab's current document was made for - and on every large-file
   load, since the loader always builds a new document. Configured the same
   way create_document_tab() sets up a new tab's document. */
static void replace_active_scintilla_document(sptr_t sciDoc, int largeFile) {
    sptr_t old = g_documents[g_activeDocument].sciDocument;
    int oldSwitching = g_switchingDocument;
    g_switchingDocument = 1;
    g_documents[g_activeDocument].sciDocument = sciDoc;
    g_largeFile = largeFile;
    set_active_editor_document(sciDoc);
    configure_scintilla(g_hEdit);
    apply_editor_font();
    SendMessageA(g_hEdit, SCI_RELEASEDOCUMENT, 0, (LPARAM)old);
    g_switchingDocument = oldSwitching;
}

/* A preview-only layout cannot show a large document because its rendered
   preview is deliberately disabled. Reveal the editor automatically; for a
   command-line/file-association launch, replace the preview pane outright so
   the initial window contains the document rather than only an explanation. */
static void ensure_large_file_editor_visible(void) {
    if (!g_largeFile || g_editorVisible) return;
    g_editorVisible = 1;
    if (g_commandLinePreviewMode) g_previewVisible = 0;
    update_view_menu(g_hMainWnd);
    layout(g_hMainWnd);
    InvalidateRect(g_hMainWnd, NULL, TRUE);
}

/* Scintilla's ILoader (ILoader.h) is a C++ interface. It has no virtual
   destructor, so its vtable is exactly these three slots in declaration
   order and can be called from C the same way the COM interfaces are. */
typedef struct SciLoader SciLoader;
typedef struct SciLoaderVtbl {
    int (SCI_METHOD *Release)(SciLoader *self);
    int (SCI_METHOD *AddData)(SciLoader *self, const char *data, Sci_Position length);
    void *(SCI_METHOD *ConvertToDocument)(SciLoader *self);
} SciLoaderVtbl;
struct SciLoader { const SciLoaderVtbl *lpVtbl; };

#define LARGE_FILE_READ_CHUNK (4 * 1024 * 1024)

/* Streams `file` into a new large-file document (see LARGE_FILE_FEATURES)
   through SCI_CREATELOADER, so the text is only ever held once - by
   Scintilla - rather than in a read buffer and then copied into the
   document by SCI_SETTEXT. The document stores no styles (halving its
   memory; it's never lexed) and uses 64-bit positions, which is what lets
   files past 2 GB open at all. Any encoding other than UTF-8 is converted
   a chunk at a time on the way in. `forcedEncoding` is a TextEncoding, or
   -1 to detect it: from the first chunk, then - for a file that looks like
   UTF-8 - by validating the rest as it streams, starting over as ANSI if a
   later chunk turns out not to be. Returns 0 on failure, with *outOfMemory
   set when Scintilla couldn't allocate the text. */
static sptr_t read_large_file_document(HANDLE file, size_t length, int forcedEncoding,
                                       int *outEncoding, int *outHasBom, int *outOfMemory,
                                       LargeOperationProgress *progress) {
    *outOfMemory = 0;
    char *chunk = (char *)malloc(LARGE_FILE_READ_CHUNK);
    char *decoded = (char *)malloc(text_decode_capacity(LARGE_FILE_READ_CHUNK));
    if (!chunk || !decoded) {
        free(chunk); free(decoded);
        *outOfMemory = 1;
        return 0;
    }
    sptr_t result = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        LARGE_INTEGER start = {0};
        if (attempt && !SetFilePointerEx(file, start, NULL, FILE_BEGIN)) break;
        if (!large_operation_progress_update(progress, 0, length)) break;
        SciLoader *loader = (SciLoader *)SendMessageA(g_hEdit, SCI_CREATELOADER, (WPARAM)length,
            (LPARAM)(SC_DOCUMENTOPTION_STYLES_NONE | SC_DOCUMENTOPTION_TEXT_LARGE));
        if (!loader) { *outOfMemory = 1; break; }
        TextCodecState codec = { TEXT_ENCODING_UTF8, {0}, 0 };
        Utf8Validator validator;
        utf8_validator_init(&validator);
        int validating = 0, ok = 1, restart = 0;
        *outHasBom = 0;
        size_t total = 0;
        while (ok && total < length) {
            DWORD want = (DWORD)((length - total) > LARGE_FILE_READ_CHUNK ? LARGE_FILE_READ_CHUNK : (length - total));
            DWORD got = 0;
            if (!ReadFile(file, chunk, want, &got, NULL) || got == 0) { ok = 0; break; }
            const char *data = chunk;
            size_t dataLength = got;
            if (total == 0) {
                size_t bom = 0;
                if (forcedEncoding >= 0) {
                    codec.encoding = (TextEncoding)forcedEncoding;
                    bom = text_bom_length(chunk, got, codec.encoding);
                } else {
                    codec.encoding = detect_text_encoding(chunk, got, got == length, &bom);
                    /* A first chunk that looks like UTF-8 only says so much. */
                    validating = codec.encoding == TEXT_ENCODING_UTF8 && !bom;
                }
                *outHasBom = bom > 0;
                data += bom;
                dataLength -= bom;
            }
            total += got;
            if (!large_operation_progress_update(progress, total, length)) { ok = 0; break; }
            if (validating && !utf8_validator_feed(&validator, data, dataLength)) { restart = 1; break; }
            if (codec.encoding != TEXT_ENCODING_UTF8) {
                size_t produced = 0;
                if (!text_decode(&codec, data, dataLength, total == length, decoded,
                                 text_decode_capacity(LARGE_FILE_READ_CHUNK), &produced)) { ok = 0; break; }
                data = decoded;
                dataLength = produced;
            }
            int status = dataLength ? loader->lpVtbl->AddData(loader, data, (Sci_Position)dataLength) : SC_STATUS_OK;
            if (status != SC_STATUS_OK) {
                *outOfMemory = status == SC_STATUS_BADALLOC;
                ok = 0;
            }
        }
        if (ok && !restart && validating && !utf8_validator_finish(&validator)) restart = 1;
        if (!ok || restart) {
            loader->lpVtbl->Release(loader);
            if (!ok) break;
            forcedEncoding = TEXT_ENCODING_ANSI;
            continue;
        }
        *outEncoding = codec.encoding;
        result = (sptr_t)loader->lpVtbl->ConvertToDocument(loader);
        break;
    }
    free(chunk);
    free(decoded);
    return result;
}

/* `forcedEncoding` is a TextEncoding to read the file as (File > Reopen
   with Encoding), or -1 to detect it. */
static int load_file_as(const WCHAR *path, int forcedEncoding) {
    WCHAR *displayPath = absolute_path(path);
    WCHAR *ioPath = extended_path(path);
    HANDLE hFile = ioPath ? CreateFileW(ioPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL)
        : INVALID_HANDLE_VALUE;
    free(ioPath);
    if (hFile == INVALID_HANDLE_VALUE) {
        MessageBoxW(g_hMainWnd, L"Could not open file.", L"MarkEdit", MB_OK | MB_ICONERROR);
        recent_list_remove(&g_recentFileList, path);
        refresh_menu(g_hMainWnd);
        free(displayPath);
        return 0;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart < 0) {
        CloseHandle(hFile);
        MessageBoxW(g_hMainWnd, L"Could not read file.", L"MarkEdit", MB_OK | MB_ICONERROR);
        free(displayPath);
        return 0;
    }
    size_t length = (size_t)size.QuadPart;
    /* Past INT_MAX a file has to open in large-file mode whatever the
       threshold is set to: only a large-file document has 64-bit positions. */
    int largeFile = size.QuadPart >= (LONGLONG)g_largeFileThresholdMB * 1024 * 1024 ||
                    size.QuadPart > INT_MAX;
    char *buf = NULL;
    size_t total = 0;
    int ok = 0, loaded = 0, outOfMemory = 0, tooLarge = 0, cancelled = 0;
    char *decoded = NULL;
    if (largeFile) {
        /* SCI_CREATELOADER resets g_hEdit's per-view line state as a side
           effect (Editor.cxx's CreateLoader). A successful load swaps in the
           new document, which rebuilds it; a failed one re-attaches the
           tab's own document below to do the same, so the view is saved
           first to put the caret and scroll position back. */
        save_active_document_view();
        HCURSOR oldCursor = SetCursor(LoadCursorA(NULL, IDC_WAIT));
        int hasBom = 0, encoding = TEXT_ENCODING_UTF8;
        LargeOperationProgress progress;
        large_operation_progress_begin(&progress, L"Opening large file", L"Loading text...");
        sptr_t sciDoc = read_large_file_document(hFile, length, forcedEncoding, &encoding,
                                                 &hasBom, &outOfMemory, &progress);
        cancelled = progress.cancelled;
        large_operation_progress_end(&progress);
        SetCursor(oldCursor);
        if (!sciDoc) {
            const Document *doc = &g_documents[g_activeDocument];
            g_switchingDocument = 1;
            set_active_editor_document(doc->sciDocument);
            restore_document_view(g_hEdit, doc);
            g_switchingDocument = 0;
        } else {
            g_switchingDocument = 1;
            replace_active_scintilla_document(sciDoc, 1);
            apply_detected_eol_mode(g_hEdit);
            /* A loader-built document starts with undo collection off. */
            SendMessageA(g_hEdit, SCI_SETUNDOCOLLECTION, TRUE, 0);
            SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
            SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
            g_switchingDocument = 0;
            g_hasBom = hasBom;
            g_encoding = encoding;
            g_documents[g_activeDocument].encodingForced = forcedEncoding >= 0;
            /* Swapping in a loader-built document raises no SCN_MODIFIED,
               which is what refreshes these after an ordinary load. */
            update_linenumber_margin_width(g_hEdit);
            if (g_hSplitEdit) update_linenumber_margin_width(g_hSplitEdit);
            update_status_bar();
            loaded = 1;
            /* No convert_indentation() here, unlike the ordinary path
               below: rewriting every line of a large file on load is the
               kind of whole-document pass this mode exists to avoid. */
        }
    } else {
        buf = (char *)malloc(length + 1);
        if (!buf) outOfMemory = 1;
    }
    while (buf && total < length) {
        DWORD chunk = (DWORD)((length - total) > 1024 * 1024 ? 1024 * 1024 : (length - total));
        DWORD got = 0;
        if (!ReadFile(hFile, buf + total, chunk, &got, NULL) || got == 0) break;
        total += got;
    }
    if (buf && total == length) {
        buf[length] = 0;
        size_t bom = 0;
        TextEncoding encoding = forcedEncoding >= 0 ? (TextEncoding)forcedEncoding
                                                    : detect_text_encoding(buf, length, 1, &bom);
        if (forcedEncoding >= 0) bom = text_bom_length(buf, length, encoding);
        const char *content = buf + bom;
        size_t contentLength = length - bom;
        if (encoding != TEXT_ENCODING_UTF8) {
            size_t decodedLength = 0;
            decoded = text_decode_all(encoding, content, length - bom, &decodedLength);
            contentLength = decodedLength;
            if (!decoded) outOfMemory = 1;
            /* ANSI text can triple in size as UTF-8, past what an ordinary
               (32-bit) document holds; only large-file mode can take that. */
            else if (decodedLength > INT_MAX) tooLarge = 1;
            content = outOfMemory || tooLarge ? NULL : decoded;
        }
        if (content) {
            int decodedLargeMode = contentLength >=
                (size_t)g_largeFileThresholdMB * 1024 * 1024;
            /* Reloading a tab that was in large-file mode with a file that's
               since shrunk needs an ordinary, lexable document again. */
            if (g_largeFile) {
                sptr_t sciDoc = (sptr_t)SendMessageA(g_hEdit, SCI_CREATEDOCUMENT, 0, 0);
                if (sciDoc) replace_active_scintilla_document(sciDoc, 0);
                else decodedLargeMode = 1; /* keep the existing style-less document mode */
            }
            /* The threshold describes editor size, not merely bytes on disk:
               ANSI text can expand to three UTF-8 bytes per source byte. */
            g_largeFile = decodedLargeMode;
            g_hasBom = bom > 0;
            g_encoding = encoding;
            g_documents[g_activeDocument].encodingForced = forcedEncoding >= 0;
            g_switchingDocument = 1;
            set_edit_text(g_hEdit, content, (Sci_Position)contentLength);
            /* Matches toggle_indent_use_spaces(): when the setting is on, a
               file opened (or dropped) with tab-indentation should come in
               already converted rather than leaving a mix of tabs and spaces.
               Done inside the g_switchingDocument window and before
               SCI_EMPTYUNDOBUFFER so it's folded into the loaded state - no
               spurious dirty flag and nothing to undo back into tabs. */
            if (g_indentUseSpaces && !g_largeFile) convert_indentation(g_hEdit, 1);
            SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
            SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
            g_switchingDocument = 0;
            loaded = 1;
        }
    }
    if (loaded) {
        replace_wstring(&g_currentFile, displayPath ? displayPath : path);
        apply_document_language(g_currentFile);
        restore_persisted_breakpoints(g_currentFile);
        ensure_large_file_editor_visible();
        /* An ordinary load's SCN_MODIFIED schedules this; a large-file
           load raises none, so clear the outline the old text built. */
        if (g_largeFile) update_outline();
        record_document_file_time(g_activeDocument);
        update_document_mapping();
        update_preview();
        set_dirty(0);
        recent_list_add(&g_recentFileList, displayPath ? displayPath : path);
        refresh_menu(g_hMainWnd);
        ok = 1;
        fire_plugin_event("open", g_currentFile);
    } else {
        /* A cancelled large-file read is an expected outcome, not a read error. */
        if (!cancelled)
            MessageBoxW(g_hMainWnd, outOfMemory ? L"Not enough memory to open this file."
                    : tooLarge ? L"This file is too large to open as text below the large-file "
                                  L"threshold. Lower View.LargeFileThresholdMB in settings.json "
                                  L"to open it in large-file mode."
                    : L"Could not read file.",
                    L"MarkEdit", MB_OK | MB_ICONERROR);
    }
    free(decoded);
    free(buf);
    CloseHandle(hFile);
    free(displayPath);
    return ok;
}

static int load_file(const WCHAR *path) {
    return load_file_as(path, -1);
}

/* SCN_MODIFIED is delivered from inside Scintilla's edit operation, so the
   notification only flips the cheap per-document flag and posts this work.
   Reconfiguring lexers/views after control returns avoids replacing view
   state re-entrantly while preserving the ordinary document's undo history. */
static void apply_large_file_mode_after_growth(int documentIndex) {
    if (documentIndex < 0 || documentIndex >= g_documentCount ||
        documentIndex != g_activeDocument || !g_largeFile) return;
    KillTimer(g_hMainWnd, IDT_PREVIEW_UPDATE);
    KillTimer(g_hMainWnd, IDT_SPELL_CHECK);
    KillTimer(g_hMainWnd, IDT_OUTLINE_UPDATE);
    apply_document_language(g_currentFile);
    ensure_large_file_editor_visible();
    update_outline();
    update_status_bar();
    if (g_previewVisible) update_preview();
    /* Remove any now-stale recovery copy made while the document was still
       below the threshold; large-file documents are intentionally excluded. */
    write_recovery_snapshot();
    refresh_menu(g_hMainWnd);
}

/* File > Reopen with Encoding: rereads the file from disk as `encoding`,
   for when detection guessed wrong. */
static void reopen_with_encoding(TextEncoding encoding) {
    if (!g_currentFile) {
        MessageBoxW(g_hMainWnd, L"Save the document first - there's no file to reopen yet.",
                    L"MarkEdit", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g_dirty && MessageBoxW(g_hMainWnd, L"Reopening discards your unsaved changes. Continue?",
                               L"MarkEdit", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    WCHAR *path = wcsdup_heap(g_currentFile);
    if (path) load_file_as(path, encoding);
    free(path);
}

/* Changes how the next save stores the active document. The plugin API uses
   this primitive; conversion commands themselves live in plugins/. */
static void set_document_encoding(TextEncoding encoding, int hasBom) {
    if (g_encoding == (int)encoding && g_hasBom == hasBom) return;
    g_encoding = encoding;
    g_hasBom = hasBom;
    set_dirty(1);
}

static int active_document_is_reusable(void) {
    return !g_currentFile && !g_dirty &&
           (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0) == 0;
}

static int open_file_in_tab(const WCHAR *path) {
    WCHAR *full = absolute_path(path);
    if (full) {
        for (int i = 0; i < g_documentCount; i++) {
            if (g_documents[i].path && _wcsicmp(g_documents[i].path, full) == 0) {
                switch_document(i);
                free(full);
                return 1;
            }
        }
    }
    free(full);
    int created = 0;
    if (!active_document_is_reusable()) {
        if (!create_document_tab()) return 0;
        created = 1;
    }
    if (load_file(path)) return 1;
    if (created && g_documentCount > 1) {
        sptr_t failedDoc = g_documents[g_activeDocument].sciDocument;
        int removeIndex = g_activeDocument;
        int target = removeIndex > 0 ? removeIndex - 1 : 0;
        TabCtrl_DeleteItem(g_hTabs, removeIndex);
        g_documentCount--;
        g_activeDocument = target;
        set_active_editor_document(g_documents[target].sciDocument);
        apply_editor_font();
        apply_document_language(g_currentFile);
        restore_document_view(g_hEdit, &g_documents[target]);
        SendMessageA(g_hEdit, SCI_RELEASEDOCUMENT, 0, (LPARAM)failedDoc);
        TabCtrl_SetCurSel(g_hTabs, target);
        update_document_mapping();
        update_preview();
        update_title();
    }
    return 0;
}

static void save_session_documents(void) {
    if (g_commandLinePreviewMode) return;
    WCHAR *settings = get_settings_path();
    if (!settings) return;
    int savedCount = 0;
    for (int i = 0; i < g_documentCount; i++) {
        if (!g_documents[i].path) continue;
        WCHAR key[24];
        swprintf(key, sizeof(key) / sizeof(key[0]), L"File%d", savedCount++);
        write_setting_string(L"Session", key, g_documents[i].path, settings);
    }
    for (int i = savedCount; i < MAX_DOCUMENTS; i++) {
        WCHAR key[24];
        swprintf(key, sizeof(key) / sizeof(key[0]), L"File%d", i);
        write_setting_string(L"Session", key, NULL, settings);
    }
    WCHAR count[24];
    swprintf(count, sizeof(count) / sizeof(count[0]), L"%d", savedCount);
    write_setting_string(L"Session", L"Count", count, settings);
    write_setting_string(L"Session", L"ActiveFile",
                         g_currentFile ? g_currentFile : L"", settings);
    free(settings);
}

static void restore_session_documents(void) {
    WCHAR *settings = get_settings_path();
    if (!settings) return;
    int count = read_setting_int(L"Session", L"Count", 0, settings);
    if (count < 0) count = 0;
    if (count > MAX_DOCUMENTS) count = MAX_DOCUMENTS;
    WCHAR *activeFile = read_profile_string(L"Session", L"ActiveFile", settings);
    for (int i = 0; i < count; i++) {
        WCHAR key[24];
        swprintf(key, sizeof(key) / sizeof(key[0]), L"File%d", i);
        WCHAR *file = read_profile_string(L"Session", key, settings);
        if (file) open_file_in_tab(file);
        free(file);
    }
    if (activeFile) {
        for (int i = 0; i < g_documentCount; i++) {
            if (g_documents[i].path && _wcsicmp(g_documents[i].path, activeFile) == 0) {
                switch_document(i);
                break;
            }
        }
    }
    free(activeFile);
    free(settings);
}

/* Prompt used when closing a dirty tab or exiting with dirty documents.
   Returns 0 if the caller should abort. */
static int confirm_discard_if_dirty(void) {
    if (!g_dirty) return 1;
    int r = MessageBoxW(g_hMainWnd, L"Save changes to the current document?", L"MarkEdit", MB_YESNOCANCEL | MB_ICONWARNING);
    if (r == IDCANCEL) return 0;
    if (r == IDYES) {
        if (!do_save()) return 0;
    }
    return 1;
}

static void do_open_recent(int index) {
    if (index < 0 || index >= g_recentFileCount) return;
    WCHAR *path = wcsdup_heap(g_recentFiles[index]);
    if (path) open_file_in_tab(path); /* opening may mutate the recent-files array */
    free(path);
}

static void do_open(void) {
    DWORD cap = 32768;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"Markdown Files (*.md;*.markdown)\0*.md;*.markdown\0Python Files (*.py;*.pyw)\0*.py;*.pyw\0C/C++ Files (*.c;*.cc;*.cpp;*.cxx)\0*.c;*.cc;*.cpp;*.cxx\0Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 5; /* "All Files (*.*)" - the dialog otherwise defaults to entry 1 */
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    ofn.lpstrDefExt = L"md";
    if (GetOpenFileNameW(&ofn)) {
        WCHAR *next = path + wcslen(path) + 1;
        if (!*next) {
            open_file_in_tab(path);
        } else {
            while (*next) {
                WCHAR *full = path_join(path, next);
                if (full) open_file_in_tab(full);
                free(full);
                next += wcslen(next) + 1;
            }
        }
    }
    free(path);
}

/* Inserts the contents of a chosen file at the current caret position in the
   active editor, replacing any selection - the same semantics SCI_REPLACESEL
   gives every other insert command in this file. */
static void do_insert_file(void) {
    if (!g_hEdit) return;
    DWORD cap = 32768;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"Markdown Files (*.md;*.markdown)\0*.md;*.markdown\0Python Files (*.py;*.pyw)\0*.py;*.pyw\0C/C++ Files (*.c;*.cc;*.cpp;*.cxx)\0*.c;*.cc;*.cpp;*.cxx\0Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 5; /* "All Files (*.*)" - the dialog otherwise defaults to entry 1 */
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrDefExt = L"md";
    if (GetOpenFileNameW(&ofn)) {
        char *text = read_text_file(path, NULL, NULL, NULL);
        if (text) {
            SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
            SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)text);
            SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
            SetFocus(g_hEdit);
        } else {
            MessageBoxW(g_hMainWnd, L"Could not read the selected file.", L"MarkEdit",
                        MB_OK | MB_ICONERROR);
        }
        free(text);
    }
    free(path);
}

static int is_image_file(const WCHAR *path) {
    const WCHAR *extension = PathFindExtensionW(path);
    return extension && extension_in_list(extension, L".png;.jpg;.jpeg;.gif;.bmp;.webp;.svg");
}

/* Create a collision-safe destination in an "images" folder next to the
   current Markdown file. The returned relative path always uses URL-style
   slashes so it can be inserted directly into Markdown. */
static int create_image_destination(const WCHAR *preferredName, const WCHAR *defaultExtension,
                                    WCHAR **destinationOut, WCHAR **relativeOut) {
    *destinationOut = NULL;
    *relativeOut = NULL;
    if (!g_currentFile) {
        if (!do_save()) return 0;
    }
    WCHAR *documentDirectory = path_directory(g_currentFile);
    WCHAR *imagesDirectory = documentDirectory ? path_join(documentDirectory, L"images") : NULL;
    free(documentDirectory);
    if (!imagesDirectory) return 0;
    WCHAR *ioDirectory = extended_path(imagesDirectory);
    BOOL made = CreateDirectoryW(ioDirectory ? ioDirectory : imagesDirectory, NULL);
    DWORD directoryError = made ? ERROR_SUCCESS : GetLastError();
    free(ioDirectory);
    if (!made && directoryError != ERROR_ALREADY_EXISTS) {
        free(imagesDirectory);
        MessageBoxW(g_hMainWnd, L"Could not create the document's images folder.",
                    L"MarkEdit", MB_OK | MB_ICONERROR);
        return 0;
    }

    const WCHAR *leaf = preferredName && preferredName[0] ? PathFindFileNameW(preferredName) : L"image";
    size_t leafLength = wcslen(leaf);
    WCHAR *safe = (WCHAR *)malloc((leafLength + 16) * sizeof(WCHAR));
    if (!safe) { free(imagesDirectory); return 0; }
    size_t o = 0;
    for (size_t i = 0; i < leafLength; i++) {
        WCHAR c = leaf[i];
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
        safe[o++] = c;
    }
    safe[o] = 0;
    WCHAR *extension = PathFindExtensionW(safe);
    if (!extension || !extension[0]) {
        wcscat(safe, defaultExtension ? defaultExtension : L".png");
        extension = PathFindExtensionW(safe);
    }
    size_t extensionOffset = (size_t)(extension - safe);
    WCHAR *extensionCopy = wcsdup_heap(extension);
    safe[extensionOffset] = 0;
    if (!safe[0]) wcscpy(safe, L"image");

    for (unsigned suffix = 0; suffix < 10000; suffix++) {
        size_t nameCap = wcslen(safe) + (extensionCopy ? wcslen(extensionCopy) : 0) + 32;
        WCHAR *name = (WCHAR *)malloc(nameCap * sizeof(WCHAR));
        if (!name) break;
        if (suffix == 0) swprintf(name, nameCap, L"%ls%ls", safe, extensionCopy ? extensionCopy : L"");
        else swprintf(name, nameCap, L"%ls-%u%ls", safe, suffix, extensionCopy ? extensionCopy : L"");
        WCHAR *candidate = path_join(imagesDirectory, name);
        WCHAR *ioCandidate = candidate ? extended_path(candidate) : NULL;
        DWORD attrs = candidate ? GetFileAttributesW(ioCandidate ? ioCandidate : candidate) : INVALID_FILE_ATTRIBUTES;
        free(ioCandidate);
        if (candidate && attrs == INVALID_FILE_ATTRIBUTES) {
            size_t relativeCap = wcslen(name) + 8;
            WCHAR *relative = (WCHAR *)malloc(relativeCap * sizeof(WCHAR));
            if (relative) {
                swprintf(relative, relativeCap, L"images/%ls", name);
                *destinationOut = candidate;
                *relativeOut = relative;
                free(name);
                free(extensionCopy);
                free(safe);
                free(imagesDirectory);
                return 1;
            }
        }
        free(candidate);
        free(name);
    }
    free(extensionCopy);
    free(safe);
    free(imagesDirectory);
    return 0;
}

static int insert_markdown_reference(const WCHAR *relativePath, const WCHAR *altText, int isImage) {
    char *relative = to_utf8(relativePath);
    char *alt = to_utf8(altText && altText[0] ? altText : (isImage ? L"image" : L"file"));
    if (!relative || !alt) { free(relative); free(alt); return 0; }
    size_t escapedCap = strlen(alt) * 2 + 1;
    char *escapedAlt = (char *)malloc(escapedCap);
    if (!escapedAlt) { free(relative); free(alt); return 0; }
    size_t o = 0;
    for (size_t i = 0; alt[i]; i++) {
        if (alt[i] == '\\' || alt[i] == '[' || alt[i] == ']') escapedAlt[o++] = '\\';
        escapedAlt[o++] = alt[i];
    }
    escapedAlt[o] = 0;
    size_t cap = o + strlen(relative) + 16;
    char *markdown = (char *)malloc(cap);
    int inserted = markdown != NULL;
    if (markdown) {
        snprintf(markdown, cap, "%s[%s](<%s>)", isImage ? "!" : "", escapedAlt, relative);
        SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)markdown);
        SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
    }
    free(markdown);
    free(escapedAlt);
    free(relative);
    free(alt);
    return inserted;
}

static int insert_markdown_image(const WCHAR *relativePath, const WCHAR *altText) {
    return insert_markdown_reference(relativePath, altText, 1);
}

/* Writes `target`'s path relative to the folder holding `sourceFile` into
   `relative` (PathRelativePathToW()'s MAX_PATH-bounded buffer). Returns 0
   when there's no relative path, e.g. the two are on different drives. */
static int relative_path_from_file(const WCHAR *sourceFile, const WCHAR *target,
                                   int targetIsDirectory, WCHAR relative[MAX_PATH]) {
    WCHAR *directory = path_directory(sourceFile);
    int ok = directory && PathRelativePathToW(relative, directory, FILE_ATTRIBUTE_DIRECTORY, target,
                                              targetIsDirectory ? FILE_ATTRIBUTE_DIRECTORY
                                                                : FILE_ATTRIBUTE_NORMAL);
    free(directory);
    return ok;
}

/* A heap copy of `path` written the way Markdown links spell it: forward
   slashes, with no leading "./". Sized off `path` itself, since an absolute
   fallback (from DragQueryFileW(), say) isn't MAX_PATH-limited. */
static WCHAR *markdown_link_path(const WCHAR *path) {
    if (path[0] == L'.' && (path[1] == L'\\' || path[1] == L'/')) path += 2;
    WCHAR *link = wcsdup_heap(path);
    if (link)
        for (WCHAR *character = link; *character; character++)
            if (*character == L'\\') *character = L'/';
    return link;
}

/* Links to a dropped file where it already sits on disk (relative to the
   current document's directory) instead of copying it into images/ - used
   for Shift+drop, which is meant for "point at this file" rather than
   "import this image into the document". Images still get an image link;
   everything else gets a plain link. */
static int shift_drop_link(const WCHAR *path) {
    /* Without a current document, or across drives, the link falls back to
       the dropped file's absolute path. */
    WCHAR relative[MAX_PATH];
    const WCHAR *target = g_currentFile && relative_path_from_file(g_currentFile, path, 0, relative)
        ? relative : path;
    WCHAR *normalized = markdown_link_path(target);
    if (!normalized) return 0;

    WCHAR *alt = wcsdup_heap(PathFindFileNameW(path));
    if (alt) PathRemoveExtensionW(alt);
    int inserted = insert_markdown_reference(normalized, alt, is_image_file(path));
    free(alt);
    free(normalized);
    return inserted;
}

static int import_image_file(const WCHAR *source) {
    WCHAR *destination = NULL, *relative = NULL;
    if (!create_image_destination(source, PathFindExtensionW(source), &destination, &relative)) return 0;
    WCHAR *ioSource = extended_path(source);
    WCHAR *ioDestination = extended_path(destination);
    BOOL copied = CopyFileW(ioSource ? ioSource : source,
                            ioDestination ? ioDestination : destination, TRUE);
    free(ioSource);
    free(ioDestination);
    if (!copied) {
        MessageBoxW(g_hMainWnd, L"Could not copy the dropped image into the document's images folder.",
                    L"MarkEdit", MB_OK | MB_ICONERROR);
        free(destination);
        free(relative);
        return 0;
    }
    WCHAR *alt = wcsdup_heap(PathFindFileNameW(source));
    if (alt) PathRemoveExtensionW(alt);
    int inserted = insert_markdown_image(relative, alt ? alt : L"image");
    free(alt);
    free(destination);
    free(relative);
    return inserted;
}

static int save_hbitmap_as_png(HBITMAP bitmap, const WCHAR *path) {
    IWICImagingFactory *factory = NULL;
    IWICBitmap *source = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *properties = NULL;
    UINT width = 0, height = 0;
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory, (void **)&factory);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateBitmapFromHBITMAP(
        factory, bitmap, NULL, WICBitmapIgnoreAlpha, &source);
    if (SUCCEEDED(hr)) hr = IWICBitmap_GetSize(source, &width, &height);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(factory, &stream);
    WCHAR *ioPath = extended_path(path);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(stream, ioPath ? ioPath : path, GENERIC_WRITE);
    free(ioPath);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &properties);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(frame, properties);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(frame, width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &format);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WriteSource(frame, (IWICBitmapSource *)source, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(encoder);
    if (properties) IPropertyBag2_Release(properties);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IWICStream_Release(stream);
    if (source) IWICBitmap_Release(source);
    if (factory) IWICImagingFactory_Release(factory);
    return SUCCEEDED(hr);
}

static int paste_image_from_clipboard(void) {
    if (!IsClipboardFormatAvailable(CF_BITMAP)) return 0;
    /* Rich sources (browsers, Word, Excel) offer text alongside a rendered
       bitmap. Text is what the user means to paste into Markdown source. */
    if (IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT)) return 0;
    if (!g_currentFile) {
        if (!do_save()) return 1;
    }
    if (!OpenClipboard(g_hMainWnd)) return 1;
    HBITMAP bitmap = (HBITMAP)GetClipboardData(CF_BITMAP);
    if (!bitmap) { CloseClipboard(); return 1; }
    SYSTEMTIME now;
    GetLocalTime(&now);
    WCHAR name[64];
    swprintf(name, sizeof(name) / sizeof(name[0]), L"image-%04u%02u%02u-%02u%02u%02u.png",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    WCHAR *destination = NULL, *relative = NULL;
    int made = create_image_destination(name, L".png", &destination, &relative);
    int saved = made && save_hbitmap_as_png(bitmap, destination);
    CloseClipboard();
    if (!saved) {
        MessageBoxW(g_hMainWnd, L"Could not save the clipboard image.",
                    L"MarkEdit", MB_OK | MB_ICONERROR);
        free(destination);
        free(relative);
        return 1;
    }
    insert_markdown_image(relative, L"image");
    free(destination);
    free(relative);
    return 1;
}

static void handle_dropfiles(HDROP hDrop) {
    /* Held at message-processing time, which in practice still reflects the
       physical key held through the drop gesture - not a hard real-time
       guarantee, but Shift-drop is a deliberate two-handed action so the
       window is wide enough to be reliable. */
    int shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
    int insertedImage = 0;
    int undoActive = 0;
    for (UINT i = 0; i < count; i++) {
        UINT cap = DragQueryFileW(hDrop, i, NULL, 0) + 1;
        WCHAR *path = (WCHAR *)malloc((size_t)cap * sizeof(WCHAR));
        if (path && DragQueryFileW(hDrop, i, path, cap)) {
            if (g_editorVisible && shiftHeld) {
                if (!undoActive) {
                    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
                    undoActive = 1;
                }
                if (insertedImage) SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)editor_eol_sequence());
                if (shift_drop_link(path)) insertedImage = 1;
            } else if (g_editorVisible && is_image_file(path)) {
                if (!undoActive) {
                    SendMessageA(g_hEdit, SCI_BEGINUNDOACTION, 0, 0);
                    undoActive = 1;
                }
                if (insertedImage) SendMessageA(g_hEdit, SCI_REPLACESEL, 0, (LPARAM)editor_eol_sequence());
                if (import_image_file(path)) insertedImage = 1;
            } else {
                if (undoActive) {
                    SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
                    undoActive = 0;
                }
                open_file_in_tab(path);
                insertedImage = 0;
            }
        }
        free(path);
    }
    if (undoActive) SendMessageA(g_hEdit, SCI_ENDUNDOACTION, 0, 0);
    DragFinish(hDrop);
}

/* save_to_path()'s write, once the save handlers have run. */
static int write_active_document(const WCHAR *path) {
    /* Written straight from Scintilla's own buffer: SCI_GETCHARACTERPOINTER
       closes the gap so the text is contiguous, and the pointer stays valid
       until the document is next modified. Only the encoding prompt below
       can let that happen (its message loop runs the file-watch timer), so
       the pointer is fetched again after it. A copy via get_edit_text()
       would need a second document-sized allocation, which a large-file
       document may not have room for. */
    Sci_Position len = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
    const char *text = (const char *)SendMessageA(g_hEdit, SCI_GETCHARACTERPOINTER, 0, 0);
    if (!text) {
        MessageBoxW(g_hMainWnd, L"Not enough memory to save the document.", L"MarkEdit", MB_OK | MB_ICONERROR);
        return 0;
    }
    TextEncoding encoding = (TextEncoding)g_encoding;
    int hasBom = g_hasBom;
    LargeOperationProgress progress = {0};
    if (g_largeFile)
        large_operation_progress_begin(&progress, L"Saving large file", L"Checking text encoding...");
    int lossless = text_encoding_is_lossless_progress(text, (size_t)len, encoding,
        progress.dialog ? large_operation_progress_callback : NULL, &progress);
    if (lossless == -2) {
        large_operation_progress_end(&progress);
        return 0;
    }
    if (lossless < 0) {
        large_operation_progress_end(&progress);
        MessageBoxW(g_hMainWnd, L"Not enough memory to save the document.", L"MarkEdit", MB_OK | MB_ICONERROR);
        return 0;
    }
    if (!lossless) {
        large_operation_progress_end(&progress);
        WCHAR message[400];
        if (encoding == TEXT_ENCODING_ANSI)
            swprintf(message, sizeof(message) / sizeof(message[0]),
                     L"Some characters in this document can't be stored in ANSI (code page %u).\n\n"
                     L"Yes: save it as UTF-8 instead.\n"
                     L"No: save as ANSI anyway, replacing those characters with '?'.",
                     text_ansi_code_page());
        else
            swprintf(message, sizeof(message) / sizeof(message[0]),
                     L"This document contains bytes that aren't valid text - it was probably "
                     L"reopened with the wrong encoding - and they can't be stored in UTF-16.\n\n"
                     L"Yes: save it as UTF-8 instead, keeping those bytes as they are.\n"
                     L"No: save as UTF-16 anyway, replacing them with \x201C\xFFFD\x201D.");
        int answer = MessageBoxW(g_hMainWnd, message, L"MarkEdit", MB_YESNOCANCEL | MB_ICONWARNING);
        if (answer == IDCANCEL) return 0;
        if (answer == IDYES) {
            encoding = TEXT_ENCODING_UTF8;
            hasBom = 0;
        }
        /* The prompt's message loop runs timers - an external-change reload
           can replace the text under it - so the buffer is fetched again. */
        len = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLENGTH, 0, 0);
        text = (const char *)SendMessageA(g_hEdit, SCI_GETCHARACTERPOINTER, 0, 0);
        if (!text) {
            MessageBoxW(g_hMainWnd, L"Not enough memory to save the document.", L"MarkEdit", MB_OK | MB_ICONERROR);
            return 0;
        }
        if (g_largeFile)
            large_operation_progress_begin(&progress, L"Saving large file", L"Writing text...");
    } else {
        large_operation_progress_set_line(&progress, L"Writing text...");
    }
    WCHAR *displayPath = absolute_path(path);
    WCHAR *ioPath = extended_path(path);
    const WCHAR *target = ioPath ? ioPath : path;
    int ok = write_file_atomic_progress(target, text, (size_t)len, encoding, hasBom,
        progress.dialog ? large_operation_progress_callback : NULL, &progress);
    int cancelled = progress.cancelled;
    large_operation_progress_end(&progress);
    if (!ok) {
        free(displayPath);
        free(ioPath);
        if (!cancelled)
            MessageBoxW(g_hMainWnd, L"Could not write the complete file; the original was left unchanged.", L"MarkEdit", MB_OK | MB_ICONERROR);
        return 0;
    }
    g_encoding = encoding;
    g_hasBom = hasBom;
    /* Record the path before replace_wstring(): when saving the current file,
       `path` may alias g_currentFile, which replace_wstring() can free. */
    recent_list_add(&g_recentFileList, displayPath ? displayPath : path);
    replace_wstring(&g_currentFile, displayPath ? displayPath : path);
    apply_document_language(g_currentFile);
    /* Save As changes the persistence key from the old path to the new one;
       write the current markers immediately instead of waiting for exit. */
    save_breakpoints_for_active_document(g_currentFile);
    free(ioPath);
    record_document_file_time(g_activeDocument);
    update_document_mapping();
    /* HTML's preview (see show_html_file_preview()) is a real navigation to
       the saved file, not a re-render of the live buffer like Markdown's -
       it only reflects this save's content once it's refreshed here. */
    if (g_activeLanguage == LANG_HTML) update_preview();
    SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
    set_dirty(0);
    free(displayPath);
    refresh_menu(g_hMainWnd);
    return 1;
}

static int save_to_path(const WCHAR *requestedPath) {
    /* The save handlers and the encoding prompt run message loops, where an
       external-change reload can replace (and free) g_currentFile - which
       requestedPath usually is - so the save works from a copy. */
    WCHAR *path = wcsdup_heap(requestedPath);
    if (!path) {
        MessageBoxW(g_hMainWnd, L"Not enough memory to save the document.", L"MarkEdit", MB_OK | MB_ICONERROR);
        return 0;
    }
    /* First, so whatever the handlers change is what gets saved. `save` is
       retained as the compatibility alias for before_save. */
    fire_plugin_event("before_save", path);
    fire_plugin_event("save", path);
    int ok = write_active_document(path);
    if (ok) fire_plugin_event("after_save", path);
    free(path);
    return ok;
}

static int do_saveas(void) {
    DWORD cap = 32768;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return 0;
    if (g_currentFile) wcsncpy(path, g_currentFile, cap - 1);
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"Markdown Files (*.md)\0*.md\0Python Files (*.py;*.pyw)\0*.py;*.pyw\0C/C++ Files (*.c;*.cc;*.cpp;*.cxx)\0*.c;*.cc;*.cpp;*.cxx\0Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!g_currentFile && g_activeLanguage == LANG_TEXT) {
        /* An untitled document defaults to plain text now (see
           language_for_path()); default Save As to match instead of always
           offering "Untitled.md", which would contradict the plain-text tab
           label and preview the user has been shown all along. */
        ofn.nFilterIndex = 4; /* "Text Files (*.txt)", the 4th filter entry above */
        ofn.lpstrDefExt = L"txt";
    } else {
        ofn.lpstrDefExt = L"md";
    }
    int result = GetSaveFileNameW(&ofn) ? save_to_path(path) : 0;
    free(path);
    return result;
}

static int do_save(void) {
    return g_currentFile ? save_to_path(g_currentFile) : do_saveas();
}

/* Saves every dirty tab in turn, the same switch-to-it-then-act pattern
   do_exit() uses to visit every document through the single shared editor.
   An untitled dirty document still prompts for a path via do_save()'s own
   Save As fallback, same as saving it individually would. Stop on the first
   cancellation or failure, and return to the originally selected tab. */
static int do_save_all(void) {
    int original = g_activeDocument;
    int ok = 1;
    for (int i = 0; i < g_documentCount; i++) {
        if (!g_documents[i].dirty) continue;
        switch_document(i);
        if (!do_save()) { ok = 0; break; }
    }
    if (original < g_documentCount) switch_document(original);
    return ok;
}

static void do_export_html(void) {
    if (!g_webview || !g_webviewReady) {
        MessageBoxW(g_hMainWnd, L"The preview is not ready yet.", L"MarkEdit", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g_exportPath) {
        MessageBoxW(g_hMainWnd, L"An HTML export is already in progress.", L"MarkEdit", MB_OK | MB_ICONINFORMATION);
        return;
    }

    DWORD cap = 32768;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    if (g_currentFile) {
        wcsncpy(path, g_currentFile, cap - 1);
        PathRenameExtensionW(path, L".html");
    } else {
        wcsncpy(path, L"Untitled.html", cap - 1);
    }
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"HTML Files (*.html;*.htm)\0*.html;*.htm\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"html";
    if (!GetSaveFileNameW(&ofn)) { free(path); return; }

    WCHAR *directory = document_or_module_directory();
    WCHAR *directorySlash = directory ? path_join(directory, L"") : NULL; /* adds the trailing '\' */
    WCHAR url[32768];
    DWORD urlLength = sizeof(url) / sizeof(url[0]);
    if (!directorySlash || FAILED(UrlCreateFromPathW(directorySlash, url, &urlLength, 0))) wcscpy(url, L"");
    free(directorySlash);
    free(directory);

    const WCHAR *titleWide = document_short_name(&g_documents[g_activeDocument]);
    if (!titleWide) titleWide = L"Untitled";
    char *baseUtf8 = to_utf8(url);
    char *titleUtf8 = to_utf8(titleWide);
    char *baseEscaped = baseUtf8 ? json_escape(baseUtf8) : NULL;
    char *titleEscaped = titleUtf8 ? json_escape(titleUtf8) : NULL;
    if (!baseEscaped || !titleEscaped) {
        free(path); free(baseUtf8); free(titleUtf8); free(baseEscaped); free(titleEscaped);
        return;
    }
    size_t scriptCap = strlen(baseEscaped) + strlen(titleEscaped) + 512;
    char *script = (char *)malloc(scriptCap);
    if (!script) {
        free(path); free(baseUtf8); free(titleUtf8); free(baseEscaped); free(titleEscaped);
        return;
    }
    g_exportRequest++;
    snprintf(script, scriptCap,
        "(()=>{if(!window.exportRenderedHTML)return false;"
        "Promise.resolve().then(()=>window.exportRenderedHTML('%s','%s'))"
        ".then(html=>chrome.webview.postMessage('export-html:%lu:'+JSON.stringify(html)))"
        ".catch(()=>chrome.webview.postMessage('export-html:%lu:null'));return true;})()",
        baseEscaped, titleEscaped, g_exportRequest, g_exportRequest);
    g_exportPath = path;
    SetTimer(g_hMainWnd, IDT_HTML_EXPORT, 30000, NULL);
    update_preview();
    HRESULT hr = execute_webview_script(script, &g_exportScriptHandler);
    if (FAILED(hr)) {
        KillTimer(g_hMainWnd, IDT_HTML_EXPORT);
        free(g_exportPath);
        g_exportPath = NULL;
        MessageBoxW(g_hMainWnd, L"Could not start the HTML export.", L"MarkEdit", MB_OK | MB_ICONERROR);
    }
    free(script); free(baseUtf8); free(titleUtf8); free(baseEscaped); free(titleEscaped);
}

static void prepare_rendered_print(int exportPdf) {
    if (!g_webview || !g_webviewReady) {
        MessageBoxW(g_hMainWnd, L"The rendered preview is not ready yet.", L"MarkEdit",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g_pendingPrintAction != PRINT_ACTION_NONE) {
        MessageBoxW(g_hMainWnd, L"A print or PDF operation is already in progress.", L"MarkEdit",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (exportPdf) {
        DWORD capacity = 32768;
        WCHAR *path = (WCHAR *)calloc(capacity, sizeof(WCHAR));
        if (!path) return;
        if (g_currentFile) {
            wcsncpy(path, g_currentFile, capacity - 1);
            PathRenameExtensionW(path, L".pdf");
        } else wcscpy(path, L"Untitled.pdf");
        OPENFILENAMEW dialog = {0};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = g_hMainWnd;
        dialog.lpstrFilter = L"PDF Documents (*.pdf)\0*.pdf\0All Files (*.*)\0*.*\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = capacity;
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        dialog.lpstrDefExt = L"pdf";
        if (!GetSaveFileNameW(&dialog)) { free(path); return; }
        g_pendingPdfPath = path;
        g_pendingPrintAction = PRINT_ACTION_EXPORT_PDF;
    } else {
        g_pendingPrintAction = PRINT_ACTION_DIALOG;
    }
    if (!g_previewVisible) {
        g_previewVisible = 1;
        update_view_menu(g_hMainWnd);
        layout(g_hMainWnd);
    }
    update_preview();
    SetTimer(g_hMainWnd, IDT_PRINT_PREVIEW, 300, NULL);
}

static void run_pending_print(void) {
    KillTimer(g_hMainWnd, IDT_PRINT_PREVIEW);
    if (!g_webview || g_pendingPrintAction == PRINT_ACTION_NONE) return;
    if (g_pendingPrintAction == PRINT_ACTION_DIALOG) {
        ICoreWebView2_16 *printWebView = NULL;
        if (SUCCEEDED(ICoreWebView2_QueryInterface(g_webview, &IID_ICoreWebView2_16,
                                                    (void **)&printWebView)) && printWebView) {
            HRESULT result = ICoreWebView2_16_ShowPrintUI(printWebView,
                COREWEBVIEW2_PRINT_DIALOG_KIND_SYSTEM);
            ICoreWebView2_16_Release(printWebView);
            if (FAILED(result))
                MessageBoxW(g_hMainWnd, L"Could not open the print dialog.", L"MarkEdit",
                            MB_OK | MB_ICONERROR);
        } else {
            MessageBoxW(g_hMainWnd, L"This WebView2 Runtime does not support printing.",
                        L"MarkEdit", MB_OK | MB_ICONERROR);
        }
        g_pendingPrintAction = PRINT_ACTION_NONE;
    } else if (g_pendingPrintAction == PRINT_ACTION_EXPORT_PDF && g_pendingPdfPath) {
        ICoreWebView2_7 *pdfWebView = NULL;
        if (SUCCEEDED(ICoreWebView2_QueryInterface(g_webview, &IID_ICoreWebView2_7,
                                                   (void **)&pdfWebView)) && pdfWebView) {
            HRESULT result = ICoreWebView2_7_PrintToPdf(pdfWebView, g_pendingPdfPath, NULL,
                                                        &g_pdfHandler);
            ICoreWebView2_7_Release(pdfWebView);
            if (SUCCEEDED(result)) return;
        }
        MessageBoxW(g_hMainWnd, L"This WebView2 Runtime could not start PDF export.",
                    L"MarkEdit", MB_OK | MB_ICONERROR);
        free(g_pendingPdfPath);
        g_pendingPdfPath = NULL;
        g_pendingPrintAction = PRINT_ACTION_NONE;
    }
}

static void release_document_reference(Document *doc) {
    if (doc->sciDocument && g_hEdit)
        SendMessageA(g_hEdit, SCI_RELEASEDOCUMENT, 0, (LPARAM)doc->sciDocument);
    free(doc->path);
    memset(doc, 0, sizeof(*doc));
}

static void push_closed_document(Document removed, int index) {
    removed.closedIndex = index;
    /* Holding a large-file tab's document for Reopen Closed Tab would keep
       the whole file in memory; it keeps only its path and is read from
       disk again on reopen (see do_reopen_closed_tab()). */
    if (removed.largeFile && removed.sciDocument) {
        SendMessageA(g_hEdit, SCI_RELEASEDOCUMENT, 0, (LPARAM)removed.sciDocument);
        removed.sciDocument = 0;
    }
    if (g_closedDocumentCount == MAX_CLOSED_DOCUMENTS) {
        release_document_reference(&g_closedDocuments[0]);
        for (int i = 0; i + 1 < g_closedDocumentCount; i++)
            g_closedDocuments[i] = g_closedDocuments[i + 1];
        g_closedDocumentCount--;
    }
    g_closedDocuments[g_closedDocumentCount++] = removed;
}

static void do_close_document_at(int index) {
    if (index < 0 || index >= g_documentCount) return;
    if (index != g_activeDocument) switch_document(index);
    fire_plugin_event("close", g_currentFile);
    if (!confirm_discard_if_dirty()) return;
    save_active_document_view();
    /* Still dirty here means the user chose not to save, so its markers
       follow discarded edits and the persisted copy is left as it was. */
    if (!g_dirty) save_breakpoints_for_active_document(g_currentFile);
    int removeIndex = g_activeDocument;
    Document removed = g_documents[removeIndex];

    if (g_documentCount <= 1) {
        sptr_t newDoc = (sptr_t)SendMessageA(g_hEdit, SCI_CREATEDOCUMENT, 0, 0);
        if (!newDoc) return;
        memset(&g_documents[0], 0, sizeof(g_documents[0]));
        g_documents[0].sciDocument = newDoc;
        /* Closing the final tab leaves one canonical, reusable blank document.
           Numbered names are reserved for additional unsaved tabs. */
        g_documents[0].untitledNumber = 0;
        g_switchingDocument = 1;
        set_active_editor_document(newDoc);
        configure_scintilla(g_hEdit);
        apply_editor_font();
        if (g_hSplitEdit) apply_language_to_view(g_hSplitEdit, g_currentFile);
        SendMessageA(g_hEdit, SCI_CLEARALL, 0, 0);
        SendMessageA(g_hEdit, SCI_EMPTYUNDOBUFFER, 0, 0);
        SendMessageA(g_hEdit, SCI_SETSAVEPOINT, 0, 0);
        g_switchingDocument = 0;
        push_closed_document(removed, 0);
        g_recoverySnapshotStale = 1;
        write_recovery_snapshot();
        update_tab_label(0);
        update_document_mapping();
        update_preview();
        update_title();
        refresh_menu(g_hMainWnd);
        SetFocus(g_hEdit);
        return;
    }

    TabCtrl_DeleteItem(g_hTabs, removeIndex);
    for (int i = removeIndex; i + 1 < g_documentCount; i++) g_documents[i] = g_documents[i + 1];
    memset(&g_documents[g_documentCount - 1], 0, sizeof(g_documents[0]));
    g_documentCount--;
    g_activeDocument = removeIndex < g_documentCount ? removeIndex : g_documentCount - 1;
    g_switchingDocument = 1;
    set_active_editor_document(g_documents[g_activeDocument].sciDocument);
    apply_editor_font();
    apply_document_language(g_currentFile);
    restore_document_view(g_hEdit, &g_documents[g_activeDocument]);
    g_switchingDocument = 0;
    push_closed_document(removed, removeIndex);
    g_recoverySnapshotStale = 1;
    write_recovery_snapshot();
    TabCtrl_SetCurSel(g_hTabs, g_activeDocument);
    update_linenumber_margin_width(g_hEdit);
    update_document_mapping();
    update_preview();
    update_title();
    refresh_menu(g_hMainWnd);
    prompt_reload_active_external_change();
    SetFocus(g_hEdit);
}

static void do_reopen_closed_tab(void) {
    if (g_closedDocumentCount <= 0) {
        MessageBeep(MB_ICONASTERISK);
        return;
    }
    Document restored = g_closedDocuments[--g_closedDocumentCount];
    memset(&g_closedDocuments[g_closedDocumentCount], 0, sizeof(g_closedDocuments[0]));
    if (!restored.sciDocument) { /* a large-file tab, see push_closed_document() */
        if (restored.path) open_file_in_tab(restored.path);
        free(restored.path);
        return;
    }
    save_active_document_view();

    int replaceBlank = g_documentCount == 1 && active_document_is_reusable();
    int index = restored.closedIndex;
    if (index < 0) index = 0;
    if (index > g_documentCount) index = g_documentCount;
    if (replaceBlank) {
        Document blank = g_documents[0];
        g_documents[0] = restored;
        index = 0;
        g_activeDocument = 0;
        g_switchingDocument = 1;
        set_active_editor_document(restored.sciDocument);
        g_switchingDocument = 0;
        release_document_reference(&blank);
    } else {
        if (g_documentCount >= MAX_DOCUMENTS) {
            g_closedDocuments[g_closedDocumentCount++] = restored;
            MessageBoxW(g_hMainWnd, L"Close a tab before reopening this document.", L"MarkEdit", MB_OK | MB_ICONINFORMATION);
            return;
        }
        for (int i = g_documentCount; i > index; i--) g_documents[i] = g_documents[i - 1];
        g_documents[index] = restored;
        g_documentCount++;
        g_activeDocument = index;
        g_switchingDocument = 1;
        set_active_editor_document(restored.sciDocument);
        g_switchingDocument = 0;
        TCITEMW item = {0};
        item.mask = TCIF_TEXT;
        item.pszText = L"";
        SendMessageW(g_hTabs, TCM_INSERTITEMW, (WPARAM)index, (LPARAM)&item);
    }
    apply_editor_font();
    apply_document_language(g_currentFile);
    restore_document_view(g_hEdit, &restored);
    TabCtrl_SetCurSel(g_hTabs, index);
    update_tab_label(index);
    update_linenumber_margin_width(g_hEdit);
    g_recoverySnapshotStale = 1;
    write_recovery_snapshot();
    update_document_mapping();
    update_preview();
    update_title();
    refresh_menu(g_hMainWnd);
    prompt_reload_active_external_change();
    SetFocus(g_hEdit);
}

static void prompt_reload_active_external_change(void) {
    if (g_documentCount <= 0 || !g_currentFile || !g_documents[g_activeDocument].externalChanged) return;
    Document *doc = &g_documents[g_activeDocument];
    doc->externalChanged = 0;
    InvalidateRect(g_hTabs, NULL, FALSE);
    if (doc->externalDeleted) {
        MessageBoxW(g_hMainWnd,
                    g_dirty
                        ? L"This file was deleted or moved on disk. Your unsaved text is still open; saving will recreate the old path."
                        : L"This file was deleted or moved on disk. Its text remains open; saving will recreate the old path.",
                    L"MarkEdit", MB_OK | MB_ICONWARNING);
        return;
    }
    const WCHAR *message = g_dirty
        ? L"This file changed on disk. Reload it and discard your unsaved changes?"
        : L"This file changed on disk. Reload the updated version?";
    if (MessageBoxW(g_hMainWnd, message, L"MarkEdit", MB_YESNO | MB_ICONQUESTION) == IDYES) {
        WCHAR *path = wcsdup_heap(g_currentFile);
        if (path) load_file_as(path, doc->encodingForced ? doc->encoding : -1);
        free(path);
    }
}

static void check_external_file_change(void) {
    int changedVisual = 0;
    for (int i = 0; i < g_documentCount; i++) {
        Document *doc = &g_documents[i];
        if (!doc->path) continue;
        WCHAR *ioPath = extended_path(doc->path);
        if (!ioPath) continue;
        WIN32_FILE_ATTRIBUTE_DATA data;
        int exists = GetFileAttributesExW(ioPath, GetFileExInfoStandard, &data);
        DWORD attributeError = exists ? ERROR_SUCCESS : GetLastError();
        free(ioPath);
        if (!exists) {
            if (attributeError != ERROR_FILE_NOT_FOUND && attributeError != ERROR_PATH_NOT_FOUND)
                continue;
            if (doc->hasFileTime && !doc->externalDeleted) {
                doc->externalDeleted = 1;
                doc->externalChanged = 1;
                changedVisual = 1;
            }
            doc->hasFileTime = 0;
            continue;
        }
        if (!doc->hasFileTime) {
            doc->fileTime = data.ftLastWriteTime;
            doc->hasFileTime = 1;
            if (doc->externalDeleted) {
                doc->externalDeleted = 0;
                doc->externalChanged = 1;
                changedVisual = 1;
            }
            continue;
        }
        if (CompareFileTime(&doc->fileTime, &data.ftLastWriteTime) != 0) {
            doc->fileTime = data.ftLastWriteTime;
            if (!doc->externalChanged) changedVisual = 1;
            doc->externalChanged = 1;
        }
    }
    if (changedVisual) InvalidateRect(g_hTabs, NULL, FALSE);
    prompt_reload_active_external_change();
}

static void do_exit(void) {
    int original = g_activeDocument;
    for (int i = 0; i < g_documentCount; i++) {
        if (!g_documents[i].dirty) continue;
        switch_document(i);
        if (!confirm_discard_if_dirty()) {
            if (original < g_documentCount) switch_document(original);
            return;
        }
    }
    if (original < g_documentCount) switch_document(original);
    save_all_document_breakpoints();
    save_layout_settings();
    save_session_documents();
    clear_recovery_snapshot();
    for (int i = 0; i < g_documentCount; i++) {
        SendMessageA(g_hEdit, SCI_RELEASEDOCUMENT, 0, (LPARAM)g_documents[i].sciDocument);
        g_documents[i].sciDocument = 0;
    }
    for (int i = 0; i < g_closedDocumentCount; i++)
        release_document_reference(&g_closedDocuments[i]);
    g_closedDocumentCount = 0;
    DestroyWindow(g_hMainWnd);
}
