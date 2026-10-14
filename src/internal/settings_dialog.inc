/* ---- Settings dialog ----------------------------------------------------
   A single window over six categories (left-hand list, one panel of
   controls shown at a time) that just launches or toggles the same actions
   already reachable from the View menu one at a time - do_choose_font(),
   select_theme(), toggle_word_wrap(), etc. Every control applies and
   persists immediately via whichever of those it calls, the same as
   clicking the equivalent menu item would, so there's nothing to commit or
   revert: Close (and Esc) just dismiss the window.

   The controls are described by SETTINGS_CONTROLS below, one row each, and
   named by SettingsControl. A control's window ID is derived from its name
   (ID_SETTINGS_FIRST + name), so the creation code and WM_COMMAND handling
   can't disagree about which number means what. */

enum { ID_SETTINGS_CATEGORIES = 10, ID_SETTINGS_FIRST = 100 };

typedef enum {
    SETTINGS_APPEARANCE, SETTINGS_FONT, SETTINGS_WORD_WRAP,
    SETTINGS_PREVIEW, SETTINGS_EDITOR, SETTINGS_COMPILER, SETTINGS_CATEGORY_COUNT
} SettingsCategory;

typedef enum {
    SC_THEME_LIGHT, SC_THEME_DARK,
    SC_FONT_LABEL, SC_FONT_CHOOSE,
    SC_WORD_WRAP,
    SC_PREVIEW_ON_LEFT, SC_PREVIEW_THEME_CAPTION, SC_PREVIEW_THEME_LIGHT, SC_PREVIEW_THEME_DARK,
    SC_CSS_CAPTION, SC_CSS_LABEL, SC_CSS_CHOOSE, SC_CSS_CLEAR,
    SC_SPELL_CHECK, SC_SHOW_WHITESPACE, SC_INDENT_GUIDES, SC_MINIMAP, SC_USE_SPACES,
    SC_TAB_WIDTH_CAPTION, SC_TAB_WIDTH,
    SC_SPELL_LANGUAGE_CAPTION, SC_SPELL_LANGUAGE_LABEL, SC_SPELL_LANGUAGE_CHOOSE,
    SC_COMPILER_HINT,
    SC_VCVARS_CAPTION, SC_VCVARS, SC_VCVARS_BROWSE,
    SC_GCC_CAPTION, SC_GCC, SC_GCC_BROWSE,
    SC_CLANG_CAPTION, SC_CLANG, SC_CLANG_BROWSE,
    SC_ZIG_CAPTION, SC_ZIG, SC_ZIG_BROWSE,
    SC_LUA_CAPTION, SC_LUA, SC_LUA_BROWSE,
    SC_GDB_CAPTION, SC_GDB, SC_GDB_BROWSE,
    SC_FLAGS_CAPTION, SC_FLAGS,
    SC_COUNT
} SettingsControl;

typedef enum {
    SETTINGS_TEXT,        /* a STATIC caption or value label */
    SETTINGS_CHECK,       /* checkbox showing *setting != 0 */
    SETTINGS_RADIO_FIRST, /* first radio button of a group */
    SETTINGS_RADIO,       /* checked when *setting equals its value */
    SETTINGS_BUTTON,      /* owner-drawn push button */
    SETTINGS_PATH_EDIT,   /* text box editing *path; committed on losing focus */
    SETTINGS_NUMBER_EDIT, /* digits-only text box */
} SettingsControlKind;

typedef struct SettingsControlSpec {
    SettingsCategory category;
    SettingsControlKind kind;
    const WCHAR *text;
    int x, y, width, height; /* relative to the panel's top-left corner */
    const int *setting;      /* SETTINGS_CHECK / SETTINGS_RADIO* */
    int value;               /* SETTINGS_RADIO*: the setting value that checks it */
    WCHAR **path;            /* SETTINGS_PATH_EDIT */
} SettingsControlSpec;

/* One row per control, indexed by SettingsControl, so a new control needs a
   name above and a row here. Browse buttons directly follow the path box
   they fill in. */
static const SettingsControlSpec SETTINGS_CONTROLS[SC_COUNT] = {
    [SC_THEME_LIGHT] = { SETTINGS_APPEARANCE, SETTINGS_RADIO_FIRST, L"Light", 0, 0, 220, 22, &g_editorDarkMode, 0, NULL },
    [SC_THEME_DARK] = { SETTINGS_APPEARANCE, SETTINGS_RADIO, L"Dark", 0, 28, 220, 22, &g_editorDarkMode, 1, NULL },

    [SC_FONT_LABEL] = { SETTINGS_FONT, SETTINGS_TEXT, NULL, 0, 0, 320, 20, NULL, 0, NULL },
    [SC_FONT_CHOOSE] = { SETTINGS_FONT, SETTINGS_BUTTON, L"Choose Font...", 0, 28, 140, 26, NULL, 0, NULL },

    [SC_WORD_WRAP] = { SETTINGS_WORD_WRAP, SETTINGS_CHECK, L"Wrap long lines in the editor", 0, 0, 320, 22, &g_wordWrap, 1, NULL },

    [SC_PREVIEW_ON_LEFT] = { SETTINGS_PREVIEW, SETTINGS_CHECK, L"Show preview on the left", 0, 0, 320, 22, &g_previewOnLeft, 1, NULL },
    [SC_PREVIEW_THEME_CAPTION] = { SETTINGS_PREVIEW, SETTINGS_TEXT, L"Markdown theme:", 0, 36, 320, 20, NULL, 0, NULL },
    [SC_PREVIEW_THEME_LIGHT] = { SETTINGS_PREVIEW, SETTINGS_RADIO_FIRST, L"GitHub Light", 0, 58, 220, 22, &g_previewDarkMode, 0, NULL },
    [SC_PREVIEW_THEME_DARK] = { SETTINGS_PREVIEW, SETTINGS_RADIO, L"GitHub Dark", 0, 86, 220, 22, &g_previewDarkMode, 1, NULL },
    [SC_CSS_CAPTION] = { SETTINGS_PREVIEW, SETTINGS_TEXT, L"Custom preview CSS:", 0, 122, 320, 20, NULL, 0, NULL },
    [SC_CSS_LABEL] = { SETTINGS_PREVIEW, SETTINGS_TEXT, NULL, 0, 144, 320, 20, NULL, 0, NULL },
    [SC_CSS_CHOOSE] = { SETTINGS_PREVIEW, SETTINGS_BUTTON, L"Choose CSS...", 0, 170, 120, 26, NULL, 0, NULL },
    [SC_CSS_CLEAR] = { SETTINGS_PREVIEW, SETTINGS_BUTTON, L"Clear", 128, 170, 90, 26, NULL, 0, NULL },

    [SC_SPELL_CHECK] = { SETTINGS_EDITOR, SETTINGS_CHECK, L"Spell check", 0, 0, 320, 22, &g_spellCheckEnabled, 1, NULL },
    [SC_SHOW_WHITESPACE] = { SETTINGS_EDITOR, SETTINGS_CHECK, L"Show whitespace", 0, 28, 320, 22, &g_showWhitespace, 1, NULL },
    [SC_INDENT_GUIDES] = { SETTINGS_EDITOR, SETTINGS_CHECK, L"Indent guides", 0, 56, 320, 22, &g_indentGuides, 1, NULL },
    [SC_MINIMAP] = { SETTINGS_EDITOR, SETTINGS_CHECK, L"Minimap", 0, 84, 320, 22, &g_minimapVisible, 1, NULL },
    [SC_USE_SPACES] = { SETTINGS_EDITOR, SETTINGS_CHECK, L"Use spaces for indentation", 0, 112, 320, 22, &g_indentUseSpaces, 1, NULL },
    [SC_TAB_WIDTH_CAPTION] = { SETTINGS_EDITOR, SETTINGS_TEXT, L"Spaces per tab:", 0, 142, 120, 20, NULL, 0, NULL },
    [SC_TAB_WIDTH] = { SETTINGS_EDITOR, SETTINGS_NUMBER_EDIT, NULL, 124, 140, 50, 24, NULL, 0, NULL },
    [SC_SPELL_LANGUAGE_CAPTION] = { SETTINGS_EDITOR, SETTINGS_TEXT, L"Spell check language:", 0, 172, 200, 20, NULL, 0, NULL },
    [SC_SPELL_LANGUAGE_LABEL] = { SETTINGS_EDITOR, SETTINGS_TEXT, NULL, 0, 194, 320, 20, NULL, 0, NULL },
    [SC_SPELL_LANGUAGE_CHOOSE] = { SETTINGS_EDITOR, SETTINGS_BUTTON, L"Choose...", 0, 220, 120, 26, NULL, 0, NULL },

    [SC_COMPILER_HINT] = { SETTINGS_COMPILER, SETTINGS_TEXT,
        L"Optional overrides (leave blank to search PATH automatically):", 0, 0, 380, 18, NULL, 0, NULL },
#define COMPILER_PATH_ROW(name, label, y, slot) \
    [name##_CAPTION] = { SETTINGS_COMPILER, SETTINGS_TEXT, label, 0, (y) + 2, 90, 20, NULL, 0, NULL }, \
    [name] = { SETTINGS_COMPILER, SETTINGS_PATH_EDIT, NULL, 96, (y), 176, 24, NULL, 0, (slot) }, \
    [name##_BROWSE] = { SETTINGS_COMPILER, SETTINGS_BUTTON, L"Browse...", 280, (y), 90, 24, NULL, 0, NULL }
    COMPILER_PATH_ROW(SC_VCVARS, L"MSVC vcvars:", 22, &g_vcvarsallOverridePath),
    COMPILER_PATH_ROW(SC_GCC, L"GCC/G++:", 56, &g_gccBinOverride),
    COMPILER_PATH_ROW(SC_CLANG, L"Clang:", 90, &g_clangBinOverride),
    COMPILER_PATH_ROW(SC_ZIG, L"Zig:", 124, &g_zigBinOverride),
    COMPILER_PATH_ROW(SC_LUA, L"Lua:", 158, &g_luaBinOverride),
    COMPILER_PATH_ROW(SC_GDB, L"GDB:", 192, &g_gdbBinOverride),
#undef COMPILER_PATH_ROW
    [SC_FLAGS_CAPTION] = { SETTINGS_COMPILER, SETTINGS_TEXT,
        L"Extra flags passed to Run/Debug C/C++ builds:", 0, 228, 380, 20, NULL, 0, NULL },
    [SC_FLAGS] = { SETTINGS_COMPILER, SETTINGS_PATH_EDIT, NULL, 0, 252, 380, 24, NULL, 0, &g_compilerExtraFlags },
};

typedef struct SettingsDialogState {
    HWND categoryList;
    int activeCategory;
    HWND controls[SC_COUNT];
} SettingsDialogState;

static void update_settings_font_label(SettingsDialogState *state) {
    WCHAR text[MAX_FONT_FACE_NAME + 32];
    swprintf(text, sizeof(text) / sizeof(text[0]), L"%ls, %dpt", g_fontFaceName, g_fontPointSize);
    SetWindowTextW(state->controls[SC_FONT_LABEL], text);
}

static void update_settings_css_label(SettingsDialogState *state) {
    SetWindowTextW(state->controls[SC_CSS_LABEL],
                   g_customCssPath && g_customCssPath[0] ? g_customCssPath : L"(none)");
    EnableWindow(state->controls[SC_CSS_CLEAR], g_customCssPath && g_customCssPath[0]);
}

/* Mirrors choose_spell_language()'s own label building (LOCALE_SLOCALIZEDDISPLAYNAME
   over the raw BCP-47 tag) so the Editor category shows the same name the
   language picker itself lists it under. */
static void update_settings_spell_label(SettingsDialogState *state) {
    WCHAR displayName[LOCALE_NAME_MAX_LENGTH] = L"";
    if (g_spellLanguage[0])
        GetLocaleInfoEx(g_spellLanguage, LOCALE_SLOCALIZEDDISPLAYNAME, displayName, LOCALE_NAME_MAX_LENGTH);
    SetWindowTextW(state->controls[SC_SPELL_LANGUAGE_LABEL], displayName[0] ? displayName : g_spellLanguage);
}

static void update_settings_tab_width(SettingsDialogState *state) {
    WCHAR text[16];
    swprintf(text, sizeof(text) / sizeof(text[0]), L"%d", g_tabWidth);
    SetWindowTextW(state->controls[SC_TAB_WIDTH], text);
}

static int is_flag_edit_whitespace(WCHAR ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

/* Applied on EN_KILLFOCUS from any of the Compiler category's edit boxes
   (the extra-flags box and the GCC/Clang/Zig/Lua/GDB/MSVC path overrides alike),
   the same as set_tab_width() is for the Editor category's - trims
   surrounding whitespace, persists via save_compiler_settings(), and is a
   no-op when the trimmed text hasn't actually changed so tabbing through a
   field without editing it doesn't rewrite settings.json. For the flags box,
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
    DWORD cap = LONG_PATH_CAPACITY;
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

/* Shows the active category's controls and hides every other one. */
static void update_settings_category(SettingsDialogState *state) {
    for (int i = 0; i < SC_COUNT; i++)
        ShowWindow(state->controls[i],
                   (int)SETTINGS_CONTROLS[i].category == state->activeCategory ? SW_SHOW : SW_HIDE);
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
    /* Checkboxes/radios stay unthemed (see create_settings_control() for
       why); nothing to redo here since that opt-out doesn't depend on
       light/dark. */
    RedrawWindow(hwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void create_settings_category_list(HWND hwnd, SettingsDialogState *state, HINSTANCE instance) {
    static const WCHAR *const categories[SETTINGS_CATEGORY_COUNT] = {
        L"Appearance", L"Font", L"Word Wrap", L"Preview", L"Editor", L"Compiler"
    };
    state->categoryList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
        LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
        12, 12, 140, 300, hwnd, (HMENU)ID_SETTINGS_CATEGORIES, instance, NULL);
    for (int i = 0; i < SETTINGS_CATEGORY_COUNT; i++)
        SendMessageW(state->categoryList, LB_ADDSTRING, 0, (LPARAM)categories[i]);
    apply_explorer_theme(state->categoryList);
}

/* Creates one control from its SETTINGS_CONTROLS row, positioned relative
   to the panel at (panelX, panelY), and shows the setting it reflects. */
static HWND create_settings_control(HWND hwnd, HINSTANCE instance, SettingsControl control,
                                    int panelX, int panelY) {
    const SettingsControlSpec *spec = &SETTINGS_CONTROLS[control];
    const WCHAR *windowClass = L"BUTTON";
    DWORD style = WS_CHILD | WS_TABSTOP, exStyle = 0;
    switch (spec->kind) {
        case SETTINGS_TEXT: windowClass = L"STATIC"; style = WS_CHILD; break;
        case SETTINGS_CHECK: style |= BS_AUTOCHECKBOX; break;
        case SETTINGS_RADIO_FIRST: style |= WS_GROUP | BS_AUTORADIOBUTTON; break;
        case SETTINGS_RADIO: style |= BS_AUTORADIOBUTTON; break;
        case SETTINGS_BUTTON: style |= BS_OWNERDRAW; break;
        case SETTINGS_NUMBER_EDIT: style |= ES_NUMBER; /* fall through */
        case SETTINGS_PATH_EDIT:
            windowClass = L"EDIT"; exStyle = WS_EX_CLIENTEDGE; style |= ES_AUTOHSCROLL; break;
    }
    HMENU id = spec->kind == SETTINGS_TEXT ? NULL : (HMENU)(UINT_PTR)(ID_SETTINGS_FIRST + control);
    HWND window = CreateWindowExW(exStyle, windowClass, spec->text ? spec->text : L"", style,
        panelX + spec->x, panelY + spec->y, spec->width, spec->height, hwnd, id, instance, NULL);
    if (spec->setting) {
        int on = (*spec->setting != 0) == (spec->value != 0);
        SendMessageW(window, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        /* The visual style draws checkbox and radio labels in the system
           text colour, unreadable on the dark theme's background; unthemed,
           they take the colour WM_CTLCOLORSTATIC/BTN gives them. */
        SetWindowTheme(window, L"", L"");
    }
    if (spec->path) SetWindowTextW(window, *spec->path ? *spec->path : L"");
    return window;
}

static void initialize_settings_dialog(HWND hwnd, SettingsDialogState *state) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    const int panelX = 168, panelY = 16;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
    create_settings_category_list(hwnd, state, instance);
    for (int i = 0; i < SC_COUNT; i++)
        state->controls[i] = create_settings_control(hwnd, instance, (SettingsControl)i, panelX, panelY);
    EnableWindow(state->controls[SC_SPELL_CHECK], g_spellChecker != NULL);
    EnableWindow(state->controls[SC_SPELL_LANGUAGE_CHOOSE], g_spellChecker != NULL);
    CreateWindowExW(0, L"BUTTON", L"Close",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
        496, 330, 78, 26, hwnd, (HMENU)IDCANCEL, instance, NULL);
    update_settings_font_label(state);
    update_settings_css_label(state);
    update_settings_spell_label(state);
    update_settings_tab_width(state);
    SendMessageW(state->categoryList, LB_SETCURSEL, 0, 0);
    update_settings_category(state);
    SetFocus(state->categoryList);
    popup_dialog_theme_opened(hwnd);
}

/* An edit box lost focus: store what it now says. */
static void commit_settings_edit(SettingsDialogState *state, SettingsControl control) {
    HWND edit = state->controls[control];
    if (control == SC_TAB_WIDTH) {
        WCHAR text[16];
        GetWindowTextW(edit, text, sizeof(text) / sizeof(text[0]));
        set_tab_width((int)wcstol(text, NULL, 10));
        update_settings_tab_width(state);
        return;
    }
    WCHAR **slot = SETTINGS_CONTROLS[control].path;
    if (!slot) return;
    WCHAR text[1024];
    GetWindowTextW(edit, text, sizeof(text) / sizeof(text[0]));
    set_compiler_string_setting(slot, text);
    SetWindowTextW(edit, *slot ? *slot : L"");
}

static void handle_settings_button(HWND hwnd, SettingsDialogState *state, SettingsControl control) {
    switch (control) {
        case SC_THEME_LIGHT: select_theme(g_hMainWnd, 0); refresh_settings_dialog_theme(hwnd, state); break;
        case SC_THEME_DARK: select_theme(g_hMainWnd, 1); refresh_settings_dialog_theme(hwnd, state); break;
        case SC_FONT_CHOOSE: do_choose_font(); update_settings_font_label(state); break;
        case SC_WORD_WRAP: toggle_word_wrap(g_hMainWnd); break;
        case SC_PREVIEW_ON_LEFT: toggle_panel_sides(g_hMainWnd); break;
        case SC_CSS_CHOOSE: do_choose_custom_css(); update_settings_css_label(state); break;
        case SC_CSS_CLEAR: clear_custom_css(); update_settings_css_label(state); break;
        case SC_PREVIEW_THEME_LIGHT: select_preview_theme(g_hMainWnd, 0); break;
        case SC_PREVIEW_THEME_DARK: select_preview_theme(g_hMainWnd, 1); break;
        case SC_SPELL_CHECK: toggle_spell_check(g_hMainWnd); break;
        case SC_SHOW_WHITESPACE: toggle_show_whitespace(g_hMainWnd); break;
        case SC_INDENT_GUIDES: toggle_indent_guides(g_hMainWnd); break;
        case SC_MINIMAP: toggle_minimap(g_hMainWnd); break;
        case SC_USE_SPACES: toggle_indent_use_spaces(g_hMainWnd); break;
        case SC_SPELL_LANGUAGE_CHOOSE: choose_spell_language(); update_settings_spell_label(state); break;
        case SC_VCVARS_BROWSE: choose_vcvarsall_path(state->controls[SC_VCVARS]); break;
        case SC_GCC_BROWSE: case SC_CLANG_BROWSE: case SC_ZIG_BROWSE:
        case SC_LUA_BROWSE: case SC_GDB_BROWSE:
            /* Each Browse button directly follows the path box it fills in. */
            choose_compiler_folder(SETTINGS_CONTROLS[control - 1].path, state->controls[control - 1]);
            break;
        default: break;
    }
}

static void handle_settings_command(HWND hwnd, SettingsDialogState *state, WPARAM wParam) {
    WORD controlId = LOWORD(wParam);
    WORD event = HIWORD(wParam);
    if (controlId == ID_SETTINGS_CATEGORIES) {
        if (event != LBN_SELCHANGE) return;
        int selection = (int)SendMessageW(state->categoryList, LB_GETCURSEL, 0, 0);
        if (selection != LB_ERR) {
            state->activeCategory = selection;
            update_settings_category(state);
        }
    } else if (controlId == IDCANCEL) {
        DestroyWindow(hwnd);
    } else if (controlId >= ID_SETTINGS_FIRST && controlId < ID_SETTINGS_FIRST + SC_COUNT) {
        SettingsControl control = (SettingsControl)(controlId - ID_SETTINGS_FIRST);
        if (event == EN_KILLFOCUS) commit_settings_edit(state, control);
        else if (event == BN_CLICKED) handle_settings_button(hwnd, state, control);
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
    if (!register_popup_class(L"MarkEditSettingsDialog", SettingsWndProc)) return;
    SettingsDialogState state = {0};
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MarkEditSettingsDialog", L"Settings",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 400, g_hMainWnd, NULL,
        GetModuleHandleW(NULL), &state);
    if (!dialog) return;
    run_modal_popup(dialog);
}

/* Menu commands, grouped by the feature they operate on. */
