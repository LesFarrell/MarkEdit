/* ================= WebView2 host page ================= */

/* WebView2 accepts UTF-16 scripts. Centralizing the conversion keeps every
   fire-and-forget call on the same allocation/error path and lets LTO inline
   it into the small fixed-script callers. */
static HRESULT execute_webview_script(const char *script,
                                      ICoreWebView2ExecuteScriptCompletedHandler *handler) {
    if (!g_webview || !script) return E_HANDLE;
    WCHAR *wide = to_wide(script);
    if (!wide) return E_OUTOFMEMORY;
    HRESULT result = ICoreWebView2_ExecuteScript(g_webview, wide, handler);
    free(wide);
    return result;
}

/* Runs `prefix` + `text` escaped for a single-quoted JS string + `suffix`
   in the preview. */
static void execute_webview_string_call(const char *prefix, const char *text, const char *suffix) {
    size_t escapedLength = 0;
    char *escaped = json_escape_length(text, &escapedLength);
    if (!escaped) return;
    size_t prefixLength = strlen(prefix);
    char *script = (char *)malloc(prefixLength + escapedLength + strlen(suffix) + 1);
    if (script) {
        memcpy(script, prefix, prefixLength);
        memcpy(script + prefixLength, escaped, escapedLength);
        strcpy(script + prefixLength + escapedLength, suffix);
        execute_webview_script(script, &g_scriptHandler);
        free(script);
    }
    free(escaped);
}

static void apply_custom_preview_css(int reportErrors) {
    if (!g_webviewReady || !g_webview || g_previewShowingRawHtml) return;
    char *css = g_customCssPath ? read_utf8_text_file(g_customCssPath, CUSTOM_CSS_MAX_BYTES) : _strdup("");
    if (!css) {
        if (reportErrors)
            show_error(L"The custom stylesheet could not be read. It must be a UTF-8 CSS file smaller than 4 MB.");
        css = _strdup("");
    }
    if (!css) return;
    execute_webview_string_call("(()=>{let s=document.getElementById('tinkeredit-custom-css');"
        "if(!s){s=document.createElement('style');s.id='tinkeredit-custom-css';document.head.appendChild(s)}"
        "s.textContent='",
        css, "';})()");
    free(css);
}

/* ---- generic single-use COM completion handler (AddRef/Release are no-ops; the handlers are static) ---- */

static ULONG STDMETHODCALLTYPE Handler_AddRef(void *this_) { (void)this_; return 1; }
static ULONG STDMETHODCALLTYPE Handler_Release(void *this_) { (void)this_; return 1; }
static const IID TINKEREDIT_IID_IUNKNOWN =
    {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static HRESULT Handler_QueryInterface(void *this_, REFIID riid, void **out, REFIID handlerIid) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualIID(riid, &TINKEREDIT_IID_IUNKNOWN) || IsEqualIID(riid, handlerIid)) {
        *out = this_;
        Handler_AddRef(this_);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE EnvHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out,
        &IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler);
}

static HRESULT STDMETHODCALLTYPE CtrlHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out,
        &IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler);
}

static HRESULT STDMETHODCALLTYPE ScriptHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out,
        &IID_ICoreWebView2ExecuteScriptCompletedHandler);
}

static HRESULT STDMETHODCALLTYPE NavHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out,
        &IID_ICoreWebView2NavigationCompletedEventHandler);
}

static HRESULT STDMETHODCALLTYPE NavStartHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out,
        &IID_ICoreWebView2NavigationStartingEventHandler);
}

static HRESULT STDMETHODCALLTYPE NewWindowHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out,
        &IID_ICoreWebView2NewWindowRequestedEventHandler);
}

static HRESULT STDMETHODCALLTYPE WebMessageHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out,
        &IID_ICoreWebView2WebMessageReceivedEventHandler);
}

static HRESULT STDMETHODCALLTYPE AccelKeyHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out,
        &IID_ICoreWebView2AcceleratorKeyPressedEventHandler);
}

/* Environment created handler */
static HRESULT STDMETHODCALLTYPE EnvCreated_Invoke(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *this_, HRESULT errorCode, ICoreWebView2Environment *env);
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl g_envVtbl = {
    (void*)EnvHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)EnvCreated_Invoke
};
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler g_envHandler = { &g_envVtbl };

/* Controller created handler */
static HRESULT STDMETHODCALLTYPE CtrlCreated_Invoke(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *this_, HRESULT errorCode, ICoreWebView2Controller *ctrl);
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl g_ctrlVtbl = {
    (void*)CtrlHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)CtrlCreated_Invoke
};
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandler g_ctrlHandler = { &g_ctrlVtbl };

/* ExecuteScript completed handler: no-op, we don't need the result */
static HRESULT STDMETHODCALLTYPE ScriptDone_Invoke(ICoreWebView2ExecuteScriptCompletedHandler *this_, HRESULT errorCode, LPCWSTR result) {
    (void)this_; (void)errorCode; (void)result;
    return S_OK;
}
static ICoreWebView2ExecuteScriptCompletedHandlerVtbl g_scriptVtbl = {
    (void*)ScriptHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)ScriptDone_Invoke
};
static ICoreWebView2ExecuteScriptCompletedHandler g_scriptHandler = { &g_scriptVtbl };

static HRESULT STDMETHODCALLTYPE ExportDone_Invoke(ICoreWebView2ExecuteScriptCompletedHandler *this_, HRESULT errorCode, LPCWSTR result) {
    (void)this_;
    if (!g_exportPath) return S_OK;
    KillTimer(g_hMainWnd, IDT_HTML_EXPORT);
    WCHAR *html = SUCCEEDED(errorCode) ? decode_json_string(result) : NULL;
    char *utf8 = html ? to_utf8(html) : NULL;
    int ok = 0;
    if (utf8 && *utf8) {
        WCHAR *ioPath = extended_path(g_exportPath);
        ok = write_bytes_file(ioPath ? ioPath : g_exportPath, utf8, strlen(utf8));
        free(ioPath);
    }
    if (!ok) show_error(L"Could not export the rendered HTML.");
    free(utf8);
    free(html);
    free(g_exportPath);
    g_exportPath = NULL;
    return S_OK;
}
/* ExecuteScript reports only whether the asynchronous export was started.
   The resolved HTML arrives through WebMessage_Invoke. */
static HRESULT STDMETHODCALLTYPE ExportStarted_Invoke(ICoreWebView2ExecuteScriptCompletedHandler *this_,
                                                       HRESULT errorCode, LPCWSTR result) {
    if (FAILED(errorCode) || !result || wcscmp(result, L"true") != 0)
        return ExportDone_Invoke(this_, E_FAIL, NULL);
    return S_OK;
}
static ICoreWebView2ExecuteScriptCompletedHandlerVtbl g_exportScriptVtbl = {
    (void*)ScriptHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)ExportStarted_Invoke
};
static ICoreWebView2ExecuteScriptCompletedHandler g_exportScriptHandler = { &g_exportScriptVtbl };

static HRESULT STDMETHODCALLTYPE PdfHandler_QI(void *this_, REFIID riid, void **out) {
    return Handler_QueryInterface(this_, riid, out, &IID_ICoreWebView2PrintToPdfCompletedHandler);
}

static HRESULT STDMETHODCALLTYPE PdfDone_Invoke(ICoreWebView2PrintToPdfCompletedHandler *this_,
                                                 HRESULT errorCode, BOOL result) {
    (void)this_;
    if (FAILED(errorCode) || !result)
        show_error(L"Could not export the rendered preview to PDF.");
    else
        MessageBoxW(g_hMainWnd, L"PDF export completed.", L"Tinker Editor",
                    MB_OK | MB_ICONINFORMATION);
    free(g_pendingPdfPath);
    g_pendingPdfPath = NULL;
    g_pendingPrintAction = PRINT_ACTION_NONE;
    return S_OK;
}

static ICoreWebView2PrintToPdfCompletedHandlerVtbl g_pdfVtbl = {
    (void*)PdfHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)PdfDone_Invoke
};
static ICoreWebView2PrintToPdfCompletedHandler g_pdfHandler = { &g_pdfVtbl };

static void open_wikilink_target(const WCHAR *encoded);

/* The preview shell page itself, with or without a query (the initial
   "?theme=dark|light") or fragment. */
static int is_shell_page_url(const WCHAR *uri) {
    static const WCHAR shellPrefix[] = L"https://tinkeredit.local/preview_shell.html";
    size_t shellLength = wcslen(shellPrefix);
    return uri && _wcsnicmp(uri, shellPrefix, shellLength) == 0 &&
           (uri[shellLength] == 0 || uri[shellLength] == L'?' || uri[shellLength] == L'#');
}

static int is_preview_shell_uri(const WCHAR *uri) {
    return is_shell_page_url(uri) || (uri && !g_webview3 && _wcsicmp(uri, L"about:blank") == 0);
}

static int web_message_from_preview_shell(ICoreWebView2WebMessageReceivedEventArgs *args) {
    LPWSTR source = NULL;
    int trusted = SUCCEEDED(ICoreWebView2WebMessageReceivedEventArgs_get_Source(args, &source)) &&
                  is_preview_shell_uri(source);
    CoTaskMemFree(source);
    return trusted;
}

static HRESULT STDMETHODCALLTYPE WebMessage_Invoke(ICoreWebView2WebMessageReceivedEventHandler *this_,
                                                     ICoreWebView2 *sender,
                                                     ICoreWebView2WebMessageReceivedEventArgs *args) {
    (void)this_; (void)sender;
    LPWSTR message = NULL;
    if (FAILED(ICoreWebView2WebMessageReceivedEventArgs_TryGetWebMessageAsString(args, &message)) ||
        !message) return S_OK;
    /* Raw HTML previews execute their own scripts. They must not be able to
       impersonate the trusted shell and trigger native scrolling, file
       creation through wiki links, or export writes. */
    if (!web_message_from_preview_shell(args)) {
        CoTaskMemFree(message);
        return S_OK;
    }
    WCHAR exportPrefix[64];
    swprintf(exportPrefix, 64, L"export-html:%lu:", g_exportRequest);
    if (g_exportPath && wcsncmp(message, exportPrefix, wcslen(exportPrefix)) == 0) {
        ExportDone_Invoke(NULL, S_OK, message + wcslen(exportPrefix));
        CoTaskMemFree(message);
        return S_OK;
    }
    /* Scroll sync only makes sense with both panes on screen; wiki-link
       navigation should work regardless of which panes are visible. */
    if (g_editorVisible && g_previewVisible && wcsncmp(message, L"scroll:", 7) == 0) {
        WCHAR *end = NULL;
        long line = wcstol(message + 7, &end, 10);
        Sci_Position lineCount = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
        if (end && *end == 0 && line >= 1 && line <= lineCount) {
            g_ignoreNextEditorScroll = 1;
            KillTimer(g_hMainWnd, IDT_SCROLL_SYNC);
            SendMessageA(tracked_edit_view(), SCI_SETFIRSTVISIBLELINE, (WPARAM)(line - 1), 0);
        }
    } else if (wcsncmp(message, L"wikilink:", 9) == 0) {
        open_wikilink_target(message + 9);
    }
    CoTaskMemFree(message);
    return S_OK;
}
static ICoreWebView2WebMessageReceivedEventHandlerVtbl g_webMessageVtbl = {
    (void*)WebMessageHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)WebMessage_Invoke
};
static ICoreWebView2WebMessageReceivedEventHandler g_webMessageHandler = { &g_webMessageVtbl };

/* AcceleratorKeyPressed handler: disabling AreBrowserAcceleratorKeysEnabled
   (see CtrlCreated_Invoke) stops WebView2 acting on its own accelerators,
   but the preview runs in a separate process with its own input queue, so
   the key never otherwise reaches this window's accelerator table - e.g.
   Ctrl+S, or Alt+Left for "Go Back" after following a wiki-link, would
   silently do nothing while focus is in the preview. Replaying the event as
   a real WM_(SYS)KEYDOWN through TranslateAcceleratorA against the same
   table WinMain's message loop uses puts every accelerator back to work
   from the preview, and is a no-op (falls through unhandled) for any key
   that isn't actually one of them. Ctrl+A is the exception: it's left to
   the preview so it selects the rendered text there, rather than invisibly
   selecting the whole of the unfocused editor. */
static HRESULT STDMETHODCALLTYPE AccelKey_Invoke(ICoreWebView2AcceleratorKeyPressedEventHandler *this_,
                                                   ICoreWebView2Controller *sender,
                                                   ICoreWebView2AcceleratorKeyPressedEventArgs *args) {
    (void)this_; (void)sender;
    if (!g_hAccel || !g_hMainWnd) return S_OK;
    COREWEBVIEW2_KEY_EVENT_KIND kind;
    if (FAILED(ICoreWebView2AcceleratorKeyPressedEventArgs_get_KeyEventKind(args, &kind))) return S_OK;
    if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
        return S_OK;
    UINT32 virtualKey = 0;
    INT keyLParam = 0;
    ICoreWebView2AcceleratorKeyPressedEventArgs_get_VirtualKey(args, &virtualKey);
    ICoreWebView2AcceleratorKeyPressedEventArgs_get_KeyEventLParam(args, &keyLParam);
    if (virtualKey == 'A' && (GetKeyState(VK_CONTROL) & 0x8000) &&
        !(GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000))
        return S_OK;
    MSG msg = {0};
    msg.hwnd = g_hMainWnd;
    msg.message = (kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) ? WM_SYSKEYDOWN : WM_KEYDOWN;
    msg.wParam = (WPARAM)virtualKey;
    msg.lParam = (LPARAM)keyLParam;
    if (TranslateAcceleratorA(g_hMainWnd, g_hAccel, &msg))
        ICoreWebView2AcceleratorKeyPressedEventArgs_put_Handled(args, TRUE);
    return S_OK;
}
static ICoreWebView2AcceleratorKeyPressedEventHandlerVtbl g_accelKeyVtbl = {
    (void*)AccelKeyHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)AccelKey_Invoke
};
static ICoreWebView2AcceleratorKeyPressedEventHandler g_accelKeyHandler = { &g_accelKeyVtbl };

/* NavigationCompleted handler: the initial shell page is ready, flush any pending content */
static HRESULT STDMETHODCALLTYPE NavDone_Invoke(ICoreWebView2NavigationCompletedEventHandler *this_, ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) {
    (void)this_;
    BOOL success = FALSE;
    LPWSTR source = NULL;
    if (FAILED(ICoreWebView2NavigationCompletedEventArgs_get_IsSuccess(args, &success)) || !success ||
        FAILED(ICoreWebView2_get_Source(sender, &source)) || !is_preview_shell_uri(source)) {
        CoTaskMemFree(source);
        return S_OK;
    }
    CoTaskMemFree(source);
    g_webviewReady = 1;
    apply_preview_theme();
    apply_custom_preview_css(0);
    if (g_pendingHtmlBody) {
        char *body = g_pendingHtmlBody;
        g_pendingHtmlBody = NULL;
        push_html_to_webview(body);
        free(body);
    }
    return S_OK;
}
static ICoreWebView2NavigationCompletedEventHandlerVtbl g_navVtbl = {
    (void*)NavHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)NavDone_Invoke
};
static ICoreWebView2NavigationCompletedEventHandler g_navHandler = { &g_navVtbl };

/* True if the backslash-separated relative path escapes its base directory
   via a ".." segment, or is itself rooted (drive letter or UNC share) rather
   than relative. Used to keep clicked document-relative links confined to
   the folder the open Markdown file lives in. */
static int path_escapes_base(const WCHAR *relative) {
    if (relative[0] == L'\\' || (relative[0] && relative[1] == L':')) return 1;
    const WCHAR *segment = relative;
    for (;;) {
        size_t len = wcscspn(segment, L"\\");
        if (len == 2 && segment[0] == L'.' && segment[1] == L'.') return 1;
        if (segment[len] == 0) break;
        segment += len + 1;
    }
    return 0;
}

/* Percent-decodes [encoded, end) - which may contain UTF-16 surrogate pairs
   as well as %XX-escaped UTF-8 bytes, exactly as URIs from the preview's
   WebView2 content arrive - into a plain UTF-16 string. Shared by every
   caller that turns a link or postMessage payload from the preview back
   into a native string (document-relative links, wiki-link targets). */
static WCHAR *percent_decode_wide(const WCHAR *encoded, const WCHAR *end) {
    size_t byteCap = (size_t)(end - encoded) * 4 + 1;
    char *bytes = (char *)malloc(byteCap);
    if (!bytes) return NULL;
    size_t o = 0;
    for (const WCHAR *p = encoded; p < end;) {
        if (*p == L'%' && p + 2 < end) {
            int hi = hex_digit_value(p[1]);
            int lo = hex_digit_value(p[2]);
            if (hi >= 0 && lo >= 0) {
                bytes[o++] = (char)((hi << 4) | lo);
                p += 3;
                continue;
            }
        }
        int units = 1;
        if (*p >= 0xD800 && *p <= 0xDBFF && p + 1 < end &&
            p[1] >= 0xDC00 && p[1] <= 0xDFFF) units = 2;
        int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, p, units,
                                           bytes + o, (int)(byteCap - o), NULL, NULL);
        if (written <= 0) { free(bytes); return NULL; }
        o += (size_t)written;
        p += units;
    }
    bytes[o] = 0;
    WCHAR *decoded = o ? to_wide(bytes) : NULL;
    free(bytes);
    return decoded;
}

/* Encode a single filesystem name for use as one URL path segment. Encoding
   every byte except RFC 3986's unreserved set also keeps '#' and '%' in valid
   Windows filenames from being interpreted as a fragment or escape. */
static WCHAR *percent_encode_url_segment(const WCHAR *segment) {
    char *utf8 = to_utf8(segment);
    if (!utf8) return NULL;
    size_t length = strlen(utf8);
    if (length > (SIZE_MAX - 1) / 3) {
        free(utf8);
        return NULL;
    }
    WCHAR *encoded = (WCHAR *)malloc((length * 3 + 1) * sizeof(WCHAR));
    if (!encoded) {
        free(utf8);
        return NULL;
    }
    static const WCHAR hex[] = L"0123456789ABCDEF";
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)utf8[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
            encoded[out++] = (WCHAR)c;
        } else {
            encoded[out++] = L'%';
            encoded[out++] = hex[c >> 4];
            encoded[out++] = hex[c & 0x0F];
        }
    }
    encoded[out] = 0;
    free(utf8);
    return encoded;
}

/* The active document's folder, or tinkeredit.exe's own for an unsaved one -
   the base for relative links, the preview's document.local mapping, export,
   and Open Terminal Here. Caller frees. */
static WCHAR *document_or_module_directory(void) {
    WCHAR *directory = g_currentFile ? path_directory(g_currentFile) : NULL;
    return directory ? directory : module_directory();
}

static WCHAR *decode_document_url(const WCHAR *uri) {
    static const WCHAR prefix[] = L"https://document.local/";
    if (_wcsnicmp(uri, prefix, wcslen(prefix)) != 0) return NULL;
    const WCHAR *encoded = uri + wcslen(prefix);
    const WCHAR *end = encoded + wcscspn(encoded, L"?#");
    WCHAR *relative = percent_decode_wide(encoded, end);
    if (!relative) return NULL;
    for (WCHAR *p = relative; *p; p++) if (*p == L'/') *p = L'\\';
    if (path_escapes_base(relative)) { free(relative); return NULL; }
    WCHAR *directory = document_or_module_directory();
    WCHAR *full = directory ? path_join(directory, relative) : NULL;
    free(directory);
    free(relative);
    return full;
}

static int open_file_in_tab(const WCHAR *path);

/* Records the current document as a "Go Back" target, called right before a
   wiki-link jump actually navigates away from it. Bounded and FIFO like
   g_closedDocuments, and silently a no-op for an unsaved document - see the
   g_wikiBackStack comment. */
static void push_wiki_back(void) {
    if (!g_currentFile) return;
    WCHAR *copy = wcsdup_heap(g_currentFile);
    if (!copy) return;
    if (g_wikiBackCount == MAX_WIKI_BACK) {
        free(g_wikiBackStack[0]);
        for (int i = 0; i + 1 < g_wikiBackCount; i++) g_wikiBackStack[i] = g_wikiBackStack[i + 1];
        g_wikiBackCount--;
    }
    g_wikiBackStack[g_wikiBackCount++] = copy;
}

/* Returns to wherever the most recent wiki-link jump was made from. */
static void go_wiki_back(void) {
    if (g_wikiBackCount <= 0) return;
    WCHAR *path = g_wikiBackStack[--g_wikiBackCount];
    open_file_in_tab(path);
    free(path);
    refresh_menu(g_hMainWnd);
}

/* Resolves a [[wiki-link]] target clicked in the preview (see the
   "wikilink:" case in WebMessage_Invoke) to a note file and opens it. The
   target names a note by title, resolved against the current document's
   directory exactly like a regular document-relative Markdown link - a
   ".md" extension is assumed unless the author already gave one. If the
   note doesn't exist yet, offers to create it rather than silently failing,
   since that's the point of a wiki-link over a plain reference. */
static void open_wikilink_target(const WCHAR *encoded) {
    WCHAR *name = percent_decode_wide(encoded, encoded + wcslen(encoded));
    if (!name) return;
    for (WCHAR *p = name; *p; p++) if (*p == L'/') *p = L'\\';
    if (!*name || path_escapes_base(name)) { free(name); return; }

    WCHAR *directory = document_or_module_directory();
    if (!directory) { free(name); return; }

    WCHAR *combined = NULL;
    const WCHAR *target = name;
    if (!*PathFindExtensionW(name)) {
        combined = (WCHAR *)malloc((wcslen(name) + 4) * sizeof(WCHAR));
        if (combined) {
            wcscpy(combined, name);
            wcscat(combined, L".md");
            target = combined;
        }
    }
    WCHAR *full = path_join(directory, target);
    free(directory);
    free(combined);
    free(name);
    if (!full) return;

    DWORD attrs = GetFileAttributesW(full);
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        push_wiki_back();
        open_file_in_tab(full);
        refresh_menu(g_hMainWnd);
    } else {
        const WCHAR *fileName = PathFindFileNameW(full);
        size_t promptCapacity = wcslen(fileName) + 40;
        WCHAR *prompt = (WCHAR *)malloc(promptCapacity * sizeof(WCHAR));
        if (!prompt) { free(full); return; }
        swprintf(prompt, promptCapacity, L"\"%ls\" doesn't exist yet. Create it?", fileName);
        int create = MessageBoxW(g_hMainWnd, prompt, L"Tinker Editor", MB_YESNO | MB_ICONQUESTION) == IDYES;
        free(prompt);
        if (create) {
            HANDLE file = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (file == INVALID_HANDLE_VALUE) {
                show_error(L"Could not create the note.");
            } else {
                CloseHandle(file);
                push_wiki_back();
                open_file_in_tab(full);
                refresh_menu(g_hMainWnd);
            }
        }
    }
    free(full);
}

/* Opens a document-relative link clicked in the preview. Handing every such
   link to ShellExecuteW would let a cloned README run [Setup](install.bat)
   in one click, so: file types the editor knows open in a tab (scripts
   included, as text); folders open in Explorer; types Windows itself treats
   as dangerous to open are refused; anything else (images, PDFs) opens in
   its default program only after the user confirms. */
static void open_local_link(const WCHAR *path) {
    DWORD attrs = GetFileAttributesW(path);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    if (attrs != INVALID_FILE_ATTRIBUTES && language_for_known_path(path)) {
        push_wiki_back();
        open_file_in_tab(path);
        refresh_menu(g_hMainWnd);
        return;
    }
    const WCHAR *fileName = PathFindFileNameW(path);
    size_t capacity = wcslen(fileName) + 80;
    WCHAR *message = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!message) return;
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        swprintf(message, capacity, L"\"%ls\" doesn't exist.", fileName);
        show_error(message);
    } else if (AssocIsDangerous(path)) {
        swprintf(message, capacity,
                 L"\"%ls\" is a program or script, so Tinker Editor won't open it from the preview.", fileName);
        show_error(message);
    } else {
        swprintf(message, capacity, L"Open \"%ls\" with its default program?", fileName);
        if (MessageBoxW(g_hMainWnd, message, L"Tinker Editor", MB_YESNO | MB_ICONQUESTION) == IDYES)
            ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
    }
    free(message);
}

/* Where a link the preview won't navigate to goes instead: a document-relative
   link to open_local_link(), an http(s)/mailto link to the default browser.
   Anything else is dropped. */
static void open_preview_link(const WCHAR *uri) {
    WCHAR *localPath = decode_document_url(uri);
    if (localPath) {
        open_local_link(localPath);
        free(localPath);
    } else if (_wcsnicmp(uri, L"https://", 8) == 0 ||
               _wcsnicmp(uri, L"http://", 7) == 0 ||
               _wcsnicmp(uri, L"mailto:", 7) == 0) {
        ShellExecuteW(NULL, L"open", uri, NULL, NULL, SW_SHOWNORMAL);
    }
}

/* NavigationStarting handler: links clicked inside the preview (Markdown links,
   images-as-links, etc.) should open in the user's default browser instead of
   navigating the embedded WebView away from the preview shell. Our own shell
   page load is allowed through - either the virtual-host URL
   (https://tinkeredit.local/preview_shell.html) or, on older WebView2 Runtimes
   lacking ICoreWebView2_3, the "about:..." page produced by NavigateToString -
   and so is show_html_file_preview()'s own navigation to the HTML file being
   previewed (g_expectedHtmlPreviewUrl, consumed here so it only ever allows
   that one navigation through: a click on a link inside the previewed page
   itself, even to that exact same URL, still gets redirected out below,
   same as every other link in the preview). A previewed HTML file can't load
   the shell page itself: g_previewShowingRawHtml would stay set, so theme
   and stylesheet changes would never reach the shell. Anything else is cancelled here and, when the user clicked it,
   goes to open_preview_link(); a page navigating itself (a meta refresh, say)
   opens nothing. */
static HRESULT STDMETHODCALLTYPE NavStarting_Invoke(ICoreWebView2NavigationStartingEventHandler *this_, ICoreWebView2 *sender, ICoreWebView2NavigationStartingEventArgs *args) {
    (void)this_; (void)sender;
    LPWSTR uri = NULL;
    if (SUCCEEDED(ICoreWebView2NavigationStartingEventArgs_get_Uri(args, &uri)) && uri) {
        int isExpectedHtmlPreview = g_expectedHtmlPreviewUrl && _wcsicmp(uri, g_expectedHtmlPreviewUrl) == 0;
        int isShellPage = (is_shell_page_url(uri) && !g_previewShowingRawHtml) ||
                           (!g_webviewReady && _wcsicmp(uri, L"about:blank") == 0) ||
                           isExpectedHtmlPreview;
        if (isExpectedHtmlPreview) replace_wstring(&g_expectedHtmlPreviewUrl, NULL);
        if (!isShellPage) {
            ICoreWebView2NavigationStartingEventArgs_put_Cancel(args, TRUE);
            BOOL userInitiated = FALSE;
            if (SUCCEEDED(ICoreWebView2NavigationStartingEventArgs_get_IsUserInitiated(args, &userInitiated)) &&
                userInitiated)
                open_preview_link(uri);
        }
        CoTaskMemFree(uri);
    }
    return S_OK;
}
static ICoreWebView2NavigationStartingEventHandlerVtbl g_navStartVtbl = {
    (void*)NavStartHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)NavStarting_Invoke
};
static ICoreWebView2NavigationStartingEventHandler g_navStartHandler = { &g_navStartVtbl };

/* NewWindowRequested handler: a link opened in a new window (Ctrl/Shift+click,
   target="_blank", window.open) would otherwise get a bare WebView2 window
   that NavStarting_Invoke never sees. No window is created; a clicked link
   goes where any other link would. */
static HRESULT STDMETHODCALLTYPE NewWindow_Invoke(ICoreWebView2NewWindowRequestedEventHandler *this_, ICoreWebView2 *sender, ICoreWebView2NewWindowRequestedEventArgs *args) {
    (void)this_; (void)sender;
    ICoreWebView2NewWindowRequestedEventArgs_put_Handled(args, TRUE);
    BOOL userInitiated = FALSE;
    LPWSTR uri = NULL;
    if (SUCCEEDED(ICoreWebView2NewWindowRequestedEventArgs_get_IsUserInitiated(args, &userInitiated)) &&
        userInitiated && SUCCEEDED(ICoreWebView2NewWindowRequestedEventArgs_get_Uri(args, &uri)) && uri)
        open_preview_link(uri);
    CoTaskMemFree(uri);
    return S_OK;
}
static ICoreWebView2NewWindowRequestedEventHandlerVtbl g_newWindowVtbl = {
    (void*)NewWindowHandler_QI, (void*)Handler_AddRef, (void*)Handler_Release, (void*)NewWindow_Invoke
};
static ICoreWebView2NewWindowRequestedEventHandler g_newWindowHandler = { &g_newWindowVtbl };

static void update_document_mapping(void) {
    if (!g_webview3) return;
    WCHAR *directory = document_or_module_directory();
    if (directory) {
        ICoreWebView2_3_SetVirtualHostNameToFolderMapping(g_webview3, L"document.local", directory,
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        free(directory);
    }
}

/* Launches a terminal rooted in the current file's folder (the app's own
   folder for an unsaved document). Windows Terminal is tried first, since
   it's the modern default on Windows 11; cmd.exe is the universal fallback
   for machines that don't have it installed. */
static void open_terminal_here(void) {
    WCHAR *directory = document_or_module_directory();
    if (!directory) return;
    HINSTANCE result = ShellExecuteW(g_hMainWnd, L"open", L"wt.exe", NULL, directory, SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32)
        ShellExecuteW(g_hMainWnd, L"open", L"cmd.exe", NULL, directory, SW_SHOWNORMAL);
    free(directory);
}

/* Shared with the file explorer panel's "Reveal in Explorer" context-menu
   command (passed to explorer_init() as a callback, since file_explorer.c
   can't call a static tinkeredit.c function by name). */
static void reveal_path_in_explorer(const WCHAR *path) {
    size_t capacity = wcslen(path) + 16;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!parameters) return;
    swprintf(parameters, capacity, L"/select,\"%ls\"", path);
    ShellExecuteW(g_hMainWnd, L"open", L"explorer.exe", parameters, NULL, SW_SHOWNORMAL);
    free(parameters);
}

/* Opens Explorer with the current file pre-selected. Unlike Open Terminal
   Here, there's no reasonable fallback for an unsaved document - Explorer
   can't select a file that doesn't exist on disk yet. */
static void reveal_in_explorer(void) {
    if (!g_currentFile) {
        MessageBoxW(g_hMainWnd, L"Save the document first to show it in Explorer.",
                    L"Show in Explorer", MB_OK | MB_ICONINFORMATION);
        return;
    }
    reveal_path_in_explorer(g_currentFile);
}

/* Sends the preview's light/dark choice, and the scrollbar colours from the
   matching palette - themes.lua may have changed them - so the page's bars
   match the app's own (see the scrollbar rules in preview_shell.html). */
static void apply_preview_theme(void) {
    if (!g_webviewReady || !g_webview || g_previewShowingRawHtml) return;
    const ThemePalette *palette = preview_palette();
    const COLORREF thumbs[3] = { palette->scrollThumb, palette->scrollThumbHot, palette->scrollThumbDrag };
    char colours[3][8];
    for (int i = 0; i < 3; i++)
        wsprintfA(colours[i], "#%02x%02x%02x", GetRValue(thumbs[i]), GetGValue(thumbs[i]), GetBValue(thumbs[i]));
    char script[128];
    wsprintfA(script, "window.setTheme('%s', ['%s', '%s', '%s']);", g_previewDarkMode ? "dark" : "light",
              colours[0], colours[1], colours[2]);
    execute_webview_script(script, &g_scriptHandler);
}

static void push_html_to_webview(const char *htmlFragment) {
    if (!htmlFragment) return;
    if (!g_webviewReady || !g_webview) {
        free(g_pendingHtmlBody);
        g_pendingHtmlBody = _strdup(htmlFragment);
        return;
    }
    execute_webview_string_call("setContent('", htmlFragment, "')");
}

/* Navigates the preview pane to the app's own shell page - the one
   push_html_to_webview()'s setContent() call (Markdown, and the "preview
   not available" message) needs loaded. Used both for the very first load
   (CtrlCreated_Invoke) and to return from show_html_file_preview()'s raw
   HTML file preview. g_webviewReady is cleared first: it means "the loaded
   page is our scriptable shell", which stops being true the moment this
   navigation starts, not just once NavDone_Invoke's completion notice
   arrives - without that, a push_html_to_webview() call made right after
   this one would see the still-stale-true flag and try to script the old
   (about to be replaced) page immediately instead of queuing into
   g_pendingHtmlBody for NavDone_Invoke to flush once the shell has actually
   (re)loaded. */
static void navigate_to_shell(void) {
    if (!g_webview) return;
    g_previewShowingRawHtml = 0;
    g_webviewReady = 0;
    if (g_webview3) {
        /* Pass the starting theme via query string so the page can apply it
           immediately on load, instead of defaulting to light and flipping
           to dark only once apply_preview_theme() runs after navigation
           completes (which caused a visible light-then-dark flash). */
        const WCHAR *shellUrl = g_previewDarkMode
            ? L"https://tinkeredit.local/preview_shell.html?theme=dark"
            : L"https://tinkeredit.local/preview_shell.html?theme=light";
        ICoreWebView2_Navigate(g_webview, shellUrl);
    } else {
        WCHAR *wHtml = to_wide(preview_fallback_html);
        if (wHtml) ICoreWebView2_NavigateToString(g_webview, wHtml);
        free(wHtml);
    }
}

/* Renders an HTML document by navigating the preview pane directly to it,
   instead of markdown_to_html()/push_html_to_webview()'s fragment-into-shell
   injection - this runs the page's own <script> tags and resolves its own
   relative <link>/<script>/<img> references exactly like a real browser,
   via the same https://document.local/ virtual host mapping
   (update_document_mapping()) the shell already uses for Markdown's own
   relative images. Only ever reached when g_currentFile is set:
   language_for_path() only assigns LANG_HTML to a document with a real,
   already-saved path, so this always reflects the file's last-saved
   content, not unsaved edits - write_active_document() calls update_preview() again
   after every successful save to keep it current. See NavStarting_Invoke
   for how this navigation avoids being redirected to the system browser
   like every other link in the preview is. */
static void show_html_file_preview(void) {
    if (!g_webview || !g_webview3 || !g_currentFile) {
        push_html_to_webview("<p>HTML preview needs a newer WebView2 Runtime.</p>");
        return;
    }
    const WCHAR *name = PathFindFileNameW(g_currentFile);
    WCHAR *encodedName = percent_encode_url_segment(name);
    if (!encodedName) return;
    size_t capacity = wcslen(encodedName) + 32;
    WCHAR *url = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!url) {
        free(encodedName);
        return;
    }
    swprintf(url, capacity, L"https://document.local/%ls", encodedName);
    free(encodedName);
    replace_wstring(&g_expectedHtmlPreviewUrl, url);
    g_previewShowingRawHtml = 1;
    g_webviewReady = 0;
    ICoreWebView2_Navigate(g_webview, url);
    free(url);
}

static HRESULT STDMETHODCALLTYPE CtrlCreated_Invoke(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *this_, HRESULT errorCode, ICoreWebView2Controller *ctrl) {
    (void)this_;
    if (FAILED(errorCode) || !ctrl) return S_OK;

    g_webviewController = ctrl;
    ICoreWebView2Controller_AddRef(ctrl);

    RECT rc;
    GetClientRect(g_hPreview, &rc);
    ICoreWebView2Controller_put_Bounds(ctrl, rc);

    apply_webview_background_colour();

    ICoreWebView2Controller_put_IsVisible(ctrl, TRUE);

    ICoreWebView2 *webview = NULL;
    HRESULT webviewResult = ICoreWebView2Controller_get_CoreWebView2(ctrl, &webview);
    if (FAILED(webviewResult) || !webview) return S_OK;
    g_webview = webview;

    ICoreWebView2Settings *settings = NULL;
    if (SUCCEEDED(ICoreWebView2_get_Settings(webview, &settings)) && settings) {
        ICoreWebView2Settings_put_AreDevToolsEnabled(settings, FALSE);
        ICoreWebView2Settings_put_AreDefaultScriptDialogsEnabled(settings, FALSE);
        ICoreWebView2Settings_put_IsWebMessageEnabled(settings, TRUE);
        /* The preview is a document view, not a browser: let the app's own
           accelerators through instead of WebView2's F11 fullscreen, Ctrl+P,
           refresh and friends. Absent on WebView2 Runtimes before 1.0.864.35. */
        ICoreWebView2Settings3 *settings3 = NULL;
        if (SUCCEEDED(ICoreWebView2Settings_QueryInterface(settings, &IID_ICoreWebView2Settings3,
                                                           (void **)&settings3)) && settings3) {
            ICoreWebView2Settings3_put_AreBrowserAcceleratorKeysEnabled(settings3, FALSE);
            ICoreWebView2Settings3_Release(settings3);
        }
        ICoreWebView2Settings_Release(settings);
    }

    EventRegistrationToken token;
    ICoreWebView2_add_NavigationCompleted(webview, &g_navHandler, &token);
    EventRegistrationToken navStartToken;
    ICoreWebView2_add_NavigationStarting(webview, &g_navStartHandler, &navStartToken);
    EventRegistrationToken newWindowToken;
    ICoreWebView2_add_NewWindowRequested(webview, &g_newWindowHandler, &newWindowToken);
    if (SUCCEEDED(ICoreWebView2_add_WebMessageReceived(webview, &g_webMessageHandler,
                                                       &g_webMessageToken)))
        g_webMessageRegistered = 1;
    EventRegistrationToken accelKeyToken;
    ICoreWebView2Controller_add_AcceleratorKeyPressed(ctrl, &g_accelKeyHandler, &accelKeyToken);

    /* Serve the shell + mermaid.min.js from the exe's folder via a virtual host,
       so the page has a real origin and can load mermaid.min.js by relative URL. */
    ICoreWebView2_QueryInterface(webview, &IID_ICoreWebView2_3, (void **)&g_webview3);
    if (g_webview3) {
        WCHAR *exeDir = module_directory();
        if (!exeDir) return S_OK;
        ICoreWebView2_3_SetVirtualHostNameToFolderMapping(g_webview3, L"tinkeredit.local", exeDir, COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
        free(exeDir);
        update_document_mapping();
    }
    navigate_to_shell();

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE EnvCreated_Invoke(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *this_, HRESULT errorCode, ICoreWebView2Environment *env) {
    (void)this_;
    if (FAILED(errorCode) || !env) {
        char msg[256];
        wsprintfA(msg, "Failed to create WebView2 environment (hr=0x%08lX). Is the WebView2 Runtime installed?", (unsigned long)errorCode);
        MessageBoxA(g_hMainWnd, msg, "Tinker Editor", MB_OK | MB_ICONERROR);
        return S_OK;
    }
    ICoreWebView2Environment_CreateCoreWebView2Controller(env, g_hPreview, &g_ctrlHandler);
    return S_OK;
}

static void init_webview(void) {
    /* The browser profile lives with the other per-user state rather than
       beside tinkeredit.exe: an installed copy in Program Files can't write
       there, which made creating the WebView2 environment fail outright. */
    WCHAR *userDataDir = get_local_app_directory(L"WebView2");
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(NULL, userDataDir, NULL, &g_envHandler);
    free(userDataDir);
    if (FAILED(hr)) {
        char msg[256];
        wsprintfA(msg, "CreateCoreWebView2EnvironmentWithOptions call failed (hr=0x%08lX).", (unsigned long)hr);
        MessageBoxA(g_hMainWnd, msg, "Tinker Editor", MB_OK | MB_ICONERROR);
    }
}

/* Documents below this size render in well under a frame, so skipping the
   cursor change for them avoids an hourglass flicker on every keystroke
   pause; above it, rendering can take long enough to be worth signaling. */
#define PREVIEW_SLOW_RENDER_THRESHOLD (256 * 1024)

/* Above this size a source-code preview skips syntax highlighting:
   highlight.js reruns over the whole block on every refresh, so a big file
   would stall the preview after each pause in typing. */
#define PREVIEW_HIGHLIGHT_MAX_LENGTH (512 * 1024)

/* Wraps a non-Markdown document as a single fenced code block tagged with
   `tag`, so it previews as highlighted source through the same Markdown
   pipeline (highlight.js picks the block up by its language class). The
   fence is one backtick longer than the longest run of backticks in the
   text - CommonMark fences can be any length from three - so nothing in
   the source can close it early. */
static char *source_as_markdown(const char *text, size_t length, const char *tag,
                                size_t *resultLength) {
    size_t longest = 0, run = 0;
    for (size_t i = 0; i < length; i++) {
        run = text[i] == '`' ? run + 1 : 0;
        if (run > longest) longest = run;
    }
    size_t fence = longest >= 3 ? longest + 1 : 3;
    size_t tagLength = strlen(tag);
    char *markdown = (char *)malloc(fence * 2 + tagLength + length + 4);
    if (!markdown) return NULL;
    size_t o = 0;
    memset(markdown + o, '`', fence); o += fence;
    memcpy(markdown + o, tag, tagLength); o += tagLength;
    markdown[o++] = '\n';
    memcpy(markdown + o, text, length); o += length;
    if (length && text[length - 1] != '\n') markdown[o++] = '\n';
    memset(markdown + o, '`', fence); o += fence;
    markdown[o++] = '\n';
    markdown[o] = 0;
    if (resultLength) *resultLength = o;
    return markdown;
}

static void update_preview(void) {
    if (g_hMainWnd) KillTimer(g_hMainWnd, IDT_PREVIEW_UPDATE);
    /* Loading/restoring/switching tabs should not parse Markdown while the
       preview is hidden. Showing the pane calls update_preview(), and export
       sets g_exportPath first to force a current off-screen render. */
    if (!g_previewVisible && !g_exportPath) return;
    /* HTML gets a real browser-style render (see show_html_file_preview());
       export bypasses it, since that navigation isn't a render the export can
       capture, and gets the highlighted-source render other languages use. */
    if (g_activeLanguage == LANG_HTML && !g_exportPath) {
        show_html_file_preview();
        return;
    }
    /* Returning here from a raw HTML preview: push_html_to_webview() below
       needs the shell page loaded to run its setContent(), not whatever
       HTML file was last navigated to. */
    if (g_previewShowingRawHtml) navigate_to_shell();
    /* Checked before the language: a large-file document is always plain
       text (see apply_document_language()), which would otherwise get the
       message below as though it were simply the wrong kind of file. */
    if (g_largeFile) {
        push_html_to_webview("<p>Preview is turned off for large files.</p>");
        return;
    }
    Sci_Position len;
    char *text = get_edit_text(g_hEdit, &len);
    if (!text) return;
    /* Running markdown_to_html() straight over a JSON/C/Python/etc. document
       would misread ordinary source lines (a leading '#' comment, say) as
       Markdown syntax, so every other language previews as its own source,
       syntax-highlighted. An untitled tab has no type yet - its editor
       stays plain text (see language_for_path()) - and previews as
       Markdown, as a saved file of an unknown type does. */
    if (g_activeLanguage != LANG_MARKDOWN && !(g_activeLanguage == LANG_TEXT && !g_currentFile)) {
        const char *tag = len > PREVIEW_HIGHLIGHT_MAX_LENGTH ? "" : LANGUAGES[g_activeLanguage].fenceTag;
        size_t sourceLength = 0;
        char *source = source_as_markdown(text, (size_t)len, tag, &sourceLength);
        free(text);
        if (!source) return;
        text = source;
        len = (Sci_Position)sourceLength;
    }

    int showWaitCursor = len >= PREVIEW_SLOW_RENDER_THRESHOLD;
    HCURSOR oldCursor = showWaitCursor ? SetCursor(LoadCursorA(NULL, IDC_WAIT)) : NULL;

    char *html = markdown_to_html_n(text, (size_t)len);
    free(text);

    if (showWaitCursor) SetCursor(oldCursor);

    if (!html) return;
    push_html_to_webview(html);
    free(html);
    if (g_hMainWnd && g_editorVisible && g_previewVisible)
        SetTimer(g_hMainWnd, IDT_SCROLL_SYNC, SCROLL_SYNC_AFTER_RENDER_DELAY_MS, NULL);
}

static void sync_preview_to_editor_scroll(void) {
    if (g_hMainWnd) KillTimer(g_hMainWnd, IDT_SCROLL_SYNC);
    if (!g_webviewReady || !g_webview || !g_editorVisible || !g_previewVisible) return;
    HWND view = tracked_edit_view();
    Sci_Position line = (Sci_Position)SendMessageA(view, SCI_GETFIRSTVISIBLELINE, 0, 0) + 1;
    char script[96];
    snprintf(script, sizeof(script),
             "if(window.scrollToSourceLine)window.scrollToSourceLine(%lld);", (long long)line);
    execute_webview_script(script, &g_scriptHandler);
}
