#ifndef SETTINGS_H
#define SETTINGS_H

#include <windows.h>
#include <commdlg.h>

/* Wider than LOGFONT's own LF_FACESIZE (32 WCHARs): that cap is a limit of
   the classic ChooseFont/LOGFONT API's round-trip, not of anything this app
   needs to store or hand to Scintilla (SCI_STYLESETFONT takes a plain
   NUL-terminated string). do_choose_font() works around ChooseFontW's
   truncation with a dialog hook so real font names longer than 31
   characters survive intact. */
#define MAX_FONT_FACE_NAME 128

/* Pixel width of the editor/preview splitter; save_layout_settings() converts
   the live splitter position to/from a ratio using this. Also used by the
   splitter's own layout/hit-testing code in markedit.c. */
#define SPLITTER_WIDTH 6

/* User-configurable Run/Debug C/C++ compiler backend; see load_compiler_settings()
   and find_native_compiler(). Auto keeps the existing GCC/Clang/Zig/MSVC
   priority search; the others pin Run/Debug to just one of them. */
typedef enum {
    COMPILER_BACKEND_AUTO = 0, COMPILER_BACKEND_GCC, COMPILER_BACKEND_CLANG,
    COMPILER_BACKEND_ZIG, COMPILER_BACKEND_MSVC
} CompilerBackendPreference;

/* Shared application state persisted to %APPDATA%\markedit\markedit.ini.
   Defined (without static) in markedit.c; read and written here. */
extern HWND g_hMainWnd;
extern WCHAR g_fontFaceName[MAX_FONT_FACE_NAME];
extern int g_fontPointSize;
extern int g_fontWeight;
extern int g_fontItalic;
extern int g_previewDarkMode;
extern int g_editorDarkMode;
extern WCHAR *g_customCssPath;
extern int g_previewOnLeft;
extern int g_previewVisible;
extern int g_editorVisible;
extern int g_focusMode;
extern int g_wordWrap;
extern int g_spellCheckEnabled;
extern int g_showWhitespace;
extern int g_indentGuides;
extern int g_indentUseSpaces;
extern int g_tabWidth;
extern int g_outlineVisible;
extern int g_minimapVisible;
extern int g_editorZoom;
extern WCHAR g_spellLanguage[LOCALE_NAME_MAX_LENGTH];
extern int g_splitterRatio;
extern int g_splitterPos;
extern int g_splitEditorVisible;
extern int g_splitEditorRatio;
extern int g_commandLinePreviewMode;
extern RECT g_savedWindowRect;
extern int g_haveSavedWindowRect;
extern int g_startMaximized;
extern WCHAR g_findWhat[256];
extern WCHAR g_replaceWith[256];
extern DWORD g_findFlags;
extern CompilerBackendPreference g_compilerBackend;
extern WCHAR *g_vcvarsallOverridePath;
extern WCHAR *g_compilerExtraFlags;

WCHAR *get_settings_path(void);

void load_font_settings(void);
void save_font_settings(void);
void load_theme_settings(void);
void save_theme_settings(void);
void load_layout_settings(void);
void save_layout_settings(void);
void load_find_settings(void);
void save_find_settings(void);
void load_compiler_settings(void);
void save_compiler_settings(void);

#endif
