/* ---- Settings dialog ----------------------------------------------------
   A single window over six categories (left-hand list, one panel of
   controls shown at a time) that just launches or toggles the same actions
   already reachable from the View menu one at a time - do_choose_font(),
   select_theme(), toggle_word_wrap(), etc. Every control applies and
   persists immediately via whichever of those it calls, the same as
   clicking the equivalent menu item would, so there's nothing to commit or
   revert: Close (and Esc) just dismiss the window. */
typedef struct SettingsDialogState {
    HWND categoryList;
    int activeCategory;
    HWND themeLightRadio, themeDarkRadio;
    HWND fontLabel, fontChooseButton;
    HWND wordWrapCheck;
    HWND previewOnLeftCheck, customCssCaption, customCssLabel, customCssChooseButton, customCssClearButton;
    HWND previewThemeCaption, previewThemeLightRadio, previewThemeDarkRadio;
    HWND spellCheckCheck, showWhitespaceCheck, indentGuidesCheck, minimapCheck;
    HWND indentUseSpacesCheck, tabWidthCaption, tabWidthEdit;
    HWND spellLanguageCaption, spellLanguageLabel, spellLanguageChooseButton;
    HWND compilerPathsHintCaption;
    HWND vcvarsallCaption, vcvarsallEdit, vcvarsallBrowseButton;
    HWND gccCaption, gccEdit, gccBrowseButton;
    HWND clangCaption, clangEdit, clangBrowseButton;
    HWND zigCaption, zigEdit, zigBrowseButton;
    HWND luaCaption, luaEdit, luaBrowseButton;
    HWND gdbCaption, gdbEdit, gdbBrowseButton;
    HWND compilerFlagsCaption, compilerFlagsEdit;
} SettingsDialogState;

static void update_settings_font_label(SettingsDialogState *state) {
    WCHAR text[MAX_FONT_FACE_NAME + 32];
    swprintf(text, sizeof(text) / sizeof(text[0]), L"%ls, %dpt", g_fontFaceName, g_fontPointSize);
    SetWindowTextW(state->fontLabel, text);
}

static void update_settings_css_label(SettingsDialogState *state) {
    SetWindowTextW(state->customCssLabel, g_customCssPath && g_customCssPath[0] ? g_customCssPath : L"(none)");
    EnableWindow(state->customCssClearButton, g_customCssPath && g_customCssPath[0]);
}

/* Mirrors choose_spell_language()'s own label building (LOCALE_SLOCALIZEDDISPLAYNAME
   over the raw BCP-47 tag) so the Editor category shows the same name the
   language picker itself lists it under. */
static void update_settings_spell_label(SettingsDialogState *state) {
    WCHAR displayName[LOCALE_NAME_MAX_LENGTH] = L"";
    if (g_spellLanguage[0])
        GetLocaleInfoEx(g_spellLanguage, LOCALE_SLOCALIZEDDISPLAYNAME, displayName, LOCALE_NAME_MAX_LENGTH);
    SetWindowTextW(state->spellLanguageLabel, displayName[0] ? displayName : g_spellLanguage);
}

static int is_flag_edit_whitespace(WCHAR ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

/* Applied on EN_KILLFOCUS from any of the Compiler category's edit boxes
   (the extra-flags box and the GCC/Clang/Zig/Lua/GDB/MSVC path overrides alike),
   the same as set_tab_width() is for the Editor category's - trims
   surrounding whitespace, persists via save_compiler_settings(), and is a
   no-op when the trimmed text hasn't actually changed so tabbing through a
   field without editing it doesn't touch the ini file. For the flags box,
   quote_extra_compiler_flags() (see launch_native_file()/launch_msvc_file())
   re-splits the saved text into individual flags at build time. */
static void set_compiler_string_setting(WCHAR **slot, const WCHAR *text) {
    size_t start = 0, end = text ? wcslen(text) : 0;
    while (start < end && is_flag_edit_whitespace(text[start])) start++;
    while (end > start && is_flag_edit_whitespace(text[end - 1])) end--;
    WCHAR *value = NULL;
    if (end > start) {
        value = (WCHAR *)malloc((end - start + 1) * sizeof(WCHAR));
        if (value) {
            for (size_t i = 0; i < end - start; i++) value[i] = text[start + i];
            value[end - start] = 0;
        }
    }
    if (value && *slot && wcscmp(value, *slot) == 0) {
        free(value);
        return;
    }
    if (!value && !*slot) return;
    free(*slot);
    *slot = value;
    save_compiler_settings();
}

/* The MSVC path box's Browse button: unlike the GCC/Clang/Zig/Lua/GDB boxes
   below, an override here names one specific file (vcvarsall.bat) rather
   than a folder to search, so this picks a file the same way
   do_choose_custom_css() does instead of reusing choose_workspace_folder(). */
static void choose_vcvarsall_path(HWND editControl) {
    DWORD cap = 32768;
    WCHAR *path = (WCHAR *)calloc(cap, sizeof(WCHAR));
    if (!path) return;
    if (g_vcvarsallOverridePath) wcsncpy(path, g_vcvarsallOverridePath, cap - 1);
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMainWnd;
    ofn.lpstrFilter = L"vcvarsall.bat\0vcvarsall.bat\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = cap;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) {
        replace_wstring(&g_vcvarsallOverridePath, path);
        save_compiler_settings();
        SetWindowTextW(editControl, g_vcvarsallOverridePath);
    }
    free(path);
}

/* Shared by the GCC/Clang/Zig/Lua/GDB Browse buttons: each of those overrides
   names a folder to search for that toolchain's executable(s) instead of
   PATH (see find_native_compiler()/find_lua_interpreter()), so picking one
   is just choose_workspace_folder()'s existing folder picker reused for a
   different setting. */
static void choose_compiler_folder(WCHAR **slot, HWND editControl) {
    WCHAR *folder = choose_workspace_folder();
    if (!folder) return;
    replace_wstring(slot, folder);
    free(folder);
    save_compiler_settings();
    SetWindowTextW(editControl, *slot);
}

static void update_settings_category(SettingsDialogState *state) {
    HWND all[] = {
        state->themeLightRadio, state->themeDarkRadio,
        state->fontLabel, state->fontChooseButton,
        state->wordWrapCheck,
        state->previewOnLeftCheck, state->customCssCaption, state->customCssLabel,
        state->customCssChooseButton, state->customCssClearButton,
        state->previewThemeCaption, state->previewThemeLightRadio, state->previewThemeDarkRadio,
        state->spellCheckCheck, state->showWhitespaceCheck, state->indentGuidesCheck, state->minimapCheck,
        state->indentUseSpacesCheck, state->tabWidthCaption, state->tabWidthEdit,
        state->spellLanguageCaption, state->spellLanguageLabel, state->spellLanguageChooseButton,
        state->compilerPathsHintCaption,
        state->vcvarsallCaption, state->vcvarsallEdit, state->vcvarsallBrowseButton,
        state->gccCaption, state->gccEdit, state->gccBrowseButton,
        state->clangCaption, state->clangEdit, state->clangBrowseButton,
        state->zigCaption, state->zigEdit, state->zigBrowseButton,
        state->luaCaption, state->luaEdit, state->luaBrowseButton,
        state->gdbCaption, state->gdbEdit, state->gdbBrowseButton,
        state->compilerFlagsCaption, state->compilerFlagsEdit,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        if (all[i]) ShowWindow(all[i], SW_HIDE);
    switch (state->activeCategory) {
        case 0:
            ShowWindow(state->themeLightRadio, SW_SHOW);
            ShowWindow(state->themeDarkRadio, SW_SHOW);
            break;
        case 1:
            ShowWindow(state->fontLabel, SW_SHOW);
            ShowWindow(state->fontChooseButton, SW_SHOW);
            break;
        case 2:
            ShowWindow(state->wordWrapCheck, SW_SHOW);
            break;
        case 3:
            ShowWindow(state->previewOnLeftCheck, SW_SHOW);
            ShowWindow(state->customCssCaption, SW_SHOW);
            ShowWindow(state->customCssLabel, SW_SHOW);
            ShowWindow(state->customCssChooseButton, SW_SHOW);
            ShowWindow(state->customCssClearButton, SW_SHOW);
            ShowWindow(state->previewThemeCaption, SW_SHOW);
            ShowWindow(state->previewThemeLightRadio, SW_SHOW);
            ShowWindow(state->previewThemeDarkRadio, SW_SHOW);
            break;
        case 4:
            ShowWindow(state->spellCheckCheck, SW_SHOW);
            ShowWindow(state->showWhitespaceCheck, SW_SHOW);
            ShowWindow(state->indentGuidesCheck, SW_SHOW);
            ShowWindow(state->minimapCheck, SW_SHOW);
            ShowWindow(state->indentUseSpacesCheck, SW_SHOW);
            ShowWindow(state->tabWidthCaption, SW_SHOW);
            ShowWindow(state->tabWidthEdit, SW_SHOW);
            ShowWindow(state->spellLanguageCaption, SW_SHOW);
            ShowWindow(state->spellLanguageLabel, SW_SHOW);
            ShowWindow(state->spellLanguageChooseButton, SW_SHOW);
            break;
        case 5:
            ShowWindow(state->compilerPathsHintCaption, SW_SHOW);
            ShowWindow(state->vcvarsallCaption, SW_SHOW);
            ShowWindow(state->vcvarsallEdit, SW_SHOW);
            ShowWindow(state->vcvarsallBrowseButton, SW_SHOW);
            ShowWindow(state->gccCaption, SW_SHOW);
            ShowWindow(state->gccEdit, SW_SHOW);
            ShowWindow(state->gccBrowseButton, SW_SHOW);
            ShowWindow(state->clangCaption, SW_SHOW);
            ShowWindow(state->clangEdit, SW_SHOW);
            ShowWindow(state->clangBrowseButton, SW_SHOW);
            ShowWindow(state->zigCaption, SW_SHOW);
            ShowWindow(state->zigEdit, SW_SHOW);
            ShowWindow(state->zigBrowseButton, SW_SHOW);
            ShowWindow(state->luaCaption, SW_SHOW);
            ShowWindow(state->luaEdit, SW_SHOW);
            ShowWindow(state->luaBrowseButton, SW_SHOW);
            ShowWindow(state->gdbCaption, SW_SHOW);
            ShowWindow(state->gdbEdit, SW_SHOW);
            ShowWindow(state->gdbBrowseButton, SW_SHOW);
            ShowWindow(state->compilerFlagsCaption, SW_SHOW);
            ShowWindow(state->compilerFlagsEdit, SW_SHOW);
            break;
    }
}

/* The shared g_popupBkBrush is normally created once when a popup dialog
   opens and never changes for that dialog's lifetime (see its own comment
   above) - a fine assumption for every other popup here, none of which can
   change the theme while they're open. This one can (the Appearance
   radios), so picking a theme needs to also recreate that brush and re-skin
   the controls that were themed at creation time, then force everything to
   repaint with the new colors instead of leaving stale ones on screen. */
static void refresh_settings_dialog_theme(HWND hwnd, SettingsDialogState *state) {
    popup_dialog_theme_created(hwnd);
    apply_explorer_theme(state->categoryList);
    /* Checkboxes/radios stay unthemed (see the WM_CREATE block below for why);
       nothing to redo here since that opt-out doesn't depend on light/dark. */
    RedrawWindow(hwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void create_settings_category_list(HWND hwnd, SettingsDialogState *state, HINSTANCE instance) {
    const WCHAR *categories[] = {
        L"Appearance", L"Font", L"Word Wrap", L"Preview", L"Editor", L"Compiler"
    };
    state->categoryList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
        LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        12, 12, 140, 300, hwnd, (HMENU)10, instance, NULL);
    for (size_t i = 0; i < sizeof(categories) / sizeof(categories[0]); i++)
        SendMessageW(state->categoryList, LB_ADDSTRING, 0, (LPARAM)categories[i]);
    apply_explorer_theme(state->categoryList);
}

static void create_general_settings_controls(HWND hwnd, SettingsDialogState *state,
                                             HINSTANCE instance, int x, int y) {
    state->themeLightRadio = CreateWindowExW(0, L"BUTTON", L"Light",
        WS_CHILD | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
        x, y, 220, 22, hwnd, (HMENU)20, instance, NULL);
    state->themeDarkRadio = CreateWindowExW(0, L"BUTTON", L"Dark",
        WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON,
        x, y + 28, 220, 22, hwnd, (HMENU)21, instance, NULL);
    SendMessageW(g_editorDarkMode ? state->themeDarkRadio : state->themeLightRadio,
                 BM_SETCHECK, BST_CHECKED, 0);

    state->fontLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
        x, y, 320, 20, hwnd, NULL, instance, NULL);
    state->fontChooseButton = CreateWindowExW(0, L"BUTTON", L"Choose Font...",
        WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
        x, y + 28, 140, 26, hwnd, (HMENU)30, instance, NULL);
    state->wordWrapCheck = CreateWindowExW(0, L"BUTTON", L"Wrap long lines in the editor",
        WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
        x, y, 320, 22, hwnd, (HMENU)40, instance, NULL);
    SendMessageW(state->wordWrapCheck, BM_SETCHECK,
                 g_wordWrap ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void create_preview_settings_controls(HWND hwnd, SettingsDialogState *state,
                                             HINSTANCE instance, int x, int y) {
    state->previewOnLeftCheck = CreateWindowExW(0, L"BUTTON", L"Show preview on the left",
        WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
        x, y, 320, 22, hwnd, (HMENU)50, instance, NULL);
    SendMessageW(state->previewOnLeftCheck, BM_SETCHECK,
                 g_previewOnLeft ? BST_CHECKED : BST_UNCHECKED, 0);
    state->previewThemeCaption = CreateWindowExW(0, L"STATIC", L"Markdown theme:", WS_CHILD,
        x, y + 36, 320, 20, hwnd, NULL, instance, NULL);
    state->previewThemeLightRadio = CreateWindowExW(0, L"BUTTON", L"GitHub Light",
        WS_CHILD | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
        x, y + 58, 220, 22, hwnd, (HMENU)53, instance, NULL);
    state->previewThemeDarkRadio = CreateWindowExW(0, L"BUTTON", L"GitHub Dark",
        WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON,
        x, y + 86, 220, 22, hwnd, (HMENU)54, instance, NULL);
    SendMessageW(g_previewDarkMode ? state->previewThemeDarkRadio : state->previewThemeLightRadio,
                 BM_SETCHECK, BST_CHECKED, 0);
    state->customCssCaption = CreateWindowExW(0, L"STATIC", L"Custom preview CSS:", WS_CHILD,
        x, y + 122, 320, 20, hwnd, NULL, instance, NULL);
    state->customCssLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
        x, y + 144, 320, 20, hwnd, NULL, instance, NULL);
    state->customCssChooseButton = CreateWindowExW(0, L"BUTTON", L"Choose CSS...",
        WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
        x, y + 170, 120, 26, hwnd, (HMENU)51, instance, NULL);
    state->customCssClearButton = CreateWindowExW(0, L"BUTTON", L"Clear",
        WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
        x + 128, y + 170, 90, 26, hwnd, (HMENU)52, instance, NULL);
}

static void create_editor_settings_controls(HWND hwnd, SettingsDialogState *state,
                                            HINSTANCE instance, int x, int y) {
    state->spellCheckCheck = CreateWindowExW(0, L"BUTTON", L"Spell check",
        WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, x, y, 320, 22, hwnd, (HMENU)60, instance, NULL);
    SendMessageW(state->spellCheckCheck, BM_SETCHECK,
                 g_spellCheckEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(state->spellCheckCheck, g_spellChecker != NULL);
    state->showWhitespaceCheck = CreateWindowExW(0, L"BUTTON", L"Show whitespace",
        WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, x, y + 28, 320, 22, hwnd, (HMENU)61, instance, NULL);
    SendMessageW(state->showWhitespaceCheck, BM_SETCHECK,
                 g_showWhitespace ? BST_CHECKED : BST_UNCHECKED, 0);
    state->indentGuidesCheck = CreateWindowExW(0, L"BUTTON", L"Indent guides",
        WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, x, y + 56, 320, 22, hwnd, (HMENU)62, instance, NULL);
    SendMessageW(state->indentGuidesCheck, BM_SETCHECK,
                 g_indentGuides ? BST_CHECKED : BST_UNCHECKED, 0);
    state->minimapCheck = CreateWindowExW(0, L"BUTTON", L"Minimap",
        WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, x, y + 84, 320, 22, hwnd, (HMENU)63, instance, NULL);
    SendMessageW(state->minimapCheck, BM_SETCHECK,
                 g_minimapVisible ? BST_CHECKED : BST_UNCHECKED, 0);
    state->indentUseSpacesCheck = CreateWindowExW(0, L"BUTTON", L"Use spaces for indentation",
        WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX, x, y + 112, 320, 22, hwnd, (HMENU)64, instance, NULL);
    SendMessageW(state->indentUseSpacesCheck, BM_SETCHECK,
                 g_indentUseSpaces ? BST_CHECKED : BST_UNCHECKED, 0);
    state->tabWidthCaption = CreateWindowExW(0, L"STATIC", L"Spaces per tab:", WS_CHILD,
        x, y + 142, 120, 20, hwnd, NULL, instance, NULL);
    state->tabWidthEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
        x + 124, y + 140, 50, 24, hwnd, (HMENU)65, instance, NULL);
    WCHAR tabWidthText[16];
    swprintf(tabWidthText, sizeof(tabWidthText) / sizeof(tabWidthText[0]), L"%d", g_tabWidth);
    SetWindowTextW(state->tabWidthEdit, tabWidthText);
    state->spellLanguageCaption = CreateWindowExW(0, L"STATIC", L"Spell check language:", WS_CHILD,
        x, y + 172, 200, 20, hwnd, NULL, instance, NULL);
    state->spellLanguageLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
        x, y + 194, 320, 20, hwnd, NULL, instance, NULL);
    state->spellLanguageChooseButton = CreateWindowExW(0, L"BUTTON", L"Choose...",
        WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
        x, y + 220, 120, 26, hwnd, (HMENU)66, instance, NULL);
    EnableWindow(state->spellLanguageChooseButton, g_spellChecker != NULL);
    update_settings_spell_label(state);
}

static void create_compiler_path_row(HWND hwnd, HINSTANCE instance, int x, int y,
                                     const WCHAR *label, WORD editId, WORD browseId,
                                     const WCHAR *value, HWND *caption, HWND *edit, HWND *button) {
    *caption = CreateWindowExW(0, L"STATIC", label, WS_CHILD,
        x, y + 2, 90, 20, hwnd, NULL, instance, NULL);
    *edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
        x + 96, y, 176, 24, hwnd, (HMENU)(UINT_PTR)editId, instance, NULL);
    *button = CreateWindowExW(0, L"BUTTON", L"Browse...",
        WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
        x + 280, y, 90, 24, hwnd, (HMENU)(UINT_PTR)browseId, instance, NULL);
    SetWindowTextW(*edit, value ? value : L"");
}

static void create_compiler_settings_controls(HWND hwnd, SettingsDialogState *state,
                                              HINSTANCE instance, int x, int y) {
    state->compilerPathsHintCaption = CreateWindowExW(0, L"STATIC",
        L"Optional overrides (leave blank to search PATH automatically):", WS_CHILD,
        x, y, 380, 18, hwnd, NULL, instance, NULL);
    create_compiler_path_row(hwnd, instance, x, y + 22, L"MSVC vcvars:", 71, 72,
        g_vcvarsallOverridePath, &state->vcvarsallCaption, &state->vcvarsallEdit, &state->vcvarsallBrowseButton);
    create_compiler_path_row(hwnd, instance, x, y + 56, L"GCC/G++:", 73, 74,
        g_gccBinOverride, &state->gccCaption, &state->gccEdit, &state->gccBrowseButton);
    create_compiler_path_row(hwnd, instance, x, y + 90, L"Clang:", 75, 76,
        g_clangBinOverride, &state->clangCaption, &state->clangEdit, &state->clangBrowseButton);
    create_compiler_path_row(hwnd, instance, x, y + 124, L"Zig:", 77, 78,
        g_zigBinOverride, &state->zigCaption, &state->zigEdit, &state->zigBrowseButton);
    create_compiler_path_row(hwnd, instance, x, y + 158, L"Lua:", 79, 80,
        g_luaBinOverride, &state->luaCaption, &state->luaEdit, &state->luaBrowseButton);
    create_compiler_path_row(hwnd, instance, x, y + 192, L"GDB:", 81, 82,
        g_gdbBinOverride, &state->gdbCaption, &state->gdbEdit, &state->gdbBrowseButton);
    state->compilerFlagsCaption = CreateWindowExW(0, L"STATIC",
        L"Extra flags passed to Run/Debug C/C++ builds:", WS_CHILD,
        x, y + 228, 380, 20, hwnd, NULL, instance, NULL);
    state->compilerFlagsEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
        x, y + 252, 380, 24, hwnd, (HMENU)70, instance, NULL);
    SetWindowTextW(state->compilerFlagsEdit, g_compilerExtraFlags ? g_compilerExtraFlags : L"");
}

static void finish_settings_dialog_setup(HWND hwnd, SettingsDialogState *state, HINSTANCE instance) {
    HWND checkboxes[] = { state->wordWrapCheck, state->previewOnLeftCheck, state->spellCheckCheck,
                          state->showWhitespaceCheck, state->indentGuidesCheck, state->minimapCheck,
                          state->indentUseSpacesCheck, state->themeLightRadio, state->themeDarkRadio,
                          state->previewThemeLightRadio, state->previewThemeDarkRadio };
    for (size_t i = 0; i < sizeof(checkboxes) / sizeof(checkboxes[0]); i++)
        SetWindowTheme(checkboxes[i], L"", L"");
    CreateWindowExW(0, L"BUTTON", L"Close",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
        496, 330, 78, 26, hwnd, (HMENU)IDCANCEL, instance, NULL);
    update_settings_font_label(state);
    update_settings_css_label(state);
    SendMessageW(state->categoryList, LB_SETCURSEL, 0, 0);
    update_settings_category(state);
    SetFocus(state->categoryList);
    popup_dialog_theme_opened(hwnd);
}

static void initialize_settings_dialog(HWND hwnd, SettingsDialogState *state) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    const int panelX = 168, panelY = 16;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
    create_settings_category_list(hwnd, state, instance);
    create_general_settings_controls(hwnd, state, instance, panelX, panelY);
    create_preview_settings_controls(hwnd, state, instance, panelX, panelY);
    create_editor_settings_controls(hwnd, state, instance, panelX, panelY);
    create_compiler_settings_controls(hwnd, state, instance, panelX, panelY);
    finish_settings_dialog_setup(hwnd, state, instance);
}

static void commit_tab_width(SettingsDialogState *state) {
    WCHAR text[16];
    GetWindowTextW(state->tabWidthEdit, text, sizeof(text) / sizeof(text[0]));
    set_tab_width((int)wcstol(text, NULL, 10));
    swprintf(text, sizeof(text) / sizeof(text[0]), L"%d", g_tabWidth);
    SetWindowTextW(state->tabWidthEdit, text);
}

static int commit_compiler_setting(SettingsDialogState *state, WORD controlId) {
    struct CompilerField { WORD id; HWND edit; WCHAR **setting; } fields[] = {
        { 70, state->compilerFlagsEdit, &g_compilerExtraFlags },
        { 71, state->vcvarsallEdit, &g_vcvarsallOverridePath },
        { 73, state->gccEdit, &g_gccBinOverride },
        { 75, state->clangEdit, &g_clangBinOverride },
        { 77, state->zigEdit, &g_zigBinOverride },
        { 79, state->luaEdit, &g_luaBinOverride },
        { 81, state->gdbEdit, &g_gdbBinOverride },
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (controlId != fields[i].id) continue;
        WCHAR text[1024];
        GetWindowTextW(fields[i].edit, text, sizeof(text) / sizeof(text[0]));
        set_compiler_string_setting(fields[i].setting, text);
        SetWindowTextW(fields[i].edit, *fields[i].setting ? *fields[i].setting : L"");
        return 1;
    }
    return 0;
}

static void handle_settings_button(HWND hwnd, SettingsDialogState *state, WORD controlId) {
    switch (controlId) {
        case 20: select_theme(g_hMainWnd, 0); refresh_settings_dialog_theme(hwnd, state); break;
        case 21: select_theme(g_hMainWnd, 1); refresh_settings_dialog_theme(hwnd, state); break;
        case 30: do_choose_font(); update_settings_font_label(state); break;
        case 40: toggle_word_wrap(g_hMainWnd); break;
        case 50: toggle_panel_sides(g_hMainWnd); break;
        case 51: do_choose_custom_css(); update_settings_css_label(state); break;
        case 52: clear_custom_css(); update_settings_css_label(state); break;
        case 53: select_preview_theme(g_hMainWnd, 0); break;
        case 54: select_preview_theme(g_hMainWnd, 1); break;
        case 60: toggle_spell_check(g_hMainWnd); break;
        case 61: toggle_show_whitespace(g_hMainWnd); break;
        case 62: toggle_indent_guides(g_hMainWnd); break;
        case 63: toggle_minimap(g_hMainWnd); break;
        case 64: toggle_indent_use_spaces(g_hMainWnd); break;
        case 66: choose_spell_language(); update_settings_spell_label(state); break;
        case 72: choose_vcvarsall_path(state->vcvarsallEdit); break;
        case 74: choose_compiler_folder(&g_gccBinOverride, state->gccEdit); break;
        case 76: choose_compiler_folder(&g_clangBinOverride, state->clangEdit); break;
        case 78: choose_compiler_folder(&g_zigBinOverride, state->zigEdit); break;
        case 80: choose_compiler_folder(&g_luaBinOverride, state->luaEdit); break;
        case 82: choose_compiler_folder(&g_gdbBinOverride, state->gdbEdit); break;
        case IDCANCEL: DestroyWindow(hwnd); break;
    }
}

static void handle_settings_command(HWND hwnd, SettingsDialogState *state, WPARAM wParam) {
    WORD controlId = LOWORD(wParam);
    WORD event = HIWORD(wParam);
    if (controlId == 10 && event == LBN_SELCHANGE) {
        int selection = (int)SendMessageW(state->categoryList, LB_GETCURSEL, 0, 0);
        if (selection != LB_ERR) {
            state->activeCategory = selection;
            update_settings_category(state);
        }
    } else if (controlId == 65 && event == EN_KILLFOCUS) {
        commit_tab_width(state);
    } else if (event == EN_KILLFOCUS) {
        commit_compiler_setting(state, controlId);
    } else if (event == BN_CLICKED) {
        handle_settings_button(hwnd, state, controlId);
    }
}

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsDialogState *state = (SettingsDialogState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        state = (SettingsDialogState *)((CREATESTRUCTW *)lParam)->lpCreateParams;
        initialize_settings_dialog(hwnd, state);
        return 0;
    }
    if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
        case WM_ERASEBKGND: case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN:
        case WM_CTLCOLORLISTBOX: {
            LRESULT handled = 0;
            LRESULT result = popup_dialog_theme_message(hwnd, msg, wParam, &handled);
            if (handled) return result;
            break;
        }
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lParam;
            if (mis->CtlType == ODT_LISTBOX) { measure_themed_listbox_item(state->categoryList, mis); return TRUE; }
            break;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_BUTTON) { draw_popup_button(dis); return TRUE; }
            if (dis->CtlType == ODT_LISTBOX && dis->hwndItem == state->categoryList) {
                draw_themed_listbox_item(state->categoryList, dis);
                return TRUE;
            }
            break;
        }
        case WM_NCDESTROY:
            popup_dialog_theme_destroyed();
            break;
        case WM_COMMAND:
            handle_settings_command(hwnd, state, wParam);
            return 0;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void show_settings_dialog(void) {
    static int registered = 0;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MarkEditSettingsDialog";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        registered = 1;
    }
    SettingsDialogState state = {0};
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MarkEditSettingsDialog", L"Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 400, g_hMainWnd, NULL,
        GetModuleHandleW(NULL), &state);
    if (!dialog) return;
    run_modal_popup(dialog);
}

/* Menu commands, grouped by the feature they operate on. */
