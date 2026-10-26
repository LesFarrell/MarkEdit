/* ---- The output panel: Run's output, a real terminal, and the debugger --
   Docked under the editor and preview with a splitter above it (see
   layout_output_panel()). The Output tab is a read-only Scintilla log that
   scrolls sideways rather than wraps, showing whatever Run started plus
   what plugins write with the output.* API (see write_plugin_output()); the
   line along the bottom sends typed input to it. The Terminal tab is a real
   PowerShell on a Windows pseudoconsole (ConPTY), rendered and driven by
   terminal_view.c - full-screen programs (vim, less, PSReadLine's live
   completion) work there the way they would in any other terminal, and
   keystrokes go to it directly rather than through the shared input line.
   The Debug tab shows a running debug session's status and variables
   (populated by debugger.inc, shown via its show_debug_panel()); it reuses
   each tab's view/input-line split for the variables list and status line.

   output_process.c runs the Output tab's programs and output_text.c decodes
   what they write; this file only connects them to the Scintilla views.
   terminal_view.c owns its TerminalProcess and TerminalGrid, while
   terminal_input.c encodes non-text keys as VT sequences; this file only
   starts, sizes, themes and stops the terminal. */

#define OUTPUT_MAX_LENGTH (4 * 1024 * 1024) /* older output is dropped beyond this */
#define OUTPUT_HISTORY_SIZE 50

/* Styles of the panel's own text, after the decoder's ANSI colours. */
enum { OUTPUT_STYLE_NOTE = OUTPUT_STYLE_COUNT, OUTPUT_STYLE_FAILURE, OUTPUT_STYLE_INPUT };

/* Python writes to a pipe in large blocks and in the ANSI code page unless
   asked otherwise; everything else inherits MarkEdit's environment. */
static const WCHAR OUTPUT_ENVIRONMENT[] = L"PYTHONUNBUFFERED=1\0PYTHONIOENCODING=utf-8\0";

typedef struct OutputPane {
    HWND view, tab;
    OutputProcess *process;
    int session;
    OutputDecoder decoder;
    int replaceLine;  /* a lone carriage return: the next text replaces the line */
    WCHAR *directory; /* where relative paths in the output are found */
} OutputPane;

static OutputPane g_outputPanes[OUTPUT_TAB_COUNT];
static TerminalView *g_terminalView; /* the Terminal tab; g_outputPanes[OUTPUT_TAB_TERMINAL].view is its window */
static int g_outputTab = OUTPUT_TAB_RUN;
static int g_outputNextSession = 1;
static HWND g_hOutputClear, g_hOutputStop, g_hOutputClose;
static char *g_outputHistory[OUTPUT_HISTORY_SIZE];
static int g_outputHistoryCount = 0;
static int g_outputHistoryIndex = 0; /* g_outputHistoryCount while not browsing it */

/* The 16 ANSI colours, as most terminals show them on a light and on a dark
   background. Dark's "black" is grey, so text in it stays readable. */
static const COLORREF OUTPUT_ANSI_LIGHT[16] = {
    RGB(0, 0, 0), RGB(205, 49, 49), RGB(0, 135, 0), RGB(148, 152, 0),
    RGB(4, 81, 165), RGB(188, 5, 188), RGB(5, 152, 188), RGB(85, 85, 85),
    RGB(102, 102, 102), RGB(205, 49, 49), RGB(20, 160, 20), RGB(181, 186, 0),
    RGB(4, 81, 165), RGB(188, 5, 188), RGB(5, 152, 188), RGB(165, 165, 165),
};
static const COLORREF OUTPUT_ANSI_DARK[16] = {
    RGB(118, 118, 118), RGB(205, 49, 49), RGB(13, 188, 121), RGB(229, 229, 16),
    RGB(36, 114, 200), RGB(188, 63, 188), RGB(17, 168, 205), RGB(229, 229, 229),
    RGB(102, 102, 102), RGB(241, 76, 76), RGB(35, 209, 139), RGB(245, 245, 67),
    RGB(59, 142, 234), RGB(214, 112, 214), RGB(41, 184, 219), RGB(229, 229, 229),
};

static OutputPane *active_output_pane(void) {
    return &g_outputPanes[g_outputTab];
}

static int output_pane_running(const OutputPane *pane) {
    return pane->process != NULL;
}

/* Whether the given tab has something running - the Run tab's OutputProcess,
   or the Terminal tab's own shell. */
static int tab_running(int tab) {
    return tab == OUTPUT_TAB_TERMINAL ? terminal_view_running(g_terminalView)
                                      : output_pane_running(&g_outputPanes[tab]);
}

/* Where Tab/pane-cycling (ui_shell.inc's cycle_pane_focus) should send focus
   when it reaches the output panel: the shared input line normally, the
   Terminal tab's own control (which takes keystrokes directly), or the
   Debug tab's variables list. */
static HWND active_output_focus_target(void) {
    if (g_outputTab == OUTPUT_TAB_TERMINAL) return terminal_view_window(g_terminalView);
    if (g_outputTab == OUTPUT_TAB_DEBUG) return g_hDebugVariables;
    return g_hOutputInput;
}

/* WM_MOUSEWHEEL follows the focused-window chain on systems/settings that do
   not route it to the window under the pointer. Forward it explicitly when
   the visible Terminal pane is under the wheel, so hovering is sufficient. */
static int route_terminal_mouse_wheel(WPARAM wParam, LPARAM lParam) {
    if (g_outputTab != OUTPUT_TAB_TERMINAL || !g_terminalView) return 0;
    HWND terminal = terminal_view_window(g_terminalView);
    if (!terminal || !IsWindowVisible(terminal)) return 0;
    RECT bounds;
    POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    if (!GetWindowRect(terminal, &bounds) || !PtInRect(&bounds, point)) return 0;
    SendMessageW(terminal, WM_MOUSEWHEEL, wParam, lParam);
    return 1;
}

/* ---- appearance ---- */

static void apply_output_view_theme(HWND view) {
    if (!view) return;
    themed_scrollbar_refresh(view);
    const ThemePalette *p = theme_palette();
    SendMessageA(view, SCI_STYLESETFORE, STYLE_DEFAULT, p->fg);
    SendMessageA(view, SCI_STYLESETBACK, STYLE_DEFAULT, p->bg);
    SendMessageA(view, SCI_STYLECLEARALL, 0, 0);
    const COLORREF *ansi = g_editorDarkMode ? OUTPUT_ANSI_DARK : OUTPUT_ANSI_LIGHT;
    for (int i = 0; i < 16; i++)
        SendMessageA(view, SCI_STYLESETFORE, (WPARAM)(OUTPUT_STYLE_COLOUR + i), (LPARAM)ansi[i]);
    SendMessageA(view, SCI_STYLESETFORE, OUTPUT_STYLE_NOTE, (LPARAM)p->comment);
    SendMessageA(view, SCI_STYLESETFORE, OUTPUT_STYLE_FAILURE, (LPARAM)p->error);
    SendMessageA(view, SCI_STYLESETFORE, OUTPUT_STYLE_INPUT, (LPARAM)p->keyword);
    SendMessageA(view, SCI_SETCARETFORE, p->fg, 0);
    SendMessageA(view, SCI_SETSELBACK, TRUE, p->selectionBg);
    SendMessageA(view, SCI_SETSELFORE, TRUE, p->selectionFg);
}

/* The debugger stays in the output panel instead of a tool window of its
   own. Its buttons remain native controls for keyboard/accessibility, with
   owner-drawing used only to match the app's exact border palette. */
static void apply_debug_tab_theme(void) {
    HWND controls[] = { g_hDebugStatus, g_hDebugContinue, g_hDebugStepOver,
                        g_hDebugStepInto, g_hDebugStepOut, g_hDebugVariables };
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++)
        if (controls[i]) {
            apply_explorer_theme(controls[i]);
            InvalidateRect(controls[i], NULL, TRUE);
        }
    if (g_hDebugVariables) {
        ListView_SetBkColor(g_hDebugVariables, theme_bg_color());
        ListView_SetTextBkColor(g_hDebugVariables, theme_bg_color());
        ListView_SetTextColor(g_hDebugVariables, theme_fg_color());
        HWND header = ListView_GetHeader(g_hDebugVariables);
        if (header) {
            apply_explorer_theme(header);
            InvalidateRect(header, NULL, TRUE);
        }
    }
}

static void apply_output_panel_theme(void) {
    if (!g_hOutputPanel) return;
    apply_output_view_theme(g_outputPanes[OUTPUT_TAB_RUN].view);
    apply_output_view_theme(g_hOutputInput);
    const ThemePalette *p = theme_palette();
    const COLORREF *ansi = g_editorDarkMode ? OUTPUT_ANSI_DARK : OUTPUT_ANSI_LIGHT;
    terminal_view_apply_theme(g_terminalView, ansi, p->fg, p->bg);
    apply_debug_tab_theme();
    InvalidateRect(g_hOutputPanel, NULL, TRUE);
    HWND buttons[] = { g_outputPanes[0].tab, g_outputPanes[1].tab, g_outputPanes[2].tab,
                       g_hOutputClear, g_hOutputStop, g_hOutputClose,
                       g_hDebugContinue, g_hDebugStepOver, g_hDebugStepInto, g_hDebugStepOut };
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        if (buttons[i]) InvalidateRect(buttons[i], NULL, TRUE);
}

/* The editor's font, at its size, for the Run tab's views, the input line
   and the Terminal tab. */
static void apply_output_panel_font(const char *faceUtf8) {
    if (!g_hOutputPanel) return;
    HWND views[] = { g_outputPanes[OUTPUT_TAB_RUN].view, g_hOutputInput };
    for (size_t i = 0; i < sizeof(views) / sizeof(views[0]); i++) {
        SendMessageA(views[i], SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)faceUtf8);
        SendMessageA(views[i], SCI_STYLESETSIZE, STYLE_DEFAULT, g_fontPointSize);
        SendMessageA(views[i], SCI_STYLESETWEIGHT, STYLE_DEFAULT, g_fontWeight);
        apply_output_view_theme(views[i]); /* its STYLECLEARALL spreads the font */
    }
    terminal_view_apply_font(g_terminalView, faceUtf8, g_fontPointSize, g_fontWeight);
}

/* The Stop button and the tab labels follow what's running. */
static void update_output_controls(void) {
    if (!g_hOutputPanel) return;
    EnableWindow(g_hOutputStop, tab_running(g_outputTab));
    for (int i = 0; i < OUTPUT_TAB_COUNT; i++) InvalidateRect(g_outputPanes[i].tab, NULL, TRUE);
}

/* ---- text ---- */

/* Keeps the newest output when a view grows past OUTPUT_MAX_LENGTH, cutting
   at a line start so the first line shown is whole. */
static void trim_output_view(HWND view) {
    Sci_Position length = (Sci_Position)SendMessageA(view, SCI_GETLENGTH, 0, 0);
    if (length <= OUTPUT_MAX_LENGTH) return;
    Sci_Position line = (Sci_Position)SendMessageA(view, SCI_LINEFROMPOSITION,
                                                   (WPARAM)(length - OUTPUT_MAX_LENGTH * 3 / 4), 0);
    Sci_Position cut = (Sci_Position)SendMessageA(view, SCI_POSITIONFROMLINE, (WPARAM)(line + 1), 0);
    if (cut > 0) SendMessageA(view, SCI_DELETERANGE, 0, (LPARAM)cut);
}

static void append_output(OutputPane *pane, int style, const char *text, size_t length) {
    HWND view = pane->view;
    Sci_Position end = (Sci_Position)SendMessageA(view, SCI_GETLENGTH, 0, 0);
    /* Follow new output only while the reader is at the end of it. */
    int follow = (Sci_Position)SendMessageA(view, SCI_GETCURRENTPOS, 0, 0) == end &&
                 SendMessageA(view, SCI_GETSELECTIONEMPTY, 0, 0);
    SendMessageA(view, SCI_SETREADONLY, FALSE, 0);
    if (pane->replaceLine) {
        Sci_Position line = (Sci_Position)SendMessageA(view, SCI_LINEFROMPOSITION, (WPARAM)end, 0);
        Sci_Position start = (Sci_Position)SendMessageA(view, SCI_POSITIONFROMLINE, (WPARAM)line, 0);
        SendMessageA(view, SCI_DELETERANGE, (WPARAM)start, (LPARAM)(end - start));
        end = start;
        pane->replaceLine = 0;
    }
    SendMessageA(view, SCI_APPENDTEXT, (WPARAM)length, (LPARAM)text);
    SendMessageA(view, SCI_STARTSTYLING, (WPARAM)end, 0);
    SendMessageA(view, SCI_SETSTYLING, (WPARAM)length, (LPARAM)style);
    trim_output_view(view);
    SendMessageA(view, SCI_SETREADONLY, TRUE, 0);
    if (follow) {
        Sci_Position newEnd = (Sci_Position)SendMessageA(view, SCI_GETLENGTH, 0, 0);
        SendMessageA(view, SCI_GOTOPOS, (WPARAM)newEnd, 0);
    }
}

static void decoded_output_text(void *context, int style, const char *utf8, size_t length) {
    append_output((OutputPane *)context, style, utf8, length);
}

static void decoded_carriage_return(void *context) {
    ((OutputPane *)context)->replaceLine = 1;
}

static const OutputSink OUTPUT_PANE_SINK = { decoded_output_text, decoded_carriage_return };

/* A line of the panel's own, starting on a line of its own. */
static void append_output_note(OutputPane *pane, int style, const WCHAR *text) {
    char *utf8 = to_utf8(text);
    if (!utf8) return;
    Sci_Position length = (Sci_Position)SendMessageA(pane->view, SCI_GETLENGTH, 0, 0);
    if (length > 0 && (char)SendMessageA(pane->view, SCI_GETCHARAT, (WPARAM)(length - 1), 0) != '\n')
        append_output(pane, OUTPUT_STYLE_DEFAULT, "\n", 1);
    append_output(pane, style, utf8, strlen(utf8));
    append_output(pane, style, "\n", 1);
    free(utf8);
}

static void clear_output_pane(OutputPane *pane) {
    SendMessageA(pane->view, SCI_SETREADONLY, FALSE, 0);
    SendMessageA(pane->view, SCI_CLEARALL, 0, 0);
    SendMessageA(pane->view, SCI_SETSCROLLWIDTH, 1, 0); /* tracking only ever widens it */
    SendMessageA(pane->view, SCI_SETREADONLY, TRUE, 0);
    pane->replaceLine = 0;
}

/* ---- programs ---- */

/* Ends the pane's program, if any, without waiting for its last output. */
static void end_output_program(OutputPane *pane) {
    if (!pane->process) return;
    output_process_free(pane->process);
    pane->process = NULL;
    pane->session = 0; /* chunks still on their way are dropped */
    output_decoder_finish(&pane->decoder, &OUTPUT_PANE_SINK, pane);
}

static int start_output_program(OutputPane *pane, const WCHAR *commandLine, const WCHAR *directory) {
    end_output_program(pane);
    output_decoder_init(&pane->decoder);
    pane->replaceLine = 0;
    int session = g_outputNextSession++;
    pane->process = output_process_start(commandLine, directory, OUTPUT_ENVIRONMENT,
                                         g_hMainWnd, WM_APP_OUTPUT, session);
    if (!pane->process) return 0;
    pane->session = session;
    free(pane->directory);
    pane->directory = directory ? _wcsdup(directory) : NULL;
    return 1;
}

/* WM_APP_OUTPUT: output from Run's program, or the news that it has
   finished. The Terminal tab's own output arrives at its own window
   instead - see terminal_view.c's WM_TERMINAL_CHUNK. */
static void handle_output_chunk(OutputChunk *chunk) {
    OutputPane *pane = &g_outputPanes[OUTPUT_TAB_RUN];
    if (pane->process && pane->session == chunk->session) {
        output_decoder_feed(&pane->decoder, chunk->data, chunk->length, &OUTPUT_PANE_SINK, pane);
        if (chunk->finished) {
            end_output_program(pane);
            update_output_controls();
        }
    }
    free(chunk);
}

/* The folder a new terminal starts in: the workspace, or the document's
   folder, or the user's profile. Caller frees. */
static WCHAR *terminal_directory(void) {
    if (g_currentWorkspaceFolder) return _wcsdup(g_currentWorkspaceFolder);
    if (g_currentFile) return path_directory(g_currentFile);
    WCHAR profile[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    return length > 0 && length < MAX_PATH ? _wcsdup(profile) : NULL;
}

/* Starts (or restarts) the Terminal tab's shell: a real, interactive
   PowerShell session, the same as the user would get opening it directly -
   unlike the old pipe-based terminal, there's no need to suppress its
   profile or wrap commands to survive a non-interactive pipe. */
static int start_terminal(void) {
    if (terminal_view_running(g_terminalView)) return 1;
    WCHAR *powershell = locate_powershell();
    WCHAR *directory = terminal_directory();
    size_t capacity = (powershell ? wcslen(powershell) : 0) + 16;
    WCHAR *command = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    int started = 0;
    if (powershell && command && swprintf(command, capacity, L"\"%ls\" -NoLogo", powershell) > 0)
        started = terminal_view_start(g_terminalView, command, directory);
    if (!started)
        terminal_view_show_message(g_terminalView, L"PowerShell could not be started.");
    free(command);
    free(directory);
    free(powershell);
    update_output_controls();
    return started;
}

/* ---- showing the panel ---- */

static void size_output_panel(HWND panel); /* defined below, in "the panel window" section */

static void select_output_tab(int tab) {
    g_outputTab = tab;
    for (int i = 0; i < OUTPUT_TAB_COUNT; i++)
        ShowWindow(g_outputPanes[i].view, i == tab ? SW_SHOW : SW_HIDE);
    /* The Terminal tab's content rect and the input line's visibility both
       depend on which tab is selected (see output_content_rect()). */
    if (g_hOutputPanel) size_output_panel(g_hOutputPanel);
    update_output_controls();
    if (tab == OUTPUT_TAB_TERMINAL) start_terminal();
}

static void show_output_panel(HWND hwnd, int tab, int focusInput) {
    if (!g_hOutputPanel) return;
    select_output_tab(tab);
    if (!g_outputPanelVisible) {
        g_outputPanelVisible = 1;
        relayout_and_save(hwnd);
    }
    if (focusInput) SetFocus(active_output_focus_target());
}

static void hide_output_panel(HWND hwnd) {
    if (!g_outputPanelVisible) return;
    g_outputPanelVisible = 0;
    relayout_and_save(hwnd);
    if (g_editorVisible) SetFocus(g_hEdit);
}

/* View > Output Panel (Ctrl+`). */
static void toggle_output_panel(HWND hwnd) {
    if (g_outputPanelVisible) hide_output_panel(hwnd);
    else show_output_panel(hwnd, g_outputTab, 1);
}

/* View > Terminal (Ctrl+Shift+`). */
static void show_terminal(HWND hwnd) {
    show_output_panel(hwnd, OUTPUT_TAB_TERMINAL, 1);
}

/* Run's way into the panel: runs `"file" parameters` on the Output tab,
   replacing whatever ran there before. title names the run ("Run
   Python"). Returns 0 when the program can't be started. */
static int run_in_output_panel(const WCHAR *file, const WCHAR *parameters,
                               const WCHAR *directory, const WCHAR *title) {
    OutputPane *pane = &g_outputPanes[OUTPUT_TAB_RUN];
    if (!g_hOutputPanel) return 0;
    size_t capacity = wcslen(file) + wcslen(parameters) + 4;
    WCHAR *command = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    if (!command || swprintf(command, capacity, L"\"%ls\" %ls", file, parameters) < 0) {
        free(command);
        return 0;
    }
    end_output_program(pane);
    clear_output_pane(pane);
    size_t noteCapacity = wcslen(title) + (g_currentFile ? wcslen(g_currentFile) : 0) + 8;
    WCHAR *note = (WCHAR *)malloc(noteCapacity * sizeof(WCHAR));
    if (note) {
        swprintf(note, noteCapacity, L"%ls: %ls", title, g_currentFile ? g_currentFile : L"");
        append_output_note(pane, OUTPUT_STYLE_NOTE, note);
        free(note);
    }
    int started = start_output_program(pane, command, directory);
    free(command);
    show_output_panel(g_hMainWnd, OUTPUT_TAB_RUN, 0);
    update_output_controls();
    return started;
}

/* The plugin API's output.write(), on the Output tab. Text from plugins
   isn't a program's stream, so it has a decoder of its own. */
static void write_plugin_output(const char *text) {
    if (!g_hOutputPanel) return;
    static OutputDecoder decoder;
    static int initialized = 0;
    if (!initialized) { output_decoder_init(&decoder); initialized = 1; }
    OutputPane *pane = &g_outputPanes[OUTPUT_TAB_RUN];
    output_decoder_feed(&decoder, text, strlen(text), &OUTPUT_PANE_SINK, pane);
    output_decoder_finish(&decoder, &OUTPUT_PANE_SINK, pane);
}

/* ---- the input line ---- */

static char *output_input_text(void) {
    Sci_Position length = (Sci_Position)SendMessageA(g_hOutputInput, SCI_GETLENGTH, 0, 0);
    char *text = (char *)malloc((size_t)length + 1);
    if (text) SendMessageA(g_hOutputInput, SCI_GETTEXT, (WPARAM)length + 1, (LPARAM)text);
    return text;
}

static void remember_output_input(const char *text) {
    if (!text[0]) return;
    if (g_outputHistoryCount && strcmp(g_outputHistory[g_outputHistoryCount - 1], text) == 0) return;
    if (g_outputHistoryCount == OUTPUT_HISTORY_SIZE) {
        free(g_outputHistory[0]);
        memmove(g_outputHistory, g_outputHistory + 1, (OUTPUT_HISTORY_SIZE - 1) * sizeof(char *));
        g_outputHistoryCount--;
    }
    char *copy = _strdup(text);
    if (copy) g_outputHistory[g_outputHistoryCount++] = copy;
}

static void browse_output_history(int delta) {
    int index = g_outputHistoryIndex + delta;
    if (index < 0 || index > g_outputHistoryCount) return;
    g_outputHistoryIndex = index;
    const char *text = index < g_outputHistoryCount ? g_outputHistory[index] : "";
    SendMessageA(g_hOutputInput, SCI_SETTEXT, 0, (LPARAM)text);
    SendMessageA(g_hOutputInput, SCI_DOCUMENTEND, 0, 0);
}

/* Enter: sends the line to the Run tab's program, echoing it in the output.
   The shared input line is Run-tab-only; the Terminal tab takes keystrokes
   directly (see terminal_view.c). */
static void submit_output_input(void) {
    char *text = output_input_text();
    if (!text) return;
    OutputPane *pane = active_output_pane();
    remember_output_input(text);
    g_outputHistoryIndex = g_outputHistoryCount;
    SendMessageA(g_hOutputInput, SCI_CLEARALL, 0, 0);
    size_t length = strlen(text);
    if (output_pane_running(pane)) {
        append_output(pane, OUTPUT_STYLE_INPUT, text, length);
        append_output(pane, OUTPUT_STYLE_INPUT, "\n", 1);
        output_process_write(pane->process, text, length);
        output_process_write(pane->process, "\r\n", 2);
    } else {
        append_output_note(pane, OUTPUT_STYLE_NOTE,
                           L"[Nothing is running. Run a file with Ctrl+F5, or use the Terminal tab.]");
    }
    free(text);
}

static LRESULT CALLBACK OutputInputSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                UINT_PTR id, DWORD_PTR data) {
    (void)id; (void)data;
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) { submit_output_input(); return 0; }
        if (wParam == VK_UP) { browse_output_history(-1); return 0; }
        if (wParam == VK_DOWN) { browse_output_history(1); return 0; }
        if (wParam == VK_ESCAPE && g_editorVisible) { SetFocus(g_hEdit); return 0; }
    } else if (msg == WM_CHAR && (wParam == '\r' || wParam == '\n')) {
        return 0;
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, OutputInputSubclassProc, 1);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

/* Typing into an output view types into the input line instead. */
static LRESULT CALLBACK OutputViewSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                               UINT_PTR id, DWORD_PTR data) {
    (void)data;
    if (msg == WM_CHAR && wParam >= ' ' && !(GetKeyState(VK_CONTROL) & 0x8000)) {
        SetFocus(g_hOutputInput);
        SendMessageW(g_hOutputInput, WM_CHAR, wParam, lParam);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, OutputViewSubclassProc, id);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

/* ---- file locations ---- */

/* The file named by the location's path, relative to directory if it isn't
   absolute, or NULL when there's no such file. Caller frees. */
static WCHAR *output_location_file(const char *line, const OutputLocation *location,
                                   const WCHAR *directory) {
    char *path = (char *)malloc(location->pathLength + 1);
    if (!path) return NULL;
    memcpy(path, line + location->pathStart, location->pathLength);
    path[location->pathLength] = 0;
    WCHAR *wide = to_wide(path);
    free(path);
    if (!wide) return NULL;
    WCHAR combined[MAX_PATH];
    const WCHAR *candidate = wide;
    if (PathIsRelativeW(wide) && directory && PathCombineW(combined, directory, wide))
        candidate = combined;
    WCHAR *file = file_exists(candidate) ? absolute_path(candidate) : NULL;
    free(wide);
    return file;
}

/* Opens the file location on the line holding position, if there is one. */
static int open_output_location(OutputPane *pane, Sci_Position position) {
    Sci_Position lineNumber = (Sci_Position)SendMessageA(pane->view, SCI_LINEFROMPOSITION, (WPARAM)position, 0);
    Sci_Position length = (Sci_Position)SendMessageA(pane->view, SCI_LINELENGTH, (WPARAM)lineNumber, 0);
    char *line = (char *)malloc((size_t)length + 1);
    if (!line) return 0;
    SendMessageA(pane->view, SCI_GETLINE, (WPARAM)lineNumber, (LPARAM)line);
    line[length] = 0;
    OutputLocation location;
    WCHAR *file = NULL;
    if (output_find_location(line, (size_t)length, &location)) {
        do {
            file = output_location_file(line, &location, pane->directory);
        } while (!file && output_location_next_candidate(line, &location));
    }
    free(line);
    if (!file) return 0;
    int opened = open_file_in_tab(file);
    free(file);
    if (!opened) return 0;
    Sci_Position target = (Sci_Position)SendMessageA(g_hEdit, SCI_FINDCOLUMN, (WPARAM)(location.line - 1),
                                                     (LPARAM)(location.column > 0 ? location.column - 1 : 0));
    SendMessageA(g_hEdit, SCI_GOTOPOS, (WPARAM)target, 0);
    SendMessageA(g_hEdit, SCI_SCROLLCARET, 0, 0);
    SetFocus(g_hEdit);
    return 1;
}

/* ---- the panel window ---- */

static void paint_debug_header(HWND header, HDC hdc) {
    RECT client;
    GetClientRect(header, &client);
    fill_rect_color(hdc, &client, theme_bg_color());

    HFONT font = (HFONT)SendMessageW(header, WM_GETFONT, 0, 0);
    if (!font) font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    int count = (int)SendMessageW(header, HDM_GETITEMCOUNT, 0, 0);
    for (int column = 0; column < count; column++) {
        RECT rc = {0};
        if (!SendMessageW(header, HDM_GETITEMRECT, (WPARAM)column, (LPARAM)&rc)) continue;
        WCHAR label[64] = L"";
        HDITEMW item = {0};
        item.mask = HDI_TEXT;
        item.pszText = label;
        item.cchTextMax = sizeof(label) / sizeof(label[0]);
        SendMessageW(header, HDM_GETITEMW, (WPARAM)column, (LPARAM)&item);

        RECT divider = { rc.right - 1, rc.top, rc.right, rc.bottom };
        fill_rect_color(hdc, &divider, theme_border_color());
        rc.left += 7;
        rc.right -= 5;
        SetTextColor(hdc, theme_fg_color());
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, label, -1, &rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    RECT bottom = { client.left, client.bottom - 1, client.right, client.bottom };
    fill_rect_color(hdc, &bottom, theme_border_color());
    SelectObject(hdc, oldFont);
}

static LRESULT CALLBACK DebugHeaderSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                                 LPARAM lParam, UINT_PTR id,
                                                 DWORD_PTR data) {
    (void)lParam; (void)id; (void)data;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        paint_debug_header(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_PRINTCLIENT) {
        paint_debug_header(hwnd, (HDC)wParam);
        return 0;
    }
    if (msg == WM_THEMECHANGED) {
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, DebugHeaderSubclassProc, 1);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

/* The debug buttons' owner-draw painter; output_panel.inc's own WM_DRAWITEM
   falls back to it for anything that isn't a tab button (Clear/Stop/Close
   use it too - a plain bordered push-button looks the same either way). */
static void draw_debug_panel_button(const DRAWITEMSTRUCT *dis) {
    int pressed = (dis->itemState & ODS_SELECTED) != 0;
    int disabled = (dis->itemState & ODS_DISABLED) != 0;
    COLORREF fill = pressed ? theme_palette()->pressedBg : theme_bg_color();
    COLORREF text = disabled ? theme_palette()->disabledFg : theme_fg_color();
    fill_rect_color(dis->hDC, &dis->rcItem, fill);

    HPEN pen = CreatePen(PS_SOLID, 1, theme_border_color());
    HPEN oldPen = (HPEN)SelectObject(dis->hDC, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
    Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top,
              dis->rcItem.right, dis->rcItem.bottom);
    SelectObject(dis->hDC, oldBrush);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(pen);

    WCHAR label[64];
    GetWindowTextW(dis->hwndItem, label, sizeof(label) / sizeof(label[0]));
    RECT textRect = dis->rcItem;
    SetTextColor(dis->hDC, text);
    SetBkMode(dis->hDC, TRANSPARENT);
    DrawTextW(dis->hDC, label, -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (dis->itemState & ODS_FOCUS) {
        InflateRect(&textRect, -3, -3);
        DrawFocusRect(dis->hDC, &textRect);
    }
}

static void draw_output_tab_button(const DRAWITEMSTRUCT *dis, int tab) {
    const ThemePalette *p = theme_palette();
    int selected = tab == g_outputTab;
    fill_rect_color(dis->hDC, &dis->rcItem, selected ? p->tabSelectedBg : p->tabBg);
    if (selected) {
        RECT accent = { dis->rcItem.left, dis->rcItem.bottom - 2, dis->rcItem.right, dis->rcItem.bottom };
        fill_rect_color(dis->hDC, &accent, p->accent);
    }
    WCHAR label[32];
    GetWindowTextW(dis->hwndItem, label, sizeof(label) / sizeof(label[0]));
    /* A running program shows as a dot after its tab's name. */
    if (tab_running(tab) && wcslen(label) < 28) wcscat(label, L" \x2022");
    RECT textRect = dis->rcItem;
    SetTextColor(dis->hDC, selected ? p->tabSelectedFg : p->tabFg);
    SetBkMode(dis->hDC, TRANSPARENT);
    DrawTextW(dis->hDC, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (dis->itemState & ODS_FOCUS) {
        InflateRect(&textRect, -3, -3);
        DrawFocusRect(dis->hDC, &textRect);
    }
}

/* The panel's own rectangles, at the window's DPI. */
typedef struct OutputPanelLayout {
    int pad, header, input;
    RECT view, inputBox;
} OutputPanelLayout;

static OutputPanelLayout output_panel_layout(HWND panel) {
    RECT rc;
    GetClientRect(panel, &rc);
    int dpi = GetDpiForWindow(panel);
    OutputPanelLayout layout;
    layout.pad = MulDiv(OUTPUT_PANEL_PADDING, dpi, 96);
    layout.header = MulDiv(OUTPUT_HEADER_HEIGHT, dpi, 96);
    layout.input = MulDiv(OUTPUT_INPUT_HEIGHT, dpi, 96);
    int inputTop = rc.bottom - layout.pad - layout.input;
    layout.inputBox = (RECT){ layout.pad, inputTop, rc.right - layout.pad, rc.bottom - layout.pad };
    layout.view = (RECT){ layout.pad, layout.header, rc.right - layout.pad, inputTop - layout.pad };
    return layout;
}

/* The Terminal tab has no separate input line - it takes keystrokes
   directly - so its content fills the view rect and the input box's space
   below it; the Run tab keeps the two separate. */
static RECT output_content_rect(const OutputPanelLayout *layout) {
    RECT rect = layout->view;
    if (g_outputTab == OUTPUT_TAB_TERMINAL) rect.bottom = layout->inputBox.bottom;
    return rect;
}

#define OUTPUT_HEADER_BUTTON_MAX 5

typedef struct { HWND hwnd; int width; } OutputHeaderButton;

/* The header row's right-aligned buttons for the active tab, built right to
   left so Close ends up rightmost: Run/Terminal get Clear/Stop/Close; Debug
   gets its Continue/Step commands instead, in the same order the standalone
   debug panel used to show them. Returns how many of buttons were filled. */
static int active_output_buttons(OutputHeaderButton *buttons, int dpi) {
    int closeWidth = MulDiv(DEBUG_CLOSE_BUTTON_WIDTH, dpi, 96);
    int n = 0;
    buttons[n++] = (OutputHeaderButton){ g_hOutputClose, closeWidth };
    if (g_outputTab == OUTPUT_TAB_DEBUG) {
        int debugWidth = MulDiv(DEBUG_BUTTON_WIDTH, dpi, 96);
        buttons[n++] = (OutputHeaderButton){ g_hDebugStepOut, debugWidth };
        buttons[n++] = (OutputHeaderButton){ g_hDebugStepInto, debugWidth };
        buttons[n++] = (OutputHeaderButton){ g_hDebugStepOver, debugWidth };
        buttons[n++] = (OutputHeaderButton){ g_hDebugContinue, debugWidth };
    } else {
        int buttonWidth = MulDiv(OUTPUT_BUTTON_WIDTH, dpi, 96);
        buttons[n++] = (OutputHeaderButton){ g_hOutputStop, buttonWidth };
        buttons[n++] = (OutputHeaderButton){ g_hOutputClear, buttonWidth };
    }
    return n;
}

/* The tab's status line, shown where the Run tab's input line normally is:
   the input line itself, or the Debug tab's read-only status label. The
   Terminal tab has neither - it takes keystrokes directly. */
static HWND active_output_status_control(void) {
    if (g_outputTab == OUTPUT_TAB_RUN) return g_hOutputInput;
    if (g_outputTab == OUTPUT_TAB_DEBUG) return g_hDebugStatus;
    return NULL;
}

static void size_output_panel(HWND panel) {
    RECT rc;
    GetClientRect(panel, &rc);
    int dpi = GetDpiForWindow(panel);
    OutputPanelLayout layout = output_panel_layout(panel);
    int row = layout.header - layout.pad;
    int tabWidth = MulDiv(OUTPUT_TAB_WIDTH, dpi, 96);
    for (int i = 0; i < OUTPUT_TAB_COUNT; i++)
        MoveWindow(g_outputPanes[i].tab, layout.pad + i * tabWidth, layout.pad / 2, tabWidth, row, TRUE);
    /* The buttons line up from the right edge. Any that would run into the
       tabs are hidden instead, the leftmost first and Close last, since
       overlapping controls paint over each other. Buttons from a tab that
       isn't active are hidden outright, rather than left stacked underneath
       the active tab's. */
    HWND inactiveButtons[] = { g_hOutputClear, g_hOutputStop, g_hDebugContinue,
                               g_hDebugStepOver, g_hDebugStepInto, g_hDebugStepOut };
    for (size_t i = 0; i < sizeof(inactiveButtons) / sizeof(inactiveButtons[0]); i++)
        ShowWindow(inactiveButtons[i], SW_HIDE);
    int tabsRight = layout.pad + OUTPUT_TAB_COUNT * tabWidth;
    OutputHeaderButton buttons[OUTPUT_HEADER_BUTTON_MAX];
    int buttonCount = active_output_buttons(buttons, dpi);
    int x = rc.right;
    int room = 1;
    for (int i = 0; i < buttonCount; i++) {
        x -= layout.pad + buttons[i].width;
        room = room && x >= tabsRight + layout.pad;
        if (room) MoveWindow(buttons[i].hwnd, x, layout.pad / 2, buttons[i].width, row, TRUE);
        ShowWindow(buttons[i].hwnd, room ? SW_SHOW : SW_HIDE);
    }
    /* A control that only moves can keep the pixels it had, so the tabs and
       buttons redraw themselves at every size. */
    for (int i = 0; i < OUTPUT_TAB_COUNT; i++) InvalidateRect(g_outputPanes[i].tab, NULL, TRUE);
    for (int i = 0; i < buttonCount; i++) InvalidateRect(buttons[i].hwnd, NULL, TRUE);
    /* One pixel inside the borders paint_output_panel() draws. Each tab's
       view gets its own content rect: the Terminal tab's spans the input
       box's space too, since it has no separate input line. */
    for (int i = 0; i < OUTPUT_TAB_COUNT; i++) {
        RECT content = i == OUTPUT_TAB_TERMINAL ? output_content_rect(&layout) : layout.view;
        MoveWindow(g_outputPanes[i].view, content.left + 1, content.top + 1,
                   content.right - content.left - 2, content.bottom - content.top - 2, TRUE);
    }
    RECT input = layout.inputBox;
    HWND statusControl = active_output_status_control();
    ShowWindow(g_hOutputInput, statusControl == g_hOutputInput ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hDebugStatus, statusControl == g_hDebugStatus ? SW_SHOW : SW_HIDE);
    if (statusControl)
        MoveWindow(statusControl, input.left + 1, input.top + 1,
                   input.right - input.left - 2, input.bottom - input.top - 2, TRUE);
}

static void frame_rect(HDC hdc, const RECT *rc, COLORREF colour) {
    RECT edge = { rc->left, rc->top, rc->right, rc->top + 1 };
    fill_rect_color(hdc, &edge, colour);
    edge = (RECT){ rc->left, rc->bottom - 1, rc->right, rc->bottom };
    fill_rect_color(hdc, &edge, colour);
    edge = (RECT){ rc->left, rc->top, rc->left + 1, rc->bottom };
    fill_rect_color(hdc, &edge, colour);
    edge = (RECT){ rc->right - 1, rc->top, rc->right, rc->bottom };
    fill_rect_color(hdc, &edge, colour);
}

static void paint_output_panel(HWND panel, HDC hdc) {
    RECT rc;
    GetClientRect(panel, &rc);
    fill_rect_color(hdc, &rc, theme_bg_color());
    OutputPanelLayout layout = output_panel_layout(panel);
    RECT content = output_content_rect(&layout);
    frame_rect(hdc, &content, theme_border_color());
    if (g_outputTab != OUTPUT_TAB_TERMINAL) frame_rect(hdc, &layout.inputBox, theme_border_color());
}

static LRESULT CALLBACK OutputPanelSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                                LPARAM lParam, UINT_PTR id, DWORD_PTR data) {
    (void)id; (void)data;
    switch (msg) {
        case WM_SIZE:
            size_output_panel(hwnd);
            /* The borders sit at positions that follow the size, and a STATIC
               repaints only what a resize uncovers, which would leave the
               old ones behind. */
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        /* The panel paints itself: a STATIC control would otherwise fill its
           client area with the system's light dialog colour. */
        case WM_ERASEBKGND:
            return 1;
        /* An owner-drawn button erases itself with this brush before it is
           drawn; the default light one flashes on every resize in dark mode. */
        case WM_CTLCOLORBTN:
            if (g_mainBkBrush) return (LRESULT)g_mainBkBrush;
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            paint_output_panel(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            paint_output_panel(hwnd, (HDC)wParam);
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_OUTPUT_TAB_RUN: select_output_tab(OUTPUT_TAB_RUN); SetFocus(g_hOutputInput); return 0;
                case ID_OUTPUT_TAB_TERMINAL:
                    select_output_tab(OUTPUT_TAB_TERMINAL);
                    SetFocus(terminal_view_window(g_terminalView));
                    return 0;
                case ID_OUTPUT_TAB_DEBUG:
                    select_output_tab(OUTPUT_TAB_DEBUG);
                    SetFocus(g_hDebugVariables);
                    return 0;
                case IDM_DEBUG_CONTINUE: case IDM_DEBUG_STEP_OVER:
                case IDM_DEBUG_STEP_INTO: case IDM_DEBUG_STEP_OUT:
                    /* debugger.inc's run_debugger_command() is reached through
                       the main window's own WM_COMMAND dispatch. */
                    SendMessageW(g_hMainWnd, WM_COMMAND, LOWORD(wParam), 0);
                    return 0;
                case ID_OUTPUT_CLEAR:
                    if (g_outputTab == OUTPUT_TAB_TERMINAL) {
                        terminal_view_clear_scrollback(g_terminalView);
                    } else {
                        clear_output_pane(active_output_pane());
                    }
                    SetFocus(active_output_focus_target());
                    return 0;
                case ID_OUTPUT_STOP:
                    if (g_outputTab == OUTPUT_TAB_TERMINAL) {
                        if (terminal_view_running(g_terminalView)) terminal_view_stop(g_terminalView);
                    } else {
                        OutputPane *pane = active_output_pane();
                        if (pane->process) {
                            end_output_program(pane);
                            append_output_note(pane, OUTPUT_STYLE_FAILURE, L"[Stopped]");
                            update_output_controls();
                        }
                    }
                    return 0;
                case ID_OUTPUT_CLOSE:
                    hide_output_panel(g_hMainWnd);
                    return 0;
            }
            break;
        case WM_NOTIFY: {
            NMHDR *notification = (NMHDR *)lParam;
            if (notification->code == SCN_DOUBLECLICK) {
                for (int i = 0; i < OUTPUT_TAB_COUNT; i++)
                    if (notification->hwndFrom == g_outputPanes[i].view)
                        open_output_location(&g_outputPanes[i],
                                             ((struct SCNotification *)lParam)->position);
            } else if (notification->code == TVN_EXITED) {
                update_output_controls();
            } else if (notification->code == TVN_RESTART_REQUESTED) {
                start_terminal();
            }
            return 0;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType != ODT_BUTTON) break;
            int tab = -1;
            for (int i = 0; i < OUTPUT_TAB_COUNT; i++)
                if (dis->hwndItem == g_outputPanes[i].tab) { tab = i; break; }
            if (tab >= 0) draw_output_tab_button(dis, tab);
            else draw_debug_panel_button(dis);
            return TRUE;
        }
        /* g_hDebugStatus is a plain (non-owner-drawn) STATIC, so Windows asks
           for its colours here rather than through WM_DRAWITEM. */
        case WM_CTLCOLORSTATIC:
            if (g_mainBkBrush) {
                HDC hdc = (HDC)wParam;
                SetTextColor(hdc, theme_fg_color());
                SetBkMode(hdc, TRANSPARENT);
                return (LRESULT)g_mainBkBrush;
            }
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, OutputPanelSubclassProc, 1);
            break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static HWND create_output_scintilla(HWND parent, int id, int visible) {
    HWND view = CreateWindowExA(0, "Scintilla", "",
                                WS_CHILD | WS_CLIPSIBLINGS | (visible ? WS_VISIBLE : 0) | WS_TABSTOP,
                                0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(view, SCI_SETCODEPAGE, SC_CP_UTF8, 0);
    SendMessageA(view, SCI_SETEOLMODE, SC_EOL_LF, 0);
    for (int margin = 0; margin < 5; margin++) SendMessageA(view, SCI_SETMARGINWIDTHN, (WPARAM)margin, 0);
    SendMessageA(view, SCI_SETMARGINLEFT, 0, 4);
    SendMessageA(view, SCI_SETCARETLINEVISIBLE, FALSE, 0);
    return view;
}

static HWND create_output_button(HWND parent, const WCHAR *label, int id) {
    HWND button = CreateWindowExW(0, L"BUTTON", label,
                                  WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(button, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return button;
}

static void create_output_panel(HWND hwnd) {
    g_hOutputPanel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_CLIPCHILDREN,
                                     0, 0, 0, 0, hwnd, (HMENU)ID_OUTPUT_PANEL, GetModuleHandleW(NULL), NULL);
    SetWindowSubclass(g_hOutputPanel, OutputPanelSubclassProc, 1, 0);
    static const WCHAR *const labels[OUTPUT_TAB_COUNT] = { L"Output", L"Terminal", L"Debug" };
    static const int tabIds[OUTPUT_TAB_COUNT] = { ID_OUTPUT_TAB_RUN, ID_OUTPUT_TAB_TERMINAL, ID_OUTPUT_TAB_DEBUG };
    for (int i = 0; i < OUTPUT_TAB_COUNT; i++)
        g_outputPanes[i].tab = create_output_button(g_hOutputPanel, labels[i], tabIds[i]);

    OutputPane *runPane = &g_outputPanes[OUTPUT_TAB_RUN];
    runPane->view = create_output_scintilla(g_hOutputPanel, ID_OUTPUT_VIEW, g_outputTab == OUTPUT_TAB_RUN);
    SendMessageA(runPane->view, SCI_SETREADONLY, TRUE, 0);
    SendMessageA(runPane->view, SCI_SETUNDOCOLLECTION, FALSE, 0);
    /* Long lines scroll sideways rather than wrap, so output keeps its
       columns; the scrollbar appears only once a line is too wide. */
    SendMessageA(runPane->view, SCI_SETWRAPMODE, SC_WRAP_NONE, 0);
    SendMessageA(runPane->view, SCI_SETSCROLLWIDTHTRACKING, TRUE, 0);
    SendMessageA(runPane->view, SCI_SETSCROLLWIDTH, 1, 0);
    themed_scrollbar_attach(runPane->view, THEMED_SCROLL_SCINTILLA);
    SetWindowSubclass(runPane->view, OutputViewSubclassProc, (UINT_PTR)(OUTPUT_TAB_RUN + 1), 0);
    output_decoder_init(&runPane->decoder);

    g_terminalView = terminal_view_create(g_hOutputPanel, ID_TERMINAL_VIEW);
    g_outputPanes[OUTPUT_TAB_TERMINAL].view = terminal_view_window(g_terminalView);
    ShowWindow(g_outputPanes[OUTPUT_TAB_TERMINAL].view, g_outputTab == OUTPUT_TAB_TERMINAL ? SW_SHOW : SW_HIDE);

    int dpi = GetDpiForWindow(hwnd);
    g_hDebugStatus = CreateWindowExW(0, L"STATIC", L"Debugger",
        WS_CHILD | SS_CENTERIMAGE | SS_LEFTNOWORDWRAP,
        0, 0, 0, 0, g_hOutputPanel, (HMENU)ID_DEBUG_STATUS, GetModuleHandleW(NULL), NULL);
    g_hDebugContinue = create_output_button(g_hOutputPanel, L"Continue  F6", IDM_DEBUG_CONTINUE);
    g_hDebugStepOver = create_output_button(g_hOutputPanel, L"Step Over  F10", IDM_DEBUG_STEP_OVER);
    g_hDebugStepInto = create_output_button(g_hOutputPanel, L"Step Into  F11", IDM_DEBUG_STEP_INTO);
    g_hDebugStepOut = create_output_button(g_hOutputPanel, L"Step Out  Shift+F11", IDM_DEBUG_STEP_OUT);
    HWND debugButtons[] = { g_hDebugContinue, g_hDebugStepOver, g_hDebugStepInto, g_hDebugStepOut };
    for (size_t i = 0; i < sizeof(debugButtons) / sizeof(debugButtons[0]); i++)
        ShowWindow(debugButtons[i], g_outputTab == OUTPUT_TAB_DEBUG ? SW_SHOW : SW_HIDE);
    g_outputPanes[OUTPUT_TAB_DEBUG].view = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
        0, 0, 0, 0, g_hOutputPanel, (HMENU)ID_DEBUG_VARIABLES, GetModuleHandleW(NULL), NULL);
    g_hDebugVariables = g_outputPanes[OUTPUT_TAB_DEBUG].view;
    ListView_SetExtendedListViewStyle(g_hDebugVariables,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    LVCOLUMNW debugColumn = {0};
    debugColumn.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    debugColumn.pszText = L"Name"; debugColumn.cx = MulDiv(DEBUG_NAME_COLUMN_WIDTH, dpi, 96);
    SendMessageW(g_hDebugVariables, LVM_INSERTCOLUMNW, 0, (LPARAM)&debugColumn);
    debugColumn.pszText = L"Value"; debugColumn.cx = MulDiv(DEBUG_VALUE_COLUMN_WIDTH, dpi, 96); debugColumn.iSubItem = 1;
    SendMessageW(g_hDebugVariables, LVM_INSERTCOLUMNW, 1, (LPARAM)&debugColumn);
    HWND debugHeader = ListView_GetHeader(g_hDebugVariables);
    if (debugHeader) SetWindowSubclass(debugHeader, DebugHeaderSubclassProc, 1, 0);
    ShowWindow(g_hDebugVariables, g_outputTab == OUTPUT_TAB_DEBUG ? SW_SHOW : SW_HIDE);
    /* create_output_button() already set DEFAULT_GUI_FONT on the four debug
       buttons; these two aren't made that way. */
    HFONT debugFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(g_hDebugStatus, WM_SETFONT, (WPARAM)debugFont, TRUE);
    SendMessageW(g_hDebugVariables, WM_SETFONT, (WPARAM)debugFont, TRUE);

    g_hOutputClear = create_output_button(g_hOutputPanel, L"Clear", ID_OUTPUT_CLEAR);
    g_hOutputStop = create_output_button(g_hOutputPanel, L"Stop", ID_OUTPUT_STOP);
    g_hOutputClose = create_output_button(g_hOutputPanel, L"X", ID_OUTPUT_CLOSE);
    g_hOutputInput = create_output_scintilla(g_hOutputPanel, ID_OUTPUT_INPUT, 1);
    SendMessageA(g_hOutputInput, SCI_SETVSCROLLBAR, FALSE, 0);
    SendMessageA(g_hOutputInput, SCI_SETHSCROLLBAR, FALSE, 0);
    SendMessageA(g_hOutputInput, SCI_SETWRAPMODE, SC_WRAP_NONE, 0);
    SetWindowSubclass(g_hOutputInput, OutputInputSubclassProc, 1, 0);
    apply_output_panel_theme();
    update_output_controls();
}

/* Ends every program the panel started, when MarkEdit closes. */
static void shutdown_output_panel(void) {
    end_output_program(&g_outputPanes[OUTPUT_TAB_RUN]);
    free(g_outputPanes[OUTPUT_TAB_RUN].directory);
    g_outputPanes[OUTPUT_TAB_RUN].directory = NULL;
    terminal_view_free(g_terminalView);
    g_terminalView = NULL;
    for (int i = 0; i < g_outputHistoryCount; i++) free(g_outputHistory[i]);
    g_outputHistoryCount = g_outputHistoryIndex = 0;
}
