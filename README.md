# MarkEdit

A tiny, native Win32 Markdown editor with live HTML preview.

- **Left pane** — a [Scintilla](https://www.scintilla.org/) source editor for your Markdown, with line numbers, current-line highlighting, smart four-column Tab/Shift+Tab indentation, and syntax highlighting via vendored [Lexilla](https://github.com/ScintillaOrg/lexilla) lexers, chosen from the file extension.
- **Right pane** — a WebView2-hosted HTML preview, refreshed shortly after edits and rendered by the vendored [cmark-gfm](https://github.com/github/cmark-gfm) library. Every file type previews: Markdown renders as a document, HTML files render as the saved web page, and any other recognised type (source code, JSON, YAML, `.txt`) shows as its own syntax-highlighted source, with highlighting skipped above 512 KB. Files of an unrecognised type, and new untitled tabs, preview as Markdown.
- A formatting toolbar above the editor for bold, italic, strikethrough, inline code, fenced code blocks, headings, blockquotes, bullet/numbered/task lists, links, images, tables, footnotes, horizontal rules, alerts, toggling the preview pane, and swapping the panel sides.
- Preview supports [Mermaid](https://mermaid.js.org/) diagrams (fenced ` ```mermaid ` blocks) with zoom/pan controls, [KaTeX](https://katex.org/) mathematics, and GFM-style tables, task lists, strikethrough, links/images, and nested lists.
- GitHub Light and GitHub Dark themes (View → Editor Theme and View → Markdown Theme) apply to the editor, preview, and native window chrome (including a dark title bar), are remembered between sessions, and the preview starts directly in the selected theme (no wrong-theme flash). Their colours can be changed in Lua: Plugins → Customize Theme Colours writes `%APPDATA%\markedit\themes.lua` listing every colour, and `plugins/examples/themes.lua` is a complete example.
- The window appears immediately on launch; spell-check activation, the WebView2 preview engine, and reopening the previous session's files happen in the background right after, instead of delaying the window's appearance.

Launching with no arguments opens the editor with the saved workspace layout; launching with a Markdown file path shows just the rendered preview (`markedit.exe notes.md`), and any other file opens in the editor.

## Features

MarkEdit combines a native Markdown workspace with the tools needed for
writing and small software projects:

- Live GFM preview with local Mermaid, KaTeX and syntax-highlighting assets.
- Multi-document Scintilla editing, workspaces, search, recovery and spell checking.
- Integrated Run, Terminal, Problems, Debug and Git tabs.
- Python and C/C++ running/debugging, plus sandboxed Lua extensions.
- Bundled text, data and retro-computing tools, including Z80 TAP Compile and Run.

See [docs/features.md](docs/features.md) for the complete feature catalogue.

## Building

The standard build requires Visual Studio with the **Desktop development with
C++** workload. `build.bat` finds the newest installed MSVC toolchain through
Visual Studio Installer, so it works from a normal command prompt as well as a
Developer Command Prompt.

```
build.bat
```

This uses MSVC's C/C++ toolchain with size optimization, function/data-level
linking, whole-program optimization, and a statically linked C/C++ runtime.
After compiling, the script safely clears and recreates `dist/`, then stages
`markedit.exe`, the required runtime DLLs, offline preview assets, and licence
files there.

A [premake5.lua](premake5.lua) is also provided for MinGW-w64 GCC, Zig, and MSVC builds. Each builds the same targets (`markedit.exe` in `build\bin`, statically linking Lexilla and Lua, and the test programs under `build\tests`) and stages `dist/` the way `build.bat` does:

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

Run the tests (Markdown parsing, editor commands, language detection, outline/minimap, themes, file and text handling, output decoding/processes, terminal grid/input/ConPTY processes, recovery, the plugin host, the menu definition, the formatting and Lua-formatting commands, the assemblers, and, when Node.js is installed, the preview bridge) with:

```
test.bat
```

After `build.bat`, the native UI smoke harness exercises output-panel tab
switching, close/reopen, Git-panel reopening and resize layout against the
packaged executable:

```powershell
pwsh -File tests/test_ui_smoke.ps1 -Executable dist/markedit.exe
```

The Windows CI workflow runs all three steps and uploads the staged `dist/`
folder.

Running `markedit.exe` also requires the [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) to be installed (present by default on current Windows 10/11).

## Distribution folder

`dist/` is the clean, unpacked release staging folder generated by `build.bat`. It contains only:

- `markedit.exe` (Lexilla is linked straight into it, not shipped separately), the two required runtime DLLs (`Scintilla.dll` and `WebView2Loader.dll`), and the standalone bundled GDB 17.1 executable;
- the preview shell, Mermaid, Highlight.js, GitHub Markdown CSS, and the complete `katex/` asset tree needed for offline rendering;
- the `plugins/` folder with bundled extensions, the plugin guide, examples, and Lua Language Server definitions (Lua itself is linked into `markedit.exe`); and
- `LICENSE`, `THIRD_PARTY_NOTICES.md`, and the referenced component licence/version files under `third_party/`. The packaged GDB folder contains only `gdb.exe` and its small licence/provenance files; corresponding source remains in the MarkEdit source repository rather than the binary distribution.

Do not package `settings.json`, debug symbols, import libraries, compiler objects, or temporary archives. Application preferences and Lua plugin settings share `%APPDATA%\markedit\settings.json`; the WebView2 browser profile and recovery snapshots live under `%LOCALAPPDATA%\MarkEdit`. The Inno Setup manifest mirrors this runtime-and-licences layout.

## Project layout

```
src/                     application sources, including:
  markedit.c / .h          Win32 application and UI orchestration
  internal/                named private Win32 subsystems included by markedit.c
  command_ids.h            WM_COMMAND IDs of the built-in commands
  app_accelerators.c / .h  fixed Win32 shortcuts, checked against the command registry by tests
  menus.lua                default menu bar definition, built into markedit.exe
  lua/formatting_*.lua     Markdown and Markdown-aware Edit commands built into markedit.exe
  menu_commands.c / .h     built-in command names used by menus.lua, with their shortcut text
  settings.c / settings*.h  settings.json persistence; settings_values.h holds the persisted
                            values, settings_io.h the load/save surface, settings.h the umbrella
  help/                    Help pages built into markedit.exe: markdown-reference.md, and keyboard-shortcuts.lua,
                           which generates the Keyboard Shortcuts page from the menus and shortcut table
  file_explorer.c / .h     workspace file explorer panel
  layout_metrics.c / .h    explorer panel geometry shared by layout and settings
  toolbar_icons.c / .h     formatting-toolbar icon painters
  path_utils.c / .h        Windows path utilities, including allocation-free workspace filtering
  text_codec.c / .h        UTF conversion, text-file encoding detection and conversion, JSON string encoding
  git_status.c / .h        testable parser for NUL-delimited Git porcelain v2 status
  markdown.c / .h          cmark-gfm rendering adapter
  plugin_host.c / .h       plugin host entry points, menu definition and help page rendering
  plugin_lua_runtime.c     sandboxed Lua state, memory and time limits, protected calls
  plugin_registry.c        plugin API tables and the command/event registries
  plugin_discovery.c       extension discovery and the folder-local require loader
  lua_editor_api.c / .h    Scintilla-facing Lua editor API and multi-selection operations
  outline_minimap.c / .h   outline panel and minimap subsystem
  scintilla_config.c / .h  Scintilla view setup: margins, markers, wrap/whitespace/indent
  progress_dialog.c / .h   cancellable IProgressDialog wrapper for large opens and saves
  output_process.c / .h    output-panel programs on pipes, in a job, with a batched reader thread
  output_text.c / .h       batched output decoding (ANSI colours, OEM fallback), file locations in
                           error messages
  terminal_process.c / .h  the Terminal tab's shell on a Windows pseudoconsole (ConPTY),
                           in a job, with reader, queued-input writer and exit-watcher threads
  terminal_grid.c / .h     VT100/ECMA-48 screen buffer: cursor-addressed cells, SGR colours,
                           scroll regions, the alternate screen, bounded ring-buffer scrollback
  terminal_input.c / .h    Win32 virtual keys to xterm-compatible VT input sequences
  terminal_view.c / .h     the Terminal tab's rendering and input control; batches key repeats
                           and routes focused keys ahead of queued output and app accelerators
  theme.c / .h             light/dark palettes by role, their themes.lua names and template,
                           and paint helpers
plugins/                 bundled Lua extensions, plugin guide, examples, and language-server types
docs/lua.md              developer guide to MarkEdit's Lua files: what each is, load order, how commands reach Lua
docs/architecture.md     native/Lua boundaries and subsystem ownership
release-flags.rsp        release flags used by the optional GCC/Zig Premake builds
cmark-gfm-sources.rsp    vendored renderer source list and its defines/include paths
markedit-sources.rsp     application source list used by build.bat
lexilla-sources.rsp      supported lexer source list used by build.bat
lua-sources.rsp          Lua core and plugin-library source list used by build.bat and test.bat
plugin-sources.rsp       plugin host sources the plugin, menu, formatting and theme tests link
check_structure.ps1      checks the source-layout rules in AGENTS.md; run by test.bat
gen_compdb.ps1, .clangd  generate compile_commands.json for the clangd language server
markedit.rc / .ico       Win32 resources (icon, version info)
markedit.manifest        DPI-awareness and long-path declarations
preview_shell.html       HTML/JS shell hosting the WebView2 preview + Mermaid
github-markdown.css      GitHub-flavored preview stylesheet (light + dark)
mermaid.min.js           bundled Mermaid renderer
highlight*.js / .css     bundled Highlight.js renderer and light/dark themes
katex/                    bundled KaTeX renderer, stylesheet, and fonts
make_icon.ps1            regenerates markedit.ico
build.bat                size-optimized MSVC build and distribution script
scripts/*_msvc.ps1       shared MSVC compilation logic for build.bat and test.bat
scripts/msvc-env.bat     locates Visual Studio and initializes its x64 tools
premake5.lua             alternative build for MinGW GCC, Zig (make) or MSVC (Visual Studio)
markedit.iss             Inno Setup installer script
dist/                    clean unpacked release staging folder (generated, not tracked)
test.bat / tests/        test programs: parser, editor commands, language, outline, theme, core,
                          output, Git status/workflows, terminal grid/input/process/view, recovery, plugin host, menus,
                         formatting, Lua formatting, assemblers, preview bridge
third_party/webview2/    WebView2 headers + import libs
third_party/scintilla/   Scintilla headers, licence, and built Scintilla.dll
third_party/lexilla/     Lexilla 5.5.1 lexer sources and licence, trimmed to the supported languages
third_party/cmark-gfm/   cmark-gfm 0.29.0.gfm.13 source (config.h adjusted for MSVC) and BSD-2-Clause licence
third_party/gdb/         standalone GDB 17.1 runtime, GPLv3 text, corresponding source, and build recipe (only runtime/licence metadata is packaged)
third_party/lua/         Lua 5.4.9 library sources (unmodified), MIT licence and provenance
third_party/highlightjs/ Highlight.js 11.11.1 licence and provenance
third_party/katex/       KaTeX 0.17.0 licence and provenance
third_party/scowl/       SCOWL 2026.02.25 licence and provenance for the anagram word list
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
can't access arbitrary files or use the network, and a plugin that runs for
more than five seconds is stopped. Binary output can only be written through
a user-controlled Save As dialog; its optional open-after-save action can use
the saved file's Windows association or a plugin-supplied application, but
only after that dialog is accepted. Only install plugins you trust. Personal
extensions live in `%APPDATA%\markedit\plugins`; folder extensions can read
only their own Lua modules through MarkEdit's restricted `require` loader.

## Licence

MarkEdit is available under the MIT License. See `LICENSE` and
`THIRD_PARTY_NOTICES.md` for bundled-component licences and provenance.
