/* ---- Run / compile / debug: source launching, compiler discovery, GDB and
   Python debugger control, and breakpoint persistence. Included directly from
   markedit.c so the dependency is visible in one place. */

typedef enum DebugSourceKind {
    DEBUG_SOURCE_NONE = 0, DEBUG_SOURCE_PYTHON, DEBUG_SOURCE_C, DEBUG_SOURCE_CPP, DEBUG_SOURCE_LUA
} DebugSourceKind;

static DebugSourceKind debug_source_kind(const WCHAR *path) {
    const WCHAR *extension = path ? PathFindExtensionW(path) : NULL;
    if (!extension) return DEBUG_SOURCE_NONE;
    if (extension_in_list(extension, L".py;.pyw")) return DEBUG_SOURCE_PYTHON;
    if (extension_in_list(extension, L".c")) return DEBUG_SOURCE_C;
    if (extension_in_list(extension, L".cc;.cpp;.cxx")) return DEBUG_SOURCE_CPP;
    if (extension_in_list(extension, L".lua")) return DEBUG_SOURCE_LUA;
    return DEBUG_SOURCE_NONE;
}

/* Only Python/C/C++ have a debugger backend (pdb, GDB); Lua is Run-only, so
   it must not be treated as debuggable just because debug_source_kind()
   returns something other than DEBUG_SOURCE_NONE for it - that value also
   drives whether clicking the margin sets a breakpoint vs. a bookmark. */
static int is_debuggable_source(DebugSourceKind kind) {
    return kind == DEBUG_SOURCE_PYTHON || kind == DEBUG_SOURCE_C || kind == DEBUG_SOURCE_CPP;
}

static int collect_breakpoint_lines(int **outLines);

/* Each absolute, case-folded path gets a compact stable 64-bit FNV-1a settings
   key. The value is simply a
   comma-separated list of 1-based line numbers; deleting every breakpoint
   deletes the key as well. */
static int breakpoint_profile_key(const WCHAR *path, WCHAR *key, size_t capacity) {
    if (!path || !path[0] || !key || capacity < 18) return 0;
    WCHAR *normalized = absolute_path(path);
    if (!normalized) normalized = wcsdup_heap(path);
    if (!normalized) return 0;
    CharLowerBuffW(normalized, (DWORD)wcslen(normalized));
    ULONGLONG hash = 1469598103934665603ULL;
    for (const WCHAR *character = normalized; *character; character++) {
        hash ^= (unsigned char)(*character & 0xFF);
        hash *= 1099511628211ULL;
        hash ^= (unsigned char)((*character >> 8) & 0xFF);
        hash *= 1099511628211ULL;
    }
    free(normalized);
    return swprintf(key, capacity, L"F%016llX", (unsigned long long)hash) > 0;
}

static void save_breakpoints_for_active_document(const WCHAR *path) {
    if (!path || !is_debuggable_source(debug_source_kind(path))) return;
    WCHAR key[32];
    WCHAR *settings = get_settings_path();
    if (!settings || !breakpoint_profile_key(path, key, sizeof(key) / sizeof(key[0]))) {
        free(settings);
        return;
    }
    int *lines = NULL;
    int count = collect_breakpoint_lines(&lines);
    if (count <= 0) {
        write_setting_string(L"Breakpoints", key, NULL, settings);
    } else {
        size_t capacity = (size_t)count * 24 + 1;
        WCHAR *value = (WCHAR *)malloc(capacity * sizeof(WCHAR));
        if (value) {
            size_t used = 0;
            value[0] = 0;
            for (int i = 0; i < count; i++) {
                int written = swprintf(value + used, capacity - used,
                                       i ? L",%d" : L"%d", lines[i] + 1);
                if (written < 0) break;
                used += (size_t)written;
            }
            write_setting_string(L"Breakpoints", key, value, settings);
            free(value);
        }
    }
    free(lines);
    free(settings);
}

static void restore_persisted_breakpoints(const WCHAR *path) {
    SendMessageA(g_hEdit, SCI_MARKERDELETEALL, (WPARAM)MARKER_BREAKPOINT, 0);
    if (!path || !is_debuggable_source(debug_source_kind(path))) return;
    WCHAR key[32];
    WCHAR *settings = get_settings_path();
    if (!settings || !breakpoint_profile_key(path, key, sizeof(key) / sizeof(key[0]))) {
        free(settings);
        return;
    }
    WCHAR *value = read_profile_string(L"Breakpoints", key, settings);
    free(settings);
    if (!value) return;
    Sci_Position lineCount = (Sci_Position)SendMessageA(g_hEdit, SCI_GETLINECOUNT, 0, 0);
    WCHAR *cursor = value;
    while (*cursor) {
        WCHAR *end = NULL;
        long line = wcstol(cursor, &end, 10);
        if (end == cursor) {
            cursor++;
            continue;
        }
        if (line > 0 && (Sci_Position)line <= lineCount)
            SendMessageA(g_hEdit, SCI_MARKERADD, (WPARAM)(line - 1), MARKER_BREAKPOINT);
        cursor = end;
        while (*cursor == L',' || *cursor == L' ' || *cursor == L'\t') cursor++;
    }
    free(value);
}

/* Persisted breakpoint lines have to match the file on disk. A dirty tab's
   markers follow unsaved edits that may yet be discarded, so it's skipped
   here; save_to_path() writes its markers once the text is saved. */
static void save_all_document_breakpoints(void) {
    if (!g_hEdit || g_documentStore.count <= 0) return;
    BackgroundDocumentAccess access;
    begin_background_document_access(&access);
    for (int i = 0; i < g_documentStore.count; i++) {
        if (!g_documentStore.documents[i].sciDocument || !g_documentStore.documents[i].path || g_documentStore.documents[i].dirty) continue;
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_documentStore.documents[i].sciDocument);
        save_breakpoints_for_active_document(g_documentStore.documents[i].path);
    }
    end_background_document_access(&access);
}

/* Debugger breakpoints are separate from general-purpose bookmarks, but share
   the same narrow marker margin. Scintilla keeps them attached to their lines
   as text is inserted or removed. */
static void toggle_breakpoint_at_line(Sci_Position line) {
    if (!is_debuggable_source(debug_source_kind(g_currentFile))) {
        MessageBoxW(g_hMainWnd, L"Breakpoints are available for Python, C, and C++ files.",
                    L"Debugging", MB_OK | MB_ICONINFORMATION);
        return;
    }
    int mask = (int)SendMessageA(g_hEdit, SCI_MARKERGET, (WPARAM)line, 0);
    if (mask & (1 << MARKER_BREAKPOINT))
        SendMessageA(g_hEdit, SCI_MARKERDELETE, (WPARAM)line, (LPARAM)MARKER_BREAKPOINT);
    else
        SendMessageA(g_hEdit, SCI_MARKERADD, (WPARAM)line, (LPARAM)MARKER_BREAKPOINT);
    /* See save_all_document_breakpoints(): a dirty tab persists on save. */
    if (!g_dirty) save_breakpoints_for_active_document(g_currentFile);
}

static void toggle_breakpoint_at_cursor(void) {
    Sci_Position pos = (Sci_Position)SendMessageA(g_hEdit, SCI_GETCURRENTPOS, 0, 0);
    Sci_Position line = (Sci_Position)SendMessageA(g_hEdit, SCI_LINEFROMPOSITION, (WPARAM)pos, 0);
    toggle_breakpoint_at_line(line);
}

static void clear_debug_breakpoints(void) {
    if (g_hEdit && g_documentStore.count > 0) {
        BackgroundDocumentAccess access;
        begin_background_document_access(&access);
        for (int i = 0; i < g_documentStore.count; i++) {
            if (!g_documentStore.documents[i].sciDocument) continue;
            SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_documentStore.documents[i].sciDocument);
            SendMessageA(g_hEdit, SCI_MARKERDELETEALL, (WPARAM)MARKER_BREAKPOINT, 0);
        }
        end_background_document_access(&access);
    }

    /* The command says "all", so remove entries for closed files too rather
       than allowing their persisted markers to reappear when reopened. */
    WCHAR *settings = get_settings_path();
    if (settings) {
        clear_setting_section(L"Breakpoints", settings);
        free(settings);
    }
}

/* `directory`\`name` when that file exists, else NULL. Used for
   user-configured folder overrides (Settings > Compiler's GCC/Clang/Zig/Lua/
   GDB path boxes) - the counterpart to executable_on_search_path() - and for
   other known install locations. */
static WCHAR *existing_file_in_directory(const WCHAR *directory, const WCHAR *name) {
    if (!directory || !directory[0]) return NULL;
    WCHAR *candidate = path_join(directory, name);
    if (file_exists(candidate)) return candidate;
    free(candidate);
    return NULL;
}

/* Windows PowerShell's full path under the system directory, or plain
   "powershell.exe" for a PATH lookup when it isn't there. Caller frees. */
static WCHAR *locate_powershell(void) {
    WCHAR systemDirectory[MAX_PATH];
    WCHAR *powershell = GetSystemDirectoryW(systemDirectory, MAX_PATH)
        ? existing_file_in_directory(systemDirectory, L"WindowsPowerShell\\v1.0\\powershell.exe")
        : NULL;
    return powershell ? powershell : wcsdup_heap(L"powershell.exe");
}

/* PowerShell's options for a Run command: a separate console stays open
   afterwards so its results can be read, while the output panel keeps them
   once the program has ended. */
static const WCHAR *run_shell_options(void) {
    return g_runInConsole ? L"-NoLogo -NoProfile -NoExit -Command" : L"-NoLogo -NoProfile -Command";
}

/* Runs `file parameters` in `directory` for Run: in the output panel, or in
   a visible console with Run > Run in Separate Console (debug sessions use
   launch_debug_console()). Reports `failure` under `title` when it can't
   be started. */
static void launch_run_command(const WCHAR *file, const WCHAR *parameters, const WCHAR *directory,
                               const WCHAR *failure, const WCHAR *title) {
    if (!g_runInConsole) {
        if (!run_in_output_panel(file, parameters, directory, title))
            MessageBoxW(g_hMainWnd, failure, title, MB_OK | MB_ICONERROR);
        return;
    }
    SHELLEXECUTEINFOW execute = {0};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.hwnd = g_hMainWnd;
    execute.lpVerb = L"open";
    execute.lpFile = file;
    execute.lpParameters = parameters;
    execute.lpDirectory = directory;
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute))
        MessageBoxW(g_hMainWnd, failure, title, MB_OK | MB_ICONERROR);
    else if (execute.hProcess)
        CloseHandle(execute.hProcess);
}

/* PowerShell arguments that run `& interpreter[extra] script` and then
   report its exit code (see run_shell_options()). Both names are already
   PowerShell literals (powershell_literal()). Caller frees. */
static WCHAR *run_script_parameters(const WCHAR *quotedInterpreter, const WCHAR *extraArguments,
                                    const WCHAR *quotedScript) {
    size_t capacity = wcslen(quotedInterpreter) + wcslen(extraArguments) + wcslen(quotedScript) + 160;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (parameters &&
        swprintf(parameters, capacity,
                 L"%ls \"& %ls%ls %ls; Write-Host ''; "
                 L"Write-Host ('Process exited with code ' + $LASTEXITCODE)\"",
                 run_shell_options(), quotedInterpreter, extraArguments, quotedScript) < 0) {
        free(parameters);
        parameters = NULL;
    }
    return parameters;
}

/* Collects the 0-based line numbers of every breakpoint marker in the active
   document in one pass, so callers don't need a separate counting scan
   before walking the same markers again to emit debugger commands. Returns
   the count; *outLines is NULL when it's 0. Caller frees a non-NULL result. */
static int collect_breakpoint_lines(int **outLines) {
    int capacity = 0, count = 0;
    int *lines = NULL;
    Sci_Position line = -1;
    while ((line = (Sci_Position)SendMessageA(g_hEdit, SCI_MARKERNEXT, (WPARAM)(line + 1),
                                      (LPARAM)(1 << MARKER_BREAKPOINT))) >= 0) {
        if (count >= capacity) {
            int next = capacity ? capacity * 2 : 8;
            int *grown = (int *)realloc(lines, (size_t)next * sizeof(int));
            if (!grown) break;
            lines = grown;
            capacity = next;
        }
        /* Debugger "file:line" commands take an int; breakpoints are only
           offered on Python/C/C++ sources, nowhere near 2^31 lines. */
        lines[count++] = (int)line;
    }
    *outLines = lines;
    return count;
}

#define VENV_PYTHON L"Scripts\\python.exe"

static WCHAR *python_from_named_environment(const WCHAR *directory, const WCHAR *name) {
    WCHAR *root = path_join(directory, name);
    WCHAR *python = existing_file_in_directory(root, VENV_PYTHON);
    free(root);
    return python;
}

static WCHAR *executable_on_search_path(const WCHAR *name) {
    DWORD capacity = SearchPathW(NULL, name, NULL, 0, NULL, NULL);
    if (!capacity) return NULL;
    WCHAR *path = (WCHAR *)malloc(((size_t)capacity + 1) * sizeof(WCHAR));
    if (!path) return NULL;
    DWORD length = SearchPathW(NULL, name, NULL, capacity + 1, path, NULL);
    if (!length || length > capacity) {
        free(path);
        return NULL;
    }
    return path;
}

/* A release keeps GDB beside MarkEdit under third_party/gdb/bin. Developer
   builds use the same layout at the repository root. An explicit setting is
   authoritative; otherwise prefer that reproducibly packaged copy and retain
   PATH as a fallback for older/custom distributions. */
static WCHAR *find_gdb_executable(void) {
    if (g_gdbBinOverride) return existing_file_in_directory(g_gdbBinOverride, L"gdb.exe");

    WCHAR *directory = module_directory();
    WCHAR *bundled = existing_file_in_directory(directory, L"third_party\\gdb\\bin\\gdb.exe");
    free(directory);
    return bundled ? bundled : executable_on_search_path(L"gdb.exe");
}

/* Prefer an activated environment, then a project-local .venv/venv found by
   walking up from the script, and finally the standard launcher/interpreters
   on PATH. This lets common Python projects work without MarkEdit settings. */
static WCHAR *find_python_interpreter(const WCHAR *scriptPath) {
    DWORD environmentLength = GetEnvironmentVariableW(L"VIRTUAL_ENV", NULL, 0);
    if (environmentLength) {
        WCHAR *environment = (WCHAR *)malloc((size_t)environmentLength * sizeof(WCHAR));
        if (environment && GetEnvironmentVariableW(L"VIRTUAL_ENV", environment,
                                                    environmentLength)) {
            WCHAR *python = existing_file_in_directory(environment, VENV_PYTHON);
            free(environment);
            if (python) return python;
        } else {
            free(environment);
        }
    }

    WCHAR *directory = path_directory(scriptPath);
    while (directory) {
        WCHAR *python = python_from_named_environment(directory, L".venv");
        if (!python) python = python_from_named_environment(directory, L"venv");
        if (python) {
            free(directory);
            return python;
        }
        WCHAR *parent = path_directory(directory);
        if (!parent || _wcsicmp(parent, directory) == 0) {
            free(parent);
            break;
        }
        free(directory);
        directory = parent;
    }
    free(directory);

    static const WCHAR *names[] = { L"py.exe", L"python.exe", L"python3.exe" };
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        WCHAR *python = executable_on_search_path(names[i]);
        if (python) return python;
    }
    return NULL;
}

/* A PowerShell single-quoted literal only treats a single quote specially;
   doubling it gives us safe arguments even for paths containing shell syntax.
   PowerShell counts the typographic quotes U+2018-U+201B as single quotes
   too, so a file named x’;calc;’.c would otherwise end the literal early. */
static int is_powershell_single_quote(WCHAR c) {
    return c == L'\'' || (c >= 0x2018 && c <= 0x201B);
}

static WCHAR *powershell_literal(const WCHAR *value) {
    size_t length = wcslen(value);
    WCHAR *literal = (WCHAR *)malloc((length * 2 + 3) * sizeof(WCHAR));
    if (!literal) return NULL;
    size_t output = 0;
    literal[output++] = L'\'';
    for (size_t i = 0; i < length; i++) {
        if (is_powershell_single_quote(value[i])) literal[output++] = value[i];
        literal[output++] = value[i];
    }
    literal[output++] = L'\'';
    literal[output] = 0;
    return literal;
}

typedef enum { FLAG_QUOTE_POWERSHELL, FLAG_QUOTE_ARGV } FlagQuoteStyle;

/* Re-quotes one already-tokenized extra-compiler-flag for its target:
   powershell_literal() above for the PowerShell console launch_native_file()
   builds, or the Microsoft C runtime's argv rules for cl.exe, which
   launch_msvc_file() hands the flags through an environment variable so
   cmd.exe never parses them. Under those rules backslashes are literal
   except in a run that ends at a double quote, so such a run is doubled
   and the quote itself escaped. */
static WCHAR *quote_token_for_shell(const WCHAR *token, FlagQuoteStyle style) {
    if (style == FLAG_QUOTE_POWERSHELL) return powershell_literal(token);
    size_t length = wcslen(token);
    if (length && !wcspbrk(token, L" \t\"")) return wcsdup_heap(token);
    WCHAR *quoted = (WCHAR *)malloc((length * 2 + 3) * sizeof(WCHAR));
    if (!quoted) return NULL;
    size_t out = 0, backslashes = 0;
    quoted[out++] = L'"';
    for (size_t i = 0; i < length; i++) {
        if (token[i] == L'"') {
            for (size_t j = 0; j <= backslashes; j++) quoted[out++] = L'\\';
        }
        backslashes = token[i] == L'\\' ? backslashes + 1 : 0;
        quoted[out++] = token[i];
    }
    while (backslashes--) quoted[out++] = L'\\';
    quoted[out++] = L'"';
    quoted[out] = 0;
    return quoted;
}

/* Splits the free-form "extra compiler flags" text from Settings > Compiler
   the way a shell would - via CommandLineToArgvW, so a quoted flag like
   -D"FOO=bar baz" keeps its embedded space instead of being cut in two - and
   re-quotes each token for its target (see quote_token_for_shell()), so the
   text can't break out of the generated build command. Returns NULL when
   there's nothing to add, so callers can treat that the same as "no extra
   flags configured". */
static WCHAR *quote_extra_compiler_flags(const WCHAR *flags, FlagQuoteStyle style) {
    if (!flags || !flags[0]) return NULL;
    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(flags, &argc);
    if (!argv || argc == 0) {
        if (argv) LocalFree(argv);
        return NULL;
    }
    size_t capacity = 4;
    for (int i = 0; i < argc; i++) capacity += wcslen(argv[i]) * 2 + 4;
    WCHAR *joined = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (joined) {
        joined[0] = 0;
        size_t used = 0;
        for (int i = 0; i < argc; i++) {
            WCHAR *quoted = quote_token_for_shell(argv[i], style);
            if (!quoted) continue;
            int written = swprintf(joined + used, capacity - used, used ? L" %ls" : L"%ls", quoted);
            free(quoted);
            if (written > 0) used += (size_t)written;
        }
    }
    LocalFree(argv);
    return joined;
}

static WCHAR *debug_temporary_path(const WCHAR *extension) {
    WCHAR temporaryDirectory[MAX_PATH];
    DWORD length = GetTempPathW(MAX_PATH, temporaryDirectory);
    if (!length || length >= MAX_PATH) return NULL;
    WCHAR name[96];
    swprintf(name, sizeof(name) / sizeof(name[0]), L"markedit-debug-%lu-%llu.%ls",
             GetCurrentProcessId(), (unsigned long long)GetTickCount64(), extension);
    return path_join(temporaryDirectory, name);
}

static void clear_debug_execution_markers(void) {
    if (!g_hEdit || g_documentStore.count <= 0) return;
    BackgroundDocumentAccess access;
    begin_background_document_access(&access);
    for (int i = 0; i < g_documentStore.count; i++) {
        /* do_exit() has already released every document by the time
           shutdown clears these markers; attaching 0 would only make
           Scintilla create a throwaway empty document. */
        if (!g_documentStore.documents[i].sciDocument) continue;
        SendMessageA(g_hEdit, SCI_SETDOCPOINTER, 0, (LPARAM)g_documentStore.documents[i].sciDocument);
        SendMessageA(g_hEdit, SCI_MARKERDELETEALL, MARKER_DEBUG_LINE, 0);
        SendMessageA(g_hEdit, SCI_MARKERDELETEALL, MARKER_DEBUG_ARROW, 0);
    }
    end_background_document_access(&access);
}

static WCHAR *debug_location_to_wide(const char *text);

static void set_debug_panel_status(const WCHAR *status) {
    if (g_hDebugStatus) SetWindowTextW(g_hDebugStatus, status ? status : L"Debugger");
}

/* Shows the output panel's Debug tab, where the session's status and
   variables are (see output_panel.inc) - without moving focus away from the
   editor, same as this did before the debug and output panels were merged. */
static void show_debug_panel(void) {
    show_output_panel(g_hMainWnd, OUTPUT_TAB_DEBUG, 0);
}

static void add_debug_variable(const char *name, const char *value) {
    if (!g_hDebugVariables || !name || !name[0]) return;
    WCHAR *wideName = debug_location_to_wide(name);
    WCHAR *wideValue = debug_location_to_wide(value ? value : "");
    if (!wideName || !wideValue) {
        free(wideName); free(wideValue);
        return;
    }
    LVITEMW item = {0};
    item.mask = LVIF_TEXT;
    item.iItem = ListView_GetItemCount(g_hDebugVariables);
    item.pszText = wideName;
    int row = (int)SendMessageW(g_hDebugVariables, LVM_INSERTITEMW, 0, (LPARAM)&item);
    if (row >= 0) {
        LVITEMW subitem = {0};
        subitem.mask = LVIF_TEXT;
        subitem.iSubItem = 1; subitem.pszText = wideValue;
        SendMessageW(g_hDebugVariables, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&subitem);
    }
    free(wideName); free(wideValue);
}

/* Both debugger backends append a small variables section to the same state
   snapshot that carries the source location. Python emits tab-separated
   name/type/value rows; GDB emits its native `name = value` rows. Rewrites
   `snapshot` in place, and only ever at or after the marker, so a location
   prefix parsed either side of this call still reads correctly. */
static void update_debug_variables(char *snapshot) {
    if (!g_hDebugVariables || !snapshot) return;
    char *variables = strstr(snapshot, "__MARKEDIT_VARIABLES__");
    if (!variables) return;
    variables += strlen("__MARKEDIT_VARIABLES__");
    while (*variables == '\r' || *variables == '\n') variables++;
    ListView_DeleteAllItems(g_hDebugVariables);
    char *line = variables;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t length = strlen(line);
        while (length && (line[length - 1] == '\r' || line[length - 1] == ' '))
            line[--length] = 0;
        if (line[0] && strcmp(line, "Arguments:") != 0 && strcmp(line, "Locals:") != 0) {
            char *firstTab = strchr(line, '\t');
            if (firstTab) {
                *firstTab++ = 0;
                char *secondTab = strchr(firstTab, '\t');
                if (secondTab) {
                    *secondTab++ = 0;
                    add_debug_variable(line, secondTab);
                }
            } else {
                char *equals = strstr(line, " = ");
                if (equals) {
                    *equals = 0;
                    add_debug_variable(line, equals + 3);
                }
            }
        }
        line = next;
    }
}

static void stop_debug_location_tracking(void) {
    int hadSession = g_activeDebuggerKind != 0;
    if (g_hMainWnd) KillTimer(g_hMainWnd, IDT_DEBUG_LOCATION);
    if (g_debugJob) {
        /* JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE terminates PowerShell, the
           debugger (pdb or GDB), and the program being debugged together,
           including when MarkEdit itself is exiting. See launch_debug_console(). */
        CloseHandle(g_debugJob);
        g_debugJob = NULL;
        /* Closing the job requests termination; wait briefly for its file
           handles to close before removing the generated debugger files. */
        if (g_debugProcess) WaitForSingleObject(g_debugProcess, DEBUGGER_EXIT_WAIT_MS);
    }
    if (g_debugProcess) {
        CloseHandle(g_debugProcess);
        g_debugProcess = NULL;
    }
    if (g_debugLocationPath) DeleteFileW(g_debugLocationPath);
    if (g_debugCommandPath) DeleteFileW(g_debugCommandPath);
    if (g_debugOutputPath) DeleteFileW(g_debugOutputPath);
    free(g_debugLocationPath); g_debugLocationPath = NULL;
    free(g_debugWorkingDirectory); g_debugWorkingDirectory = NULL;
    free(g_debugCommandPath); g_debugCommandPath = NULL;
    free(g_debugOutputPath); g_debugOutputPath = NULL;
    free(g_lastDebugSource); g_lastDebugSource = NULL;
    free(g_lastDebugState); g_lastDebugState = NULL;
    g_lastDebugLine = -1;
    g_activeDebuggerKind = 0;
    clear_debug_execution_markers();
    if (hadSession) set_debug_panel_status(L"Debug session ended");
}

static void start_debug_location_tracking(const WCHAR *locationPath,
                                          const WCHAR *workingDirectory,
                                          int debuggerKind, HANDLE process, HANDLE job,
                                          const WCHAR *commandPath,
                                          const WCHAR *outputPath) {
    stop_debug_location_tracking();
    g_debugLocationPath = wcsdup_heap(locationPath);
    g_debugWorkingDirectory = wcsdup_heap(workingDirectory);
    g_debugCommandPath = wcsdup_heap(commandPath);
    g_debugOutputPath = wcsdup_heap(outputPath);
    g_debugProcess = process;
    g_debugJob = job;
    g_activeDebuggerKind = debuggerKind;
    if (g_hDebugVariables) ListView_DeleteAllItems(g_hDebugVariables);
    set_debug_panel_status(L"Debugger starting...");
    show_debug_panel();
    if (g_hMainWnd) SetTimer(g_hMainWnd, IDT_DEBUG_LOCATION, DEBUG_LOCATION_POLL_MS, NULL);
}

/* Starts a debugger's PowerShell host inside a JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
   job, so closing the job (stop_debug_location_tracking(), including when
   MarkEdit exits) ends the shell, the debugger, and the program being
   debugged together. The process is created suspended and only resumed
   once it's in the job, so nothing it starts can be launched outside it.
   Returns the process handle, with the job in *outJob (NULL if a job
   couldn't be set up - the session then runs without one). */
static HANDLE launch_debug_console(const WCHAR *executable, const WCHAR *parameters,
                                   const WCHAR *directory, int showWindow, HANDLE *outJob) {
    *outJob = NULL;
    size_t capacity = wcslen(executable) + wcslen(parameters) + 4;
    WCHAR *commandLine = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!commandLine) return NULL;
    swprintf(commandLine, capacity, L"\"%ls\" %ls", executable, parameters);

    STARTUPINFOW startup = {0};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = (WORD)showWindow;
    PROCESS_INFORMATION process = {0};
    BOOL started = CreateProcessW(NULL, commandLine, NULL, NULL, FALSE,
                                  CREATE_SUSPENDED | CREATE_NEW_CONSOLE, NULL, directory,
                                  &startup, &process);
    free(commandLine);
    if (!started) return NULL;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo = {0};
    jobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job && (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                         &jobInfo, sizeof(jobInfo)) ||
                !AssignProcessToJobObject(job, process.hProcess))) {
        CloseHandle(job);
        job = NULL;
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    *outJob = job;
    return process.hProcess;
}

static WCHAR *debug_location_to_wide(const char *text) {
    WCHAR *wide = to_wide(text);
    if (wide) return wide;
    int length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
    if (length <= 0) return NULL;
    wide = (WCHAR *)malloc((size_t)length * sizeof(WCHAR));
    if (!wide || !MultiByteToWideChar(CP_ACP, 0, text, -1, wide, length)) {
        free(wide);
        return NULL;
    }
    return wide;
}

static int parse_debug_location(char *text, WCHAR **sourceOut, int *lineOut) {
    if (!text || !sourceOut || !lineOut) return 0;
    char *source = text;
    char *separator = strchr(text, '\n');
    if (!separator) return 0;
    *separator = 0;
    char *lineText = separator + 1;
    /* GDB's `frame` output always starts with the "#N" frame number;
       matching on " at " too would misidentify a pdb sentinel (a bare
       "path\nline") whose path happens to contain that substring. */
    if (text[0] == '#') {
        *separator = '\n';
        char *at = strstr(text, " at ");
        if (!at) return 0;
        source = at + 4;
        char *end = strchr(source, '\n');
        if (end) *end = 0;
        char *colon = strrchr(source, ':');
        if (!colon) return 0;
        *colon = 0;
        lineText = colon + 1;
    }
    char *endNumber = NULL;
    long line = strtol(lineText, &endNumber, 10);
    if (endNumber == lineText || line <= 0 || line > INT_MAX) return 0;
    size_t sourceLength = strlen(source);
    while (sourceLength && (source[sourceLength - 1] == '\r' || source[sourceLength - 1] == ' '))
        source[--sourceLength] = 0;
    WCHAR *wide = debug_location_to_wide(source);
    if (!wide) return 0;
    for (WCHAR *character = wide; *character; character++)
        if (*character == L'/') *character = L'\\';
    WCHAR *resolved = PathIsRelativeW(wide) && g_debugWorkingDirectory
        ? path_join(g_debugWorkingDirectory, wide) : wcsdup_heap(wide);
    free(wide);
    if (!resolved) return 0;
    WCHAR *absolute = absolute_path(resolved);
    free(resolved);
    *sourceOut = absolute;
    *lineOut = (int)line;
    return absolute != NULL;
}

static void poll_debug_location(void) {
    if (!g_debugLocationPath) return;
    if (g_debugProcess && WaitForSingleObject(g_debugProcess, 0) == WAIT_OBJECT_0) {
        stop_debug_location_tracking();
        return;
    }
    int length = 0;
    char *text = read_bytes_file(g_debugLocationPath, &length);
    if (!text) {
        /* A debugger may have the file briefly open while replacing its
           contents. Only a genuinely missing file means the session ended. */
        if (file_exists(g_debugLocationPath)) return;
        stop_debug_location_tracking();
        return;
    }
    /* GDB invokes hook-stop once more after the inferior exits. At that
       point `frame` reports "No stack" instead of a source location. Treat
       that sentinel as session completion; closing the job also closes the
       otherwise-idle hidden GDB prompt. */
    if (g_activeDebuggerKind == 2 && strstr(text, "Error while running hook_stop:") &&
        strstr(text, "No stack.")) {
        free(text);
        stop_debug_location_tracking();
        return;
    }
    if (g_lastDebugState && strcmp(g_lastDebugState, text) == 0) {
        free(text);
        return;
    }
    char *stateCopy = (char *)malloc((size_t)length + 1);
    if (stateCopy) {
        memcpy(stateCopy, text, (size_t)length + 1);
        free(g_lastDebugState);
        g_lastDebugState = stateCopy;
    }
/* Order matters: the variables section is located with strstr() from the start
       of the snapshot, so it has to run before parse_debug_location()
       NUL-terminates the snapshot just after the leading "file\nline". In
       exchange it only rewrites separators at or after the marker, which
       leaves the location prefix both backends put first intact. */
    update_debug_variables(text);
    WCHAR *source = NULL;
    int line = 0;
    int parsed = parse_debug_location(text, &source, &line);
    free(text);
    if (!parsed) return;
    WCHAR status[MAX_PATH + 64];
    swprintf(status, sizeof(status) / sizeof(status[0]), L"Paused at %ls:%d",
             PathFindFileNameW(source), line);
    set_debug_panel_status(status);
    if (g_lastDebugSource && _wcsicmp(g_lastDebugSource, source) == 0 &&
        g_lastDebugLine == line) {
        free(source);
        return;
    }
    if (!file_exists(source) || !open_file_in_tab(source)) {
        free(source);
        return;
    }
    clear_debug_execution_markers();
    SendMessageA(g_hEdit, SCI_MARKERADD, (WPARAM)(line - 1), MARKER_DEBUG_LINE);
    SendMessageA(g_hEdit, SCI_MARKERADD, (WPARAM)(line - 1), MARKER_DEBUG_ARROW);
    SendMessageA(g_hEdit, SCI_ENSUREVISIBLE, (WPARAM)(line - 1), 0);
    Sci_Position visibleLines = (Sci_Position)SendMessageA(g_hEdit, SCI_LINESONSCREEN, 0, 0);
    SendMessageA(g_hEdit, SCI_SETFIRSTVISIBLELINE,
                 (WPARAM)(line > visibleLines / 2 ? line - visibleLines / 2 - 1 : 0), 0);
    replace_wstring(&g_lastDebugSource, source);
    g_lastDebugLine = line;
    free(source);
}

static int send_debug_console_command(const WCHAR *command) {
    if (!g_debugProcess || WaitForSingleObject(g_debugProcess, 0) != WAIT_TIMEOUT)
        return 0;
    DWORD processId = GetProcessId(g_debugProcess);
    if (!processId) return 0;
    if (!AttachConsole(processId)) {
        if (GetLastError() != ERROR_ACCESS_DENIED) return 0;
        FreeConsole();
        if (!AttachConsole(processId)) return 0;
    }
    HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, 0, NULL);
    int ok = 0;
    if (input != INVALID_HANDLE_VALUE) {
        size_t length = wcslen(command);
        INPUT_RECORD *records = (INPUT_RECORD *)calloc(length * 2 + 2, sizeof(INPUT_RECORD));
        if (records) {
            DWORD count = 0;
            for (size_t i = 0; i < length; i++) {
                records[count].EventType = KEY_EVENT;
                records[count].Event.KeyEvent.bKeyDown = TRUE;
                records[count].Event.KeyEvent.wRepeatCount = 1;
                records[count++].Event.KeyEvent.uChar.UnicodeChar = command[i];
                records[count] = records[count - 1];
                records[count++].Event.KeyEvent.bKeyDown = FALSE;
            }
            records[count].EventType = KEY_EVENT;
            records[count].Event.KeyEvent.bKeyDown = TRUE;
            records[count].Event.KeyEvent.wRepeatCount = 1;
            records[count].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
            records[count++].Event.KeyEvent.uChar.UnicodeChar = L'\r';
            records[count] = records[count - 1];
            records[count++].Event.KeyEvent.bKeyDown = FALSE;
            DWORD inserted = 0;
            ok = WriteConsoleInputW(input, records, count, &inserted) && inserted == count;
            free(records);
        }
        CloseHandle(input);
    }
    FreeConsole();
    return ok;
}

static void run_debugger_command(int action) {
    const WCHAR *command = NULL;
    if (g_activeDebuggerKind == 1)
        command = action == IDM_DEBUG_STEP_OVER ? L"nv" :
                  action == IDM_DEBUG_STEP_INTO ? L"sv" :
                  action == IDM_DEBUG_STEP_OUT ? L"rv" : L"cv";
    else if (g_activeDebuggerKind == 2)
        command = action == IDM_DEBUG_STEP_OVER ? L"next" :
                  action == IDM_DEBUG_STEP_INTO ? L"step" :
                  action == IDM_DEBUG_STEP_OUT ? L"finish" : L"continue";
    if (!command || !send_debug_console_command(command))
        MessageBoxW(g_hMainWnd, L"Start a debug session with F5 before using this command.",
                    L"Debugging", MB_OK | MB_ICONINFORMATION);
}

static WCHAR *python_double_quoted_literal(const WCHAR *value) {
    size_t length = wcslen(value);
    WCHAR *literal = (WCHAR *)malloc((length * 2 + 3) * sizeof(WCHAR));
    if (!literal) return NULL;
    size_t output = 0;
    literal[output++] = L'"';
    for (size_t i = 0; i < length; i++) {
        if (value[i] == L'\\' || value[i] == L'"') literal[output++] = L'\\';
        literal[output++] = value[i];
    }
    literal[output++] = L'"';
    literal[output] = 0;
    return literal;
}

/* Writes `text` to `path` as UTF-8, replacing the file. */
static int write_utf8_file(const WCHAR *path, const WCHAR *text) {
    char *utf8 = to_utf8(text);
    if (!utf8) return 0;
    int result = write_bytes_file(path, utf8, strlen(utf8));
    free(utf8);
    return result;
}

static int write_pdb_driver(const WCHAR *path, const WCHAR *statePath, const WCHAR *scriptPath,
                            const int *breakpointLines, int breakpointCount) {
    WCHAR *stateLiteral = python_double_quoted_literal(statePath);
    WCHAR *scriptLiteral = python_double_quoted_literal(scriptPath);
    if (!stateLiteral || !scriptLiteral) {
        free(stateLiteral); free(scriptLiteral);
        return 0;
    }
    size_t capacity = wcslen(stateLiteral) + wcslen(scriptLiteral) +
                      (size_t)breakpointCount * 64 + 5120;
    WCHAR *driver = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!driver) {
        free(stateLiteral); free(scriptLiteral);
        return 0;
    }
    /* Two Python-3.13+ pdb quirks this driver has to work around:
       1. debugger._run(pdb._ScriptTarget(...)) (used below when available)
          clears the driver's own __main__ globals before running the target
          script, so a plain module-level lookup of STATE/pprint from inside
          interaction() - called back later, once the script is running -
          raises NameError. Capturing them as default-argument values bakes
          them into the function object at *definition* time instead, which
          survives the globals wipe.
       2. pdb._ScriptTarget resolves the script path through realpath-style
          normalization for the code object it compiles, so a breakpoint set
          against the path as given (which can be an 8.3 short name, e.g. for
          a file under a long-username or %TEMP%-style directory) silently
          never matches. Normalizing SCRIPT the same way before set_break()
          keeps both in agreement regardless of which path form was passed in. */
    int written = swprintf(driver, capacity,
        L"import os, pdb, pprint, sys\n"
        L"STATE = %ls\n"
        L"SCRIPT = %ls\n"
        L"SCRIPT = os.path.realpath(SCRIPT)\n"
        L"class MarkEditPdb(pdb.Pdb):\n"
        L"    def interaction(self, frame, traceback, _markedit_state=STATE, _markedit_pprint=pprint):\n"
        L"        try:\n"
        L"            with open(_markedit_state, 'w', encoding='utf-8') as stream:\n"
        L"                stream.write(frame.f_code.co_filename + '\\n' + str(frame.f_lineno) + "
        L"'\\n__MARKEDIT_VARIABLES__\\n')\n"
        L"                for key in sorted(frame.f_locals, key=str):\n"
        L"                    if str(key).startswith('__'):\n"
        L"                        continue\n"
        L"                    value = frame.f_locals[key]\n"
        L"                    try:\n"
        L"                        rendered = _markedit_pprint.pformat(value, width=100, compact=True)\n"
        L"                    except Exception as error:\n"
        L"                        rendered = '<unavailable: ' + repr(error) + '>'\n"
        L"                    rendered = rendered.replace('\\\\', '\\\\\\\\').replace('\\t', '\\\\t').replace('\\r', '\\\\r').replace('\\n', '\\\\n')\n"
        L"                    if len(rendered) > 2000:\n"
        L"                        rendered = rendered[:1997] + '...'\n"
        L"                    stream.write(str(key) + '\\t' + type(value).__name__ + '\\t' + rendered + '\\n')\n"
        L"        except OSError:\n"
        L"            pass\n"
        L"        print('\\nLocals:')\n"
        L"        _markedit_pprint.pprint({key: value for key, value in frame.f_locals.items() "
        L"if not key.startswith('__')})\n"
        L"        return super().interaction(frame, traceback)\n"
        L"debugger = MarkEditPdb()\n"
        L"debugger.rcLines.extend([\n"
        L"    'alias vars pp locals()',\n"
        L"    'alias globals pp globals()',\n"
        L"    'alias nv next',\n"
        L"    'alias sv step',\n"
        L"    'alias rv return',\n"
        L"    'alias cv continue',\n"
        L"])\n",
        stateLiteral, scriptLiteral);
    free(stateLiteral); free(scriptLiteral);
    if (written < 0) {
        free(driver);
        return 0;
    }
    size_t used = (size_t)written;
    for (int i = 0; i < breakpointCount; i++) {
        written = swprintf(driver + used, capacity - used,
                           L"debugger.set_break(SCRIPT, %d)\n", breakpointLines[i] + 1);
        if (written < 0) { free(driver); return 0; }
        used += (size_t)written;
    }
    if (breakpointCount) {
        written = swprintf(driver + used, capacity - used,
                           L"debugger.rcLines.append('continue')\n");
        if (written < 0) { free(driver); return 0; }
        used += (size_t)written;
    }
    written = swprintf(driver + used, capacity - used,
        L"sys.argv = [SCRIPT]\n"
        L"if hasattr(pdb, '_ScriptTarget') and hasattr(debugger, '_run'):\n"
        L"    debugger._run(pdb._ScriptTarget(SCRIPT))\n"
        L"elif hasattr(debugger, '_runscript'):\n"
        L"    debugger._runscript(SCRIPT)\n"
        L"else:\n"
        L"    namespace = {'__name__': '__main__', '__file__': SCRIPT}\n"
        L"    debugger.run(compile(open(SCRIPT, 'rb').read(), SCRIPT, 'exec'), namespace, namespace)\n");
    if (written < 0) { free(driver); return 0; }
    int result = write_utf8_file(path, driver);
    free(driver);
    return result;
}

/* Shared cleanup for launch_python_file()'s several early-exit points. */
static void free_python_launch_buffers(WCHAR *directory, WCHAR *debugLocation, WCHAR *pdbDriver,
                                        WCHAR *quotedPython, WCHAR *quotedScript,
                                        WCHAR *quotedDebugLocation, WCHAR *quotedPdbDriver) {
    free(directory);
    free(debugLocation);
    free(pdbDriver);
    free(quotedPython);
    free(quotedScript);
    free(quotedDebugLocation);
    free(quotedPdbDriver);
}

static void launch_python_file(int debug) {
    /* launch_active_source_file() already saved g_currentFile and checked
       that it's a Python file before calling here. */
    WCHAR *python = find_python_interpreter(g_currentFile);
    if (!python) {
        MessageBoxW(g_hMainWnd,
                    L"No Python interpreter was found. Install Python 3, activate a virtual "
                    L"environment, or create .venv beside the project.",
                    debug ? L"Debug Python" : L"Run Python",
                    MB_OK | MB_ICONERROR);
        return;
    }
    WCHAR *directory = path_directory(g_currentFile);
    int usePython3Launcher = _wcsicmp(PathFindFileNameW(python), L"py.exe") == 0;
    WCHAR *debugLocation = debug ? debug_temporary_path(L"location") : NULL;
    WCHAR *pdbDriver = debug ? debug_temporary_path(L"py") : NULL;
    WCHAR *quotedPython = powershell_literal(python);
    WCHAR *quotedScript = powershell_literal(g_currentFile);
    WCHAR *quotedDebugLocation = debugLocation ? powershell_literal(debugLocation) : NULL;
    WCHAR *quotedPdbDriver = pdbDriver ? powershell_literal(pdbDriver) : NULL;
    free(python);
    if (!directory || !quotedPython || !quotedScript ||
        (debug && (!debugLocation || !pdbDriver || !quotedDebugLocation || !quotedPdbDriver))) {
        free_python_launch_buffers(directory, debugLocation, pdbDriver, quotedPython,
                                    quotedScript, quotedDebugLocation, quotedPdbDriver);
        return;
    }

    int *breakpointLines = NULL;
    int breakpointCount = debug ? collect_breakpoint_lines(&breakpointLines) : 0;
    if (debug && (!write_pdb_driver(pdbDriver, debugLocation, g_currentFile,
                                     breakpointLines, breakpointCount) ||
                  !write_bytes_file(debugLocation, "starting\n", 9))) {
        free(breakpointLines);
        DeleteFileW(pdbDriver);
        DeleteFileW(debugLocation);
        free_python_launch_buffers(directory, debugLocation, pdbDriver, quotedPython,
                                    quotedScript, quotedDebugLocation, quotedPdbDriver);
        return;
    }
    free(breakpointLines);

    const WCHAR *launcherArgument = usePython3Launcher ? L" -3" : L"";
    WCHAR *parameters = NULL;
    if (debug) {
        size_t capacity = wcslen(quotedPython) + wcslen(quotedDebugLocation) +
                          wcslen(quotedPdbDriver) * 2 + 1536;
        parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
        if (parameters && swprintf(parameters, capacity,
                L"-NoLogo -NoProfile -NoExit -Command \"Write-Host "
                L"'pdb: F10/n next, F11/s step, Shift+F11/r return, F6/c continue; vars, globals, b <line>, p <expr>, where, q'; "
                L"& %ls%ls '-c' 'import runpy,sys; "
                L"runpy.run_path(sys.argv[1],run_name=\"markedit_pdb_driver\")' %ls; "
                L"try { [System.IO.File]::Delete(%ls) } catch {}; "
                L"try { [System.IO.File]::Delete(%ls) } catch {}\"",
                quotedPython, launcherArgument, quotedPdbDriver,
                quotedPdbDriver, quotedDebugLocation) < 0) {
            free(parameters);
            parameters = NULL;
        }
    } else {
        parameters = run_script_parameters(quotedPython, launcherArgument, quotedScript);
    }

    int filesHandedOff = 0;
    WCHAR *powershell = parameters ? locate_powershell() : NULL;
    if (powershell && debug) {
        HANDLE job = NULL;
        HANDLE process = launch_debug_console(powershell, parameters, directory,
                                              SW_SHOWNORMAL, &job);
        if (!process) {
            MessageBoxW(g_hMainWnd, L"The Python console could not be started.",
                        L"Debug Python", MB_OK | MB_ICONERROR);
        } else {
            filesHandedOff = 1;
            start_debug_location_tracking(debugLocation, directory, 1, process, job,
                                          pdbDriver, NULL);
        }
    } else if (powershell) {
        launch_run_command(powershell, parameters, directory,
                     L"The Python console could not be started.", L"Run Python");
    }
    free(powershell);

    if (debug && !filesHandedOff) {
        DeleteFileW(pdbDriver);
        DeleteFileW(debugLocation);
    }
    free(parameters);
    free_python_launch_buffers(directory, debugLocation, pdbDriver, quotedPython,
                                quotedScript, quotedDebugLocation, quotedPdbDriver);
}

/* Lua is Run-only: there's no bundled debugger the way GDB and pdb back
   C/C++ and Python (a standalone Lua debugger isn't something you can just
   find on PATH the way those are), so this only ever runs the script. */
static WCHAR *find_lua_interpreter(void) {
    static const WCHAR *names[] = { L"lua.exe", L"lua5.4.exe", L"lua54.exe",
                                     L"lua5.3.exe", L"lua53.exe", L"lua5.1.exe", L"luajit.exe" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        WCHAR *lua = g_luaBinOverride ? existing_file_in_directory(g_luaBinOverride, names[i])
                                       : executable_on_search_path(names[i]);
        if (lua) return lua;
    }
    return NULL;
}

static void launch_lua_file(void) {
    WCHAR *lua = find_lua_interpreter();
    if (!lua) {
        MessageBoxW(g_hMainWnd,
                    L"No Lua interpreter was found. Install Lua (or LuaJIT) and add it to PATH.",
                    L"Run Lua", MB_OK | MB_ICONERROR);
        return;
    }
    WCHAR *directory = path_directory(g_currentFile);
    WCHAR *quotedLua = powershell_literal(lua);
    WCHAR *quotedScript = powershell_literal(g_currentFile);
    free(lua);
    if (!directory || !quotedLua || !quotedScript) {
        free(directory); free(quotedLua); free(quotedScript);
        return;
    }

    WCHAR *parameters = run_script_parameters(quotedLua, L"", quotedScript);
    WCHAR *powershell = parameters ? locate_powershell() : NULL;
    if (powershell)
        launch_run_command(powershell, parameters, directory,
                     L"The Lua console could not be started.", L"Run Lua");
    free(powershell);
    free(parameters);
    free(directory);
    free(quotedLua);
    free(quotedScript);
}

typedef enum {
    NATIVE_COMPILER_GCC = 0, NATIVE_COMPILER_CLANG, NATIVE_COMPILER_ZIG, NATIVE_COMPILER_MSVC
} NativeCompilerKind;

/* MSVC has no single "cl.exe" to look up on PATH the way GCC/Clang/Zig do:
   its compiler only works once vcvarsall.bat has set up INCLUDE/LIB/PATH for
   a specific architecture, and it isn't installed on PATH by default. Rather
   than shelling out to vswhere.exe (adds a process-capture dependency for a
   value that rarely changes), check the standard install layout directly.
   Visual Studio itself always lands under the 32-bit Program Files directory
   even on 64-bit Windows; standalone Build Tools installs sometimes use the
   64-bit one, so both are checked. */
static WCHAR *find_vcvarsall(void) {
    if (g_vcvarsallOverridePath) return wcsdup_heap(g_vcvarsallOverridePath);
    static const WCHAR *years[] = { L"2022", L"2019" };
    static const WCHAR *editions[] = { L"Enterprise", L"Professional", L"Community", L"BuildTools" };
    static const WCHAR *envNames[] = { L"ProgramFiles(x86)", L"ProgramFiles" };
    for (int e = 0; e < 2; e++) {
        WCHAR programFiles[MAX_PATH];
        /* A return of 0 is "not set"; a return >= the buffer size means the
           value didn't fit and the buffer's contents are left undefined -
           both mean "skip this one", not just the zero case. */
        DWORD len = GetEnvironmentVariableW(envNames[e], programFiles, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) continue;
        for (int y = 0; y < (int)(sizeof(years) / sizeof(years[0])); y++) {
            for (int ed = 0; ed < (int)(sizeof(editions) / sizeof(editions[0])); ed++) {
                WCHAR path[MAX_PATH * 2];
                swprintf(path, MAX_PATH * 2,
                    L"%ls\\Microsoft Visual Studio\\%ls\\%ls\\VC\\Auxiliary\\Build\\vcvarsall.bat",
                    programFiles, years[y], editions[ed]);
                if (PathFileExistsW(path)) return wcsdup_heap(path);
            }
        }
    }
    return NULL;
}

/* g_compilerBackend (see load_compiler_settings()) either pins the search to
   one compiler or leaves it Auto, trying GCC, Clang, then Zig in that order
   and falling back to MSVC only for Run (never Debug: there's no GDB-
   readable debug info from cl.exe, so Debug requires a GCC- or Clang-style
   compiler same as before this setting existed). Each of GCC/Clang/Zig also
   has its own optional folder override (Settings > Compiler), searched
   instead of PATH exactly like g_vcvarsallOverridePath already does for
   MSVC's vcvarsall.bat. */
static WCHAR *find_native_compiler(DebugSourceKind kind, int debug, NativeCompilerKind *compilerKind) {
    static const WCHAR *cNames[]   = { L"gcc.exe", L"clang.exe", L"zig.exe" };
    static const WCHAR *cppNames[] = { L"g++.exe", L"clang++.exe", L"zig.exe" };
    static const NativeCompilerKind kinds[] = { NATIVE_COMPILER_GCC, NATIVE_COMPILER_CLANG, NATIVE_COMPILER_ZIG };
    const WCHAR **names = kind == DEBUG_SOURCE_C ? cNames : cppNames;

    *compilerKind = NATIVE_COMPILER_GCC;

    if (g_compilerBackend == COMPILER_BACKEND_MSVC) {
        if (debug) return NULL;
        WCHAR *vcvarsall = find_vcvarsall();
        if (vcvarsall) *compilerKind = NATIVE_COMPILER_MSVC;
        return vcvarsall;
    }

    for (int i = 0; i < 3; i++) {
        if (debug && kinds[i] == NATIVE_COMPILER_ZIG) continue;
        if (g_compilerBackend != COMPILER_BACKEND_AUTO) {
            CompilerBackendPreference want =
                kinds[i] == NATIVE_COMPILER_GCC ? COMPILER_BACKEND_GCC :
                kinds[i] == NATIVE_COMPILER_CLANG ? COMPILER_BACKEND_CLANG : COMPILER_BACKEND_ZIG;
            if (g_compilerBackend != want) continue;
        }
        const WCHAR *override = kinds[i] == NATIVE_COMPILER_GCC ? g_gccBinOverride :
            kinds[i] == NATIVE_COMPILER_CLANG ? g_clangBinOverride : g_zigBinOverride;
        WCHAR *compiler = override ? existing_file_in_directory(override, names[i])
                                    : executable_on_search_path(names[i]);
        if (compiler) {
            *compilerKind = kinds[i];
            return compiler;
        }
    }

    if (g_compilerBackend == COMPILER_BACKEND_AUTO && !debug) {
        WCHAR *vcvarsall = find_vcvarsall();
        if (vcvarsall) {
            *compilerKind = NATIVE_COMPILER_MSVC;
            return vcvarsall;
        }
    }
    return NULL;
}

static WCHAR *gdb_source_path(const WCHAR *path) {
    WCHAR *copy = wcsdup_heap(path);
    if (!copy) return NULL;
    for (WCHAR *character = copy; *character; character++)
        if (*character == L'\\') *character = L'/';
    return copy;
}

/* Multi-line GDB commands such as `define hook-stop` must come from a command
   file. Supplying each line through a separate -ex leaves GDB waiting at its
   interactive definition prompt. The hook makes every next/step/continue stop
   show all arguments and in-scope local variables without another command. */
static int write_gdb_commands(const WCHAR *path, const WCHAR *sourcePath, const WCHAR *locationPath,
                              const int *breakpointLines, int breakpointCount) {
    size_t capacity = wcslen(locationPath) + 1024 +
                      (size_t)breakpointCount * (wcslen(sourcePath) + 64);
    WCHAR *commands = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!commands) return 0;
    int written = swprintf(commands, capacity,
        L"set pagination off\n"
        /* GDB stays hidden, but its inferior gets an independent visible
           console so the program's own output and input remain accessible. */
        L"set new-console on\n"
        L"set width 0\n"
        L"set breakpoint pending on\n"
        L"set print pretty on\n"
        L"set print object on\n"
        L"set logging file %ls\n"
        L"set logging overwrite on\n"
        L"set logging redirect on\n"
        L"define vars\n"
        L"  echo \\nArguments:\\n\n"
        L"  info args\n"
        L"  echo Locals:\\n\n"
        L"  info locals\n"
        L"end\n"
        L"define hook-stop\n"
        L"  set logging enabled on\n"
        L"  frame\n"
        L"  set print pretty off\n"
        L"  echo \\n__MARKEDIT_VARIABLES__\\n\n"
        L"  echo Arguments:\\n\n"
        L"  info args\n"
        L"  echo Locals:\\n\n"
        L"  info locals\n"
        L"  set logging enabled off\n"
        L"  set print pretty on\n"
        L"  vars\n"
        L"end\n", locationPath);
    if (written < 0) {
        free(commands);
        return 0;
    }
    size_t used = (size_t)written;
    for (int i = 0; i < breakpointCount; i++) {
        written = swprintf(commands + used, capacity - used,
                           L"break %ls:%d\n", sourcePath, breakpointLines[i] + 1);
        if (written < 0) {
            free(commands);
            return 0;
        }
        used += (size_t)written;
    }
    written = swprintf(commands + used, capacity - used,
                       breakpointCount ? L"run\n" : L"start\n");
    if (written < 0) {
        free(commands);
        return 0;
    }
    int result = write_utf8_file(path, commands);
    free(commands);
    return result;
}

/* MSVC needs its own launch path rather than reusing the PowerShell one
   below: cl.exe only works once vcvarsall.bat has set up INCLUDE/LIB/PATH
   for a target architecture, and a batch file's environment changes don't
   propagate out to a parent PowerShell session the way they would to a
   further command chained in the same cmd.exe process. So the whole
   build+run+cleanup chain runs as a single cmd.exe command instead.

   cmd.exe has no quoting that is safe for arbitrary text - %VAR% expands
   even inside "...", and so does !VAR! under delayed expansion - so no path
   or flag is spliced into the command line. Each is handed to the child
   through an environment variable (set here just around the launch, which
   the console inherits) and referenced as !MARKEDIT_...!: delayed
   expansion runs after cmd has parsed quotes, & | < > and %-variables, and
   it doesn't rescan what it substitutes. /v:on is required for that, and
   for !errorlevel! to report the run's exit code rather than the value from
   when the line was parsed; setlocal has no effect outside a batch file.
   The one exception is vcvarsall.bat: `call` re-expands its argument, so a
   VS install path containing ! or % still can't be used. */
static const WCHAR *const MSVC_ENVIRONMENT[] = {
    L"MARKEDIT_VCVARS", L"MARKEDIT_SOURCE", L"MARKEDIT_OUTPUT", L"MARKEDIT_FLAGS",
};

static void launch_msvc_file(DebugSourceKind kind, WCHAR *vcvarsallPath) {
    WCHAR *output = debug_temporary_path(L"exe");
    WCHAR *directory = path_directory(g_currentFile);
    WCHAR *extraFlags = quote_extra_compiler_flags(g_compilerExtraFlags, FLAG_QUOTE_ARGV);
    const WCHAR *values[] = { vcvarsallPath, g_currentFile, output, extraFlags };
    int ready = output && directory;
    for (size_t i = 0; ready && i < sizeof(values) / sizeof(values[0]); i++)
        if (values[i] && !SetEnvironmentVariableW(MSVC_ENVIRONMENT[i], values[i])) ready = 0;

    /* The leading/trailing quote wrapping the whole /k (or /c) argument is cmd.exe's
       documented workaround for a command line that itself contains further
       quoted substrings: when the switch's remainder starts and ends with
       a quote character, cmd strips only that outer pair and leaves
       everything between - including each inner "..." - untouched. */
    WCHAR command[512];
    if (ready &&
        swprintf(command, sizeof(command) / sizeof(command[0]),
                 L"/v:on %ls \"call \"!MARKEDIT_VCVARS!\" x64 >nul && "
                 L"echo Building %ls... && "
                 L"cl.exe /nologo %ls%ls /O2 /EHsc /Fe:\"!MARKEDIT_OUTPUT!\" \"!MARKEDIT_SOURCE!\" && "
                 L"(\"!MARKEDIT_OUTPUT!\" & echo. & echo Process exited with code !errorlevel!) & "
                 L"del /f /q \"!MARKEDIT_OUTPUT!\" >nul 2>nul\"",
                 g_runInConsole ? L"/k" : L"/c",
                 kind == DEBUG_SOURCE_C ? L"C" : L"C++",
                 kind == DEBUG_SOURCE_C ? L"/std:c11" : L"/std:c++17",
                 extraFlags ? L" !MARKEDIT_FLAGS!" : L"") > 0) {
        launch_run_command(L"cmd.exe", command, directory,
                     L"The C/C++ build console could not be started.", L"Run C/C++");
    }
    for (size_t i = 0; i < sizeof(MSVC_ENVIRONMENT) / sizeof(MSVC_ENVIRONMENT[0]); i++)
        SetEnvironmentVariableW(MSVC_ENVIRONMENT[i], NULL);
    free(vcvarsallPath);
    free(extraFlags);
    free(output);
    free(directory);
}

/* Everything one C/C++ Run or Debug launch needs, owned until
   free_native_launch(). The quoted* fields are PowerShell literals. */
typedef struct NativeLaunch {
    WCHAR *directory, *output, *gdbCommands, *debugLocation;
    WCHAR *sourceForGdb, *locationForGdb, *extraFlags;
    WCHAR *quotedCompiler, *quotedDebugger, *quotedSource, *quotedOutput;
    WCHAR *quotedGdbCommands, *quotedDebugLocation;
} NativeLaunch;

static void free_native_launch(NativeLaunch *launch) {
    WCHAR *fields[] = {
        launch->directory, launch->output, launch->gdbCommands, launch->debugLocation,
        launch->sourceForGdb, launch->locationForGdb, launch->extraFlags,
        launch->quotedCompiler, launch->quotedDebugger, launch->quotedSource, launch->quotedOutput,
        launch->quotedGdbCommands, launch->quotedDebugLocation,
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) free(fields[i]);
}

/* `debugger` is NULL for Run. Returns 0 when anything the launch needs
   couldn't be allocated. */
static int prepare_native_launch(NativeLaunch *launch, const WCHAR *source,
                                 const WCHAR *compiler, const WCHAR *debugger) {
    memset(launch, 0, sizeof(*launch));
    launch->directory = path_directory(source);
    launch->output = debug_temporary_path(L"exe");
    launch->sourceForGdb = gdb_source_path(source);
    launch->extraFlags = quote_extra_compiler_flags(g_compilerExtraFlags, FLAG_QUOTE_POWERSHELL);
    launch->quotedCompiler = powershell_literal(compiler);
    launch->quotedSource = powershell_literal(source);
    launch->quotedOutput = launch->output ? powershell_literal(launch->output) : NULL;
    int ok = launch->directory && launch->output && launch->sourceForGdb &&
             launch->quotedCompiler && launch->quotedSource && launch->quotedOutput;
    if (!debugger) return ok;
    launch->gdbCommands = debug_temporary_path(L"gdb");
    launch->debugLocation = debug_temporary_path(L"location");
    launch->locationForGdb = launch->debugLocation ? gdb_source_path(launch->debugLocation) : NULL;
    launch->quotedDebugger = powershell_literal(debugger);
    launch->quotedGdbCommands = launch->gdbCommands ? powershell_literal(launch->gdbCommands) : NULL;
    launch->quotedDebugLocation = launch->debugLocation ? powershell_literal(launch->debugLocation) : NULL;
    return ok && launch->locationForGdb && launch->quotedDebugger &&
           launch->quotedGdbCommands && launch->quotedDebugLocation;
}

/* The GDB command file, carrying the active document's breakpoints, and the
   location file the session starts from. */
static int write_native_debug_files(const NativeLaunch *launch) {
    int *breakpointLines = NULL;
    int breakpointCount = collect_breakpoint_lines(&breakpointLines);
    int ok = write_gdb_commands(launch->gdbCommands, launch->sourceForGdb, launch->locationForGdb,
                                breakpointLines, breakpointCount) &&
             write_bytes_file(launch->debugLocation, "starting\n", 9);
    free(breakpointLines);
    return ok;
}

/* Appends to `buffer` at *used. Returns 0 when the text didn't fit. */
static int append_format(WCHAR *buffer, size_t capacity, size_t *used, const WCHAR *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int written = vswprintf(buffer + *used, capacity - *used, format, arguments);
    va_end(arguments);
    if (written < 0) return 0;
    *used += (size_t)written;
    return 1;
}

static size_t optional_length(const WCHAR *text) {
    return text ? wcslen(text) : 0;
}

/* The PowerShell command line that builds the source, then either runs it
   and reports its exit code or debugs it under GDB, and finally deletes the
   build output and any debugger files. */
static WCHAR *native_launch_parameters(const NativeLaunch *launch, DebugSourceKind kind,
                                       NativeCompilerKind compilerKind, int debug) {
    size_t capacity = wcslen(launch->quotedCompiler) + wcslen(launch->quotedSource) +
                      wcslen(launch->quotedOutput) + optional_length(launch->quotedDebugger) +
                      optional_length(launch->quotedGdbCommands) +
                      optional_length(launch->quotedDebugLocation) +
                      optional_length(launch->extraFlags) + 2048;
    WCHAR *parameters = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!parameters) return NULL;

    const WCHAR *compilerMode = compilerKind == NATIVE_COMPILER_ZIG
        ? (kind == DEBUG_SOURCE_C ? L" cc" : L" c++") : L"";
    const WCHAR *standard = kind == DEBUG_SOURCE_C ? L"-std=gnu11" : L"-std=gnu++17";
    const WCHAR *buildFlags = debug
        ? L"'-g' '-O0' '-fno-omit-frame-pointer'" : L"'-O2'";
    /* Debugger output and controls live in MarkEdit, so the host PowerShell
       console stays hidden for GDB sessions and exits with GDB. */
    const WCHAR *shellOptions = debug
        ? L"-NoLogo -NoProfile -WindowStyle Hidden -Command"
        : run_shell_options();
    size_t used = 0;
    int ok = append_format(parameters, capacity, &used,
        L"%ls \"Write-Host 'Building %ls...'; "
        L"& %ls%ls '%ls' %ls %ls '-o' %ls %ls; "
        L"if ($LASTEXITCODE -eq 0) { ",
        shellOptions, kind == DEBUG_SOURCE_C ? L"C" : L"C++", launch->quotedCompiler, compilerMode,
        standard, buildFlags, launch->extraFlags ? launch->extraFlags : L"",
        launch->quotedOutput, launch->quotedSource);
    if (debug)
        ok = ok && append_format(parameters, capacity, &used,
            L"Write-Host 'gdb: next/n, step/s, finish, continue/c, vars, print <expr>, bt, watch <expr>, quit'; "
            L"Write-Host 'Arguments and locals are shown automatically whenever execution stops.'; "
            L"& %ls '-q' '-x' %ls %ls; ",
            launch->quotedDebugger, launch->quotedGdbCommands, launch->quotedOutput);
    else
        ok = ok && append_format(parameters, capacity, &used,
            L"& %ls; $markeditExit = $LASTEXITCODE; Write-Host ''; "
            L"Write-Host ('Process exited with code ' + $markeditExit); ", launch->quotedOutput);
    /* Deleting the just-executed binary can transiently fail with "Access is
       denied" (Windows Defender's on-execute scan holds a brief lock on a
       freshly run .exe); a bare Delete() call would surface that as a scary
       PowerShell error right after the program's own output, even though the
       run itself succeeded. try/catch makes the best-effort cleanup silent. */
    if (debug)
        ok = ok && append_format(parameters, capacity, &used,
            L"}; try { [System.IO.File]::Delete(%ls) } catch {}; "
            L"try { [System.IO.File]::Delete(%ls) } catch {}; "
            L"try { [System.IO.File]::Delete(%ls) } catch {}\"",
            launch->quotedOutput, launch->quotedGdbCommands, launch->quotedDebugLocation);
    else
        ok = ok && append_format(parameters, capacity, &used,
            L"}; try { [System.IO.File]::Delete(%ls) } catch {}\"", launch->quotedOutput);
    if (!ok) {
        free(parameters);
        return NULL;
    }
    return parameters;
}

/* Runs `parameters` in PowerShell: a visible console for Run, or a hidden
   one tracked as the debug session for Debug. Returns 1 when the debug
   session took over the GDB command and location files. */
static int start_native_launch(const NativeLaunch *launch, const WCHAR *parameters, int debug) {
    WCHAR *powershell = locate_powershell();
    int handedOff = 0;
    if (powershell && debug) {
        HANDLE job = NULL;
        HANDLE process = launch_debug_console(powershell, parameters, launch->directory, SW_HIDE, &job);
        if (!process) {
            MessageBoxW(g_hMainWnd, L"The C/C++ build console could not be started.",
                        L"Debug C/C++", MB_OK | MB_ICONERROR);
        } else {
            handedOff = 1;
            start_debug_location_tracking(launch->debugLocation, launch->directory, 2, process, job,
                                          launch->gdbCommands, launch->output);
        }
    } else if (powershell) {
        launch_run_command(powershell, parameters, launch->directory,
                     L"The C/C++ build console could not be started.", L"Run C/C++");
    }
    free(powershell);
    return handedOff;
}

static void launch_native_file(const WCHAR *source, DebugSourceKind kind, int debug) {
    NativeCompilerKind compilerKind = NATIVE_COMPILER_GCC;
    WCHAR *compiler = find_native_compiler(kind, debug, &compilerKind);
    if (compiler && compilerKind == NATIVE_COMPILER_MSVC) {
        launch_msvc_file(kind, compiler);
        return;
    }
    WCHAR *debugger = debug ? find_gdb_executable() : NULL;
    if (!compiler || (debug && !debugger)) {
        const WCHAR *message =
            compiler ? L"GDB was not found. Choose a GDB folder in Settings, or add gdb.exe to PATH."
            : debug ? L"No GDB-compatible C/C++ compiler was found. Install GCC/G++ or Clang and add it to PATH."
            : L"No C/C++ compiler was found. Install GCC, Clang, Zig, or Visual Studio "
              L"(Desktop development with C++ workload) and add it to PATH.";
        MessageBoxW(g_hMainWnd, message, debug ? L"Debug C/C++" : L"Run C/C++", MB_OK | MB_ICONERROR);
        free(compiler);
        free(debugger);
        return;
    }

    NativeLaunch launch;
    int prepared = prepare_native_launch(&launch, source, compiler, debugger);
    free(compiler);
    free(debugger);

    int handedOff = 0;
    if (prepared && (!debug || write_native_debug_files(&launch))) {
        WCHAR *parameters = native_launch_parameters(&launch, kind, compilerKind, debug);
        if (parameters) handedOff = start_native_launch(&launch, parameters, debug);
        free(parameters);
    }
    if (!handedOff) {
        if (launch.gdbCommands) DeleteFileW(launch.gdbCommands);
        if (launch.debugLocation) DeleteFileW(launch.debugLocation);
    }
    free_native_launch(&launch);
}

static void launch_active_source_file(int debug) {
    if (!g_currentFile) {
        if (!do_save()) return;
    } else if (g_dirty && !save_to_path(g_currentFile)) {
        return;
    }
    DebugSourceKind kind = debug_source_kind(g_currentFile);
    if (kind == DEBUG_SOURCE_PYTHON) {
        launch_python_file(debug);
    } else if (kind == DEBUG_SOURCE_C || kind == DEBUG_SOURCE_CPP) {
        launch_native_file(g_currentFile, kind, debug);
    } else if (kind == DEBUG_SOURCE_LUA) {
        if (debug) {
            MessageBoxW(g_hMainWnd, L"Lua files support Run only; there is no Lua debugger integration.",
                        L"Debug File", MB_OK | MB_ICONINFORMATION);
        } else {
            launch_lua_file();
        }
    } else {
        MessageBoxW(g_hMainWnd,
                    L"Run supports .py, .pyw, .c, .cc, .cpp, .cxx, and .lua files. "
                    L"Debug supports all of those except .lua.",
                    debug ? L"Debug File" : L"Run File", MB_OK | MB_ICONINFORMATION);
    }
}
