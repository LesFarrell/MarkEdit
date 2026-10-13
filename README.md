# MarkEdit

A tiny, native Win32 Markdown editor with live HTML preview.

- **Left pane** — a [Scintilla](https://www.scintilla.org/) source editor for your Markdown, with line numbers, current-line highlighting, smart four-column Tab/Shift+Tab indentation, and syntax highlighting via vendored [Lexilla](https://github.com/ScintillaOrg/lexilla) lexers, chosen from the file extension.
- **Right pane** — a WebView2-hosted HTML preview, refreshed shortly after edits and rendered by the vendored [cmark-gfm](https://github.com/github/cmark-gfm) library. Every file type previews: Markdown renders as a document, HTML files render as the saved web page, and any other recognised type (source code, JSON, YAML, `.txt`) shows as its own syntax-highlighted source, with highlighting skipped above 512 KB. Files of an unrecognised type, and new untitled tabs, preview as Markdown.
- A formatting toolbar above the editor for bold, italic, strikethrough, inline code, fenced code blocks, headings, blockquotes, bullet/numbered/task lists, links, images, tables, footnotes, horizontal rules, alerts, toggling the preview pane, and swapping the panel sides.
- Preview supports [Mermaid](https://mermaid.js.org/) diagrams (fenced ` ```mermaid ` blocks) with zoom/pan controls, [KaTeX](https://katex.org/) mathematics, and GFM-style tables, task lists, strikethrough, links/images, and nested lists.
- GitHub Light and GitHub Dark themes (View → Editor Theme and View → Markdown Theme) apply to the editor, preview, and native window chrome (including a dark title bar), are remembered between sessions, and the preview starts directly in the selected theme (no wrong-theme flash).
- The window appears immediately on launch; spell-check activation, the WebView2 preview engine, and reopening the previous session's files happen in the background right after, instead of delaying the window's appearance.

Launching with no arguments opens the editor with the saved workspace layout; launching with a Markdown file path shows just the rendered preview (`markedit.exe notes.md`), and any other file opens in the editor.

## Features

- CommonMark plus GFM tables, task lists, strikethrough, bare-URL autolinks, and footnotes, along with code-fence language classes used by syntax-aware preview features.
- GitHub-style note, tip, important, warning, and caution alerts.
- Emoji shortcodes (`:tada:`, `:rocket:`, and similar) render as their Unicode emoji in the preview; unrecognised shortcodes are left as plain text.
- Wiki-style `[[Page Name]]` and `[[Page Name|label]]` links open the named Markdown file from the same folder as the current document, offering to create it if it doesn't exist yet. File → Go Back (Alt+Left) returns to wherever the jump was made from.
- Local syntax highlighting for named code fences in the preview, and for Markdown source in the editor, with matching light and dark themes.
- Editor syntax highlighting follows the file extension: Markdown, JSON, XML, HTML, YAML, C/C++/JavaScript/TypeScript, Python, Bash, PowerShell, and Lua. Unrecognised extensions fall back to Markdown.
- Python files can be run with Ctrl+F5 or debugged with F5 through the standard `pdb` debugger. At every stop, an integrated panel shows local variable names and values alongside Continue, Step Over, Step Into, and Step Out controls. The current execution line has a full-line highlight and gutter arrow. The same values are still printed in the console, where `vars` and `globals` remain available. MarkEdit prefers an activated virtual environment, then a project-local `.venv`/`venv`, then Python on `PATH`.
- Single-file C and C++ programs (`.c`, `.cc`, `.cpp`, `.cxx`) can likewise be built and run with Ctrl+F5 or compiled with debug symbols and opened in GDB with F5. GDB itself stays hidden while the debugged program opens its own visible console for input and output. Editor breakpoints are loaded before execution; at every stop, the integrated debugger shows function arguments and local values and highlights the current line. Use F10 to step over, F11 to step into, Shift+F11 to step out, and F6 to continue from the panel, editor, or Run menu; `print expression`, `watch expression`, and `vars` remain available in the console. Running can use GCC/G++, Clang, Zig, or MSVC (`cl.exe`, found via the `Compiler.VcvarsallPath` value in `settings.json`); debugging requires GCC/G++ or Clang and uses the bundled GDB 17.1. `Compiler.GdbPath` can select another GDB folder, and `PATH` remains a fallback if the bundled copy is absent. MSVC is Run-only. `Compiler.Backend` pins Run/Debug to one of GCC, Clang, Zig, or MSVC instead of the default auto-detect order.
- Lua files (`.lua`) can be run with Ctrl+F5 using `lua`, a versioned `lua5.x`, or `luajit` found on `PATH`. Lua is Run-only; there is no Lua debugger integration.
- F9 (or a click in the marker gutter) toggles per-file Python/C/C++ breakpoints. Breakpoints follow edited lines and are restored for that file in later MarkEdit sessions; Run → Clear All Breakpoints removes them together.
- Multiple document tabs with independent undo history, selections, dirty state, encoding, and paths. Tabs have close buttons, support middle-click closing, can be reopened, and are restored between sessions.
- Tabs can be reordered by dragging. Dirty and unsaved tabs are snapshotted every five seconds to a recovery area under Local AppData and can be restored after an interrupted session.
- A formatting toolbar (Bold, Italic, Strikethrough, Inline code, Code block, Headings, Blockquote, Lists, Link, Image, Table, Footnote, Horizontal rule, Alert, Toggle preview, Swap panel sides), shown while the editor pane is visible.
- Smart Markdown editing continues blockquotes and bullet, numbered, and task lists on Enter (incrementing numbered lists and resetting task items), terminates empty items, and pairs brackets and inline backticks.
- Smart four-column tabs use real tabs for leading indentation, nest a list item from anywhere on its line, and use spaces only for alignment within ordinary text. Enter preserves the current indentation, adds a level after C-family/JSON braces, Python/YAML colons, and shell block openers, and expands paired braces or brackets onto aligned lines. Closing braces typed on a blank indented line move back one level automatically.
- Line editing commands: duplicate, delete, join, and move lines up or down, plus trimming trailing whitespace across the document in a single undo step.
- Toggle Comment wraps the selected lines in an HTML comment (Markdown has no line-comment syntax) and unwraps them again.
- Select All Occurrences (Ctrl+Shift+L) turns every match of the word under the caret, or of the current selection, into its own selection for multi-caret editing.
- Convert Line Endings (Edit → Convert → Line Endings) rewrites the document to CRLF, LF, or CR and switches the editor's save format to match.
- Format JSON and Minify JSON (Tools menu) operate on the current selection, the fenced code block at the caret, or the whole document. Input is validated first; malformed JSON, including trailing commas or missing separators, is left untouched and its error is highlighted.
- Word wrap (View → Word Wrap or Alt+Z), remembered between sessions.
- A live status bar reports the caret line/column, word and Unicode character counts, UTF-8/BOM state, line endings, and save/recovery state.
- Native Windows spell checking (View → Spell Check), remembered between sessions. Misspelled Markdown prose gets a themed squiggle; links, inline code, and fenced code are excluded. Right-click a marked word for corrections, Ignore Word, or Add to Dictionary. View → Spell Check Language selects any installed Windows dictionary.
- Pasting a bitmap or dropping an image copies it into an `images` folder beside the saved document and inserts a relative Markdown image link; unsaved documents are offered Save As first.
- Stable heading permalinks, a floating preview outline, and an optional docked source outline whose entries jump directly to their headings.
- Inline (`$...$`) and display (`$$...$$` or fenced `math`) mathematical expressions.
- YAML front matter delimited by `---` (and closed by `---` or `...`) is treated as document metadata and omitted from the preview.
- Mermaid diagram rendering with a zoom/pan toolbar.
- Copy buttons on rendered fenced code blocks.
- Export of the enhanced preview—including highlighted code, rendered math, and available rendered diagrams—to an HTML document.
- Printing and PDF export of the enhanced rendered preview through WebView2.
- The table editor resizes an existing GFM table while preserving its cells, or creates a new table with a chosen number of rows and columns.
- A persistent workspace sidebar remembers the chosen folder and provides a hierarchical file tree with Open, New, Rename, and Refresh actions. Open the `..` entry, choose “Open Parent Folder”, or press Backspace while the tree is focused to move the workspace up one directory. Moving up from a filesystem root shows available drives and mapped network shares. Renaming a file or folder updates matching inline and reference-style Markdown links across the workspace, including links in open unsaved buffers.
- Quick Open searches file names in the active workspace, while Find in Folder searches supported text files and opens results at the matching line. If no workspace is open, either command first asks for a folder. Generated/dependency folders are skipped.
- Lua plugins: bundled extensions load from `plugins` beside `markedit.exe`, followed by personal extensions in `%APPDATA%\markedit\plugins`. **Plugins → New Plugin**, **Open Plugins Folder**, **Manage Plugins**, and **Reload Plugins** cover the full edit/test/enable workflow without administrator rights. A plugin may be one `.lua` file or a folder containing `init.lua` and private modules. Commands can have keyboard shortcuts, check marks and persistent settings. The plugin guide, `plugins/README.md` (also Help → Plugin Guide), documents the API, and `plugins/example.lua` is a working example.
- CSV tools under **Tools → CSV** parse comma-, semicolon-, tab-, and whitespace-separated data with quoted fields. Commands inspect, normalize, visually align columns, convert, sort, filter, deduplicate, transpose, edit columns, and export to JSON or Markdown tables.
- Additional bundled plugins provide CLEF-aware log inspection and filtering, SQL formatting, natural line sorting, integer-base and Roman-numeral conversion, JSON Lines/query-string/key-value conversion, and an offline heuristic grammar checker.
- The whole menu bar is defined in Lua. Plugins → Customize Menus copies the default definition to `%APPDATA%\markedit\menus.lua` and opens it; items there can be reordered, renamed, removed, or point at Lua functions, and Reload Plugins applies the changes. A definition with a mistake is reported and the default menus are used instead.
- Markdown and HTML formatting, plus their Markdown-aware Edit commands, are written in focused Lua files under `src/lua/` and built into `markedit.exe`. The Edit → Convert submenu is supplied entirely by `plugins/edit-convert.lua`; JSON formatting, validation, error highlighting, key sorting, string escaping, and JSON/YAML conversion come from `plugins/json-tools.lua`. Bundled plugins are ordinary files in `plugins/`, so their commands and menus can be changed without duplicating native command implementations.
- Detection of files changed by another program, with an offer to reload the active document and an indicator on changed background tabs.
- Synchronized scrolling between the Markdown editor and rendered preview.
- Recently opened files (File → Recent Files), persisted across runs and pruned automatically if a file no longer exists.
- Insert File at Cursor (File menu) reads a chosen file's contents and inserts them at the caret, replacing any selection.
- Editor font is configurable (View → Font) and remembered between sessions.
- Editor zoom (View → Zoom In/Out/Reset or Ctrl+=/Ctrl+-/Ctrl+0) scales the editor text independently of the configured font size and is remembered between sessions.
- Open Terminal Here (File menu or toolbar) opens Windows Terminal, falling back to Command Prompt, in the current file's folder. Show in Explorer (File menu) opens Explorer with the current file pre-selected.
- An optional UTF-8 custom preview stylesheet (View → Markdown Theme → Custom Preview CSS), embedded into rendered HTML exports and remembered between sessions.
- Focus mode (View → Focus Mode or Ctrl+Shift+F11) hides the menu, tabs, and formatting toolbar and is remembered between sessions.
- Window position/size/maximized state, editor/preview order, workspace visibility, active workspace folder, and splitter position are remembered between sessions; View → Preview on Left controls the editor/preview order.
- Find/Replace, Go to Line, Undo/Redo, standard clipboard shortcuts. The search and replacement text, match-case setting, and search direction are remembered between sessions.
- A Markdown syntax reference (Help → Markdown Reference), opened as a live Markdown document so the preview renders every construct it documents. Help → Keyboard Shortcuts opens the shortcut list the same way, and Help → About MarkEdit shows the version.
- Tools → Settings gathers appearance, font, word wrap, preview, editor, and compiler options in one dialog.
- Unicode filenames, long Windows paths, drag-and-drop, and document-relative links/images.
- Text encodings: UTF-8 (with or without a BOM), UTF-16 LE/BE (with or without a BOM), and the system ANSI code page are detected on open and kept on save; the status bar shows which one a document uses. File → Reopen with Encoding rereads a file whose encoding was guessed wrong, and Edit → Convert → Encoding changes the one it's saved in. Saving warns first if the document has characters the chosen encoding can't store (anything outside the ANSI code page, or bytes that aren't valid text when saving UTF-16). Insert File, Find in Folder, and Replace in Folder decode files the same way; Replace in Folder writes each file back in its own encoding and leaves alone any file it can't store the replacement in.
- Failure-safe saves: content is flushed to a temporary file before atomically replacing the destination.
- Large files, including ones past 2 GB, open in large-file mode once they reach `View.LargeFileThresholdMB` in `settings.json` (64 MB by default); a smaller open document that grows past the threshold switches modes too. Large opens and saves show cancellable progress. The file is shown as plain text with word wrap off; the preview, outline, word count, word completion, brace matching, smart Enter/Tab, and autosave recovery are switched off for that document so editing stays responsive. Find/Replace, Go to Line, bookmarks, and saving work as normal.
- Preview hardening for untrusted documents: raw HTML and dangerous URLs are disabled, Mermaid runs in strict mode, and the shell uses a restrictive content-security policy.

### Keyboard shortcuts

| Shortcut | Action |
|---|---|
| Ctrl+N | New |
| Ctrl+O | Open |
| Ctrl+K | Quick Open in Folder |
| Ctrl+S | Save |
| Ctrl+Shift+S | Save As |
| Ctrl+Alt+S | Save All |
| Ctrl+P | Print rendered preview |
| Ctrl+W | Close current tab |
| Ctrl+Shift+T | Reopen last closed tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Alt+Left | Go back to where the last wiki-link jump was made from |
| Ctrl+Z / Ctrl+Y | Undo / Redo |
| Ctrl+X / Ctrl+C / Ctrl+V | Cut / Copy / Paste |
| Ctrl+A | Select All |
| Tab / Shift+Tab | Smart indent / outdent |
| Ctrl+D | Duplicate line |
| Ctrl+Shift+K | Delete line |
| Ctrl+J | Join lines |
| Alt+Up / Alt+Down | Move line or selection up / down |
| Ctrl+/ | Toggle comment |
| Ctrl+Shift+D | Select next occurrence |
| Ctrl+Shift+L | Select all occurrences |
| Ctrl+] | Jump to matching brace |
| Ctrl+F | Find |
| F3 | Find Next |
| Ctrl+H | Replace |
| Ctrl+G | Go to Line |
| Ctrl+F2 | Toggle bookmark |
| F2 / Shift+F2 | Next / previous bookmark |
| Ctrl+Shift+V | Toggle preview pane |
| Ctrl+Shift+E | Toggle editor pane |
| Ctrl+Shift+X | Toggle file explorer |
| Ctrl+\\ | Toggle horizontal editor split |
| Ctrl+F6 / Ctrl+Shift+F6 | Cycle panes forward / back |
| Ctrl+Shift+F11 | Toggle focus mode |
| Alt+Z | Toggle word wrap |
| Ctrl+= / Ctrl+- | Zoom in / out |
| Ctrl+0 | Reset zoom |
| Ctrl+F5 | Run active Python, C, C++, or Lua file |
| F5 | Debug active Python, C, or C++ file |
| F6 | Continue the active debug session |
| F10 | Step over the current line |
| F11 | Step into the current call |
| Shift+F11 | Step out of the current call |
| F9 | Toggle debugger breakpoint |

## Building

The standard build requires [Zig](https://ziglang.org/) on `PATH`.

```
build.bat
```

This uses Zig's C/C++ toolchain with size optimization, dead-section elimination, and symbol stripping. After compiling, it safely clears and recreates `dist/`, then stages `markedit.exe`, the required runtime DLLs, offline preview assets, and licence files there.

A [premake5.lua](premake5.lua) is also provided for MinGW-w64 GCC, Zig, and MSVC builds. Each builds the same targets (`markedit.exe`, statically linking Lexilla and Lua, and the test programs under `tests\`) and stages `dist/` the way `build.bat` does:

```bat
rem MinGW-w64 GCC
premake5 gmake
make -C build config=release_x64

rem Zig (zig cc / zig c++ / zig rc stand in for the GCC toolchain)
premake5 --zig gmake
make -C build config=release_x64

rem MSVC (Visual Studio 2022 or later; open build\MarkEdit.sln, or:)
premake5 vs2022
msbuild build\MarkEdit.sln /p:Configuration=Release /p:Platform=x64
```

The two `gmake` builds need GNU make running its recipes under a POSIX shell, as [w64devkit](https://github.com/skeeto/w64devkit) or MSYS2 provide.

Run the tests (Markdown parsing, editor commands, file and text handling, recovery, the plugin host, the menu definition, the formatting commands, and, when Node.js is installed, the preview bridge) with:

```
test.bat
```

GitHub Actions runs `test.bat` and `build.bat`. Successful workflow runs
produce a self-contained Windows artifact.

Running `markedit.exe` also requires the [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) to be installed (present by default on current Windows 10/11).

## Distribution folder

`dist/` is the clean, unpacked release staging folder generated by `build.bat`. It contains only:

- `markedit.exe` (Lexilla is linked straight into it, not shipped separately), the two required runtime DLLs (`Scintilla.dll` and `WebView2Loader.dll`), and the standalone bundled GDB 17.1 executable;
- the preview shell, Mermaid, Highlight.js, GitHub Markdown CSS, and the complete `katex/` asset tree needed for offline rendering;
- the `plugins/` folder with bundled extensions, the plugin guide, examples, and Lua Language Server definitions (Lua itself is linked into `markedit.exe`); and
- `LICENSE`, `THIRD_PARTY_NOTICES.md`, and the referenced component licence/version files under `third_party/`. The packaged GDB folder contains only `gdb.exe` and its small licence/provenance files; corresponding source remains in the MarkEdit source repository rather than the binary distribution.

Do not package `settings.json`, debug symbols, import libraries, compiler objects, or temporary archives. Application preferences and Lua plugin settings share `%APPDATA%\markedit\settings.json`; the WebView2 browser profile and recovery snapshots live under `%LOCALAPPDATA%\MarkEdit`. The Inno Setup and GitHub Actions manifests mirror this runtime-and-licences layout.

## Project layout

```
src/                     application sources, including:
  markedit.c / .h          Win32 application and UI orchestration
  internal/                named private Win32 subsystems included by markedit.c
  command_ids.h            WM_COMMAND IDs of the built-in commands
  menus.lua                default menu bar definition, built into markedit.exe
  lua/formatting_*.lua     Markdown, HTML and Markdown-aware Edit commands built into markedit.exe
  menu_commands.c / .h     built-in command names used by menus.lua, with their shortcut text
  settings.c / settings*.h  settings.json persistence; settings_values.h holds the persisted
                            values, settings_io.h the load/save surface, settings.h the umbrella
  help/                    Help pages built into markedit.exe: markdown-reference.md, and keyboard-shortcuts.lua,
                           which generates the Keyboard Shortcuts page from the menus and shortcut table
  file_explorer.c / .h     workspace file explorer panel
  toolbar_icons.c / .h     formatting-toolbar icon painters
  path_utils.c / .h        Windows path and heap-string utilities
  text_codec.c / .h        UTF conversion, text-file encoding detection and conversion, JSON string encoding
  markdown.c / .h          cmark-gfm rendering adapter
  plugin_host.c / .h       plugin host entry points, menu definition and help page rendering
  plugin_lua_runtime.c     sandboxed Lua state, memory and time limits, protected calls
  plugin_registry.c        plugin API tables and the command/event registries
  plugin_discovery.c       extension discovery and the folder-local require loader
  lua_editor_api.c / .h    Scintilla-facing Lua editor API and multi-selection operations
  outline_minimap.c / .h   outline panel and minimap subsystem
  scintilla_config.c / .h  Scintilla view setup: margins, markers, wrap/whitespace/indent
  progress_dialog.c / .h   cancellable IProgressDialog wrapper for large opens and saves
  theme.h                  shared native light/dark palette and paint helpers
plugins/                 bundled Lua extensions, plugin guide, examples, and language-server types
docs/lua.md              developer guide to MarkEdit's Lua files: what each is, load order, how commands reach Lua
docs/architecture.md     native/Lua boundaries and subsystem ownership
cmark-gfm-sources.rsp    vendored renderer source list and compile flags
markedit-sources.rsp     application source list used by build.bat
lexilla-sources.rsp      supported lexer source list used by build.bat
lua-sources.rsp          Lua core and plugin-library source list used by build.bat and test.bat
markedit.rc / .ico       Win32 resources (icon, version info)
markedit.manifest        DPI-awareness and long-path declarations
preview_shell.html       HTML/JS shell hosting the WebView2 preview + Mermaid
github-markdown.css      GitHub-flavored preview stylesheet (light + dark)
mermaid.min.js           bundled Mermaid renderer
highlight*.js / .css     bundled Highlight.js renderer and light/dark themes
katex/                    bundled KaTeX renderer, stylesheet, and fonts
make_icon.ps1            regenerates markedit.ico
build.bat                size-optimized Zig build script
premake5.lua             alternative build for MinGW GCC, Zig (make) or MSVC (Visual Studio)
markedit.iss             Inno Setup installer script
dist/                    clean unpacked release staging folder (generated, not tracked)
test.bat / tests/        test programs: parser, editor commands, core, recovery, plugin host, menus, formatting, preview bridge
third_party/webview2/    WebView2 headers + import libs
third_party/scintilla/   Scintilla headers, licence, and built Scintilla.dll
third_party/lexilla/     Lexilla 5.5.1 lexer sources and licence, trimmed to the supported languages
third_party/cmark-gfm/   cmark-gfm 0.29.0.gfm.13 source (config.h adjusted for MSVC) and BSD-2-Clause licence
third_party/gdb/         standalone GDB 17.1 runtime, GPLv3 text, corresponding source, and build recipe (only runtime/licence metadata is packaged)
third_party/lua/         Lua 5.4.9 library sources (unmodified), MIT licence and provenance
third_party/highlightjs/ Highlight.js 11.11.1 licence and provenance
third_party/katex/       KaTeX 0.17.0 licence and provenance
```

## GitHub-style preview

The preview uses GitHub-flavored CSS (`github-markdown.css`, checked in next to `preview_shell.html`) for a more authentic look. Its light and dark variants are synchronized with the matching GitHub editor palette. Mermaid, Highlight.js, and KaTeX assets are also local, so rendering works offline and does not silently fetch executable content, styles, or fonts from a CDN.

Build outputs such as `markedit.exe` and the assembled `dist/` folder are generated locally and are not tracked in git; neither is a `webview2_data/` folder left beside the exe by builds from before the WebView2 profile moved to `%LOCALAPPDATA%\MarkEdit\WebView2`.

## Security and Markdown scope

MarkEdit uses cmark-gfm without its unsafe-rendering option, so raw HTML is
omitted and dangerous URL schemes are scrubbed rather than executed in the
preview. KaTeX runs with trusted commands disabled and bounded expansion and
output sizing. Links using HTTP, HTTPS, and `mailto:` open in the system handler;
relative links resolve from the Markdown document's directory.

Plugins are code, and run with full access to the open document. They're
limited to Lua's string, table, math, UTF-8, and coroutine libraries, so they
can't access arbitrary files, start programs, or use the network, and a plugin
that runs for more than five seconds is stopped. Only install plugins you
trust. Personal extensions live in `%APPDATA%\markedit\plugins`; folder
extensions can read only their own Lua modules through MarkEdit's restricted
`require` loader.

## Licence

MarkEdit is available under the MIT License. See `LICENSE` and
`THIRD_PARTY_NOTICES.md` for bundled-component licences and provenance.
