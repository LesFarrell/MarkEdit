#ifndef TINKEREDIT_TERMINAL_VIEW_H
#define TINKEREDIT_TERMINAL_VIEW_H

#include <windows.h>

/* The Terminal tab's control: owns one TerminalProcess (src/terminal_process.h,
   a ConPTY-backed shell) and one TerminalGrid (src/terminal_grid.h, the
   VT100/ANSI screen it renders), and takes keyboard/mouse input directly.
   Focused keyboard messages are routed here before application accelerators,
   so control and Meta keys reach the shell instead of invoking editor commands.
   The Output/Run tab instead uses a read-only log and shared input line.

   Built the same way as the output panel's own children
   (output_panel.inc's create_output_scintilla/create_output_button): a
   stock "STATIC" window class, subclassed, WS_TABSTOP so it takes focus
   directly - no new window class to register. */

typedef struct TerminalView TerminalView;

/* An NMHDR.code sent (via WM_NOTIFY) to parent when the shell process exits
   (the control shows its own "ended" note on screen; the parent just needs
   to update the Stop button and the tab's running-dot - terminal_view_running()
   answers 0 from then), and when a key is pressed while nothing is running,
   asking the parent to start it again (the parent owns the command line -
   see output_panel.inc's locate_powershell()/terminal_directory()). */
#define TVN_EXITED ((UINT)-2000)
#define TVN_RESTART_REQUESTED ((UINT)-2001)

/* Creates the control as parent's child with control id id, initially with
   no shell running. Returns NULL on failure. */
TerminalView *terminal_view_create(HWND parent, int id);
HWND terminal_view_window(const TerminalView *view);
void terminal_view_free(TerminalView *view);

/* Call before TranslateAccelerator()/IsDialogMessage() in the application
   message loop. It translates and dispatches keyboard messages addressed to
   this control, preventing editor accelerators and menu mnemonics from taking
   terminal input. Returns nonzero when it consumed the message. */
int terminal_view_pretranslate_message(TerminalView *view, const MSG *message);

/* Starts commandLine (a shell) in directory, sized to the control's current
   client area. Ends any shell already running first. Returns 0 when ConPTY
   isn't available on this Windows or the shell can't be started. */
int terminal_view_start(TerminalView *view, const WCHAR *commandLine, const WCHAR *directory);
/* Shows message as a line of the control's own text, for when there's no
   shell to have printed anything itself (e.g. it couldn't be started). */
void terminal_view_show_message(TerminalView *view, const WCHAR *message);
int terminal_view_running(const TerminalView *view);
int terminal_view_session_active(const TerminalView *view);
/* Ends the shell and everything it started, without waiting for its last output. */
void terminal_view_stop(TerminalView *view);
/* Drops scrollback and clears the screen, keeping the shell running - like
   a real terminal's "clear" / Ctrl+L, not output_panel.inc's Clear button on
   the Run tab, which only touches its own read-only log. */
void terminal_view_clear_scrollback(TerminalView *view);

/* An in-process interactive session, used by the bundled Lua runner. It
   replaces the current shell, writes ANSI/UTF-8 directly to the terminal
   grid, and reads one echoed line from the terminal control. After end(),
   the transcript remains until the next key restarts the normal shell. */
int terminal_view_begin_session(TerminalView *view);
void terminal_view_write(TerminalView *view, const char *text, size_t length);
char *terminal_view_read_line(TerminalView *view, HWND owner, const char *prompt,
                              int *interrupted);
void terminal_view_end_session(TerminalView *view);
/* The rendered screen's text - for tests; see terminal_grid.h's own
   terminal_grid_line_text(), whose contract (trimmed, UTF-8, NUL-terminated
   within capacity) this follows. */
int terminal_view_total_lines(const TerminalView *view);
void terminal_view_line_text(const TerminalView *view, int lineIndex, char *utf8, size_t capacity);

/* The 16 ANSI colours and the default foreground/background for the current
   theme - see output_panel.inc's OUTPUT_ANSI_LIGHT/DARK, which the caller
   already has for the Run tab and reuses here. Recolours everything on
   screen and in scrollback, not just new output (see terminal_grid.h). */
void terminal_view_apply_theme(TerminalView *view, const COLORREF ansi[16],
                               COLORREF defaultFg, COLORREF defaultBg);
/* faceNameUtf8 is UTF-8, matching apply_output_panel_font()'s own caller in
   editor_view.inc, which already converts g_fontFaceName once for that. */
void terminal_view_apply_font(TerminalView *view, const char *faceNameUtf8, int pointSize, int weight);

#endif
