/* ---- The output panel: Run's output and a PowerShell terminal ----------
   Docked under the editor and preview with a splitter above it (see
   layout_output_panel()). Each tab shows one program's output in a
   read-only Scintilla view that scrolls sideways rather than wraps: the
   Output tab whatever Run started, plus what plugins write with the
   output.* API (see write_plugin_output()); the Terminal tab a PowerShell
   that starts the first time it is used. The line along the bottom sends
   what is typed to the visible tab's program: a command on the Terminal
   tab, input for the running program on the Output tab. Double-clicking a
   file location in the output opens it.

   output_process.c runs the programs and output_text.c decodes what they
   write; this file only connects them to the views. */

#define OUTPUT_MAX_LENGTH (4 * 1024 * 1024) /* older output is dropped beyond this */
#define OUTPUT_HISTORY_SIZE 50

enum { OUTPUT_TAB_RUN, OUTPUT_TAB_TERMINAL, OUTPUT_TAB_COUNT };

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

static void apply_output_panel_theme(void) {
    if (!g_hOutputPanel) return;
    for (int i = 0; i < OUTPUT_TAB_COUNT; i++) apply_output_view_theme(g_outputPanes[i].view);
    apply_output_view_theme(g_hOutputInput);
    InvalidateRect(g_hOutputPanel, NULL, TRUE);
    HWND buttons[] = { g_outputPanes[0].tab, g_outputPanes[1].tab, g_hOutputClear,
                       g_hOutputStop, g_hOutputClose };
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        if (buttons[i]) InvalidateRect(buttons[i], NULL, TRUE);
}

/* The editor's font, at its size, for the views and the input line. */
static void apply_output_panel_font(const char *faceUtf8) {
    if (!g_hOutputPanel) return;
    HWND views[] = { g_outputPanes[0].view, g_outputPanes[1].view, g_hOutputInput };
    for (size_t i = 0; i < sizeof(views) / sizeof(views[0]); i++) {
        SendMessageA(views[i], SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)faceUtf8);
        SendMessageA(views[i], SCI_STYLESETSIZE, STYLE_DEFAULT, g_fontPointSize);
        SendMessageA(views[i], SCI_STYLESETWEIGHT, STYLE_DEFAULT, g_fontWeight);
        apply_output_view_theme(views[i]); /* its STYLECLEARALL spreads the font */
    }
}

/* The Stop button and the tab labels follow what's running. */
static void update_output_controls(void) {
    if (!g_hOutputPanel) return;
    EnableWindow(g_hOutputStop, output_pane_running(active_output_pane()));
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

/* WM_APP_OUTPUT: output from a program, or the news that it has finished. */
static void handle_output_chunk(OutputChunk *chunk) {
    for (int i = 0; i < OUTPUT_TAB_COUNT; i++) {
        OutputPane *pane = &g_outputPanes[i];
        if (!pane->process || pane->session != chunk->session) continue;
        output_decoder_feed(&pane->decoder, chunk->data, chunk->length, &OUTPUT_PANE_SINK, pane);
        if (chunk->finished) {
            end_output_program(pane);
            if (i == OUTPUT_TAB_TERMINAL) {
                WCHAR note[64];
                swprintf(note, sizeof(note) / sizeof(note[0]),
                         L"[The terminal ended with code %lu. Press Enter to start it again.]",
                         (unsigned long)chunk->exitCode);
                append_output_note(pane, OUTPUT_STYLE_NOTE, note);
            }
            update_output_controls();
        }
        break;
    }
    free(chunk);
}

/* The folder new terminals start in: the workspace, or the document's
   folder, or the user's profile. Caller frees. */
static WCHAR *terminal_directory(void) {
    if (g_currentWorkspaceFolder) return _wcsdup(g_currentWorkspaceFolder);
    if (g_currentFile) return path_directory(g_currentFile);
    WCHAR profile[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    return length > 0 && length < MAX_PATH ? _wcsdup(profile) : NULL;
}

static int start_terminal(void) {
    OutputPane *pane = &g_outputPanes[OUTPUT_TAB_TERMINAL];
    if (pane->process) return 1;
    WCHAR *powershell = locate_powershell();
    WCHAR *directory = terminal_directory();
    size_t capacity = (powershell ? wcslen(powershell) : 0) + 64;
    WCHAR *command = (WCHAR *)malloc(capacity * sizeof(WCHAR));
    int started = 0;
    if (powershell && command &&
        swprintf(command, capacity, L"\"%ls\" -NoLogo -NoProfile -NoExit -Command -", powershell) > 0)
        started = start_output_program(pane, command, directory);
    if (started) {
        output_process_write(pane->process, OUTPUT_TERMINAL_STARTUP, strlen(OUTPUT_TERMINAL_STARTUP));
    } else {
        append_output_note(pane, OUTPUT_STYLE_FAILURE, L"[PowerShell could not be started.]");
    }
    free(command);
    free(directory);
    free(powershell);
    update_output_controls();
    return started;
}

/* ---- showing the panel ---- */

static void select_output_tab(int tab) {
    g_outputTab = tab;
    for (int i = 0; i < OUTPUT_TAB_COUNT; i++)
        ShowWindow(g_outputPanes[i].view, i == tab ? SW_SHOW : SW_HIDE);
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
    if (focusInput) SetFocus(g_hOutputInput);
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

/* Enter: sends the line to the visible tab's program, echoing it in the
   output. On the Terminal tab that starts PowerShell again if it ended. */
static void submit_output_input(void) {
    char *text = output_input_text();
    if (!text) return;
    OutputPane *pane = active_output_pane();
    remember_output_input(text);
    g_outputHistoryIndex = g_outputHistoryCount;
    SendMessageA(g_hOutputInput, SCI_CLEARALL, 0, 0);
    size_t length = strlen(text);
    if (g_outputTab == OUTPUT_TAB_TERMINAL) {
        if (start_terminal()) {
            append_output(pane, OUTPUT_STYLE_INPUT, text, length);
            append_output(pane, OUTPUT_STYLE_INPUT, "\n", 1);
            char *command = output_terminal_command(text, length);
            if (command) output_process_write(pane->process, command, strlen(command));
            free(command);
        }
    } else if (output_pane_running(pane)) {
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
    if (output_pane_running(&g_outputPanes[tab]) && wcslen(label) < 28) wcscat(label, L" \x2022");
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

static void size_output_panel(HWND panel) {
    RECT rc;
    GetClientRect(panel, &rc);
    int dpi = GetDpiForWindow(panel);
    OutputPanelLayout layout = output_panel_layout(panel);
    int row = layout.header - layout.pad;
    int tabWidth = MulDiv(OUTPUT_TAB_WIDTH, dpi, 96);
    int buttonWidth = MulDiv(OUTPUT_BUTTON_WIDTH, dpi, 96);
    int closeWidth = MulDiv(DEBUG_CLOSE_BUTTON_WIDTH, dpi, 96);
    for (int i = 0; i < OUTPUT_TAB_COUNT; i++)
        MoveWindow(g_outputPanes[i].tab, layout.pad + i * tabWidth, layout.pad / 2, tabWidth, row, TRUE);
    /* The buttons line up from the right edge. Any that would run into the
       tabs are hidden instead, Clear first and X last, since overlapping
       controls paint over each other. */
    int tabsRight = layout.pad + OUTPUT_TAB_COUNT * tabWidth;
    HWND buttons[] = { g_hOutputClose, g_hOutputStop, g_hOutputClear };
    int widths[] = { closeWidth, buttonWidth, buttonWidth };
    int x = rc.right;
    int room = 1;
    for (int i = 0; i < 3; i++) {
        x -= layout.pad + widths[i];
        room = room && x >= tabsRight + layout.pad;
        if (room) MoveWindow(buttons[i], x, layout.pad / 2, widths[i], row, TRUE);
        ShowWindow(buttons[i], room ? SW_SHOW : SW_HIDE);
    }
    /* A control that only moves can keep the pixels it had, so the tabs and
       buttons redraw themselves at every size. */
    for (int i = 0; i < OUTPUT_TAB_COUNT; i++) InvalidateRect(g_outputPanes[i].tab, NULL, TRUE);
    for (int i = 0; i < 3; i++) InvalidateRect(buttons[i], NULL, TRUE);
    /* One pixel inside the borders paint_output_panel() draws. */
    RECT view = layout.view, input = layout.inputBox;
    for (int i = 0; i < OUTPUT_TAB_COUNT; i++)
        MoveWindow(g_outputPanes[i].view, view.left + 1, view.top + 1,
                   view.right - view.left - 2, view.bottom - view.top - 2, TRUE);
    MoveWindow(g_hOutputInput, input.left + 1, input.top + 1,
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
    frame_rect(hdc, &layout.view, theme_border_color());
    frame_rect(hdc, &layout.inputBox, theme_border_color());
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
                case ID_OUTPUT_TAB_TERMINAL: select_output_tab(OUTPUT_TAB_TERMINAL); SetFocus(g_hOutputInput); return 0;
                case ID_OUTPUT_CLEAR: {
                    OutputPane *pane = active_output_pane();
                    clear_output_pane(pane);
                    /* A running terminal shows its prompt again. */
                    if (g_outputTab == OUTPUT_TAB_TERMINAL && pane->process) {
                        char *prompt = output_terminal_command("", 0);
                        if (prompt) output_process_write(pane->process, prompt, strlen(prompt));
                        free(prompt);
                    }
                    SetFocus(g_hOutputInput);
                    return 0;
                }
                case ID_OUTPUT_STOP: {
                    OutputPane *pane = active_output_pane();
                    if (pane->process) {
                        end_output_program(pane);
                        append_output_note(pane, OUTPUT_STYLE_FAILURE, L"[Stopped]");
                        update_output_controls();
                    }
                    return 0;
                }
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
            }
            return 0;
        }
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType != ODT_BUTTON) break;
            if (dis->hwndItem == g_outputPanes[OUTPUT_TAB_RUN].tab) draw_output_tab_button(dis, OUTPUT_TAB_RUN);
            else if (dis->hwndItem == g_outputPanes[OUTPUT_TAB_TERMINAL].tab) draw_output_tab_button(dis, OUTPUT_TAB_TERMINAL);
            else draw_debug_panel_button(dis);
            return TRUE;
        }
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
    static const WCHAR *const labels[OUTPUT_TAB_COUNT] = { L"Output", L"Terminal" };
    static const int tabIds[OUTPUT_TAB_COUNT] = { ID_OUTPUT_TAB_RUN, ID_OUTPUT_TAB_TERMINAL };
    static const int viewIds[OUTPUT_TAB_COUNT] = { ID_OUTPUT_VIEW, ID_TERMINAL_VIEW };
    for (int i = 0; i < OUTPUT_TAB_COUNT; i++) {
        OutputPane *pane = &g_outputPanes[i];
        pane->tab = create_output_button(g_hOutputPanel, labels[i], tabIds[i]);
        pane->view = create_output_scintilla(g_hOutputPanel, viewIds[i], i == g_outputTab);
        SendMessageA(pane->view, SCI_SETREADONLY, TRUE, 0);
        SendMessageA(pane->view, SCI_SETUNDOCOLLECTION, FALSE, 0);
        /* Long lines scroll sideways rather than wrap, so output keeps its
           columns; the scrollbar appears only once a line is too wide. */
        SendMessageA(pane->view, SCI_SETWRAPMODE, SC_WRAP_NONE, 0);
        SendMessageA(pane->view, SCI_SETSCROLLWIDTHTRACKING, TRUE, 0);
        SendMessageA(pane->view, SCI_SETSCROLLWIDTH, 1, 0);
        themed_scrollbar_attach(pane->view, THEMED_SCROLL_SCINTILLA);
        SetWindowSubclass(pane->view, OutputViewSubclassProc, (UINT_PTR)(i + 1), 0);
        output_decoder_init(&pane->decoder);
    }
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
    for (int i = 0; i < OUTPUT_TAB_COUNT; i++) {
        end_output_program(&g_outputPanes[i]);
        free(g_outputPanes[i].directory);
        g_outputPanes[i].directory = NULL;
    }
    for (int i = 0; i < g_outputHistoryCount; i++) free(g_outputHistory[i]);
    g_outputHistoryCount = g_outputHistoryIndex = 0;
}
