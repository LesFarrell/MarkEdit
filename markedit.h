#ifndef MARKEDIT_H
#define MARKEDIT_H

#include <windows.h>

/* ---- child control IDs ---- */
#define ID_EDIT    101
#define ID_PREVIEW 102
#define ID_TABS    103
#define ID_STATUS  104
#define ID_OUTLINE 105
#define ID_MINIMAP 106

/* ---- timer IDs ---- */
#define IDT_PREVIEW_UPDATE 1
#define IDT_FILE_WATCH 2
#define IDT_SCROLL_SYNC 3
#define IDT_SPELL_CHECK 4
#define IDT_AUTOSAVE 5
#define IDT_OUTLINE_UPDATE 6
#define IDT_PRINT_PREVIEW 7
#define IDT_DEBUG_LOCATION 8

/* ---- window messages and layout metrics ---- */
#define WM_APP_CLOSE_TAB (WM_APP + 1)
#define WM_APP_DEFERRED_INIT (WM_APP + 2)
#define SPLITTER_WIDTH 6
#define TOOLBAR_HEIGHT 32
#define TAB_HEIGHT 28
#define MINIMAP_WIDTH 120

/* ---- menu command IDs: File, Edit, View, Help ---- */
#define IDM_FILE_NEW      201
#define IDM_FILE_OPEN     202
#define IDM_FILE_SAVE     203
#define IDM_FILE_SAVEAS   204
#define IDM_FILE_EXIT     205
#define IDM_FILE_RECENT_CLEAR 206
#define IDM_FILE_EXPORT_HTML 207
#define IDM_FILE_CLOSE     208
#define IDM_FILE_REOPEN_CLOSED 241
#define IDM_FILE_OPEN_TERMINAL 243
#define IDM_FILE_RECENT_0 250 /* IDM_FILE_RECENT_0 .. IDM_FILE_RECENT_0+MAX_RECENT_FILES-1 */
#define IDM_EDIT_UNDO     210
#define IDM_EDIT_REDO     219
#define IDM_EDIT_CUT      211
#define IDM_EDIT_COPY     212
#define IDM_EDIT_PASTE    213
#define IDM_EDIT_DELETE   214
#define IDM_EDIT_SELECTALL 215
#define IDM_EDIT_FIND     216
#define IDM_EDIT_REPLACE  217
#define IDM_EDIT_FINDNEXT 218
#define IDM_EDIT_GOTOLINE  209
#define IDM_EDIT_DUPLICATE_LINE 221
#define IDM_EDIT_MOVE_LINE_UP   222
#define IDM_EDIT_MOVE_LINE_DOWN 223
#define IDM_EDIT_DELETE_LINE    224
#define IDM_EDIT_JOIN_LINES     225
#define IDM_EDIT_TOGGLE_COMMENT 226
#define IDM_EDIT_TRIM_WHITESPACE 227
#define IDM_FORMAT_FONT   220
#define IDM_VIEW_TOGGLEPREVIEW 230
#define IDM_VIEW_TOGGLEEDITOR  231
#define IDM_VIEW_THEME_GITHUB_LIGHT 232
#define IDM_VIEW_THEME_GITHUB_DARK  233
#define IDM_VIEW_PREVIEW_ON_LEFT 234
#define IDM_VIEW_NEXT_TAB        235
#define IDM_VIEW_PREVIOUS_TAB    236
#define IDM_VIEW_CUSTOM_CSS      237
#define IDM_VIEW_CLEAR_CUSTOM_CSS 238
#define IDM_VIEW_FOCUS_MODE      239
#define IDM_VIEW_WORD_WRAP       242
#define IDM_HELP_REFERENCE 240

/* ---- menu command IDs: Markdown formatting toolbar/menu ---- */
#define IDM_FMT_BOLD          260
#define IDM_FMT_ITALIC        261
#define IDM_FMT_STRIKETHROUGH 262
#define IDM_FMT_CODE          263
#define IDM_FMT_H1            264
#define IDM_FMT_H2            265
#define IDM_FMT_H3            266
#define IDM_FMT_QUOTE         267
#define IDM_FMT_BULLETLIST    268
#define IDM_FMT_NUMBERLIST    269
#define IDM_FMT_TASKLIST      270
#define IDM_FMT_LINK          271
#define IDM_FMT_IMAGE         272
#define IDM_FMT_TABLE         273
#define IDM_FMT_CODEBLOCK     274
#define IDM_FMT_FOOTNOTE      275
#define IDM_FMT_HRULE         276
#define IDM_FMT_ALERT         277
#define IDM_FMT_JSON_PRETTY   278
#define IDM_FMT_JSON_MINIFY   279

/* ---- menu command IDs: developer/editing commands, Run and Debug ----
   Developer commands start at 280: 260-277 is routed wholesale to
   toolbar_command(), so IDs in that range never reach the switch. */
#define IDM_EDIT_UPPERCASE       280
#define IDM_EDIT_LOWERCASE       281
#define IDM_EDIT_TITLECASE       282
#define IDM_EDIT_SORT_ASC        283
#define IDM_EDIT_SORT_DESC       284
#define IDM_EDIT_SORT_DEDUPE     285
#define IDM_EDIT_TABS_TO_SPACES  286
#define IDM_EDIT_SPACES_TO_TABS  287
#define IDM_EDIT_BOOKMARK_TOGGLE 288
#define IDM_EDIT_BOOKMARK_NEXT   289
#define IDM_EDIT_BOOKMARK_PREV   290
#define IDM_EDIT_BOOKMARK_CLEAR  291
#define IDM_EDIT_SELECT_NEXT     292
#define IDM_EDIT_MATCH_BRACE     293
#define IDM_VIEW_SPELL_CHECK     294
#define IDM_VIEW_SHOW_WHITESPACE 295
#define IDM_VIEW_INDENT_GUIDES   296
#define IDM_VIEW_FOLD_TOGGLE     297
#define IDM_VIEW_FOLD_ALL        298
#define IDM_VIEW_UNFOLD_ALL      299
#define IDM_FILE_PRINT           300
#define IDM_FILE_EXPORT_PDF      301
#define IDM_FILE_QUICK_OPEN      302
#define IDM_FILE_FIND_FOLDER     303
#define IDM_EDIT_COMMAND_PALETTE 304
#define IDM_FMT_TABLE_EDITOR     305
#define IDM_RUN_FILE             306
#define IDM_DEBUG_FILE           307
#define IDM_VIEW_OUTLINE         308
#define IDM_VIEW_SPELL_LANGUAGE  309
#define IDM_DEBUG_BREAKPOINT     310
#define IDM_DEBUG_CLEAR_BREAKPOINTS 311
#define IDM_DEBUG_CONTINUE       312
#define IDM_DEBUG_STEP_OVER      313
#define IDM_DEBUG_STEP_INTO      314
#define IDM_VIEW_ZOOM_IN         315
#define IDM_VIEW_ZOOM_OUT        316
#define IDM_VIEW_ZOOM_RESET      317
#define IDM_FILE_SHOW_EXPLORER   318
#define IDM_EDIT_SELECT_ALL_OCCURRENCES 319
#define IDM_EDIT_EOL_CRLF        320
#define IDM_EDIT_EOL_LF          321
#define IDM_EDIT_EOL_CR          322
#define IDM_FILE_GO_BACK         323
#define IDM_FILE_GIT_CHANGES     324
#define IDM_VIEW_MINIMAP         325

/* ---- resource IDs (markedit.rc) ---- */
#define IDI_APPICON 1

/* ---- forward declarations for functions defined in markedit.c ----
   Grouped roughly by area: preview/theme, menu/layout, spell check,
   status/outline, recovery, dialogs, and document/tab management. */
void do_save(void);
static void update_preview(void);
void push_html_to_webview(const char *htmlFragment);
static void apply_preview_theme(void);
static void apply_editor_theme(HWND hEdit);
static void apply_markdown_lexer_styles(HWND hEdit);
static void apply_language_styles(HWND hEdit);
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
static void clear_recovery_snapshot(void);
static int restore_recovery_snapshot(void);
static int show_choice_dialog(const WCHAR *title, const WCHAR *prompt,
                              WCHAR **labels, int count);
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
static WCHAR *get_settings_path(void);
static void do_close_document_at(int index);
static void do_reopen_closed_tab(void);
static void save_session_documents(void);
static void restore_session_documents(void);
static void sync_preview_to_editor_scroll(void);
static void apply_custom_preview_css(int reportErrors);
static int paste_image_from_clipboard(void);

#endif
