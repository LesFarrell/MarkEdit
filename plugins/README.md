# Writing MarkEdit plugins

A MarkEdit plugin is a Lua script that adds commands, menus, and automatic actions to the editor. Plugins can read and change the document, ask simple questions, and show messages. They can't read or write files, start programs, or reach the network.

This guide covers everything a plugin can do. [`example.lua`](example.lua), in this folder, is a working plugin that uses most of it.

**Contents:** [Quick start](#quick-start) · [How plugins are loaded](#how-plugins-are-loaded) · [Positions, ranges, and lines](#positions-ranges-and-lines) · [API reference](#api-reference) · [Where commands appear](#where-commands-appear) · [Replacing built-in commands](#replacing-built-in-commands) · [Customizing the menu bar](#customizing-the-menu-bar) · [How commands run](#how-commands-run) · [The Lua environment](#the-lua-environment) · [Limits](#limits) · [Recipes](#recipes) · [Troubleshooting](#troubleshooting)

## Quick start

1. Create a file ending in `.lua` in this `plugins` folder (the one beside `markedit.exe`), for example `hello.lua`:

   ```lua
   menu.add("Say Hello", function()
     ui.message("Hello from " .. (editor.file_path() or "an unsaved document"))
   end)

   menu.add("Text/Shout", function()
     editor.replace_selection(editor.get_selection():upper() .. "!")
   end)
   ```

2. Choose **Plugins → Reload Plugins** (or restart MarkEdit).

3. **Plugins → Say Hello** and **Plugins → Text → Shout** are now in the menu, and in the Command Palette (Ctrl+Shift+P) as `Plugins: Say Hello` and `Plugins: Text > Shout`.

For an installed copy of MarkEdit this folder is usually `C:\Program Files\MarkEdit\plugins`, and adding files there needs administrator rights.

## How plugins are loaded

- MarkEdit loads every file ending in `.lua` in the `plugins` folder when it starts, in alphabetical order of file name (ignoring case). Other files, including this one, are ignored.
- Loading a plugin runs its top-level code once. That code normally registers commands and event handlers; the functions it registers run later, when they're used.
- **Plugins → Reload Plugins** throws away everything loaded (commands, handlers, and all Lua variables) and loads every plugin again. It also reloads the menu definition, so changes to plugins and menus take effect without restarting.
- If a plugin's top-level code fails, MarkEdit lists the error, with its file name and line, in a message after loading. A plugin that fails part-way adds none of its commands or handlers. The other plugins still load.
- Plugin files must be UTF-8 text (with or without a byte-order mark) and at most 1 MiB.
- All plugins share one Lua environment, together with the menu definition and MarkEdit's own formatting commands, which load first. A global variable in one plugin is visible to the others, so declare everything `local` unless you mean to share it. To share code between plugins, put it in a global table in a file whose name sorts first (for example `0-shared.lua`).

## Positions, ranges, and lines

The `editor` functions describe places in the document the way Lua's `string.sub` does:

- **A position** is a 1-based byte offset into the document's UTF-8 text: position 1 is the first byte, and the position just past the end is `editor.length() + 1`.
- **A range** is two positions, `start, finish`, that include both ends, so `editor.get_text():sub(start, finish)` is the range's text. A range with `finish == start - 1` is empty: it marks a place between two bytes. For example, `editor.get_selection_range()` returns `5, 4` when nothing is selected and the caret is before the 5th byte.
- **Lines** are numbered from 1. A line's text and range never include its line ending; `editor.eol()` gives the document's line ending (`"\r\n"`, `"\n"`, or `"\r"`), which is what to use when inserting new lines.
- Positions count bytes, not characters. A character such as `é` is two bytes, so don't split text in the middle of one. Use Lua's `utf8` library to work in characters.

Functions that take a position, range, or line number raise an error when it's outside the document.

## API reference

Plugins see five tables: `menu`, `commands`, `events`, `editor`, and `ui`. Square brackets mark optional arguments.

### menu

#### `menu.add(label, fn)`

Adds a command. Choosing it runs `fn()`. `label` decides where it appears; see [Where commands appear](#where-commands-appear). The same label can be added more than once. Each becomes its own item.

#### `menu.separator([path])`

Adds a separator line after whatever has been added to a menu so far, so the next item added there starts a new group. `path` names the menu the same way a label's leading parts do, but with no item name at the end:

| Path | The separator goes in |
|---|---|
| none, or `""` | The Plugins menu |
| `"Lines"` | Plugins → Lines |
| `"/Snippets"` | The **Snippets** menu itself |
| `"/Snippets/Markdown"` | Snippets → Markdown |

```lua
menu.add("Lines/Sort", sort_lines)
menu.add("Lines/Reverse", reverse_lines)
menu.separator("Lines")
menu.add("Lines/Number Lines", number_lines)
```

Separators follow the same rules as `"-"` in the menu definition: one at the start or end of a menu, or next to another separator, is left out. A separator doesn't create a menu, so add at least one item to a menu before separating items in it. Each separator counts towards the [command limit](#limits).

### commands

#### `commands.register(name, fn [, title])`

Makes the built-in command called `name` run `fn()` instead of its usual code, wherever it's used from: menu, toolbar, Command Palette, or keyboard shortcut. `title` is used as the title of dialogs `fn` shows (default `"MarkEdit"`). If several plugins register the same name, the last one loaded wins. See [Replacing built-in commands](#replacing-built-in-commands).

### events

#### `events.on(name, fn)`

Runs `fn(path)` each time the event happens, where `path` is the document's file path, or `nil` if it has never been saved. A plugin can register any number of handlers; they run in the order they were registered.

| Event | When it happens | `path` |
|---|---|---|
| `"save"` | Just before the document is written to disk, so anything the handler changes is saved. | Where it's being saved, including a new name chosen with Save As. |
| `"open"` | After a file is loaded into the editor, including when a file changed by another program is reloaded. | The loaded file. |
| `"tab_changed"` | After a different document becomes the active one: switching tabs, opening a new tab, or closing one. Brief switches MarkEdit makes itself, such as Save All visiting each tab, don't count. | The newly active document. |

Handlers work on the active document, exactly as commands do. A handler that fails is reported, but doesn't stop what caused the event: a document is still saved if its `save` handler fails.

### editor

These work only while a command or event handler is running. Calling them from a plugin's top-level code raises an error.

#### Reading text

| Function | Returns |
|---|---|
| `editor.get_text()` | The whole document. |
| `editor.get_range(start, finish)` | The text of a range. |
| `editor.get_selection()` | The selected text, or `""` when nothing is selected. |
| `editor.get_line(n)` | The text of line `n`, without its line ending. |
| `editor.length()` | The document's length in bytes. |
| `editor.eol()` | The document's line ending: `"\r\n"`, `"\n"`, or `"\r"`. |

#### Changing text

| Function | Does |
|---|---|
| `editor.replace_selection(text)` | Replaces the selection with `text`, or inserts it at the caret when nothing is selected. The caret ends just after the new text. |
| `editor.replace_range(start, finish, text)` | Replaces a range with `text`. An empty range inserts `text` there. The selection moves with the text around it. |
| `editor.insert(text [, position])` | Inserts `text` at `position`, or at the caret. |
| `editor.set_text(text)` | Replaces the whole document. The caret position isn't kept; prefer `replace_range` for partial changes. |

#### Selection and caret

| Function | Does |
|---|---|
| `editor.get_selection_range()` | Returns `start, finish` of the selection (an empty range at the caret when nothing is selected). |
| `editor.set_selection(start, finish)` | Selects a range (an empty range places the caret) and scrolls it into view. |
| `editor.get_caret()` | Returns the caret's position. |
| `editor.set_caret(position)` | Moves the caret there, removing any selection, and scrolls it into view. |

When several parts of the document are selected at once (multiple selections), these functions work on the main selection only, and changing the selection leaves just one.

#### Lines

| Function | Returns |
|---|---|
| `editor.line_count()` | The number of lines (at least 1). |
| `editor.current_line()` | The number of the line holding the caret. |
| `editor.line_at(position)` | The number of the line holding `position`. |
| `editor.line_range(n)` | `start, finish` of line `n`, without its line ending (an empty range for an empty line). |

#### The document

| Function | Returns |
|---|---|
| `editor.file_path()` | The document's full path, or `nil` if it has never been saved. |
| `editor.language()` | How MarkEdit is treating the document: `"markdown"`, `"json"`, `"xml"`, `"html"`, `"yaml"`, `"cpp"` (C, C++, JavaScript, and TypeScript), `"python"`, `"bash"`, `"powershell"`, `"lua"`, or `"text"`. |

### ui

These show MarkEdit's own dialogs and wait for the user. Time spent waiting doesn't count towards the [time limit](#how-commands-run). Unless given a `title`, a dialog is titled with the command's label (or `script.lua: event` for an event handler). `ui.message` can also be used from a plugin's top-level code, while it loads; the others are for commands and event handlers.

#### `ui.message(text [, title [, kind]])`

Shows `text` in a message box with an OK button. `kind` chooses the icon: `"info"` (the default), `"warning"`, or `"error"`.

#### `ui.prompt(label [, default [, title]])`

Asks for one line of text, showing `label` above a text box that starts with `default` (empty if not given). Returns the text, or `nil` if the user cancels.

#### `ui.choose(items [, label [, title]])`

Shows a searchable list of the strings in `items` (1 to 10,000 of them) and lets the user pick one. Typing filters the list. Returns the chosen item's index and the item, or `nil` if the user cancels.

```lua
local index, choice = ui.choose({ "Note", "Tip", "Warning" }, "Alert type:")
if not index then return end
```

## Where commands appear

A command's label is a path separated by `/`. Each part before the last names a submenu; the last part is the item. An `&` before a letter marks it as the item's access key (`&&` shows a literal `&`).

| Label | Appears in |
|---|---|
| `"Word Count"` | Plugins → Word Count |
| `"Text/Word Count"` | Plugins → Text → Word Count |
| `"Text/Count/Words"` | Plugins → Text → Count → Words |
| `"/Extras/Insert Date"` | A new top-level **Extras** menu, placed before Help |
| `"/Snippets/Insert Date"` | The existing **Snippets** menu, after a separator |
| `"/Tools/Word Count"` | The existing **Tools** menu, after a separator |
| `"/Snippets/Markdown/Word Count"` | The existing Snippets → Markdown submenu |

- A leading `/` places the command relative to the menu bar. Its first part names a top-level menu: an existing one if the name matches (ignoring case and `&`), otherwise a new one. Plugins can add new top-level menus until the menu bar holds 24.
- Submenus are found the same way, by name, and created when they don't exist yet.
- A label of just `/Name` has no menu to go in, so it goes on the Plugins menu like `Name`.
- "The Plugins menu" means wherever the menu definition puts its plugins list (see [Customizing the menu bar](#customizing-the-menu-bar)). If the definition has no plugins list, MarkEdit adds a Plugins menu for these commands.
- Every plugin command is also in the Command Palette as `Plugins: ` followed by its path, for example `Plugins: Snippets > Insert Date`.
- A path has at most 8 parts; anything beyond that is ignored.
- Top-level menu names are drawn using the system's ANSI code page, so plain letters, digits, and common punctuation are safest there. Submenu and item names can use any characters.

## Replacing built-in commands

`commands.register` replaces what a built-in command does, everywhere it can be used: the menu, the formatting toolbar, the Command Palette, and keyboard shortcuts. MarkEdit's own internal actions aren't affected. For example, replacing `file.save` changes the Save command and Ctrl+S, but not the save MarkEdit offers when closing a document with unsaved changes.

```lua
-- Bold with __ instead of **.
commands.register("markdown.bold", function()
  local text = editor.get_selection()
  editor.replace_selection("__" .. text .. "__")
end)
```

MarkEdit's own Markdown, HTML, and JSON formatting commands, and the Edit menu's Toggle Comment, Trim Trailing Whitespace, Title Case, and line sorting commands (the ones marked (Lua) below), are written this way, in `src/formatting.lua` (built into `markedit.exe`). They load before any plugin, so a plugin that registers one of their names replaces them. To go back to MarkEdit's own version, remove the plugin and reload.

### Built-in command names

These are the names `commands.register` and the menu definition use.

- **File:** `file.new`, `file.open`, `file.reopen_utf8`, `file.reopen_utf16le`, `file.reopen_utf16be`, `file.reopen_ansi`, `file.quick_open`, `file.find_in_folder`, `file.replace_in_folder`, `file.open_workspace`, `file.insert_file`, `file.save`, `file.save_as`, `file.save_all`, `file.export_html`, `file.export_pdf`, `file.print`, `file.open_terminal`, `file.show_in_explorer`, `file.close_tab`, `file.reopen_closed_tab`, `file.go_back`, `file.exit`
- **Edit** (Lua): `edit.toggle_comment`, `edit.trim_whitespace`, `edit.title_case`, `edit.sort_ascending`, `edit.sort_descending`, `edit.remove_duplicate_lines`
- **Edit:** `edit.undo`, `edit.redo`, `edit.cut`, `edit.copy`, `edit.paste`, `edit.delete`, `edit.select_all`, `edit.duplicate_line`, `edit.move_line_up`, `edit.move_line_down`, `edit.delete_line`, `edit.join_lines`, `edit.select_next`, `edit.select_all_occurrences`, `edit.match_brace`, `edit.bookmark_toggle`, `edit.bookmark_next`, `edit.bookmark_previous`, `edit.bookmark_clear`, `edit.uppercase`, `edit.lowercase`, `edit.tabs_to_spaces`, `edit.spaces_to_tabs`, `edit.eol_crlf`, `edit.eol_lf`, `edit.eol_cr`, `edit.encoding_utf8`, `edit.encoding_utf8_bom`, `edit.encoding_utf16le`, `edit.encoding_utf16be`, `edit.encoding_ansi`, `edit.find`, `edit.find_next`, `edit.replace`, `edit.go_to_line`
- **View:** `view.preview`, `view.editor`, `view.next_tab`, `view.previous_tab`, `view.cycle_panes`, `view.theme_light`, `view.theme_dark`, `view.preview_theme_light`, `view.preview_theme_dark`, `view.custom_css`, `view.clear_custom_css`, `view.font`, `view.preview_on_left`, `view.focus_mode`, `view.word_wrap`, `view.spell_check`, `view.spell_check_language`, `view.outline`, `view.file_explorer`, `view.minimap`, `view.split_editor`, `view.reset_split_editor`, `view.show_whitespace`, `view.indent_guides`, `view.use_spaces`, `view.toggle_fold`, `view.fold_all`, `view.unfold_all`, `view.zoom_in`, `view.zoom_out`, `view.zoom_reset`
- **Markdown** (Lua): `markdown.bold`, `markdown.italic`, `markdown.strikethrough`, `markdown.inline_code`, `markdown.heading1`, `markdown.heading2`, `markdown.heading3`, `markdown.blockquote`, `markdown.bullet_list`, `markdown.numbered_list`, `markdown.task_list`, `markdown.link`, `markdown.image`, `markdown.table`, `markdown.table_editor`, `markdown.table_row_above`, `markdown.table_row_below`, `markdown.table_row_delete`, `markdown.table_column_left`, `markdown.table_column_right`, `markdown.table_column_delete`, `markdown.table_sort_ascending`, `markdown.table_sort_descending`, `markdown.table_align_left`, `markdown.table_align_center`, `markdown.table_align_right`, `markdown.code_block`, `markdown.footnote`, `markdown.horizontal_rule`, `markdown.alert`
- **JSON** (Lua): `json.format`, `json.minify`
- **HTML** (Lua): `html.html`, `html.head`, `html.body`, `html.paragraph`, `html.bold`, `html.italic`, `html.underline`, `html.div`, `html.span`, `html.link`, `html.image`, `html.table`, `html.bullet_list`, `html.numbered_list`, `html.line_break`
- **Tools:** `tools.command_palette`, `tools.settings`
- **Run:** `run.run_file`, `run.debug_file`, `run.continue`, `run.step_over`, `run.step_into`, `run.toggle_breakpoint`, `run.clear_breakpoints`
- **Plugins:** `plugins.reload`, `plugins.customize_menus`
- **Help:** `help.markdown_reference`, `help.keyboard_shortcuts`, `help.plugin_guide`, `help.about`

## Customizing the menu bar

The whole menu bar is defined by a Lua file. MarkEdit uses `%APPDATA%\markedit\menus.lua` when it exists, and otherwise its built-in default menus. **Plugins → Customize Menus** copies the default menus to that file (if it isn't there yet) and opens it. Edit it, save it, then choose **Plugins → Reload Plugins**. Delete the file to go back to the default menus.

The file returns a list of top-level menus. Each menu is `{ label, { items } }`, and each item is one of:

| Item | Meaning |
|---|---|
| `{ "Label", "command.name" }` | A built-in command (see [Built-in command names](#built-in-command-names)). Its shortcut and check mark are shown automatically. |
| `{ "Label", function() ... end }` | Lua code, run like a plugin command, with the same API. |
| `{ "Label", { items } }` | A submenu. |
| `"-"` | A separator. |
| `{ list = "recent_files" }` | The recently opened files, and Clear Recent Files. |
| `{ list = "recent_workspaces" }` | The recent workspace folders, and Clear Recent Workspaces. |
| `{ list = "plugins" }` | The commands plugins add without a leading `/`. |

```lua
return {
  { "&File", {
    { "&New", "file.new" },
    { "&Open...", "file.open" },
    { "&Save", "file.save" },
    "-",
    { "Recent &Files", { { list = "recent_files" } } },
    "-",
    { "E&xit", "file.exit" },
  } },
  { "&Snippets", {
    { "&Signature", function() editor.insert("Best regards," .. editor.eol()) end },
  } },
  { "&Plugins", {
    { list = "plugins" },
    "-",
    { "&Reload Plugins", "plugins.reload" },
    { "&Customize Menus...", "plugins.customize_menus" },
  } },
}
```

- Moving or renaming an item doesn't change its keyboard shortcut. Leaving a command out of the menus doesn't remove its shortcut or its Command Palette entry either.
- `{codepage}` in a label is replaced with the system's ANSI code page number.
- Separators at the start or end of a menu, or next to another separator, are left out, so a list that turns out to be empty leaves no gap.
- Plugins place `/Menu/...` commands by menu name, so renaming a menu changes where those commands go.
- Keep **Reload Plugins** and **Customize Menus** somewhere, or you'll need to restart MarkEdit to apply changes, and find the file yourself to edit it.
- If the file has a mistake (Lua that doesn't run, or a table in the wrong shape) MarkEdit says what and where, and uses the default menus instead. An unknown command name is reported and just that item is left out.

## How commands run

- **Commands act on the pane being edited**: the split editor's second pane when it has the focus, otherwise the main one. Event handlers act on the main pane.
- **One undo step.** Everything a command or event handler changes is undone together by a single Undo.
- **Errors are reported and undone.** If a command raises an error (from `error(...)`, a failed `assert`, or a bug), MarkEdit shows the message and a traceback, and undoes any changes the command had made.
- **The time limit.** A command, event handler, or plugin's top-level code that runs for more than 5 seconds is stopped, with an error, and its changes are undone. Time spent waiting for the user in a `ui` dialog doesn't count. MarkEdit's own formatting commands have no limit.
- **One plugin at a time.** While a plugin's dialog is open, events (such as an automatic save) don't run other plugins' handlers, and plugin commands can't be started.

## The Lua environment

Plugins run in Lua 5.4, built into `markedit.exe`. No separate Lua installation is needed.

- **Available:** the basic functions (`pairs`, `ipairs`, `tostring`, `tonumber`, `pcall`, `error`, `assert`, `select`, `type`, `setmetatable`, and so on) and the `string`, `table`, `math`, `utf8`, and `coroutine` libraries.
- **Not available:** `io`, `os`, `package` and `require`, `debug`, `dofile`, and `loadfile`. Plugins can't touch files, run programs, or read the clock (there is no `os.date` or `os.time`).
- **`load`** accepts Lua source text only, not precompiled chunks.
- **`print`** writes nowhere visible. To see a value while developing a plugin, use `ui.message(tostring(value))`.
- **Strings are bytes.** `string.upper`, `string.lower`, and character classes such as `%a` only understand ASCII letters, and `#text` counts bytes. Use `utf8.len`, `utf8.codes`, and `utf8.offset` for characters.

## Limits

| Limit | Value |
|---|---|
| Size of a plugin file | 1 MiB |
| Commands, in total | 256, shared by plugins (`menu.add`, `menu.separator` and `commands.register`), functions in the menu definition, and MarkEdit's own 53 formatting and text commands |
| Event handlers, in total | 100 |
| Time a call may run | 5 seconds (not counting dialogs) |
| Parts in a menu label | 8 |
| Top-level menus | 24, including the ones from the menu definition |
| Items in `ui.choose` | 10,000 |
| Text returned by `ui.prompt` | One line, about 4,000 characters |
| Menu definition nesting | 8 levels, 4,000 items |

## Recipes

### Wrap the selection, or the word at the caret

```lua
menu.add("Text/Highlight", function()
  local s, f = editor.get_selection_range()
  if f < s then
    -- Nothing selected: find the word around the caret.
    local line = editor.current_line()
    local lineStart = editor.line_range(line)
    local text = editor.get_line(line)
    local column = editor.get_caret() - lineStart + 1
    local first, last = 1, 0
    for wordStart, wordEnd in text:gmatch("()%w+()") do
      if wordStart <= column and column <= wordEnd then first, last = wordStart, wordEnd - 1 end
    end
    if last < first then return end
    s, f = lineStart + first - 1, lineStart + last - 1
  end
  editor.replace_range(s, f, "==" .. editor.get_range(s, f) .. "==")
end)
```

### Change every line of the selection

```lua
menu.add("Lines/Indent with Two Spaces", function()
  local s, f = editor.get_selection_range()
  local first, last = editor.line_at(s), editor.line_at(math.max(f, s))
  -- From the bottom up, so earlier positions don't move.
  for n = last, first, -1 do
    local start = editor.line_range(n)
    editor.insert("  ", start)
  end
end)
```

### Ask a question first

```lua
menu.add("/Snippets/Callout...", function()
  local kinds = { "NOTE", "TIP", "IMPORTANT", "WARNING", "CAUTION" }
  local _, kind = ui.choose(kinds, "Callout type:")
  if not kind then return end
  local title = ui.prompt("First line:", "")
  if title == nil then return end
  local e = editor.eol()
  editor.replace_selection("> [!" .. kind .. "]" .. e .. "> " .. title .. e)
end)
```

### Act only on some documents

```lua
-- Markdown files are saved ending with exactly one line ending.
events.on("save", function()
  if editor.language() ~= "markdown" then return end
  local text = editor.get_text()
  local body = text:gsub("[\r\n]+$", "")
  if body == "" or body .. editor.eol() == text then return end
  editor.replace_range(#body + 1, editor.length(), editor.eol())
end)
```

### Keep a setting while MarkEdit runs

Plugin variables last until MarkEdit closes or plugins are reloaded. There's no way to store anything permanently, since plugins can't write files.

```lua
local wrapWidth = 80

menu.add("Text/Set Wrap Width...", function()
  local answer = ui.prompt("Wrap width:", tostring(wrapWidth))
  local value = answer and tonumber(answer)
  if value and value >= 10 then wrapWidth = math.floor(value) end
end)
```

## Troubleshooting

| Problem | What to check |
|---|---|
| The plugin's commands don't appear. | Is the file in the `plugins` folder beside `markedit.exe`, and does its name end in `.lua`? Did you reload plugins? Was a load error reported? |
| "the editor is only available while a plugin command runs" | `editor` functions were called from a plugin's top-level code. Move that code into the function passed to `menu.add` or `events.on`. |
| "stopped after 5 seconds without finishing" | A loop that never ends, or work that's too slow on a large document. Read only the lines or range you need rather than the whole text. |
| A command does nothing visible. | Check for an `if ... then return end` that always returns. `ui.message` is the quickest way to see what a command is doing. |
| Two plugins interfere. | They probably share a global variable or function name. Make everything `local`. |
| A `commands.register` replacement doesn't take effect. | The name must match a [built-in command name](#built-in-command-names) exactly. If two plugins replace the same command, the one whose file name sorts later wins. |
| Plugins can't be added to the installed copy. | `C:\Program Files\MarkEdit\plugins` needs administrator rights to change. |
