-- MarkEdit's menu bar.
--
-- MarkEdit reads %APPDATA%\markedit\menus.lua when it exists, and otherwise
-- uses the default menus built into markedit.exe. Plugins > Customize
-- Menus... copies the default menus there (if the file isn't there yet) and
-- opens it. After editing it, save it and use Plugins > Reload Plugins.
-- Delete it to go back to the default menus. If it has a mistake, MarkEdit
-- says what and where, and uses the default menus instead.
--
-- The file returns a list of top-level menus: { label, { items } }.
-- Each item is one of:
--   { "Label", "command.name" }      a built-in command (names below)
--   { "Label", function() ... end }  Lua code, with the same API as plugins
--                                    (see plugins/README.md)
--   { "Label", { items } }           a submenu
--   "-"                              a separator
--   { list = "name" }                items MarkEdit fills in:
--                                      "recent_files", "recent_workspaces",
--                                      or "plugins" (commands that plugins add
--                                      without a leading "/")
--
-- "&" marks the access key. Shortcut keys and check marks are shown
-- automatically for built-in commands; moving an item doesn't change its
-- shortcut. "{codepage}" in a label shows the system ANSI code page.
-- Separators at the start or end of a menu, or next to another separator,
-- are left out, so a list that turns out empty leaves no gap.
--
-- Plugins can still add items here with labels such as "/Tools/Word Count",
-- which find menus by name, so renaming a menu changes where those go.

return {
  { "&File", {
    { "&New", "file.new" },
    { "&Open...", "file.open" },
    { "Reopen with &Encoding", {
      { "UTF-&8", "file.reopen_utf8" },
      { "UTF-16 &LE", "file.reopen_utf16le" },
      { "UTF-16 &BE", "file.reopen_utf16be" },
      { "&ANSI (code page {codepage})", "file.reopen_ansi" },
    } },
    { "&Quick Open in Folder...", "file.quick_open" },
    { "Find in F&older...", "file.find_in_folder" },
    { "Replace in Fol&der...", "file.replace_in_folder" },
    { "Open &Workspace Folder...", "file.open_workspace" },
    { "&Insert File at Cursor...", "file.insert_file" },
    { "&Save", "file.save" },
    { "Save &As...", "file.save_as" },
    { "Save A&ll", "file.save_all" },
    { "Export Rendered &HTML...", "file.export_html" },
    { "Export Rendered &PDF...", "file.export_pdf" },
    { "&Print Rendered Preview...", "file.print" },
    { "Open &Terminal Here", "file.open_terminal" },
    { "Show in E&xplorer", "file.show_in_explorer" },
    { "&Close Tab", "file.close_tab" },
    { "&Reopen Closed Tab", "file.reopen_closed_tab" },
    { "Go &Back", "file.go_back" },
    "-",
    { "Recent &Files", { { list = "recent_files" } } },
    { "Recent Wor&kspaces", { { list = "recent_workspaces" } } },
    "-",
    { "E&xit", "file.exit" },
  } },

  { "&Edit", {
    { "&Undo", "edit.undo" },
    { "&Redo", "edit.redo" },
    "-",
    { "Cu&t", "edit.cut" },
    { "&Copy", "edit.copy" },
    { "&Paste", "edit.paste" },
    { "De&lete", "edit.delete" },
    "-",
    { "Select &All", "edit.select_all" },
    "-",
    { "&Duplicate Line", "edit.duplicate_line" },
    { "Move Line &Up", "edit.move_line_up" },
    { "Move Line &Down", "edit.move_line_down" },
    { "De&lete Line", "edit.delete_line" },
    { "&Join Lines", "edit.join_lines" },
    "-",
    { "Toggle Co&mment", "edit.toggle_comment" },
    { "Trim Trailing &Whitespace", "edit.trim_whitespace" },
    "-",
    { "Select Next &Occurrence", "edit.select_next" },
    { "Select All O&ccurrences", "edit.select_all_occurrences" },
    { "Jump to Matching &Brace", "edit.match_brace" },
    "-",
    { "Boo&kmarks", {
      { "Toggle &Bookmark", "edit.bookmark_toggle" },
      { "&Next Bookmark", "edit.bookmark_next" },
      { "&Previous Bookmark", "edit.bookmark_previous" },
      "-",
      { "&Clear All Bookmarks", "edit.bookmark_clear" },
    } },
    { "Con&vert", {
      { "&UPPERCASE", "edit.uppercase" },
      { "&lowercase", "edit.lowercase" },
      { "&Title Case", "edit.title_case" },
      "-",
      { "Sort Lines &Ascending", "edit.sort_ascending" },
      { "Sort Lines Descend&ing", "edit.sort_descending" },
      { "Remove &Duplicate Lines", "edit.remove_duplicate_lines" },
      "-",
      { "Tabs to &Spaces", "edit.tabs_to_spaces" },
      { "Spaces to Ta&bs", "edit.spaces_to_tabs" },
      "-",
      { "Line &Endings", {
        { "&CRLF (Windows)", "edit.eol_crlf" },
        { "&LF (Unix/macOS)", "edit.eol_lf" },
        { "C&R (Classic Mac)", "edit.eol_cr" },
      } },
      { "En&coding", {
        { "UTF-&8", "edit.encoding_utf8" },
        { "UTF-8 with &BOM", "edit.encoding_utf8_bom" },
        { "UTF-16 &LE", "edit.encoding_utf16le" },
        { "UTF-16 B&E", "edit.encoding_utf16be" },
        { "&ANSI (code page {codepage})", "edit.encoding_ansi" },
      } },
    } },
    "-",
    { "&Find...", "edit.find" },
    { "Find &Next", "edit.find_next" },
    { "&Replace...", "edit.replace" },
    { "&Go to Line...", "edit.go_to_line" },
  } },

  { "&View", {
    { "Show &Preview", "view.preview" },
    { "Show &Editor", "view.editor" },
    "-",
    { "&Next Tab", "view.next_tab" },
    { "&Previous Tab", "view.previous_tab" },
    { "Cycle &Panes", "view.cycle_panes" },
    "-",
    { "&Editor Theme", {
      { "&Light", "view.theme_light" },
      { "&Dark", "view.theme_dark" },
    } },
    { "&Markdown Theme", {
      { "GitHub &Light", "view.preview_theme_light" },
      { "GitHub &Dark", "view.preview_theme_dark" },
      "-",
      { "Custom Preview &CSS...", "view.custom_css" },
      { "C&lear Custom Preview CSS", "view.clear_custom_css" },
    } },
    { "Fon&t...", "view.font" },
    "-",
    { "Preview on &Left", "view.preview_on_left" },
    { "&Focus Mode", "view.focus_mode" },
    { "&Word Wrap", "view.word_wrap" },
    { "&Spell Check", "view.spell_check" },
    { "Spell Check &Language...", "view.spell_check_language" },
    { "Document &Outline", "view.outline" },
    { "&File Explorer", "view.file_explorer" },
    { "Mini&map", "view.minimap" },
    { "Split Editor &Horizontally", "view.split_editor" },
    { "Split Editor &Vertically", "view.split_editor_vertical" },
    { "&Reset Split Editor", "view.reset_split_editor" },
    "-",
    { "Show W&hitespace", "view.show_whitespace" },
    { "&Indent Guides", "view.indent_guides" },
    { "&Use Spaces for Indentation", "view.use_spaces" },
    "-",
    { "Toggle Fo&ld", "view.toggle_fold" },
    { "Fold &All", "view.fold_all" },
    { "&Unfold All", "view.unfold_all" },
    "-",
    { "Zoom &In", "view.zoom_in" },
    { "Zoom &Out", "view.zoom_out" },
    { "&Reset Zoom", "view.zoom_reset" },
  } },

  { "&Tools", {
    { "&JSON", {
      { "&Format JSON", "json.format" },
      { "&Minify JSON", "json.minify" },
    } },
    "-",
    { "Settin&gs...", "tools.settings" },
  } },

  { "&Plugins", {
    { list = "plugins" },
    "-",
    { "&Reload Plugins", "plugins.reload" },
    { "&New Plugin...", "plugins.new" },
    { "Open Plugins &Folder", "plugins.open_folder" },
    { "&Manage Plugins...", "plugins.manage" },
    { "&Customize Menus...", "plugins.customize_menus" },
  } },

  { "&Snippets", {
    { "&Markdown", {
      { "&Bold", "markdown.bold" },
      { "&Italic", "markdown.italic" },
      { "Strikethrou&gh", "markdown.strikethrough" },
      { "Inline &Code", "markdown.inline_code" },
      "-",
      { "Heading &1", "markdown.heading1" },
      { "Heading &2", "markdown.heading2" },
      { "Heading &3", "markdown.heading3" },
      { "Block&quote", "markdown.blockquote" },
      "-",
      { "B&ullet List", "markdown.bullet_list" },
      { "&Numbered List", "markdown.numbered_list" },
      { "Tas&k List", "markdown.task_list" },
      "-",
      { "&Link", "markdown.link" },
      { "I&mage", "markdown.image" },
      { "&Table", "markdown.table" },
      { "Table &Editor...", "markdown.table_editor" },
      { "Table Row/Colu&mn", {
        { "Insert Row &Above", "markdown.table_row_above" },
        { "Insert Row &Below", "markdown.table_row_below" },
        { "&Delete Row", "markdown.table_row_delete" },
        "-",
        { "Insert Column &Left", "markdown.table_column_left" },
        { "Insert Column &Right", "markdown.table_column_right" },
        { "Delete Colu&mn", "markdown.table_column_delete" },
        "-",
        { "&Sort Rows by This Column (Ascending)", "markdown.table_sort_ascending" },
        { "Sort Rows by This Column (Descending)", "markdown.table_sort_descending" },
        "-",
        { "Align Colu&mn", {
          { "&Left", "markdown.table_align_left" },
          { "&Center", "markdown.table_align_center" },
          { "&Right", "markdown.table_align_right" },
        } },
      } },
      { "Code Bl&ock", "markdown.code_block" },
      { "&Footnote", "markdown.footnote" },
      { "&Horizontal Rule", "markdown.horizontal_rule" },
      { "&Alert", "markdown.alert" },
    } },
    { "&HTML", {
      { "&HTML", "html.html" },
      { "H&ead", "html.head" },
      { "&Body", "html.body" },
      "-",
      { "&Paragraph", "html.paragraph" },
      { "B&old", "html.bold" },
      { "&Italic", "html.italic" },
      { "&Underline", "html.underline" },
      { "&Div", "html.div" },
      { "&Span", "html.span" },
      "-",
      { "&Link", "html.link" },
      { "I&mage", "html.image" },
      { "&Table", "html.table" },
      { "B&ullet List", "html.bullet_list" },
      { "&Numbered List", "html.numbered_list" },
      "-",
      { "Line &Break", "html.line_break" },
    } },
  } },

  { "&Run", {
    { "&Run File", "run.run_file" },
    { "&Debug File", "run.debug_file" },
    { "&Continue", "run.continue" },
    { "Step &Over", "run.step_over" },
    { "Step &Into", "run.step_into" },
    { "Step O&ut", "run.step_out" },
    "-",
    { "Toggle &Breakpoint", "run.toggle_breakpoint" },
    { "&Clear All Breakpoints", "run.clear_breakpoints" },
  } },

  { "&Help", {
    { "&Markdown Reference", "help.markdown_reference" },
    { "&Keyboard Shortcuts", "help.keyboard_shortcuts" },
    { "&Plugin Guide", "help.plugin_guide" },
    "-",
    { "&About MarkEdit", "help.about" },
  } },
}
