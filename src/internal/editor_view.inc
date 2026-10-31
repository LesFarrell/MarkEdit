/* ---- the editor views ----
   Reading and replacing editor text, and setting up each Scintilla view:
   theme colours, the language lexer, view options and the editor font. The
   minimap and split pane are views onto the same documents, so they come
   through here too. */

/* ---- copying text out of the Scintilla control ---- */

/* A NUL-terminated heap copy of [start, end), or NULL. Caller frees. */
static char *get_text_range(HWND hEdit, Sci_Position start, Sci_Position end) {
    if (end < start) return NULL;
    char *text = (char *)malloc((size_t)(end - start) + 1);
    if (!text) return NULL;
    struct Sci_TextRangeFull range = { { start, end }, text };
    SendMessageA(hEdit, SCI_GETTEXTRANGEFULL, 0, (LPARAM)&range);
    text[end - start] = 0;
    return text;
}

static char *get_edit_text(HWND hEdit, Sci_Position *outLen) {
    Sci_Position len = (Sci_Position)SendMessageA(hEdit, SCI_GETLENGTH, 0, 0);
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { if (outLen) *outLen = 0; return NULL; }
    SendMessageA(hEdit, SCI_GETTEXT, (WPARAM)(len + 1), (LPARAM)buf);
    if (outLen) *outLen = len;
    return buf;
}

static const char *editor_eol_sequence(void) {
    switch ((int)SendMessageA(g_hEdit, SCI_GETEOLMODE, 0, 0)) {
        case SC_EOL_CR: return "\r";
        case SC_EOL_LF: return "\n";
        default: return "\r\n";
    }
}

/* Sets the EOL mode new lines are typed with to whichever line ending the
   freshly loaded document uses first (CRLF when it has none). Read back from
   Scintilla rather than the loaded bytes, so read_large_file_document()'s
   streamed load - which never holds the whole text in one buffer - gets the
   same answer as set_edit_text(). */
static void apply_detected_eol_mode(HWND hEdit) {
    Sci_Position length = (Sci_Position)SendMessageA(hEdit, SCI_GETLENGTH, 0, 0);
    Sci_Position lineEnd = (Sci_Position)SendMessageA(hEdit, SCI_GETLINEENDPOSITION, 0, 0);
    int eolMode = SC_EOL_CRLF;
    if (lineEnd < length) {
        char first = (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)lineEnd, 0);
        char next = lineEnd + 1 < length
            ? (char)SendMessageA(hEdit, SCI_GETCHARAT, (WPARAM)(lineEnd + 1), 0) : 0;
        if (first == '\n') eolMode = SC_EOL_LF;
        else eolMode = next == '\n' ? SC_EOL_CRLF : SC_EOL_CR;
    }
    SendMessageA(hEdit, SCI_SETEOLMODE, (WPARAM)eolMode, 0);
}

/* Replace the Scintilla control's entire contents with `length` bytes of
   `text`. A whole-document target replace rather than SCI_SETTEXT, which
   stops at the first NUL - a UTF-16 file can legitimately contain U+0000.
   It's still a single undoable change; callers loading a fresh document
   (New/Open) should follow up with reset_undo_history() so the previous
   document's content isn't reachable via Undo. */
static void set_edit_text(HWND hEdit, const char *text, Sci_Position length) {
    SendMessageA(hEdit, SCI_TARGETWHOLEDOCUMENT, 0, 0);
    SendMessageA(hEdit, SCI_REPLACETARGET, (WPARAM)length, (LPARAM)text);
    SendMessageA(hEdit, SCI_SETEMPTYSELECTION, 0, 0);
    apply_detected_eol_mode(hEdit);
}

/* Configure the Scintilla control as a source editor: a Lexilla lexer for
   syntax highlighting (a passive read-only colorizer - it never edits the
   buffer), real tab characters at a width of 4, CRLF line endings to match
   the rest of the app's file I/O, and only the change-notifications we
   actually act on. Smart Markdown input is handled explicitly by
   EditSubclassProc; Scintilla call tips remain disabled, and its autocomplete
   list is populated by update_word_completions() rather than by the lexer. */
/* Fixed point size for the minimap's copy of the editor font - deliberately
   independent of the user's chosen editor font size (and its zoom level) so
   the overview stays small and legible as an overview rather than scaling
   up along with the main editor's text. In hundredths of a point (see
   SCI_STYLESETSIZEFRACTIONAL/SC_FONT_SIZE_MULTIPLIER) so it can go below the
   smallest whole point size. */
#define MINIMAP_FONT_SIZE_HUNDREDTHS 150

/* apply_editor_theme() bolds several per-construct styles (Markdown headings/
   strong text, and various lexers' keywords) to make them stand out in the
   full-size editor. At the minimap's tiny fixed size (MINIMAP_FONT_SIZE_HUNDREDTHS)
   bold weight just reads as thick, smudged blocks rather than legible text,
   so every style is forced back to regular weight there after the theme is
   applied. */
static void strip_minimap_bold(void) {
    if (!g_hMinimap) return;
    for (int style = 0; style <= STYLE_MAX; style++)
        SendMessageA(g_hMinimap, SCI_STYLESETBOLD, (WPARAM)style, FALSE);
}

/* Apply GitHub's light/dark palette to the editor so it matches the preview.
   Called once at startup and whenever the selected theme changes. */
static void apply_editor_theme(HWND hEdit) {
    /* Scintilla draws its own client area but leaves the scrollbars to the
       system, which can't paint the track in the editor's background colour -
       so they're replaced with themed ones (see themed_scrollbar.h). */
    themed_scrollbar_refresh(hEdit);

    const ThemePalette *p = theme_palette();
    SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_DEFAULT, p->fg);
    SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_DEFAULT, p->bg);
    SendMessageA(hEdit, SCI_STYLECLEARALL, 0, 0);

    /* Everything below follows SCI_STYLECLEARALL, which resets every style.
       The gutter colours match STYLE_LINENUMBER so the margins read as one
       strip. */
    SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_LINENUMBER, p->lineNumber);
    SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_LINENUMBER, p->subtleBg);

    SendMessageA(hEdit, SCI_SETCARETLINEBACK, p->highlightBg, 0);
    SendMessageA(hEdit, SCI_SETCARETFORE, p->fg, 0);
    SendMessageA(hEdit, SCI_SETSELBACK, TRUE, p->selectionBg);
    SendMessageA(hEdit, SCI_SETSELFORE, TRUE, p->selectionFg);

    /* The word-completion popup (see update_word_completions()) is drawn
       by Scintilla itself via its Element colour API rather than styles,
       and otherwise defaults to plain system list-box colors (always
       white/black) regardless of editor theme. Mirrors STYLE_DEFAULT and
       SETSEL* above. Element colours carry an alpha byte (bits 24-31,
       unlike SCI_STYLESETFORE/BACK's plain COLORREF), hence the
       0xFF000000 for fully opaque. Its scrollbar is a separate, system-
       drawn native listbox these colours don't reach - see
       theme_autocomplete_popup(), applied each time the popup is shown. */
    SendMessageA(hEdit, SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST, (LPARAM)(p->fg | 0xFF000000));
    SendMessageA(hEdit, SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST_BACK, (LPARAM)(p->bg | 0xFF000000));
    SendMessageA(hEdit, SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST_SELECTED, (LPARAM)(p->selectionFg | 0xFF000000));
    SendMessageA(hEdit, SCI_SETELEMENTCOLOUR, SC_ELEMENT_LIST_SELECTED_BACK, (LPARAM)(p->selectionBg | 0xFF000000));

    SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_INDENTGUIDE, (LPARAM)p->faint);
    SendMessageA(hEdit, SCI_SETWHITESPACEFORE, TRUE, (LPARAM)p->faint);
    SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_BRACELIGHT, (LPARAM)p->fg);
    SendMessageA(hEdit, SCI_STYLESETBACK, STYLE_BRACELIGHT, (LPARAM)p->braceBg);
    SendMessageA(hEdit, SCI_STYLESETBOLD, STYLE_BRACELIGHT, TRUE);
    SendMessageA(hEdit, SCI_STYLESETFORE, STYLE_BRACEBAD, (LPARAM)p->error);
    SendMessageA(hEdit, SCI_STYLESETBOLD, STYLE_BRACEBAD, TRUE);

    SendMessageA(hEdit, SCI_SETFOLDMARGINCOLOUR, TRUE, (LPARAM)p->subtleBg);
    SendMessageA(hEdit, SCI_SETFOLDMARGINHICOLOUR, TRUE, (LPARAM)p->subtleBg);
    for (int marker = SC_MARKNUM_FOLDEREND; marker <= SC_MARKNUM_FOLDEROPEN; marker++) {
        SendMessageA(hEdit, SCI_MARKERSETFORE, (WPARAM)marker, (LPARAM)p->subtleBg);
        SendMessageA(hEdit, SCI_MARKERSETBACK, (WPARAM)marker, (LPARAM)p->faint);
    }
    SendMessageA(hEdit, SCI_MARKERSETFORE, MARKER_BOOKMARK, (LPARAM)p->bookmarkFore);
    SendMessageA(hEdit, SCI_MARKERSETBACK, MARKER_BOOKMARK, (LPARAM)p->bookmarkBack);
    SendMessageA(hEdit, SCI_MARKERSETFORE, MARKER_BREAKPOINT, (LPARAM)p->breakpointFore);
    SendMessageA(hEdit, SCI_MARKERSETBACK, MARKER_BREAKPOINT, (LPARAM)p->breakpointBack);
    SendMessageA(hEdit, SCI_MARKERSETBACK, MARKER_DEBUG_LINE, (LPARAM)p->debugLineBack);
    SendMessageA(hEdit, SCI_MARKERSETALPHA, MARKER_DEBUG_LINE, DEBUG_LINE_ALPHA);
    SendMessageA(hEdit, SCI_MARKERSETFORE, MARKER_DEBUG_ARROW, (LPARAM)p->bg);
    SendMessageA(hEdit, SCI_MARKERSETBACK, MARKER_DEBUG_ARROW, (LPARAM)p->debugArrowBack);
    const int changedMarkers[] = { SC_MARKNUM_HISTORY_MODIFIED, SC_MARKNUM_HISTORY_REVERTED_TO_MODIFIED };
    for (int i = 0; i < 2; i++) {
        SendMessageA(hEdit, SCI_MARKERSETFORE, (WPARAM)changedMarkers[i], (LPARAM)p->lineChanged);
        SendMessageA(hEdit, SCI_MARKERSETBACK, (WPARAM)changedMarkers[i], (LPARAM)p->lineChanged);
    }
    SendMessageA(hEdit, SCI_MARKERSETFORE, SC_MARKNUM_HISTORY_SAVED, (LPARAM)p->lineSaved);
    SendMessageA(hEdit, SCI_MARKERSETBACK, SC_MARKNUM_HISTORY_SAVED, (LPARAM)p->lineSaved);

    SendMessageA(hEdit, SCI_INDICSETSTYLE, INDICATOR_SPELLING, INDIC_SQUIGGLE);
    SendMessageA(hEdit, SCI_INDICSETFORE, INDICATOR_SPELLING, (LPARAM)p->error);
    SendMessageA(hEdit, SCI_INDICSETUNDER, INDICATOR_SPELLING, TRUE);

    apply_language_styles(hEdit, (Language)g_activeLanguage, g_editorDarkMode);
    InvalidateRect(hEdit, NULL, TRUE);
}

/* Switches the editor to the lexer for `path`, reapplying the theme so the
   new styles get colours. Safe to call when the language is unchanged. A
   large-file document is always plain text: its document has no style
   storage to lex into (see read_large_file_document()). */
static void apply_language_to_view(HWND hEdit, const WCHAR *path) {
    int isMinimap = hEdit == g_hMinimap;
    int largeFile = g_largeFile && !(hEdit == g_hSplitEdit && split_document_path());
    const LanguageInfo *language = largeFile ? &LANGUAGES[LANG_TEXT] : language_for_path(path);
    g_activeLanguage = language->id;

    void *lexer = CreateLexer(language->lexer);
    /* NULL explicitly clears the previous lexer when switching to plain text. */
    SendMessageA(hEdit, SCI_SETILEXER, 0, (LPARAM)lexer);
    if (language->keywords)
        SendMessageA(hEdit, SCI_SETKEYWORDS, 0, (LPARAM)language->keywords);
    for (size_t i = 0; i < sizeof(language->extraKeywords) / sizeof(language->extraKeywords[0]); i++)
        if (language->extraKeywords[i])
            SendMessageA(hEdit, SCI_SETKEYWORDS, (WPARAM)(i + 1), (LPARAM)language->extraKeywords[i]);

    /* fold.html is separate from fold: without it the XML/HTML lexer reports no
       fold points at all. The margin is hidden for languages that cannot fold,
       rather than shown as a dead strip. */
    SendMessageA(hEdit, SCI_SETPROPERTY, (WPARAM)"fold", (LPARAM)(language->folds ? "1" : "0"));
    SendMessageA(hEdit, SCI_SETPROPERTY, (WPARAM)"fold.compact", (LPARAM)"0");
    SendMessageA(hEdit, SCI_SETPROPERTY, (WPARAM)"fold.html", (LPARAM)"1");
    SendMessageA(hEdit, SCI_SETMARGINWIDTHN, MARGIN_FOLD,
                 !isMinimap && language->folds ? FOLD_MARGIN_WIDTH : 0);

    apply_editor_theme(hEdit);
    if (isMinimap) {
        SendMessageA(hEdit, SCI_SETWRAPMODE, SC_WRAP_NONE, 0);
        strip_minimap_bold();
    } else {
        apply_word_wrap(hEdit);
    }
    if (!largeFile) SendMessageA(hEdit, SCI_COLOURISE, 0, (LPARAM)-1);

    /* The minimap owns a throttled snapshot rather than the live document,
       so it also needs a lexer. Refresh its snapshot after configuring that
       lexer whenever a tab, path, or large-file mode changes. */
    if (hEdit == g_hEdit && g_hMinimap) {
        apply_language_to_view(g_hMinimap, path);
        refresh_minimap_after_edit();
    }
    if (!isMinimap) schedule_spell_check();
}

/* Applies the language for `path` to every editable view of the active
   document: the main pane and, when it exists, the split pane. */
static void apply_document_language(const WCHAR *path) {
    apply_language_to_view(g_hEdit, path);
    if (g_hSplitEdit) {
        int mainLanguage = g_activeLanguage;
        const WCHAR *splitPath = split_document_path();
        apply_language_to_view(g_hSplitEdit, splitPath ? splitPath : path);
        /* The focused tab remains the app's active language even when the
           secondary pane displays a different document. */
        g_activeLanguage = mainLanguage;
    }
}

static void configure_scintilla_feedback(HWND hEdit) {
    /* Syntax-highlight the source using one of Lexilla's bundled lexers
       (vendored trimmed to the languages listed in LANGUAGES - see
       third_party/lexilla). ILexer5* ownership passes to Scintilla once
       handed to SCI_SETILEXER. apply_language_to_view() picks the lexer
       from the file extension; new documents start out as Markdown.

       Explicit text colors, rather than relying on Scintilla's own defaults,
       so the caret-line highlight below is guaranteed to contrast with them.
       apply_editor_theme() sets the actual color values (light or dark),
       including the lexer's per-construct colors. */
    apply_language_to_view(hEdit, g_currentFile);

    /* Highlight the line the caret is on. SC_ALPHA_NOALPHA (rather than
       SC_ALPHA_OPAQUE) is required here: passing anything other than "no
       alpha" switches the caret line to Layer::OverText internally, which
       paints the highlight *over* the glyphs instead of behind them and is
       what was making the text unreadable. */
    SendMessageA(hEdit, SCI_SETCARETLINEVISIBLE, TRUE, 0);
    SendMessageA(hEdit, SCI_SETCARETLINEVISIBLEALWAYS, TRUE, 0); /* keep it visible even without focus */
    SendMessageA(hEdit, SCI_SETCARETLINEBACKALPHA, SC_ALPHA_NOALPHA, 0);

    /* Word completion (see update_word_completions()): matching is
       case-insensitive, the list we build is already sorted so Scintilla
       doesn't need to re-sort it, and the popup auto-hides once nothing
       typed so far matches any entry. */
    SendMessageA(hEdit, SCI_AUTOCSETIGNORECASE, TRUE, 0);
    SendMessageA(hEdit, SCI_AUTOCSETAUTOHIDE, TRUE, 0);
    /* Newline permits wiki-page candidates such as "Project Notes"; plain
       word completion uses the same separator. */
    SendMessageA(hEdit, SCI_AUTOCSETSEPARATOR, '\n', 0);
    SendMessageA(hEdit, SCI_AUTOCSETORDER, SC_ORDER_PRESORTED, 0);
    SendMessageA(hEdit, SCI_AUTOCSETMAXHEIGHT, 8, 0);
}

static void configure_scintilla(HWND hEdit) {
    configure_scintilla_editing(hEdit);
    configure_scintilla_view(hEdit);
    configure_scintilla_margins(hEdit);
    configure_scintilla_feedback(hEdit);
}

/* Apply a font face/point size to the Scintilla control by setting the
   default style (Scintilla styles text via numeric style IDs rather than an
   HFONT). SCI_STYLECLEARALL below resets every style - including the
   Markdown lexer's per-construct styles - back to STYLE_DEFAULT's font, so
   apply_editor_theme() must run afterwards to reapply them. */
static void apply_font_to_editor_view(HWND hEdit, const char *faceUtf8) {
    SendMessageA(hEdit, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)faceUtf8);
    SendMessageA(hEdit, SCI_STYLESETSIZE, STYLE_DEFAULT, g_fontPointSize);
    SendMessageA(hEdit, SCI_STYLESETWEIGHT, STYLE_DEFAULT, g_fontWeight);
    SendMessageA(hEdit, SCI_STYLESETITALIC, STYLE_DEFAULT, g_fontItalic);
    SendMessageA(hEdit, SCI_STYLECLEARALL, 0, 0); /* copies STYLE_DEFAULT (fore/back included) to all other styles, including STYLE_LINENUMBER */

    /* STYLECLEARALL just overwrote STYLE_LINENUMBER's fore/back with
       STYLE_DEFAULT's, undoing apply_editor_theme()'s margin colors (which
       are deliberately different from the text area). Re-apply the theme so
       the gutter doesn't revert to the wrong colors after a font change. */
    apply_editor_theme(hEdit);

    /* The margin's digit width depends on the font, so keep both in sync. */
    SendMessageA(hEdit, SCI_STYLESETFONT, STYLE_LINENUMBER, (LPARAM)faceUtf8);
    SendMessageA(hEdit, SCI_STYLESETSIZE, STYLE_LINENUMBER, g_fontPointSize);
    remeasure_linenumber_margin(hEdit);
}

/* Applies the configured editor font (g_fontFaceName, g_fontPointSize,
   g_fontWeight, g_fontItalic) to every view onto the document. */
static void apply_editor_font(void) {
    char *faceUtf8 = to_utf8(g_fontFaceName);
    if (!faceUtf8) faceUtf8 = _strdup("Consolas");
    if (!faceUtf8) return;
    apply_font_to_editor_view(g_hEdit, faceUtf8);

    /* The minimap is a second view onto the same document, so it keeps its
       own copy of every style and needs the same font/theme applied - but at
       a fixed small size (see MINIMAP_FONT_SIZE_HUNDREDTHS) rather than the
       editor's own point size, so it doesn't grow along with it. Zoom stays
       at 0: sizing is done via STYLESETSIZEFRACTIONAL alone. */
    if (g_hMinimap) {
        SendMessageA(g_hMinimap, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)faceUtf8);
        SendMessageA(g_hMinimap, SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, MINIMAP_FONT_SIZE_HUNDREDTHS);
        SendMessageA(g_hMinimap, SCI_STYLECLEARALL, 0, 0);
        apply_editor_theme(g_hMinimap);
        strip_minimap_bold();
        SendMessageA(g_hMinimap, SCI_SETZOOM, 0, 0);
        update_minimap_viewport();
    }

    /* The split pane is a second fully-interactive view onto the same
       document (unlike the minimap), so it gets the real point size, not a
       fixed fraction of it. */
    if (g_hSplitEdit) apply_font_to_editor_view(g_hSplitEdit, faceUtf8);
    apply_output_panel_font(faceUtf8);
    free(faceUtf8);
}
