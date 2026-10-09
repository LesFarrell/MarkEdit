# Lua in MarkEdit

This guide is for people working on MarkEdit's source. It covers each Lua file in the repository, when and how it gets loaded, and how the C side connects them. For the API plugins call (`editor.*`, `ui.*`, `menu.*`, `commands.*`, `events.*`), see the user-facing guide, [plugins/README.md](../plugins/README.md). This page links to it rather than repeating it.

**Contents:** [The Lua files](#the-lua-files) · [Load order](#load-order) · [One shared Lua state](#one-shared-lua-state) · [How a command name runs](#how-a-command-name-runs) · [src/menus.lua](#srcmenuslua) · [src/formatting.lua](#srcformattinglua) · [src/help/keyboard-shortcuts.lua](#srchelpkeyboard-shortcutslua) · [plugins/](#plugins) · [The C side](#the-c-side) · [Changing and testing](#changing-and-testing)

## The Lua files

| File | What it is | Where it lives at run time | Who edits it |
|---|---|---|---|
| [src/menus.lua](../src/menus.lua) | The default menu bar. | Built into `markedit.exe` (resource `IDR_DEFAULT_MENUS`). | MarkEdit developers |
| `%APPDATA%\markedit\menus.lua` | A user's own menu bar, which replaces the default. | On disk. **Plugins > Customize Menus** creates it as a copy of the default. | Users |
| [src/formatting.lua](../src/formatting.lua) | The Markdown, HTML and JSON formatting commands, and some Edit menu text commands. | Built into `markedit.exe` (resource `IDR_BUILTIN_FORMATTING`). | MarkEdit developers |
| [src/help/keyboard-shortcuts.lua](../src/help/keyboard-shortcuts.lua) | Builds the **Help > Keyboard Shortcuts** page. | Built into `markedit.exe` (resource `IDR_HELP_SHORTCUTS`). | MarkEdit developers |
| [plugins/*.lua](../plugins/) | Plugins. The repository ships [example.lua](../plugins/example.lua). | The `plugins` folder beside `markedit.exe` (copied to `dist/` by the build). | Users |
| [third_party/lua/](../third_party/lua/) | The Lua 5.4 interpreter itself, as unmodified C sources. | Linked statically into `markedit.exe` (listed in [lua-sources.rsp](../lua-sources.rsp)). | Nobody. It's vendored. |

The built-in scripts are added to the exe as `RCDATA` resources by [markedit.rc](../markedit.rc), along with [src/help/markdown-reference.md](../src/help/markdown-reference.md), the **Help > Markdown Reference** page. They're read from the exe, not from `src/`, so **editing them has no effect until you rebuild**.

The **Run** command (Ctrl+F5) is unrelated. It runs a `.lua` document with an external `lua` found on `PATH`, not with the embedded interpreter.

## Load order

The same sequence runs at startup and on **Plugins > Reload Plugins**. It's `load_menus_and_plugins()` in [src/markedit.c](../src/markedit.c):

```
plugin_host_shutdown()              throw away the Lua state: every command,
                                    handler, global and the menu definition
        │
load_menu_definition()
        ├─ %APPDATA%\markedit\menus.lua, if it exists
        └─ else, or if that failed:  built-in src/menus.lua
        └─ menu_definition_problems(): report unknown command/list names
        │
formatting.lua (built in)           plugin_host_load_builtin(): no time limit
        │
plugins\*.lua                       plugin_host_load_directory(): alphabetical,
                                    ignoring case; a failing file is skipped
        │
show one message listing every error, then the caller rebuilds the menu bar
```

What this order means:

- **Plugins load last**, so a plugin's `commands.register("markdown.bold", ...)` replaces the one in `formatting.lua`. When two plugins register the same name, the one whose file name sorts later wins.
- **A broken user `menus.lua` never leaves the user without menus.** The built-in definition is loaded instead, and the error is reported.
- **Each script loads all or nothing.** If a script's top-level code fails, the commands and handlers it had registered so far are discarded (`run_script()` in [src/plugin_host.c](../src/plugin_host.c)).

## One shared Lua state

Every script above runs in a single `lua_State`, created in `ensure_state()` / `open_environment()` in [src/plugin_host.c](../src/plugin_host.c). The consequences:

- **Globals are shared.** That's why `formatting.lua` and `example.lua` declare everything `local`.
- **It's sandboxed.** Only the base, `string`, `table`, `math`, `utf8` and `coroutine` libraries are opened. `dofile` and `loadfile` are removed, and `load` accepts source text only, since precompiled bytecode can corrupt memory.
- **The API is seven global tables:** `editor`, `ui`, `menu`, `commands`, `events`, `config` and `app`, each a `luaL_Reg` array in `plugin_host.c`. The `editor` functions only work while a command or event handler runs (`s_edit` is set). Called from a script's top-level code, they raise an error.
- **There's a time limit.** A count hook (`time_limit_hook`) stops any call that runs longer than `PLUGIN_TIME_LIMIT_MS` (5 s, not counting time in dialogs). Commands registered by a built-in script (`plugin_host_load_builtin`) are exempt, because formatting a large document can legitimately take longer.
- **Each command is one undo step.** `plugin_host_run()` wraps the call in a Scintilla undo action, and if the command raises an error, it undoes what the command had changed.
- **Reload starts from nothing.** Plugin state lasts only until the next reload.

## How a command name runs

Built-in commands have dotted names such as `file.save` or `markdown.bold`. [src/menu_commands.c](../src/menu_commands.c) maps each name to its `IDM_*` ID (from [src/command_ids.h](../src/command_ids.h)) and its shortcut text. When a command fires from the menu, toolbar, Command Palette or a shortcut, `handle_main_command()` in `markedit.c` does this:

1. It looks up the name for the `IDM_*` ID (`menu_command_for_id`).
2. It asks the plugin host whether a Lua function is registered for that name (`plugin_host_find_override`). If one is, it runs the function, and nothing else happens.
3. Otherwise, it runs the C implementation.

`formatting.lua` relies on step 2. The `markdown.*`, `html.*` and `json.*` commands have **no C implementation at all**. They exist only because `formatting.lua` registers them. If `formatting.lua` fails to load, those menu items and toolbar buttons do nothing.

Plugin commands added with `menu.add`, and functions written directly in `menus.lua`, aren't built-in names. They get IDs from `IDM_PLUGIN_COMMAND_0` upwards, one per entry in the host's command list.

`menu.separator(path)` entries share that list, so they keep their order among the commands placed around them. They're flagged (`plugin_host_command_is_separator`) and have nothing to run. The menu builder in `markedit.c` (`place_plugin_separator`) puts one only in a menu that already exists, then trims separators left at the end of any menu.

## src/menus.lua

The default menu bar. It's a script that **returns** a table rather than calling any API:

```lua
return {
  { "&File", {
    { "&New", "file.new" },                 -- a built-in command, by name
    { "Signature", function() ... end },    -- Lua, run like a plugin command
    { "Recent &Files", { { list = "recent_files" } } },  -- a submenu holding a generated list
    "-",                                    -- a separator
  } },
}
```

- `plugin_host_load_menu()` runs the script, and `convert_menu_items()` checks the shape of the result and converts it into a tree of `PluginMenuItem`s. Shape errors name the path to the bad item, such as `&Edit > Con&vert: ...`.
- `menu_definition_problems()` in `menu_commands.c` then reports command names and `list` names that it doesn't recognise. Only those items are left out.
- The comment block at the top of `menus.lua` is user documentation. **Customize Menus** copies this file to the user's `%APPDATA%`, so the comment explains the format to whoever edits it there.
- Any built-in command can be left out of the menu bar. Its keyboard shortcut and Command Palette entry don't come from this file, so they keep working.

The full format is in [plugins/README.md, "Customizing the menu bar"](../plugins/README.md#customizing-the-menu-bar).

## src/formatting.lua

Fifty-three commands, written against the same API plugins use. Running it as an ordinary script keeps it replaceable by plugins, and it doubles as a large worked example of the API. The file is laid out in sections, each marked `-- ---- name ----`:

| Section | Commands | Key functions |
|---|---|---|
| Shared helpers | none | `selection`, `selected_lines`, `replace_lines`, `needs_leading_eol` |
| Inline wrapping | Bold, Italic, Strikethrough, Inline Code, the HTML tag commands | `wrap_pair` toggles: it wraps, or unwraps text that's already wrapped |
| Line prefixes | Heading 1-3, Blockquote, Bullet, Numbered and Task lists | `marker_length` recognises an existing prefix so it can be replaced; `apply_line_prefix` |
| Snippets | Link, Image, Table, HTML Table/List, Code Block, Rule, Alert, Footnote | `insert_with_url` selects the `url` placeholder; `ask_table_size` |
| JSON | Format JSON, Minify JSON | `json_is_valid` (a strict recursive-descent check), then `json_reformat` |
| Markdown tables | Table Editor, Row/Column insert, delete, sort and align | `find_table` locates the table at the caret; `row_cells`, `rewrite_table` |
| Edit menu | Toggle Comment, Trim Trailing Whitespace, Title Case, Sort Lines, Remove Duplicates | `transform_lines`, `trim_trailing_whitespace`, `toggle_comment` |
| Registration | none | the `markdown`, `html`, `json` and `edit` tables, registered as `group.name` |

Conventions used throughout (also stated at the top of the file):

- Positions are 1-based byte offsets. A range `start, finish` includes both ends, like `string.sub`. An empty range (a caret) is `finish == start - 1`, so `is_empty(start, finish)` is `finish < start`.
- New lines are always joined with `editor.eol()`, so a command never mixes line endings into a CRLF document.
- Most commands leave the result selected, so applying the command again undoes it, or the user can keep typing.

### Adding a built-in Lua command

1. Write the function in the right section of `formatting.lua`, and add it to the registration table at the bottom, for example `markdown.underline = ...`.
2. Give it an ID in [src/command_ids.h](../src/command_ids.h), and add `{"markdown.underline", IDM_..., NULL}` to `COMMANDS` in [src/menu_commands.c](../src/menu_commands.c). Without this, menus can't refer to it and nothing can invoke it.
3. Add an item for it to [src/menus.lua](../src/menus.lua). Optionally, add a Command Palette entry (`PALETTE_COMMANDS` in `markedit.c`) and a toolbar button.
4. Add the name to the list of built-in command names in [plugins/README.md](../plugins/README.md#built-in-command-names), and update the "53 formatting and text commands" count in its Limits table.
5. Add a case to [tests/test_formatting.c](../tests/test_formatting.c). `test_registration` already fails if a `markdown.`/`html.`/`json.` name in `menu_commands.c` has no Lua function. A new `edit.*` Lua command also needs adding to its `textCommands` list.
6. Rebuild.

## src/help/keyboard-shortcuts.lua

**Help > Keyboard Shortcuts** is generated each time the page opens, so it can't drift from the real shortcuts, and it reflects a user's own `menus.lua`. `generate_shortcuts_page()` in `markedit.c` passes the shortcut text from `menu_commands.c` to `plugin_host_render_page()`, which does the following:

- runs the script in **a separate Lua state**, not the one plugins share, with only the base, `string`, `table` and `utf8` libraries, so no plugin can change the page;
- calls it with one table: `menus` (the loaded menu definition, as `{ kind = "command" | "submenu" | ... }` items) and `shortcuts` (command name to shortcut text);
- expects the Markdown page back as a string.

The script lists each menu's commands that have shortcuts under that menu's name, then any commands with shortcuts that aren't in the menus. Last comes a short hand-written `OTHER_KEYS` list of keys that aren't commands at all, such as Tab/Shift+Tab. **When you add a key to the accelerator table in `markedit.c` that isn't a command, add it to `OTHER_KEYS` too.** Commands get onto the page automatically through their shortcut text in `menu_commands.c`.

[tests/test_menus.c](../tests/test_menus.c) renders the page from `src/` and checks that every shortcut in the command table appears on it.

## plugins/

- [README.md](../plugins/README.md) is the plugin guide. It's shipped in `dist/plugins/`, and **Help > Plugin Guide** opens it, so it's written for users, not for MarkEdit developers.
- [example.lua](../plugins/example.lua) is a working plugin that shows each part of the API: `menu.add` with submenus and with `/Menu/...` paths, `ui.message`/`prompt`/`choose`, and an `events.on("save")` handler.
- MarkEdit loads every `*.lua` file in the folder, in name order. Other files, including `README.md`, are ignored.

## The C side

| File | Role |
|---|---|
| [src/plugin_host.c](../src/plugin_host.c) / [.h](../src/plugin_host.h) | Owns the `lua_State`. Opens the sandboxed environment, implements the API tables, loads scripts (`plugin_host_load_chunk`, `_builtin`, `_menu`, `_directory`), runs commands and event handlers (`plugin_host_run`, `plugin_host_fire`) and enforces the time limit. It knows nothing about the application: it reaches it only through the `PluginHostCallbacks` it's given. |
| [src/menu_commands.c](../src/menu_commands.c) / [.h](../src/menu_commands.h) | The table of built-in command names, their `IDM_*` IDs and shortcut text, and validation of a loaded menu definition. |
| [src/markedit.c](../src/markedit.c) | Loads everything (`load_menus_and_plugins`), builds the menu bar from the definition, routes commands to Lua overrides (`handle_main_command`) and fires the `save`, `open` and `tab_changed` events. |
| [markedit.rc](../markedit.rc) | Builds `src/menus.lua`, `src/formatting.lua` and the two `src/help/` pages into the exe. |

## Changing and testing

- **`src/formatting.lua` or `src/menus.lua`:** rebuild (`build.bat`), then restart MarkEdit or use **Reload Plugins**.
- **To iterate without rebuilding:** put a replacement in a file in `plugins/` (for example `commands.register("markdown.bold", ...)` with your new version), or edit `%APPDATA%\markedit\menus.lua`. Then use **Reload Plugins** after each change. Copy the result back into `src/` when you're done, and delete the temporary copies.
- **Tests** (`test.bat` runs them all; run them from the repository root):
  - [tests/test_plugin_host.c](../tests/test_plugin_host.c): the API, the sandbox, load failures, events and the time limit.
  - [tests/test_menus.c](../tests/test_menus.c): reads `src/menus.lua` from disk, and checks that it loads and that every name in it is known. It also renders `src/help/keyboard-shortcuts.lua` and checks that the page lists every shortcut.
  - [tests/test_formatting.c](../tests/test_formatting.c): reads `src/formatting.lua` from disk, and runs commands against a real Scintilla window.

  Because these tests read the scripts from `src/`, they test your current edits without a rebuild of `markedit.exe`.
