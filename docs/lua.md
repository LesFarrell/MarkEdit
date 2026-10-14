# Lua in MarkEdit

This guide is for people working on MarkEdit's source. It covers each Lua file in the repository, when and how it gets loaded, and how the C side connects them. For the API plugins call (`editor.*`, `ui.*`, `menu.*`, `commands.*`, `events.*`), see the user-facing guide, [plugins/README.md](../plugins/README.md). This page links to it rather than repeating it.

**Contents:** [The Lua files](#the-lua-files) · [Load order](#load-order) · [One shared Lua state](#one-shared-lua-state) · [How a command name runs](#how-a-command-name-runs) · [src/menus.lua](#srcmenuslua) · [built-in formatting](#built-in-formatting) · [src/help/keyboard-shortcuts.lua](#srchelpkeyboard-shortcutslua) · [plugins/](#plugins) · [The C side](#the-c-side) · [Changing and testing](#changing-and-testing)

## The Lua files

| File | What it is | Where it lives at run time | Who edits it |
|---|---|---|---|
| [src/menus.lua](../src/menus.lua) | The default menu bar. | Built into `markedit.exe` (resource `IDR_DEFAULT_MENUS`). | MarkEdit developers |
| `%APPDATA%\markedit\menus.lua` | A user's own menu bar, which replaces the default. | On disk. **Plugins > Customize Menus** creates it as a copy of the default. | Users |
| `%APPDATA%\markedit\themes.lua` | A user's colours for the light and dark themes, overriding the built-in ones in [src/theme.c](../src/theme.c). | On disk. **Plugins > Customize Theme Colours** creates it, listing every colour. Run in a Lua state of its own (`plugin_host_load_theme()`). | Users |
| `src/lua/formatting_*.lua` | Markdown and HTML formatting plus Markdown-aware Edit commands. | Built into `markedit.exe` as three resources and compiled together as one Lua chunk. | MarkEdit developers |
| [plugins/edit-convert.lua](../plugins/edit-convert.lua) | The complete **Edit > Convert** submenu and its transformations. | Loaded from the `plugins` folder. | MarkEdit developers |
| [plugins/json-tools.lua](../plugins/json-tools.lua) | JSON formatting, validation and related tools. | Loaded from the `plugins` folder. | MarkEdit developers |
| [plugins/csv-tools.lua](../plugins/csv-tools.lua) | Delimited-text parsing, transformations and export. | Loaded from the `plugins` folder. | MarkEdit developers |
| `plugins/*-tools.lua` and other bundled plugins | SQL, logs, data conversion, natural sorting, number conversion, grammar, Lua formatting and other optional tools. | Loaded from the `plugins` folder. | MarkEdit developers |
| [src/help/keyboard-shortcuts.lua](../src/help/keyboard-shortcuts.lua) | Builds the **Help > Keyboard Shortcuts** page. | Built into `markedit.exe` (resource `IDR_HELP_SHORTCUTS`). | MarkEdit developers |
| [plugins/*.lua](../plugins/) | Bundled plugins and examples. | The `plugins` folder beside `markedit.exe` (copied to `dist/` by the build). | MarkEdit developers |
| `%APPDATA%\markedit\plugins` | Personal `.lua` plugins or folders containing `init.lua`. | Created by **Plugins > New Plugin/Open Plugins Folder**. | Users |
| [plugins/types/markedit.lua](../plugins/types/markedit.lua) | Lua Language Server declarations for the plugin API. | Copied into the personal plugin folder with `.luarc.json`. | MarkEdit developers |
| [third_party/lua/](../third_party/lua/) | The Lua 5.4 interpreter itself, as unmodified C sources. | Linked statically into `markedit.exe` (listed in [lua-sources.rsp](../lua-sources.rsp)). | Nobody. It's vendored. |

The built-in scripts are added to the exe as `RCDATA` resources by [markedit.rc](../markedit.rc), along with [src/help/markdown-reference.md](../src/help/markdown-reference.md), the **Help > Markdown Reference** page. They're read from the exe, not from `src/`, so **editing them has no effect until you rebuild**.

The **Run** command (Ctrl+F5) is unrelated. It runs a `.lua` document with an external `lua` found on `PATH`, not with the embedded interpreter.

## Load order

The same sequence runs at startup and on **Plugins > Reload Plugins**. It's `load_menus_and_plugins()` in [src/markedit.c](../src/markedit.c):

```
load_theme_colours()                built-in palettes, then %APPDATA%\markedit\themes.lua
                                    in its own Lua state (only base and string,
                                    stopped if it doesn't finish)
        │
plugin_host_shutdown()              throw away the Lua state: every command,
                                    handler, global and the menu definition
        │
load_menu_definition()
        ├─ %APPDATA%\markedit\menus.lua, if it exists
        └─ else, or if that failed:  built-in src/menus.lua
        └─ menu_definition_problems(): report unknown command/list names
        │
formatting_*.lua (built in)         joined into one chunk; plugin_host_load_builtin(): no time limit
        │
bundled plugins                     plugin_host_load_directory(..., "bundled")
        │
personal plugins                    plugin_host_load_directory(..., "user")
                                    each location is alphabetical; entries are
                                    .lua files or folders containing init.lua
        │
report every error and offer to open the first failing source
```

What this order means:

- **Personal plugins load last**, so they can replace bundled or built-in commands. Within one location, the later alphabetical entry wins.
- **A broken user `menus.lua` never leaves the user without menus.** The built-in definition is loaded instead, and the error is reported.
- **A broken `themes.lua` never leaves the window uncoloured.** Loading starts from the built-in palettes; each bad entry is reported and skipped, and the good ones still apply.
- **Each script loads all or nothing.** If a script's top-level code fails, the commands and handlers it had registered so far are discarded (`run_script()` in [src/plugin_host.c](../src/plugin_host.c)).

## One shared Lua state

Every script above runs in a single `lua_State`, created in `ensure_state()` / `open_environment()` in [src/plugin_host.c](../src/plugin_host.c). The consequences:

- **Globals are shared.** That's why the built-in formatting chunk and `example.lua` declare everything `local`.
- **It's sandboxed.** Only the base, `string`, `table`, `math`, `utf8` and `coroutine` libraries are opened. `dofile` and `loadfile` are removed, and `load` accepts source text only. The custom `require` reads source modules only beneath the folder extension currently loading.
- **The API is eight global tables:** `editor`, `ui`, `menu`, `commands`, `events`, `config`, `app` and `documents`. All but `editor` are `luaL_Reg` arrays in `plugin_registry.c`; `editor` is built in `lua_editor_api.c`. The `editor` functions only work while a command or event handler has an active editor. Called from a script's top-level code, they raise an error.
- **There's a time limit.** A count hook (`time_limit_hook`) stops any call that runs longer than `PLUGIN_TIME_LIMIT_MS` (5 s, not counting time in dialogs). Commands registered by a built-in script (`plugin_host_load_builtin`) are exempt, because formatting a large document can legitimately take longer.
- **Each command is one undo step.** `plugin_host_run()` wraps the call in a Scintilla undo action, and if the command raises an error, it undoes what the command had changed.
- **Reload recreates the Lua state.** Lua variables last until the next reload; values saved with `config.set` remain in `settings.json`.

## How a command name runs

Built-in commands have dotted names such as `file.save` or `markdown.bold`. [src/menu_commands.c](../src/menu_commands.c) maps each name to its `IDM_*` ID (from [src/command_ids.h](../src/command_ids.h)) and its shortcut text. When a command fires from the menu, toolbar or a shortcut, `handle_main_command()` in `markedit.c` does this:

1. It looks up the name for the `IDM_*` ID (`menu_command_for_id`).
2. It asks the plugin host whether a Lua function is registered for that name (`plugin_host_find_override`). If one is, it runs the function, and nothing else happens.
3. Otherwise, it runs the C implementation.

The built-in formatting chunk relies on step 2 for `markdown.*` and `html.*`. These commands have **no C implementation at all**; if the responsible Lua code is unavailable, their menu items do nothing. Convert and JSON tools are ordinary plugin menu commands created entirely by `plugins/edit-convert.lua` and `plugins/json-tools.lua`.

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
- Any built-in command can be left out of the menu bar. Its keyboard shortcut doesn't come from this file, so it keeps working.

The full format is in [plugins/README.md, "Customizing the menu bar"](../plugins/README.md#customizing-the-menu-bar).

## Built-in formatting

The three `src/lua/formatting_*.lua` files are embedded separately and joined before Lua compiles them. They therefore share one lexical scope: helpers declared `local` in `formatting_base.lua` remain private but are available to the later parts. General conversion and JSON support are deliberately separate in `plugins/edit-convert.lua` and `plugins/json-tools.lua`, so they follow the normal plugin lifecycle.

These commands use the same API as plugins. Running them as an ordinary script keeps them replaceable, and the files double as a large worked example of the API. Each section is marked `-- ---- name ----`:

| Section | Commands | Key functions |
|---|---|---|
| Shared helpers | none | `selection`, `selected_lines`, `replace_lines`, `needs_leading_eol` |
| Inline wrapping | Bold, Italic, Strikethrough, Inline Code, the HTML tag commands | `wrap_pair` toggles: it wraps, or unwraps text that's already wrapped |
| Line prefixes | Heading 1-3, Blockquote, Bullet, Numbered and Task lists | `marker_length` recognises an existing prefix so it can be replaced; `apply_line_prefix` |
| Snippets | Link, Image, Table, HTML Table/List, Code Block, Rule, Alert, Footnote | `insert_with_url` selects the `url` placeholder; `ask_table_size` |
| Markdown tables | Table Editor, Row/Column insert, delete, sort and align | `find_table` locates the table at the caret; `row_cells`, `rewrite_table` |
| Edit menu | Toggle Comment, Trim Trailing Whitespace | `trim_trailing_whitespace`, `toggle_comment` |
| Registration | none | the `markdown`, `html` and `edit` tables, registered as `group.name` |

Conventions used throughout (also stated at the top of the file):

- Positions are 1-based byte offsets. A range `start, finish` includes both ends, like `string.sub`. An empty range (a caret) is `finish == start - 1`, so `is_empty(start, finish)` is `finish < start`.
- New lines are always joined with `editor.eol()`, so a command never mixes line endings into a CRLF document.
- Most commands leave the result selected, so applying the command again undoes it, or the user can keep typing.

### Adding a built-in Lua command

1. Write the function in the appropriate `src/lua/formatting_*.lua` file, and add it to the registration table in `formatting_text.lua`, for example `markdown.underline = ...`.
2. Give it an ID in [src/command_ids.h](../src/command_ids.h), and add `{"markdown.underline", IDM_..., NULL}` to `COMMANDS` in [src/menu_commands.c](../src/menu_commands.c). Without this, menus can't refer to it and nothing can invoke it.
3. Add an item for it to [src/menus.lua](../src/menus.lua), and add a toolbar button only when appropriate.
4. Add the name to the list of built-in command names in [plugins/README.md](../plugins/README.md#built-in-command-names).
5. Add a case to [tests/test_formatting.c](../tests/test_formatting.c). `test_registration` already fails if a `markdown.`/`html.` name in `menu_commands.c` has no Lua function. A new `edit.*` Lua command also needs adding to its `textCommands` list.
6. Rebuild.

## src/help/keyboard-shortcuts.lua

**Help > Keyboard Shortcuts** is generated each time the page opens, so it reflects the current `menus.lua` and loaded plugin shortcuts. `generate_shortcuts_page()` in `markedit.c` gathers built-in shortcuts from `menu_commands.c` and plugin shortcuts from `plugin_host.c`, then passes them to `plugin_host_render_page()`, which does the following:

- runs the script in **a separate Lua state**, not the one plugins share, with only the base, `string`, `table` and `utf8` libraries, so no plugin can change the page;
- calls it with one table: `menus` (the loaded menu definition, as `{ kind = "command" | "submenu" | ... }` items) and `shortcuts` (command name to shortcut text);
- expects the Markdown page back as a string.

The script lists each menu's commands that have shortcuts under that menu's name, then any commands with shortcuts that aren't in the menus. Last comes a short hand-written `OTHER_KEYS` list of keys that aren't commands at all, such as Tab/Shift+Tab. **When you add a key to the accelerator table in `markedit.c` that isn't a command, add it to `OTHER_KEYS` too.** Commands get onto the page automatically through their shortcut text in `menu_commands.c`.

[tests/test_menus.c](../tests/test_menus.c) renders the page from `src/` and checks that every shortcut in the command table appears on it.

## plugins/

- [README.md](../plugins/README.md) is the plugin guide. It's shipped in `dist/plugins/`, and **Help > Plugin Guide** opens it, so it's written for users, not for MarkEdit developers.
- [edit-convert.lua](../plugins/edit-convert.lua) owns every **Edit > Convert** command and creates that submenu; there are no parallel built-in command IDs or menu entries.
- [example.lua](../plugins/example.lua) is a working plugin that shows each part of the API: `menu.add` with submenus and with `/Menu/...` paths, a persistent `menu.checkbox`, `ui.message`/`prompt`/`choose`, and an `events.on("save")` handler.
- MarkEdit loads each top-level `*.lua` file and each folder containing `init.lua`, in name order. `types/`, `.luarc.json`, and other files are ignored. Personal extensions use the same rules and load afterward.

## The C side

| File | Role |
|---|---|
| [src/plugin_host.c](../src/plugin_host.c) / [.h](../src/plugin_host.h) | The plugin host entry points, the menu definition, help page rendering and shutdown. It knows nothing about the application: it reaches it only through `PluginHostCallbacks`. The host is split across siblings: [plugin_lua_runtime.c](../src/plugin_lua_runtime.c) owns the `lua_State`, sandbox, memory/time limits and calls; [plugin_registry.c](../src/plugin_registry.c) owns the API tables, command/event registries and running a registered command; [plugin_discovery.c](../src/plugin_discovery.c) owns extension discovery and the folder-local `require` loader. |
| `src/lua_editor_api.c` / `.h` | Implements the Scintilla-facing `editor.*` API, including indexed and bulk multi-selection operations. |
| [src/menu_commands.c](../src/menu_commands.c) / [.h](../src/menu_commands.h) | The table of built-in command names, their `IDM_*` IDs and shortcut text, and validation of a loaded menu definition. |
| [src/markedit.c](../src/markedit.c) | Loads everything (`load_menus_and_plugins`), builds the menu bar from the definition, routes commands to Lua overrides (`handle_main_command`) and fires the `save`, `open` and `tab_changed` events. |
| [markedit.rc](../markedit.rc) | Builds `src/menus.lua`, the three formatting Lua parts and the two `src/help/` pages into the exe. |

## Changing and testing

- **`src/lua/formatting_*.lua` or `src/menus.lua`:** rebuild (`build.bat`), then restart MarkEdit or use **Reload Plugins**.
- **To iterate without rebuilding:** put a replacement in `%APPDATA%\markedit\plugins` (for example `commands.register("markdown.bold", ...)`), or edit `%APPDATA%\markedit\menus.lua`. Then use **Reload Plugins** after each change. Copy a finished bundled extension back into the repository when appropriate.
- **Tests** (`test.bat` runs them all; run them from the repository root):
  - [tests/test_plugin_host.c](../tests/test_plugin_host.c): the API, the sandbox, load failures, events and the time limit.
  - [tests/test_menus.c](../tests/test_menus.c): reads `src/menus.lua` from disk, and checks that it loads and that every name in it is known. It also renders `src/help/keyboard-shortcuts.lua` and checks that the page lists every shortcut.
  - [tests/test_formatting.c](../tests/test_formatting.c): joins the three built-in formatting sources from disk and runs commands against a real Scintilla window.

  Because these tests read the scripts from `src/`, they test your current edits without a rebuild of `markedit.exe`.
