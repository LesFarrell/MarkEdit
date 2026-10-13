/* ---- plugin host callbacks: the app's dialogs and the active document ---- */

#define PLUGIN_PROMPT_MAX_CHARS 4096

/* Text a plugin passes that isn't valid UTF-8 can't be shown, so the dialog
   is treated as cancelled. */
static char *plugin_prompt(const char *title, const char *label, const char *initial) {
    WCHAR *wideTitle = to_wide(title);
    WCHAR *wideLabel = to_wide(label);
    WCHAR *wideInitial = to_wide(initial);
    WCHAR *answer = (WCHAR *)malloc(PLUGIN_PROMPT_MAX_CHARS * sizeof(WCHAR));
    char *result = NULL;
    if (wideTitle && wideLabel && answer &&
        show_text_input(wideTitle, wideLabel, wideInitial, answer, PLUGIN_PROMPT_MAX_CHARS))
        result = to_utf8(answer);
    free(answer);
    free(wideInitial);
    free(wideLabel);
    free(wideTitle);
    return result;
}

static int plugin_choose(const char *title, const char *label, const char *const *items, int count) {
    WCHAR *wideTitle = to_wide(title);
    WCHAR *wideLabel = to_wide(label);
    WCHAR **labels = (WCHAR **)calloc((size_t)count, sizeof(*labels));
    int ready = wideTitle && wideLabel && labels;
    for (int i = 0; ready && i < count; i++) ready = (labels[i] = to_wide(items[i])) != NULL;
    int chosen = ready ? show_choice_dialog(wideTitle, wideLabel, labels, count, L"OK") : -1;
    for (int i = 0; labels && i < count; i++) free(labels[i]);
    free(labels);
    free(wideLabel);
    free(wideTitle);
    return chosen;
}

static void plugin_message(const char *title, const char *text, const char *kind) {
    WCHAR *wideTitle = to_wide(title);
    WCHAR *wideText = to_wide(text);
    UINT icon = strcmp(kind, "error") == 0 ? MB_ICONERROR
              : strcmp(kind, "warning") == 0 ? MB_ICONWARNING : MB_ICONINFORMATION;
    MessageBoxW(g_hMainWnd, wideText ? wideText : L"(This message isn't valid UTF-8 text.)",
                wideTitle ? wideTitle : L"MarkEdit", MB_OK | icon);
    free(wideText);
    free(wideTitle);
}

static int plugin_save_binary(const char *title, const char *defaultName,
                              const char *data, size_t length) {
    const DWORD capacity = 32768;
    WCHAR *path = (WCHAR *)calloc(capacity, sizeof(WCHAR));
    WCHAR *wideDefault = to_wide(defaultName);
    WCHAR *wideTitle = to_wide(title);
    if (!path || !wideDefault || !wideTitle) {
        free(path); free(wideDefault); free(wideTitle);
        return -1;
    }
    wcsncpy(path, wideDefault, capacity - 1);
    const WCHAR *extension = PathFindExtensionW(wideDefault);
    const WCHAR *defaultExtension = extension && extension[0] == L'.' ? extension + 1 : NULL;
    OPENFILENAMEW dialog = {0};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_hMainWnd;
    dialog.lpstrFilter = L"Binary Files (*.*)\0*.*\0All Files (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = capacity;
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrDefExt = defaultExtension;
    dialog.lpstrTitle = wideTitle;
    int result = 0;
    if (GetSaveFileNameW(&dialog)) result = write_bytes_file(path, data, length) ? 1 : -1;
    free(wideTitle);
    free(wideDefault);
    free(path);
    return result;
}

static int plugin_confirm(const char *title, const char *text) {
    WCHAR *wideTitle = to_wide(title), *wideText = to_wide(text);
    int result = MessageBoxW(g_hMainWnd, wideText ? wideText : L"Confirm?",
                             wideTitle ? wideTitle : L"MarkEdit", MB_YESNO | MB_ICONQUESTION);
    free(wideTitle); free(wideText);
    return result == IDYES;
}

static void plugin_status(const char *text) {
    WCHAR *wide = to_wide(text);
    if (wide) show_plugin_reload_status(wide);
    free(wide);
}

static void plugin_log(const char *plugin, const char *text) {
    OutputDebugStringA("[MarkEdit plugin: ");
    OutputDebugStringA(plugin ? plugin : "unknown");
    OutputDebugStringA("] ");
    OutputDebugStringA(text);
    OutputDebugStringA("\n");
}

static char *plugin_pick_path(const char *title, int folder) {
    if (folder) {
        WCHAR *path = choose_workspace_folder();
        char *result = path ? to_utf8(path) : NULL;
        free(path);
        return result;
    }
    WCHAR path[32768] = L"";
    WCHAR *wideTitle = to_wide(title);
    OPENFILENAMEW dialog = {0};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_hMainWnd;
    dialog.lpstrFilter = L"All Files (*.*)\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = (DWORD)(sizeof(path) / sizeof(path[0]));
    dialog.lpstrTitle = wideTitle;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    char *result = GetOpenFileNameW(&dialog) ? to_utf8(path) : NULL;
    free(wideTitle);
    return result;
}

static int plugin_invoke_command(const char *name) {
    const MenuCommand *command = menu_command_find(name);
    if (!command || !g_hMainWnd) return 0;
    /* Queue it: executing a command while Lua owns the editor's undo group
       could invalidate that group or recursively enter the plugin host. */
    return PostMessageW(g_hMainWnd, WM_COMMAND, command->id, 0) != 0;
}

static int plugin_document_count(void) { return g_documentStore.count; }
static int plugin_document_active(void) { return g_documentStore.active; }

typedef enum {
    PLUGIN_DOCUMENT_ACTIVATE, PLUGIN_DOCUMENT_NEW, PLUGIN_DOCUMENT_OPEN,
    PLUGIN_DOCUMENT_SAVE, PLUGIN_DOCUMENT_SAVE_AS, PLUGIN_DOCUMENT_CLOSE
} PluginDocumentActionKind;
typedef struct {
    PluginDocumentActionKind kind;
    int index;
    WCHAR *path;
} PluginDocumentAction;

static int queue_plugin_document_action(PluginDocumentActionKind kind, int index,
                                        const char *path) {
    PluginDocumentAction *action = (PluginDocumentAction *)calloc(1, sizeof(*action));
    if (!action) return 0;
    action->kind = kind;
    action->index = index;
    action->path = path ? to_wide(path) : NULL;
    if ((path && !action->path) ||
        !PostMessageW(g_hMainWnd, WM_APP_PLUGIN_DOCUMENT_ACTION, 0, (LPARAM)action)) {
        free(action->path); free(action);
        return 0;
    }
    return 1;
}

static int plugin_document_activate(int index) {
    if (index < 0 || index >= g_documentStore.count) return 0;
    return queue_plugin_document_action(PLUGIN_DOCUMENT_ACTIVATE, index, NULL);
}
static int plugin_document_new(void) {
    return queue_plugin_document_action(PLUGIN_DOCUMENT_NEW, 0, NULL);
}
static int plugin_document_open(const char *path) {
    return queue_plugin_document_action(PLUGIN_DOCUMENT_OPEN, 0, path);
}
static int plugin_document_save(int saveAs) {
    return queue_plugin_document_action(saveAs ? PLUGIN_DOCUMENT_SAVE_AS : PLUGIN_DOCUMENT_SAVE,
                                        0, NULL);
}
static int plugin_document_close(int index) {
    if (index < 0 || index >= g_documentStore.count) return 0;
    return queue_plugin_document_action(PLUGIN_DOCUMENT_CLOSE, index, NULL);
}
static char *plugin_document_path(int index) {
    return index >= 0 && index < g_documentStore.count && g_documentStore.documents[index].path
        ? to_utf8(g_documentStore.documents[index].path) : NULL;
}
static int plugin_document_modified(int index) {
    return index >= 0 && index < g_documentStore.count && g_documentStore.documents[index].dirty;
}
static const char *plugin_document_encoding(void) {
    static const char *const names[] = { "utf-8", "utf-16le", "utf-16be", "ansi" };
    if (g_encoding == TEXT_ENCODING_UTF8 && g_hasBom) return "utf-8-bom";
    return g_encoding >= TEXT_ENCODING_UTF8 && g_encoding <= TEXT_ENCODING_ANSI
        ? names[g_encoding] : "utf-8";
}
static int plugin_document_set_encoding(const char *encoding) {
    int oldEncoding = g_encoding;
    int oldBom = g_hasBom;
    if (strcmp(encoding, "utf-8") == 0) set_document_encoding(TEXT_ENCODING_UTF8, 0);
    else if (strcmp(encoding, "utf-8-bom") == 0) set_document_encoding(TEXT_ENCODING_UTF8, 1);
    else if (strcmp(encoding, "utf-16le") == 0) set_document_encoding(TEXT_ENCODING_UTF16LE, 1);
    else if (strcmp(encoding, "utf-16be") == 0) set_document_encoding(TEXT_ENCODING_UTF16BE, 1);
    else if (strcmp(encoding, "ansi") == 0) set_document_encoding(TEXT_ENCODING_ANSI, 0);
    else return 0;
    if (oldEncoding != g_encoding || oldBom != g_hasBom) g_pluginChangedSaveEncoding = 1;
    return 1;
}
static char *plugin_workspace_path(void) {
    return g_currentWorkspaceFolder ? to_utf8(g_currentWorkspaceFolder) : NULL;
}

static int plugin_schedule_deferred(int token, unsigned delayMs, int cancel) {
    if (!g_hMainWnd || token <= 0) return 0;
    UINT_PTR timer = (UINT_PTR)(IDT_PLUGIN_DEFER_BASE + token);
    if (cancel) { KillTimer(g_hMainWnd, timer); return 1; }
    return SetTimer(g_hMainWnd, timer, delayMs ? delayMs : 1, NULL) != 0;
}

static char *plugin_file_path(void) {
    return g_currentFile ? to_utf8(g_currentFile) : NULL;
}

static const char *plugin_language(void) {
    static const char *const names[LANGUAGE_COUNT] = {
        "markdown", "json", "xml", "html", "yaml", "cpp", "python", "bash", "powershell", "lua", "text",
    };
    return g_activeLanguage >= 0 && g_activeLanguage < LANGUAGE_COUNT ? names[g_activeLanguage] : "text";
}

static char *plugin_config_section(const char *plugin) {
    size_t length = strlen(plugin) + 8;
    char *section = (char *)malloc(length);
    if (section) snprintf(section, length, "Plugin:%s", plugin);
    return section;
}

static char *plugin_config_get(const char *plugin, const char *key) {
    char *section = plugin_config_section(plugin);
    WCHAR *path = get_settings_path();
    char *value = path && section ? plugin_settings_get(path, section, key) : NULL;
    free(section);
    free(path);
    return value;
}

static int plugin_config_set(const char *plugin, const char *key, const char *value) {
    char *section = plugin_config_section(plugin);
    WCHAR *path = get_settings_path();
    int ok = path && section && plugin_settings_set(path, section, key, value);
    free(section);
    free(path);
    return ok;
}

static char *plugin_state_key(const char *scope, const char *name) {
    size_t size = strlen(scope) + strlen(name) + 2;
    char *key = (char *)malloc(size);
    if (key) snprintf(key, size, "%s:%s", scope, name);
    return key;
}

static int plugin_is_enabled(const char *scope, const char *name) {
    WCHAR *path = get_settings_path();
    char *key = plugin_state_key(scope, name);
    char *value = path && key ? plugin_settings_get(path, "LuaPlugins", key) : NULL;
    int enabled = !value || strcmp(value, "0") != 0;
    free(value); free(key); free(path);
    return enabled;
}

static int set_plugin_enabled(const char *scope, const char *name, int enabled) {
    WCHAR *path = get_settings_path();
    char *key = plugin_state_key(scope, name);
    int ok = path && key && plugin_settings_set(path, "LuaPlugins", key, enabled ? "1" : "0");
    free(key);
    free(path);
    return ok;
}

/* ---- loading the menu definition and plugins ---- */

#define MENU_DEFINITION_MAX_BYTES (1024 * 1024)

/* %APPDATA%\markedit\menus.lua: the user's menu definition, when it exists. */
static WCHAR *user_settings_item_path(const WCHAR *name) {
    WCHAR *settings = get_settings_path();
    WCHAR *directory = settings ? path_directory(settings) : NULL;
    WCHAR *path = directory ? path_join(directory, name) : NULL;
    free(directory);
    free(settings);
    return path;
}

static WCHAR *user_menu_definition_path(void) {
    return user_settings_item_path(L"menus.lua");
}

static void install_plugin_type_definitions(const WCHAR *directory) {
    WCHAR *types = path_join(directory, L"types");
    if (types) CreateDirectoryW(types, NULL);
    WCHAR *destination = types ? path_join(types, L"markedit.lua") : NULL;
    if (destination) {
        WCHAR *module = module_directory();
        WCHAR *source = module ? path_join(module, L"plugins\\types\\markedit.lua") : NULL;
        if (source) CopyFileW(source, destination, FALSE);
        free(source); free(module);
    }
    free(destination);
    free(types);
}

/* Adds metadata used by Lua Language Server. Neither entry is executable, and
   the plugin loader ignores them. The generated API file follows app updates;
   an existing .luarc.json is left untouched so users can customise it. */
static void install_plugin_editor_metadata(const WCHAR *directory) {
    install_plugin_type_definitions(directory);
    WCHAR *luarc = path_join(directory, L".luarc.json");
    static const char metadata[] =
        "{\n  \"workspace.library\": [\"types\"],\n  \"diagnostics.globals\": "
        "[\"editor\", \"ui\", \"menu\", \"commands\", \"events\", \"config\", \"app\"]\n}\n";
    if (luarc && GetFileAttributesW(luarc) == INVALID_FILE_ATTRIBUTES)
        write_bytes_file(luarc, metadata, sizeof(metadata) - 1);
    free(luarc);
}

/* %APPDATA%\markedit\plugins, created with editor metadata when needed. */
static WCHAR *user_plugin_directory(void) {
    WCHAR *directory = user_settings_item_path(L"plugins");
    if (!directory) return NULL;
    CreateDirectoryW(directory, NULL);
    install_plugin_editor_metadata(directory);
    return directory;
}

/* A Lua script built into the exe (markedit.rc). Not NUL-terminated. */
static const char *builtin_script(int id, size_t *length) {
    HRSRC resource = FindResourceW(NULL, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL loaded = resource ? LoadResource(NULL, resource) : NULL;
    *length = resource ? (size_t)SizeofResource(NULL, resource) : 0;
    return loaded ? (const char *)LockResource(loaded) : NULL;
}

/* Joins source resources before compiling them, so formatting can live in
   focused files while sharing one lexical scope and its local helpers. */
static char *builtin_script_parts(const int *ids, int count, size_t *length) {
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        size_t partLength;
        if (!builtin_script(ids[i], &partLength) || partLength > SIZE_MAX - total - 1) return NULL;
        total += partLength + 1;
    }
    char *joined = (char *)malloc(total ? total : 1);
    if (!joined) return NULL;
    size_t used = 0;
    for (int i = 0; i < count; i++) {
        size_t partLength;
        const char *part = builtin_script(ids[i], &partLength);
        memcpy(joined + used, part, partLength);
        used += partLength;
        joined[used++] = '\n';
    }
    *length = used;
    return joined;
}

/* Appends "<prefix><text>" as a line of a newline-separated report. */
static char *append_report(char *report, const char *prefix, const char *text) {
    size_t used = report ? strlen(report) : 0;
    size_t needed = used + strlen(prefix) + strlen(text) + 2;
    char *grown = (char *)realloc(report, needed);
    if (!grown) return report;
    snprintf(grown + used, needed - used, "%s%s%s", used ? "\n" : "", prefix, text);
    return grown;
}

/* Loads the user's menu definition, or the built-in one when there's none
   or it fails to load. Unknown command and list names are reported and
   their items left out. Returns the report, or NULL. */
static char *load_menu_definition(void) {
    char *report = NULL;
    WCHAR *path = user_menu_definition_path();
    if (path && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        size_t codeLength = 0;
        char *code = read_utf8_text_file_length(path, MENU_DEFINITION_MAX_BYTES, &codeLength);
        char *error = code ? plugin_host_load_menu("menus.lua", code, codeLength) : NULL;
        if (!code) report = append_report(report, "", "menus.lua: could not be read as UTF-8 text (1 MiB at most).");
        if (error) report = append_report(report, "", error);
        if (!code || error) report = append_report(report, "", "Using the built-in menus instead.");
        free(error);
        free(code);
    }
    free(path);
    int count;
    if (!plugin_host_menu(&count)) {
        size_t length;
        const char *code = builtin_script(IDR_DEFAULT_MENUS, &length);
        char *error = code ? plugin_host_load_menu("built-in menus.lua", code, length) : NULL;
        if (!code) report = append_report(report, "", "The built-in menus are missing from markedit.exe.");
        if (error) report = append_report(report, "", error);
        free(error);
    }
    const PluginMenuItem *menus = plugin_host_menu(&count);
    char *problems = menus ? menu_definition_problems(menus, count) : NULL;
    if (problems) report = append_report(report, "menus.lua: ", problems);
    free(problems);
    return report;
}

static char *load_builtin_formatting(char *report) {
    static const int formattingParts[] = {
        IDR_BUILTIN_FORMATTING_BASE, IDR_BUILTIN_FORMATTING_TABLES,
        IDR_BUILTIN_FORMATTING_TEXT,
    };
    size_t length;
    char *formatting = builtin_script_parts(formattingParts,
        (int)(sizeof(formattingParts) / sizeof(formattingParts[0])), &length);
    char *formattingError = formatting ? plugin_host_load_builtin("formatting.lua", formatting, length) : NULL;
    if (!formatting) report = append_report(report, "", "The formatting commands are missing from markedit.exe.");
    if (formattingError) report = append_report(report, "", formattingError);
    free(formattingError);
    free(formatting);
    return report;
}

static char *load_plugin_directory(char *report, const WCHAR *directory, const char *scope) {
    char *error = directory ? plugin_host_load_directory(directory, scope) : NULL;
    if (error) report = append_report(report, "", error);
    free(error);
    return report;
}

static WCHAR *bundled_plugin_directory(void) {
    WCHAR *directory = module_directory();
    WCHAR *pluginDirectory = directory ? path_join(directory, L"plugins") : NULL;
    free(directory);
    return pluginDirectory;
}

static void show_plugin_load_errors(const char *report) {
    const WCHAR *errorPath = plugin_host_error_path();
    if (!g_hMainWnd || !errorPath) { show_plugin_error(report); return; }
    WCHAR *wide = to_wide(report);
    size_t size = (wide ? wcslen(wide) : 0) + 80;
    WCHAR *question = (WCHAR *)malloc(size * sizeof(WCHAR));
    if (question) swprintf(question, size, L"%ls\n\nOpen the first failing script?",
                           wide ? wide : L"A plugin failed.");
    int answer = MessageBoxW(g_hMainWnd, question ? question : L"A plugin failed. Open it?",
                             L"MarkEdit Plugins", MB_YESNO | MB_ICONWARNING);
    free(question);
    free(wide);
    if (answer != IDYES || !open_file_in_tab(errorPath)) return;
    int line = plugin_host_error_line();
    if (line > 0) SendMessageA(g_hEdit, SCI_GOTOLINE, (WPARAM)(line - 1), 0);
}

/* Recreates the Lua state, then loads menus, built-in formatting, bundled
   extensions and personal extensions in that order. */
static void load_menus_and_plugins(void) {
    static const PluginHostCallbacks callbacks = {
        .prompt = plugin_prompt, .choose = plugin_choose, .file_path = plugin_file_path,
        .language = plugin_language, .message = plugin_message,
        .config_get = plugin_config_get, .config_set = plugin_config_set,
        .plugin_enabled = plugin_is_enabled, .save_binary = plugin_save_binary,
        .confirm = plugin_confirm, .status = plugin_status, .log = plugin_log,
        .pick_path = plugin_pick_path, .invoke_command = plugin_invoke_command,
        .document_count = plugin_document_count, .document_active = plugin_document_active,
        .document_activate = plugin_document_activate, .document_new = plugin_document_new,
        .document_open = plugin_document_open, .document_save = plugin_document_save,
        .document_close = plugin_document_close, .document_path = plugin_document_path,
        .document_modified = plugin_document_modified,
        .document_encoding = plugin_document_encoding,
        .document_set_encoding = plugin_document_set_encoding,
        .workspace_path = plugin_workspace_path,
        .schedule_deferred = plugin_schedule_deferred,
    };
    plugin_host_set_callbacks(&callbacks);
    plugin_host_shutdown();
    char *report = load_builtin_formatting(load_menu_definition());
    WCHAR *bundled = bundled_plugin_directory();
    WCHAR *user = user_plugin_directory();
    report = load_plugin_directory(report, bundled, "bundled");
    report = load_plugin_directory(report, user, "user");
    free(bundled);
    free(user);
    g_lastPluginLoadSucceeded = report == NULL;
    if (report) show_plugin_load_errors(report);
    free(report);
}

/* Plugins > Customize Menus: opens the user's menu definition, first
   copying the built-in one there if there isn't one yet. */
static void customize_menus(void) {
    WCHAR *path = user_menu_definition_path();
    if (!path) return;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        size_t length;
        const char *code = builtin_script(IDR_DEFAULT_MENUS, &length);
        if (!code || !write_bytes_file(path, code, length)) {
            MessageBoxW(g_hMainWnd, L"Could not create menus.lua in the settings folder.", L"MarkEdit",
                        MB_OK | MB_ICONERROR);
            free(path);
            return;
        }
    }
    open_file_in_tab(path);
    free(path);
}

static void open_plugin_folder(void) {
    WCHAR *directory = user_plugin_directory();
    if (directory) {
        HINSTANCE result = ShellExecuteW(g_hMainWnd, L"open", directory, NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)result <= 32)
            MessageBoxW(g_hMainWnd, L"Could not open the personal plugins folder.",
                        L"MarkEdit Plugins", MB_OK | MB_ICONERROR);
    }
    free(directory);
}

static int valid_plugin_file_name(const char *name) {
    size_t length = strlen(name);
    if (!length || length >= 100 || name[0] == '.' || strstr(name, "..")) return 0;
    for (size_t i = 0; i < length; i++)
        if (!(isalnum((unsigned char)name[i]) || name[i] == '-' ||
              name[i] == '_' || name[i] == '.')) return 0;
    return 1;
}

static WCHAR *new_plugin_path(const char *name) {
    size_t length = strlen(name);
    char fileName[128];
    snprintf(fileName, sizeof(fileName), "%s%s", name,
             length >= 4 && _stricmp(name + length - 4, ".lua") == 0 ? "" : ".lua");
    WCHAR *wideName = to_wide(fileName);
    WCHAR *directory = user_plugin_directory();
    WCHAR *path = directory && wideName ? path_join(directory, wideName) : NULL;
    free(directory);
    free(wideName);
    return path;
}

static void new_plugin(void) {
    char *answer = plugin_prompt("New Plugin", "File name:", "my-plugin.lua");
    if (!answer) return;
    if (!valid_plugin_file_name(answer)) {
        MessageBoxW(g_hMainWnd, L"Use a simple file name containing letters, digits, '-' or '_'.",
                    L"New Plugin", MB_OK | MB_ICONWARNING);
        free(answer); return;
    }
    WCHAR *path = new_plugin_path(answer);
    static const char starter[] =
        "-- MarkEdit plugin\n"
        "menu.add(\"My Plugin/Hello\", function()\n"
        "  ui.message(\"Hello from Lua!\")\n"
        "end, \"Ctrl+Alt+H\")\n";
    if (!path || GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_hMainWnd, L"A plugin with that name already exists.", L"New Plugin",
                    MB_OK | MB_ICONWARNING);
    } else if (!write_bytes_file(path, starter, sizeof(starter) - 1)) {
        MessageBoxW(g_hMainWnd, L"Could not create the plugin file.", L"New Plugin",
                    MB_OK | MB_ICONERROR);
    } else {
        open_file_in_tab(path);
    }
    free(path);
    free(answer);
}

static void free_plugin_choices(char **items, int count) {
    if (items) for (int i = 0; i < count; i++) free(items[i]);
    free(items);
}

static char **plugin_choices(int count) {
    char **items = (char **)calloc((size_t)count, sizeof(*items));
    for (int i = 0; items && i < count; i++) {
        const char *name = plugin_host_extension_name(i);
        const char *scope = plugin_host_extension_scope(i);
        size_t size = strlen(name) + strlen(scope) + 16;
        items[i] = (char *)malloc(size);
        if (!items[i]) { free_plugin_choices(items, count); return NULL; }
        snprintf(items[i], size, "[%c] %s / %s",
                 plugin_host_extension_is_enabled(i) ? 'x' : ' ', scope, name);
    }
    return items;
}

static void reload_plugin_ui(HWND hwnd, const WCHAR *successMessage) {
    load_menus_and_plugins();
    refresh_menu(hwnd);
    rebuild_main_accelerators();
    if (g_lastPluginLoadSucceeded && successMessage)
        show_plugin_reload_status(successMessage);
}

static void toggle_plugin(int index) {
    const char *name = plugin_host_extension_name(index);
    const char *scope = plugin_host_extension_scope(index);
    int enable = !plugin_host_extension_is_enabled(index);
    if (!set_plugin_enabled(scope, name, enable)) {
        MessageBoxW(g_hMainWnd, L"Could not save the plugin state.", L"Manage Plugins",
                    MB_OK | MB_ICONERROR);
        return;
    }
    reload_plugin_ui(g_hMainWnd, enable ? L"Plugin enabled" : L"Plugin disabled");
}

static void manage_plugins(void) {
    int count = plugin_host_extension_count();
    if (!count) {
        MessageBoxW(g_hMainWnd, L"No plugins were found.", L"Manage Plugins",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    char **items = plugin_choices(count);
    const char **choices = (const char **)items;
    int selected = items ? plugin_choose("Manage Plugins",
        "Choose a plugin to enable or disable:", choices, count) : -1;
    if (selected >= 0 && selected < count) toggle_plugin(selected);
    free_plugin_choices(items, count);
}

/* Lua commands act on the pane being edited: the split editor's when it
   has the focus, otherwise the main one. */
static void run_plugin_command(int index) {
    int wasDirty = g_dirty;
    g_pluginChangedSaveEncoding = 0;
    char *error = plugin_host_run(index, active_edit_view());
    sync_dirty_after_plugin(wasDirty);
    if (error) show_plugin_error(error);
    free(error);
    /* A command may toggle its own menu.checkbox or change a setting used by
       another checkbox through config.set(). The HMENU is a retained Win32
       object, so refresh its state explicitly after Lua returns. */
    update_view_menu(g_hMainWnd);
    if (!plugin_host_is_busy()) queue_plugin_tab_check();
    SetFocus(active_edit_view());
}

static void handle_main_command(HWND hwnd, WORD cmd) {
    if (cmd >= IDM_PLUGIN_COMMAND_0 && cmd < IDM_PLUGIN_COMMAND_0 + PLUGIN_MAX_COMMANDS) {
        run_plugin_command(cmd - IDM_PLUGIN_COMMAND_0);
        return;
    }
    /* A built-in command registered in Lua (commands.register) runs there,
       from the menu, toolbar or a shortcut alike. Built-in formatting Lua
       supplies the Markdown and HTML implementations through this route. */
    const MenuCommand *builtIn = menu_command_for_id(cmd);
    int override = builtIn ? plugin_host_find_override(builtIn->name) : -1;
    if (override >= 0) {
        run_plugin_command(override);
        return;
    }
    if (cmd == IDM_PLUGIN_RELOAD) {
        reload_plugin_ui(hwnd, L"Plugins reloaded");
        return;
    }
    if (cmd == IDM_PLUGIN_CUSTOMIZE_MENUS) {
        customize_menus();
        return;
    }
    if (cmd == IDM_PLUGIN_OPEN_FOLDER) { open_plugin_folder(); return; }
    if (cmd == IDM_PLUGIN_NEW) { new_plugin(); return; }
    if (cmd == IDM_PLUGIN_MANAGE) { manage_plugins(); return; }
    if (cmd >= IDM_FILE_RECENT_0 && cmd < IDM_FILE_RECENT_0 + MAX_RECENT_FILES) {
        do_open_recent(cmd - IDM_FILE_RECENT_0);
        return;
    }
    if (cmd >= IDM_RECENT_WORKSPACE && cmd < IDM_RECENT_WORKSPACE + MAX_RECENT_FOLDERS) {
        do_open_recent_workspace(cmd - IDM_RECENT_WORKSPACE);
        return;
    }

    /* Each handler reports whether it recognized the command. */
    if (handle_file_command(hwnd, cmd)) return;
    if (handle_edit_command(cmd)) return;
    if (handle_format_command(cmd)) return;
    if (handle_debug_command(cmd)) return;
    if (handle_view_command(hwnd, cmd)) return;
    if (handle_help_command(cmd)) return;
}
