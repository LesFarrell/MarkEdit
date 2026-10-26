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
    const DWORD capacity = LONG_PATH_CAPACITY;
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
    /* Also on the output panel's Output tab, without opening the panel. */
    const char *name = plugin ? plugin : "unknown";
    size_t length = strlen(name) + strlen(text) + 8;
    char *line = (char *)malloc(length);
    if (line) {
        snprintf(line, length, "[%s] %s\n", name, text);
        write_plugin_output(line);
        free(line);
    }
}

static void plugin_output_write(const char *text) { write_plugin_output(text); }
static void plugin_output_clear(void) { clear_output_pane(&g_outputPanes[OUTPUT_TAB_RUN]); }
static void plugin_output_show(void) { show_output_panel(g_hMainWnd, OUTPUT_TAB_RUN, 0); }
static void plugin_output_hide(void) { hide_output_panel(g_hMainWnd); }
static int plugin_output_visible(void) { return g_outputPanelVisible; }

static char *plugin_pick_path(const char *title, int folder) {
    if (folder) {
        WCHAR *path = choose_workspace_folder();
        char *result = path ? to_utf8(path) : NULL;
        free(path);
        return result;
    }
    WCHAR path[LONG_PATH_CAPACITY] = L"";
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

/* Plugins have no file access of their own, but a document they open
   becomes readable, writable and runnable through the editor and command
   APIs. So opening a file that isn't already in a tab needs the user's say. */
static int confirm_plugin_open(const WCHAR *path) {
    WCHAR *full = absolute_path(path);
    int alreadyOpen = full && find_open_document_for_path(full) >= 0;
    free(full);
    if (alreadyOpen) return 1;
    size_t capacity = wcslen(path) + 64;
    WCHAR *prompt = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!prompt) return 0;
    swprintf(prompt, capacity, L"A plugin wants to open this file:\n\n%ls\n\nOpen it?", path);
    int allowed = MessageBoxW(g_hMainWnd, prompt, L"MarkEdit", MB_YESNO | MB_ICONQUESTION) == IDYES;
    free(prompt);
    return allowed;
}

/* WM_APP_PLUGIN_DOCUMENT_ACTION: carries out, and frees, an action that
   queue_plugin_document_action() posted. */
static void run_plugin_document_action(PluginDocumentAction *action) {
    if (!action) return;
    int validIndex = action->index >= 0 && action->index < g_documentStore.count;
    switch (action->kind) {
        case PLUGIN_DOCUMENT_ACTIVATE: if (validIndex) switch_document(action->index); break;
        case PLUGIN_DOCUMENT_NEW: create_document_tab(); break;
        case PLUGIN_DOCUMENT_OPEN:
            if (action->path && confirm_plugin_open(action->path)) open_file_in_tab(action->path);
            break;
        case PLUGIN_DOCUMENT_SAVE: do_save(); break;
        case PLUGIN_DOCUMENT_SAVE_AS: do_saveas(); break;
        case PLUGIN_DOCUMENT_CLOSE: if (validIndex) do_close_document_at(action->index); break;
    }
    free(action->path);
    free(action);
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
static int plugin_document_auto_reload(int index, int value) {
    if (index < 0 || index >= g_documentStore.count) return 0;
    if (value >= 0) g_documentStore.documents[index].autoReload = value;
    return g_documentStore.documents[index].autoReload;
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
        "markdown", "json", "xml", "html", "yaml", "cpp", "python", "bash", "powershell", "lua",
        "css", "sql", "ini", "toml", "batch", "makefile", "text",
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
    if (luarc && !file_exists(luarc))
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

/* The source checkout markedit.exe was built in, when it runs from one
   (dist\ or build\ beside markedit.rc), or NULL. Reload Plugins then reads
   the bundled plugins and built-in scripts from the checkout, so edits to
   them apply without rebuilding. */
static WCHAR *source_checkout_directory(void) {
    WCHAR *module = module_directory();
    WCHAR *parent = module ? path_directory(module) : NULL;
    free(module);
    WCHAR *rc = parent ? path_join(parent, L"markedit.rc") : NULL;
    WCHAR *menus = parent ? path_join(parent, L"src\\menus.lua") : NULL;
    int found = rc && menus && file_exists(rc) && file_exists(menus);
    free(rc);
    free(menus);
    if (!found) { free(parent); return NULL; }
    return parent;
}

/* Where each built-in script's source lives in a checkout (markedit.rc). */
static const struct { int id; const WCHAR *path; } BUILTIN_SCRIPT_SOURCES[] = {
    { IDR_DEFAULT_MENUS, L"src\\menus.lua" },
    { IDR_BUILTIN_FORMATTING_BASE, L"src\\lua\\formatting_base.lua" },
    { IDR_BUILTIN_FORMATTING_TABLES, L"src\\lua\\formatting_tables.lua" },
    { IDR_BUILTIN_FORMATTING_TEXT, L"src\\lua\\formatting_text.lua" },
};

/* A built-in script: the checkout's source file when there is one and it
   can be read, otherwise the copy built into the exe. NUL-terminated;
   caller frees. */
static char *load_builtin_script(int id, size_t *length) {
    WCHAR *checkout = source_checkout_directory();
    char *code = NULL;
    for (size_t i = 0; checkout && i < sizeof(BUILTIN_SCRIPT_SOURCES) / sizeof(BUILTIN_SCRIPT_SOURCES[0]); i++) {
        if (BUILTIN_SCRIPT_SOURCES[i].id != id) continue;
        WCHAR *path = path_join(checkout, BUILTIN_SCRIPT_SOURCES[i].path);
        code = path ? read_utf8_text_file_length(path, MENU_DEFINITION_MAX_BYTES, length) : NULL;
        free(path);
    }
    free(checkout);
    if (code) return code;
    const char *embedded = builtin_script(id, length);
    if (!embedded) return NULL;
    code = (char *)malloc(*length + 1);
    if (!code) return NULL;
    memcpy(code, embedded, *length);
    code[*length] = 0;
    return code;
}

/* Joins source resources before compiling them, so formatting can live in
   focused files while sharing one lexical scope and its local helpers. */
static char *builtin_script_parts(const int *ids, int count, size_t *length) {
    char *joined = NULL;
    size_t used = 0;
    for (int i = 0; i < count; i++) {
        size_t partLength;
        char *part = load_builtin_script(ids[i], &partLength);
        char *grown = part && partLength < SIZE_MAX - used - 1
            ? (char *)realloc(joined, used + partLength + 1) : NULL;
        if (!grown) { free(part); free(joined); return NULL; }
        joined = grown;
        memcpy(joined + used, part, partLength);
        used += partLength;
        joined[used++] = '\n';
        free(part);
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
   their items left out. Adds any problems to `report` and returns it. */
static char *load_menu_definition(char *report) {
    WCHAR *path = user_menu_definition_path();
    if (file_exists(path)) {
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
        char *code = load_builtin_script(IDR_DEFAULT_MENUS, &length);
        char *error = code ? plugin_host_load_menu("built-in menus.lua", code, length) : NULL;
        if (!code) report = append_report(report, "", "The built-in menus are missing from markedit.exe.");
        if (error) report = append_report(report, "", error);
        free(error);
        free(code);
    }
    const PluginMenuItem *menus = plugin_host_menu(&count);
    char *problems = menus ? menu_definition_problems(menus, count) : NULL;
    if (problems) report = append_report(report, "menus.lua: ", problems);
    free(problems);
    return report;
}

/* ---- theme colours ---- */

static WCHAR *user_theme_path(void) {
    return user_settings_item_path(L"themes.lua");
}

static const char *set_theme_colour(void *context, int dark, const char *name, const char *value) {
    (void)context;
    COLORREF colour;
    if (!theme_parse_colour(value, &colour)) return "the colour must be written \"#RRGGBB\"";
    if (!theme_set_colour(dark, name, colour)) return "there is no colour with this name";
    return NULL;
}

/* Starts from the built-in palettes, then applies %APPDATA%\markedit\
   themes.lua when there is one. Problems are added to the report; the
   colours that did load still apply. */
static char *load_theme_colours(char *report) {
    theme_reset_palettes();
    WCHAR *path = user_theme_path();
    if (file_exists(path)) {
        size_t length = 0;
        char *code = read_utf8_text_file_length(path, MENU_DEFINITION_MAX_BYTES, &length);
        char *problems = code ? plugin_host_load_theme("themes.lua", code, length, set_theme_colour, NULL) : NULL;
        if (!code) report = append_report(report, "", "themes.lua: could not be read as UTF-8 text (1 MiB at most).");
        if (problems) report = append_report(report, "", problems);
        free(problems);
        free(code);
    }
    free(path);
    return report;
}

/* Plugins > Customize Theme Colours: opens the user's themes.lua, first
   writing one that lists every colour with its built-in value. */
static void customize_theme_colours(void) {
    WCHAR *path = user_theme_path();
    if (!path) return;
    if (!file_exists(path)) {
        char *text = theme_template_lua();
        if (!text || !write_bytes_file(path, text, strlen(text))) {
            show_error(L"Could not create themes.lua in the settings folder.");
            free(text);
            free(path);
            return;
        }
        free(text);
    }
    open_file_in_tab(path);
    free(path);
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

/* plugins\ beside markedit.exe, or the checkout's own when it runs from one. */
static WCHAR *bundled_plugin_directory(void) {
    WCHAR *directory = source_checkout_directory();
    if (!directory) directory = module_directory();
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

/* Loads the theme colours, then recreates the Lua state and loads menus,
   built-in formatting, bundled extensions and personal extensions in that
   order. */
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
        .document_auto_reload = plugin_document_auto_reload,
        .document_encoding = plugin_document_encoding,
        .document_set_encoding = plugin_document_set_encoding,
        .workspace_path = plugin_workspace_path,
        .schedule_deferred = plugin_schedule_deferred,
        .spell_language = plugin_spell_language, .spell_check = plugin_spell_check,
        .output_write = plugin_output_write, .output_clear = plugin_output_clear,
        .output_show = plugin_output_show, .output_hide = plugin_output_hide,
        .output_visible = plugin_output_visible,
    };
    plugin_host_set_callbacks(&callbacks);
    plugin_host_shutdown();
    char *report = load_builtin_formatting(load_menu_definition(load_theme_colours(NULL)));
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
    if (!file_exists(path)) {
        size_t length;
        char *code = load_builtin_script(IDR_DEFAULT_MENUS, &length);
        int written = code && write_bytes_file(path, code, length);
        free(code);
        if (!written) {
            show_error(L"Could not create menus.lua in the settings folder.");
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

enum { PLUGIN_FILE_NAME_MAX = 100 };

static int valid_plugin_file_name(const char *name) {
    size_t length = strlen(name);
    if (!length || length >= PLUGIN_FILE_NAME_MAX || name[0] == '.' || strstr(name, "..")) return 0;
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

static void reload_plugin_ui(HWND hwnd, const WCHAR *successMessage) {
    load_menus_and_plugins();
    refresh_theme(hwnd); /* themes.lua may have changed */
    refresh_menu(hwnd);
    rebuild_main_accelerators();
    if (g_lastPluginLoadSucceeded && successMessage)
        show_plugin_reload_status(successMessage);
}

/* ---- Plugins > Manage Plugins: a checklist of every discovered plugin ----
   Ticks are only recorded while the dialog is open; OK saves the ones that
   changed and reloads the plugins once, Cancel discards them. */
enum {
    ID_PLUGINS_LIST = 11, ID_PLUGINS_ENABLE_ALL = 12, ID_PLUGINS_DISABLE_ALL = 13,
    ID_PLUGINS_OPEN_FOLDER = 14,
};

typedef struct ManagePluginsState {
    int count;
    int *enabled;       /* the ticks, indexed like plugin_host_extension_*() */
    Checklist checklist;
    HWND countLabel;
    int confirmed;
} ManagePluginsState;

static void update_plugin_count_label(ManagePluginsState *state) {
    int enabled = 0;
    for (int i = 0; i < state->count; i++) if (state->enabled[i]) enabled++;
    WCHAR text[64];
    swprintf(text, 64, L"%d of %d enabled", enabled, state->count);
    SetWindowTextW(state->countLabel, text);
}

static void toggle_plugin_tick(void *context, int index) {
    ManagePluginsState *state = (ManagePluginsState *)context;
    if (index < 0 || index >= state->count) return;
    state->enabled[index] = !state->enabled[index];
    update_plugin_count_label(state);
}

static void set_all_plugin_ticks(ManagePluginsState *state, int enabled) {
    for (int i = 0; i < state->count; i++) state->enabled[i] = enabled;
    InvalidateRect(state->checklist.list, NULL, FALSE);
    update_plugin_count_label(state);
}

/* The list fills the window above a row of buttons: the bulk actions on
   the left, OK and Cancel on the right. */
static void layout_manage_plugins(HWND hwnd, ManagePluginsState *state, int width, int height) {
    int buttonTop = height - 38;
    MoveWindow(state->countLabel, width - 136, 12, 124, 20, TRUE);
    MoveWindow(state->checklist.list, 12, 36, width - 24, buttonTop - 46, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_PLUGINS_ENABLE_ALL), 12, buttonTop, 90, 26, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_PLUGINS_DISABLE_ALL), 108, buttonTop, 90, 26, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_PLUGINS_OPEN_FOLDER), 204, buttonTop, 120, 26, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDOK), width - 180, buttonTop, 80, 26, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDCANCEL), width - 92, buttonTop, 80, 26, TRUE);
}

static LRESULT CALLBACK ManagePluginsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ManagePluginsState *state = (ManagePluginsState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        state = (ManagePluginsState *)((CREATESTRUCTW *)lParam)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
        HINSTANCE instance = GetModuleHandleW(NULL);
        CreateWindowExW(0, L"STATIC", L"Ticked plugins load. Changes apply when you click OK.",
            WS_CHILD | WS_VISIBLE, 12, 12, 360, 20, hwnd, NULL, instance, NULL);
        state->countLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
            0, 0, 0, 0, hwnd, NULL, instance, NULL);
        state->checklist.toggle = toggle_plugin_tick;
        state->checklist.context = state;
        create_checklist(&state->checklist, hwnd, ID_PLUGINS_LIST);
        CreateWindowExW(0, L"BUTTON", L"Enable All", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)ID_PLUGINS_ENABLE_ALL, instance, NULL);
        CreateWindowExW(0, L"BUTTON", L"Disable All", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)ID_PLUGINS_DISABLE_ALL, instance, NULL);
        CreateWindowExW(0, L"BUTTON", L"Open Plugins Folder", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)ID_PLUGINS_OPEN_FOLDER, instance, NULL);
        CreateWindowExW(0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)IDOK, instance, NULL);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, (HMENU)IDCANCEL, instance, NULL);
        use_gui_font(hwnd);
        /* Sized before it's filled: selecting a row in a list that is still
           0 pixels tall scrolls it, which would hide the first plugin. */
        RECT client;
        GetClientRect(hwnd, &client);
        layout_manage_plugins(hwnd, state, client.right, client.bottom);
        /* "csv-tools", not "csv-tools.lua", to match folder plugins. */
        for (int i = 0; i < state->count; i++) {
            WCHAR *name = to_wide(plugin_host_extension_name(i));
            if (!name) continue;
            WCHAR *extension = PathFindExtensionW(name);
            if (_wcsicmp(extension, L".lua") == 0) *extension = 0;
            checklist_add(&state->checklist, name, i);
            free(name);
        }
        SendMessageW(state->checklist.list, LB_SETCURSEL, 0, 0);
        update_plugin_count_label(state);
        SetFocus(state->checklist.list);
        popup_dialog_theme_opened(hwnd);
        return 0;
    }
    if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
        case WM_ERASEBKGND: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
            LRESULT handled = 0;
            LRESULT result = popup_dialog_theme_message(hwnd, msg, wParam, &handled);
            if (handled) return result;
            break;
        }
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lParam;
            if (mis->CtlType == ODT_LISTBOX) { measure_themed_listbox_item(state->checklist.list, mis); return TRUE; }
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_BUTTON) { draw_popup_button(dis); return TRUE; }
            if (dis->CtlType == ODT_LISTBOX && dis->hwndItem == state->checklist.list) {
                int index = (int)dis->itemData;
                if (index < 0 || index >= state->count) return TRUE;
                const char *scope = plugin_host_extension_scope(index);
                draw_checklist_item(&state->checklist, dis, state->enabled[index],
                                    strcmp(scope, "user") == 0 ? L"Personal" : L"Bundled");
                return TRUE;
            }
            break;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_PLUGINS_ENABLE_ALL: set_all_plugin_ticks(state, 1); return 0;
                case ID_PLUGINS_DISABLE_ALL: set_all_plugin_ticks(state, 0); return 0;
                case ID_PLUGINS_OPEN_FOLDER: open_plugin_folder(); return 0;
                case IDOK: state->confirmed = 1; DestroyWindow(hwnd); return 0;
                case IDCANCEL: DestroyWindow(hwnd); return 0;
            }
            break;
        case WM_SIZE:
            layout_manage_plugins(hwnd, state, LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO *info = (MINMAXINFO *)lParam;
            info->ptMinTrackSize.x = 560;
            info->ptMinTrackSize.y = 260;
            return 0;
        }
        case WM_NCDESTROY:
            popup_dialog_theme_destroyed();
            break;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* Saves the ticks that differ from each plugin's current state. Returns how
   many changed, or -1 if one couldn't be saved. */
static int apply_plugin_ticks(const ManagePluginsState *state) {
    int changed = 0;
    for (int i = 0; i < state->count; i++) {
        if (state->enabled[i] == plugin_host_extension_is_enabled(i)) continue;
        if (!set_plugin_enabled(plugin_host_extension_scope(i), plugin_host_extension_name(i),
                                state->enabled[i]))
            return -1;
        changed++;
    }
    return changed;
}

static void manage_plugins(void) {
    int count = plugin_host_extension_count();
    if (!count) {
        MessageBoxW(g_hMainWnd, L"No plugins were found.", L"Manage Plugins",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!register_popup_class(L"MarkEditManagePlugins", ManagePluginsWndProc)) return;
    ManagePluginsState state = { count, (int *)calloc((size_t)count, sizeof(int)), {0}, NULL, 0 };
    if (!state.enabled) return;
    for (int i = 0; i < count; i++) state.enabled[i] = plugin_host_extension_is_enabled(i);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MarkEditManagePlugins", L"Manage Plugins",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 580, 500, g_hMainWnd, NULL, GetModuleHandleW(NULL), &state);
    if (dialog) run_modal_popup(dialog);
    if (state.confirmed) {
        int changed = apply_plugin_ticks(&state);
        if (changed < 0) {
            MessageBoxW(g_hMainWnd, L"Could not save the plugin state.", L"Manage Plugins",
                        MB_OK | MB_ICONERROR);
        }
        /* Reload even after a failed save: earlier changes may have been saved. */
        if (changed != 0)
            reload_plugin_ui(g_hMainWnd, changed == 1 ? L"1 plugin changed" : L"Plugins changed");
    }
    free(state.enabled);
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
    if (cmd == IDM_PLUGIN_CUSTOMIZE_THEME) { customize_theme_colours(); return; }
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
