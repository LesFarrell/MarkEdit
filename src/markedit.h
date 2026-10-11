#ifndef MARKEDIT_H
#define MARKEDIT_H

#include <windows.h>

#include "settings.h" /* SPLITTER_WIDTH is used by save_layout_settings() too */

/* ---- child control IDs ---- */
#define ID_EDIT    101
#define ID_PREVIEW 102
#define ID_TABS    103
#define ID_STATUS  104
#define ID_OUTLINE 105
#define ID_MINIMAP 106
#define ID_SPLIT_EDIT 107
#define ID_DEBUG_PANEL 108
#define ID_DEBUG_VARIABLES 109
#define ID_DEBUG_STATUS 110
#define ID_DEBUG_CLOSE 111

/* ---- timer IDs ---- */
#define IDT_PREVIEW_UPDATE 1
#define IDT_FILE_WATCH 2
#define IDT_SCROLL_SYNC 3
#define IDT_SPELL_CHECK 4
#define IDT_AUTOSAVE 5
#define IDT_OUTLINE_UPDATE 6
#define IDT_PRINT_PREVIEW 7
#define IDT_DEBUG_LOCATION 8
#define IDT_STATUS_UPDATE 9
#define IDT_EXPLORER_REFRESH 10
#define IDT_HTML_EXPORT 11
#define IDT_PLUGIN_STATUS 12
#define IDT_PLUGIN_SELECTION 13
#define IDT_PLUGIN_DEFER_BASE 1000

/* ---- window messages and layout metrics ---- */
#define WM_APP_CLOSE_TAB (WM_APP + 1)
#define WM_APP_DEFERRED_INIT (WM_APP + 2)
/* WM_APP + 3 is MARKEDIT_WM_EXPLORER_CHANGED (file_explorer.h). */
#define WM_APP_PLUGIN_TAB_CHECK (WM_APP + 4)
#define WM_APP_APPLY_LARGE_FILE_MODE (WM_APP + 5)
#define WM_APP_PLUGIN_DOCUMENT_ACTION (WM_APP + 6)
#define TOOLBAR_HEIGHT 32
#define TAB_HEIGHT 28
#define MINIMAP_WIDTH 120
#define MINIMAP_SPLITTER_WIDTH 1
#define SPLIT_EDITOR_SPLITTER_SIZE 4

#include "command_ids.h"

/* ---- resource IDs (markedit.rc) ---- */
#define IDI_APPICON 1
#define IDR_DEFAULT_MENUS 2 /* src/menus.lua, the menu bar used without a user copy */
#define IDR_BUILTIN_FORMATTING_BASE 3 /* src/lua/formatting_base.lua */
#define IDR_HELP_MARKDOWN 4 /* src/help/markdown-reference.md, Help > Markdown Reference */
#define IDR_HELP_SHORTCUTS 5 /* src/help/keyboard-shortcuts.lua, builds Help > Keyboard Shortcuts */
#define IDR_BUILTIN_FORMATTING_JSON 6
#define IDR_BUILTIN_FORMATTING_TABLES 7
#define IDR_BUILTIN_FORMATTING_TEXT 8

/* ---- forward declarations for functions defined in markedit.c ----
   Grouped roughly by area: preview/theme, menu/layout, spell check,
   status/outline, recovery, dialogs, and document/tab management. */
/* Nonzero on success; zero on cancellation or write failure. */
static int do_save(void);
static void update_preview(void);
static void push_html_to_webview(const char *htmlFragment);
static void apply_preview_theme(void);
static void apply_editor_theme(HWND hEdit);
static void apply_word_wrap(HWND hEdit);
static void apply_preview_host_theme(void);
static void refresh_menu(HWND hwnd);
static void layout(HWND hwnd);
static void update_view_menu(HWND hwnd);
static void schedule_spell_check(void);
static void perform_spell_check(void);
static void update_status_position(void);
static void update_status_bar(void);
static void update_outline(void);
static void write_recovery_snapshot(void);
static int clear_recovery_snapshot(void);
static int restore_recovery_snapshot(void);
static int show_choice_dialog(const WCHAR *title, const WCHAR *prompt,
                              WCHAR **labels, int count, const WCHAR *okLabel);
static void fire_plugin_event(const char *event, const WCHAR *path);
static int show_text_input(const WCHAR *title, const WCHAR *prompt,
                           const WCHAR *initial, WCHAR *output, int outputCapacity);
static void toggle_preview(HWND hwnd);
static void toggle_minimap(HWND hwnd);
static void update_minimap_viewport(void);
static void update_document_mapping(void);
static void update_title(void);
static void set_dirty(int dirty);
static void update_tab_label(int index);
static void switch_document(int index);
static int create_document_tab(void);
static int active_document_is_reusable(void);
static void check_external_file_change(void);
static void prompt_reload_active_external_change(void);
static void do_close_document_at(int index);
static void do_reopen_closed_tab(void);
static void save_session_documents(void);
static void restore_session_documents(void);
static void sync_preview_to_editor_scroll(void);
static void apply_custom_preview_css(int reportErrors);
static int paste_image_from_clipboard(void);
static const char *builtin_script(int id, size_t *length);
static void apply_large_file_mode_after_growth(int documentIndex);

#endif
