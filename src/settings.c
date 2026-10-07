#include "settings.h"

#include <stdlib.h>
#include <wchar.h>

#include "file_explorer.h"
#include "path_utils.h"

/* ---- persisted settings, stored in %APPDATA%\markedit\markedit.ini ---- */
WCHAR *get_settings_path(void) {
    DWORD needed = GetEnvironmentVariableW(L"APPDATA", NULL, 0);
    if (!needed) return NULL;
    WCHAR *appData = (WCHAR *)malloc((size_t)needed * sizeof(WCHAR));
    if (!appData || !GetEnvironmentVariableW(L"APPDATA", appData, needed)) {
        free(appData);
        return NULL;
    }
    WCHAR *directory = path_join(appData, L"markedit");
    free(appData);
    if (!directory) return NULL;
    CreateDirectoryW(directory, NULL);
    WCHAR *settings = path_join(directory, L"markedit.ini");
    free(directory);
    return settings;
}

/* Reads a string setting that may be arbitrarily long (a filesystem path),
   doubling the buffer until the value fits or a sane cap is hit. Returns a
   heap copy the caller frees, or NULL (nothing allocated) when the key is
   absent or empty. */
WCHAR *read_profile_string(const WCHAR *section, const WCHAR *key, const WCHAR *path) {
    DWORD cap = 512, n = 0;
    WCHAR *value = NULL;
    do {
        free(value);
        value = (WCHAR *)calloc(cap, sizeof(WCHAR));
        if (!value) return NULL;
        n = GetPrivateProfileStringW(section, key, L"", value, cap, path);
        if (n < cap - 1) break;
        cap *= 2;
    } while (cap <= 32768);
    if (value && value[0]) return value;
    free(value);
    return NULL;
}

void load_font_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    GetPrivateProfileStringW(L"Font", L"FaceName", L"Consolas",
                             g_fontFaceName, MAX_FONT_FACE_NAME, path);
    g_fontPointSize = GetPrivateProfileIntW(L"Font", L"PointSize", g_fontPointSize, path);
    if (g_fontPointSize < 1) g_fontPointSize = 1;
    g_fontWeight = GetPrivateProfileIntW(L"Font", L"Weight", FW_NORMAL, path);
    if (g_fontWeight < 1) g_fontWeight = FW_NORMAL;
    g_fontItalic = GetPrivateProfileIntW(L"Font", L"Italic", 0, path) ? 1 : 0;
    free(path);
}

void load_theme_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WCHAR value[32];
    GetPrivateProfileStringW(L"Preview", L"Theme", L"", value, 32, path);
    if (_wcsicmp(value, L"github-dark") == 0) {
        g_previewDarkMode = 1;
    } else if (_wcsicmp(value, L"github-light") == 0) {
        g_previewDarkMode = 0;
    } else {
        /* Migrate settings written by releases that exposed a DarkMode toggle. */
        GetPrivateProfileStringW(L"Preview", L"DarkMode", L"0", value, 32, path);
        g_previewDarkMode = (value[0] == L'1' || value[0] == L't' || value[0] == L'T' ||
                             value[0] == L'y' || value[0] == L'Y');
    }
    GetPrivateProfileStringW(L"Editor", L"Theme", L"", value, 32, path);
    if (_wcsicmp(value, L"github-dark") == 0) {
        g_editorDarkMode = 1;
    } else if (_wcsicmp(value, L"github-light") == 0) {
        g_editorDarkMode = 0;
    } else {
        /* No Editor Theme key yet - upgrading from a release where the editor
           and preview shared one theme, so start the editor matching whatever
           the preview was already set to. */
        g_editorDarkMode = g_previewDarkMode;
    }
    g_customCssPath = read_profile_string(L"Preview", L"CustomCSS", path);
    free(path);
}

void save_theme_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WritePrivateProfileStringW(L"Preview", L"Theme",
                               g_previewDarkMode ? L"github-dark" : L"github-light", path);
    WritePrivateProfileStringW(L"Preview", L"DarkMode", NULL, path);
    WritePrivateProfileStringW(L"Editor", L"Theme",
                               g_editorDarkMode ? L"github-dark" : L"github-light", path);
    WritePrivateProfileStringW(L"Preview", L"CustomCSS",
                               g_customCssPath ? g_customCssPath : L"", path);
    free(path);
}

static int read_profile_int(const WCHAR *section, const WCHAR *key, int defaultValue,
                            const WCHAR *path) {
    WCHAR fallback[32], value[32], *end = NULL;
    swprintf(fallback, 32, L"%d", defaultValue);
    GetPrivateProfileStringW(section, key, fallback, value, 32, path);
    long parsed = wcstol(value, &end, 10);
    return end && end != value && *end == 0 ? (int)parsed : defaultValue;
}

static void write_profile_int(const WCHAR *section, const WCHAR *key, int value,
                              const WCHAR *path) {
    WCHAR buffer[32];
    swprintf(buffer, 32, L"%d", value);
    WritePrivateProfileStringW(section, key, buffer, path);
}

/* Every persisted pane-split ratio (splitter, explorer width, split editor)
   is a permille clamped to this same [50,950] range, both when read back at
   startup and when re-derived from live pixel positions on save. */
static int clamp_ratio(int value) {
    if (value < 50) return 50;
    if (value > 950) return 950;
    return value;
}

void load_layout_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    g_previewOnLeft = read_profile_int(L"View", L"PreviewOnLeft", 0, path) != 0;
    g_previewVisible = read_profile_int(L"View", L"PreviewVisible", g_previewVisible, path) != 0;
    g_editorVisible = read_profile_int(L"View", L"EditorVisible", g_editorVisible, path) != 0;
    g_focusMode = read_profile_int(L"View", L"FocusMode", 0, path) != 0;
    g_wordWrap = read_profile_int(L"View", L"WordWrap", g_wordWrap, path) != 0;
    g_spellCheckEnabled = read_profile_int(L"View", L"SpellCheck", g_spellCheckEnabled, path) != 0;
    g_showWhitespace = read_profile_int(L"View", L"ShowWhitespace", g_showWhitespace, path) != 0;
    g_indentGuides = read_profile_int(L"View", L"IndentGuides", g_indentGuides, path) != 0;
    g_indentUseSpaces = read_profile_int(L"View", L"UseSpacesForIndent", g_indentUseSpaces, path) != 0;
    g_tabWidth = read_profile_int(L"View", L"TabWidth", g_tabWidth, path);
    if (g_tabWidth < 1) g_tabWidth = 1;
    if (g_tabWidth > 16) g_tabWidth = 16;
    g_outlineVisible = read_profile_int(L"View", L"OutlineVisible", g_outlineVisible, path) != 0;
    g_minimapVisible = read_profile_int(L"View", L"MinimapVisible", g_minimapVisible, path) != 0;
    g_explorerVisible = read_profile_int(L"View", L"ExplorerVisible", g_explorerVisible, path) != 0;
    g_explorerWidthRatio = clamp_ratio(read_profile_int(L"View", L"ExplorerWidthRatio", g_explorerWidthRatio, path));
    g_editorZoom = read_profile_int(L"View", L"EditorZoom", g_editorZoom, path);
    if (g_editorZoom < -10) g_editorZoom = -10;
    if (g_editorZoom > 20) g_editorZoom = 20;
    g_largeFileThresholdMB = read_profile_int(L"View", L"LargeFileThresholdMB", g_largeFileThresholdMB, path);
    if (g_largeFileThresholdMB < 1) g_largeFileThresholdMB = 1;
    /* Above 2 GB a file opens in large-file mode regardless (see load_file()). */
    if (g_largeFileThresholdMB > 2047) g_largeFileThresholdMB = 2047;
    GetPrivateProfileStringW(L"SpellCheck", L"Language", L"en-GB", g_spellLanguage,
                             LOCALE_NAME_MAX_LENGTH, path);
    if (!g_previewVisible && !g_editorVisible) g_editorVisible = 1;

    g_splitterRatio = clamp_ratio(read_profile_int(L"View", L"SplitterRatio", 500, path));

    g_splitEditorVisible = read_profile_int(L"View", L"SplitEditorVisible", 0, path) != 0;
    g_splitEditorRatio = clamp_ratio(read_profile_int(L"View", L"SplitEditorRatio", 500, path));

    int left = read_profile_int(L"Window", L"Left", 0, path);
    int top = read_profile_int(L"Window", L"Top", 0, path);
    int width = read_profile_int(L"Window", L"Width", 0, path);
    int height = read_profile_int(L"Window", L"Height", 0, path);
    RECT candidate = { left, top, left + width, top + height };
    if (width >= 400 && height >= 300 &&
        MonitorFromRect(&candidate, MONITOR_DEFAULTTONULL) != NULL) {
        g_savedWindowRect = candidate;
        g_haveSavedWindowRect = 1;
    }
    g_startMaximized = read_profile_int(L"Window", L"Maximized", 0, path) != 0;
    free(path);
}

void save_layout_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    write_profile_int(L"View", L"PreviewOnLeft", g_previewOnLeft, path);
    write_profile_int(L"View", L"FocusMode", g_focusMode, path);
    write_profile_int(L"View", L"WordWrap", g_wordWrap, path);
    write_profile_int(L"View", L"SpellCheck", g_spellCheckEnabled, path);
    write_profile_int(L"View", L"ShowWhitespace", g_showWhitespace, path);
    write_profile_int(L"View", L"IndentGuides", g_indentGuides, path);
    write_profile_int(L"View", L"UseSpacesForIndent", g_indentUseSpaces, path);
    write_profile_int(L"View", L"TabWidth", g_tabWidth, path);
    write_profile_int(L"View", L"OutlineVisible", g_outlineVisible, path);
    write_profile_int(L"View", L"MinimapVisible", g_minimapVisible, path);
    write_profile_int(L"View", L"ExplorerVisible", g_explorerVisible, path);
    write_profile_int(L"View", L"EditorZoom", g_editorZoom, path);
    write_profile_int(L"View", L"LargeFileThresholdMB", g_largeFileThresholdMB, path);
    WritePrivateProfileStringW(L"SpellCheck", L"Language", g_spellLanguage, path);
    if (!g_commandLinePreviewMode) {
        write_profile_int(L"View", L"PreviewVisible", g_previewVisible, path);
        write_profile_int(L"View", L"EditorVisible", g_editorVisible, path);
    }

    if (g_hMainWnd) {
        RECT client;
        GetClientRect(g_hMainWnd, &client);
        /* g_splitterPos is relative to the content area right of the
           explorer panel, not the full client width - explorer_content_x()
           is the same offset layout() shifts that content area by, so the
           persisted ratio has to be measured against the same narrower
           width or it drifts by however wide the panel currently is. */
        int contentX = explorer_content_x(g_focusMode);
        if (g_splitterPos >= 0) {
            int available = client.right - contentX - SPLITTER_WIDTH;
            if (available > 0) g_splitterRatio = clamp_ratio(MulDiv(g_splitterPos, 1000, available));
        }
        if (g_explorerWidth >= 0 && client.right > 0)
            g_explorerWidthRatio = clamp_ratio(MulDiv(g_explorerWidth, 1000, client.right));
    }
    write_profile_int(L"View", L"SplitterRatio", g_splitterRatio, path);
    write_profile_int(L"View", L"ExplorerWidthRatio", g_explorerWidthRatio, path);
    write_profile_int(L"View", L"SplitEditorVisible", g_splitEditorVisible, path);
    write_profile_int(L"View", L"SplitEditorRatio", g_splitEditorRatio, path);

    WINDOWPLACEMENT placement = {0};
    placement.length = sizeof(placement);
    if (g_hMainWnd && GetWindowPlacement(g_hMainWnd, &placement)) {
        RECT normal = placement.rcNormalPosition;
        int maximized = placement.showCmd == SW_SHOWMAXIMIZED ||
            (placement.showCmd == SW_SHOWMINIMIZED &&
             (placement.flags & WPF_RESTORETOMAXIMIZED) != 0);
        write_profile_int(L"Window", L"Left", normal.left, path);
        write_profile_int(L"Window", L"Top", normal.top, path);
        write_profile_int(L"Window", L"Width", normal.right - normal.left, path);
        write_profile_int(L"Window", L"Height", normal.bottom - normal.top, path);
        write_profile_int(L"Window", L"Maximized", maximized, path);
    }
    free(path);
}

void save_font_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WritePrivateProfileStringW(L"Font", L"FaceName", g_fontFaceName, path);
    WCHAR sizeBuf[16];
    swprintf(sizeBuf, 16, L"%d", g_fontPointSize);
    WritePrivateProfileStringW(L"Font", L"PointSize", sizeBuf, path);
    WCHAR weightBuf[16];
    swprintf(weightBuf, 16, L"%d", g_fontWeight);
    WritePrivateProfileStringW(L"Font", L"Weight", weightBuf, path);
    WritePrivateProfileStringW(L"Font", L"Italic", g_fontItalic ? L"1" : L"0", path);
    free(path);
}

void load_find_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    GetPrivateProfileStringW(L"Find", L"FindWhat", L"", g_findWhat,
                             sizeof(g_findWhat) / sizeof(g_findWhat[0]), path);
    GetPrivateProfileStringW(L"Find", L"ReplaceWith", L"", g_replaceWith,
                             sizeof(g_replaceWith) / sizeof(g_replaceWith[0]), path);
    g_findFlags = read_profile_int(L"Find", L"MatchCase", 0, path) ? FR_MATCHCASE : 0;
    if (read_profile_int(L"Find", L"SearchDown", 1, path)) g_findFlags |= FR_DOWN;
    free(path);
}

void save_find_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WritePrivateProfileStringW(L"Find", L"FindWhat", g_findWhat, path);
    WritePrivateProfileStringW(L"Find", L"ReplaceWith", g_replaceWith, path);
    write_profile_int(L"Find", L"MatchCase", (g_findFlags & FR_MATCHCASE) != 0, path);
    write_profile_int(L"Find", L"SearchDown", (g_findFlags & FR_DOWN) != 0, path);
    free(path);
}

/* Backend is hand-edited in markedit.ini rather than exposed through a menu:
   it pins Run/Debug C/C++ to one compiler instead of the default Auto
   priority search (GCC, Clang, Zig, then MSVC as a Run-only fallback). Every
   other key here - the per-toolchain path overrides and ExtraFlags - is what
   the Settings dialog's Compiler category exposes, and save_compiler_settings()
   round-trips Backend unchanged whenever it writes any of them back out. */
void load_compiler_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    WCHAR value[16];
    GetPrivateProfileStringW(L"Compiler", L"Backend", L"Auto", value, 16, path);
    if (_wcsicmp(value, L"GCC") == 0) g_compilerBackend = COMPILER_BACKEND_GCC;
    else if (_wcsicmp(value, L"Clang") == 0) g_compilerBackend = COMPILER_BACKEND_CLANG;
    else if (_wcsicmp(value, L"Zig") == 0) g_compilerBackend = COMPILER_BACKEND_ZIG;
    else if (_wcsicmp(value, L"MSVC") == 0) g_compilerBackend = COMPILER_BACKEND_MSVC;
    else g_compilerBackend = COMPILER_BACKEND_AUTO;

    g_vcvarsallOverridePath = read_profile_string(L"Compiler", L"VcvarsallPath", path);
    g_gccBinOverride = read_profile_string(L"Compiler", L"GccPath", path);
    g_clangBinOverride = read_profile_string(L"Compiler", L"ClangPath", path);
    g_zigBinOverride = read_profile_string(L"Compiler", L"ZigPath", path);
    g_luaBinOverride = read_profile_string(L"Compiler", L"LuaPath", path);
    g_gdbBinOverride = read_profile_string(L"Compiler", L"GdbPath", path);
    g_compilerExtraFlags = read_profile_string(L"Compiler", L"ExtraFlags", path);
    free(path);
}

void save_compiler_settings(void) {
    WCHAR *path = get_settings_path();
    if (!path) return;
    const WCHAR *backend =
        g_compilerBackend == COMPILER_BACKEND_GCC ? L"GCC" :
        g_compilerBackend == COMPILER_BACKEND_CLANG ? L"Clang" :
        g_compilerBackend == COMPILER_BACKEND_ZIG ? L"Zig" :
        g_compilerBackend == COMPILER_BACKEND_MSVC ? L"MSVC" : L"Auto";
    WritePrivateProfileStringW(L"Compiler", L"Backend", backend, path);
    WritePrivateProfileStringW(L"Compiler", L"VcvarsallPath",
                               g_vcvarsallOverridePath ? g_vcvarsallOverridePath : L"", path);
    WritePrivateProfileStringW(L"Compiler", L"GccPath",
                               g_gccBinOverride ? g_gccBinOverride : L"", path);
    WritePrivateProfileStringW(L"Compiler", L"ClangPath",
                               g_clangBinOverride ? g_clangBinOverride : L"", path);
    WritePrivateProfileStringW(L"Compiler", L"ZigPath",
                               g_zigBinOverride ? g_zigBinOverride : L"", path);
    WritePrivateProfileStringW(L"Compiler", L"LuaPath",
                               g_luaBinOverride ? g_luaBinOverride : L"", path);
    WritePrivateProfileStringW(L"Compiler", L"GdbPath",
                               g_gdbBinOverride ? g_gdbBinOverride : L"", path);
    WritePrivateProfileStringW(L"Compiler", L"ExtraFlags",
                               g_compilerExtraFlags ? g_compilerExtraFlags : L"", path);
    free(path);
}
