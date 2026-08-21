# MarkEdit

A tiny, native Win32 Markdown editor with live HTML preview.

- **Left pane** — a [Scintilla](https://www.scintilla.org/) source editor for your Markdown, with line numbers, current-line highlighting, smart four-column Tab/Shift+Tab indentation, and syntax highlighting via vendored [Lexilla](https://github.com/ScintillaOrg/lexilla) lexers, chosen from the file extension.
- **Right pane** — a WebView2-hosted HTML preview, refreshed shortly after edits and rendered by the vendored [cmark-gfm](https://github.com/github/cmark-gfm) library.
- A formatting toolbar above the editor for bold, italic, strikethrough, inline code, fenced code blocks, headings, blockquotes, bullet/numbered/task lists, links, images, tables, footnotes, horizontal rules, alerts, toggling the preview pane, and swapping the panel sides.
- Preview supports [Mermaid](https://mermaid.js.org/) diagrams (fenced ` ```mermaid ` blocks) with zoom/pan controls, [KaTeX](https://katex.org/) mathematics, and GFM-style tables, task lists, strikethrough, links/images, and nested lists.
- GitHub Light and GitHub Dark themes (View → Theme) apply to the editor and preview, are remembered between sessions, and the preview starts directly in the selected theme (no wrong-theme flash).

Launching with no arguments shows just the editor; launching with a file path shows just the rendered preview (`markedit.exe notes.md`).

## Features

- CommonMark plus GFM tables, task lists, strikethrough, bare-URL autolinks, and footnotes, along with code-fence language classes used by syntax-aware preview features.
- GitHub-style note, tip, important, warning, and caution alerts.
- Local syntax highlighting for named code fences in the preview, and for Markdown source in the editor, with matching light and dark themes.
- Editor syntax highlighting follows the file extension: Markdown, JSON, XML, HTML, YAML, C/C++/JavaScript/TypeScript, Python, Bash and PowerShell. Unrecognised extensions fall back to Markdown.
- Multiple document tabs with independent undo history, selections, dirty state, encoding, and paths. Tabs have close buttons, support middle-click closing, can be reopened, and are restored between sessions.
- Tabs can be reordered by dragging. Dirty and unsaved tabs are snapshotted every five seconds to a recovery area under Local AppData and can be restored after an interrupted session.
- A formatting toolbar (Bold, Italic, Strikethrough, Inline code, Code block, Headings, Blockquote, Lists, Link, Image, Table, Footnote, Horizontal rule, Alert, Toggle preview, Swap panel sides), shown while the editor pane is visible.
- Smart Markdown editing continues blockquotes and bullet, numbered, and task lists on Enter (incrementing numbered lists and resetting task items), terminates empty items, and pairs brackets and inline backticks.
- Smart four-column tabs use real tabs for leading indentation, nest a list item from anywhere on its line, and use spaces only for alignment within ordinary text.
- Line editing commands: duplicate, delete, join, and move lines up or down, plus trimming trailing whitespace across the document in a single undo step.
- Toggle Comment wraps the selected lines in an HTML comment (Markdown has no line-comment syntax) and unwraps them again.
- Format JSON and Minify JSON (Format menu) reindent the fenced code block at the caret, or the current selection. Text that cannot be read as JSON is left untouched. These re-indent rather than validate: bracket balance and string termination are checked, but grammar errors between tokens (a trailing comma, a missing colon) pass through unchanged.
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
- Markdown diagnostics find missing local links/images, broken heading links, and duplicate heading anchors. Markdown lint reports heading-spacing/level, trailing-whitespace, and inconsistent-list-marker issues; selecting a result jumps to its line.
- The table editor resizes an existing GFM table while preserving its cells, or creates a new table with a chosen number of rows and columns.
- Quick Open searches file names in a chosen folder, while Find in Folder searches supported text files and opens results at the matching line. Generated/dependency folders are skipped.
- A searchable command palette (Ctrl+Shift+P) exposes the editor's commands without navigating menus.
- Detection of files changed by another program, with an offer to reload the active document and an indicator on changed background tabs.
- Synchronized scrolling between the Markdown editor and rendered preview.
- Recently opened files (File → Recent Files), persisted across runs and pruned automatically if a file no longer exists.
- Editor font is configurable (Format → Font) and remembered between sessions.
- GitHub Light and GitHub Dark themes (View → Theme), remembered between sessions.
- An optional UTF-8 custom preview stylesheet (View → Custom Preview CSS), embedded into rendered HTML exports and remembered between sessions.
- Focus mode (View → Focus Mode or F11) hides the menu, tabs, and formatting toolbar and is remembered between sessions.
- Window position/size/maximized state, panel visibility/order, and splitter position are remembered between sessions; View → Preview on Left controls the panel order.
- Find/Replace, Go to Line, Undo/Redo, standard clipboard shortcuts. The search and replacement text, match-case setting, and search direction are remembered between sessions.
- A Markdown syntax reference (Help → Markdown Reference), opened as a live Markdown document so the preview renders every construct it documents.
- Unicode filenames, long Windows paths, drag-and-drop, and document-relative links/images.
- Failure-safe saves: content is flushed to a temporary file before atomically replacing the destination.
- Preview hardening for untrusted documents: raw HTML and dangerous URLs are disabled, Mermaid runs in strict mode, and the shell uses a restrictive content-security policy.

### Keyboard shortcuts

| Shortcut | Action |
|---|---|
| Ctrl+N | New |
| Ctrl+O | Open |
| Ctrl+K | Quick Open in Folder |
| Ctrl+S | Save |
| Ctrl+Shift+S | Save As |
| Ctrl+P | Print rendered preview |
| Ctrl+Shift+P | Command palette |
| Ctrl+W | Close current tab |
| Ctrl+Shift+T | Reopen last closed tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Ctrl+Z / Ctrl+Y | Undo / Redo |
| Ctrl+X / Ctrl+C / Ctrl+V | Cut / Copy / Paste |
| Ctrl+A | Select All |
| Ctrl+F | Find |
| Ctrl+H | Replace |
| Ctrl+G | Go to Line |
| F3 | Find Next |
| Tab / Shift+Tab | Smart indent / outdent (four-column tab stops) |
| Ctrl+D | Duplicate line |
| Ctrl+Shift+K | Delete line |
| Ctrl+J | Join lines |
| Alt+Up / Alt+Down | Move line or selection up / down |
| Ctrl+/ | Toggle comment |
| Ctrl+Shift+V | Toggle preview pane |
| Ctrl+Shift+E | Toggle editor pane |
| F11 | Toggle focus mode |
| Alt+Z | Toggle word wrap |

## Building

Requires a MinGW-w64 toolchain ([w64devkit](https://github.com/skeeto/w64devkit) or similar) with `gcc`, `g++`, and `windres` on `PATH`.

```
build.bat
```

This uses GCC size optimization, dead-section elimination, and symbol stripping; builds the vendored lexers as `Lexilla.dll`; and produces `markedit.exe` next to the WebView2/Scintilla/Lexilla runtime DLLs in the repository root.

A [premake5.lua](premake5.lua) is also provided as an alternative to `build.bat`. From Command Prompt, generate Makefiles and build the same targets with:

```bat
premake5 gmake
make -C build config=release_x64
```

Run the parser regression tests with:

```
test.bat
```

The same test and build commands run in GitHub Actions. Successful workflow
runs produce a self-contained Windows artifact.

Running `markedit.exe` also requires the [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) to be installed (present by default on current Windows 10/11).

## Distribution folder

`dist/` is the clean, unpacked release staging folder. It contains only:

- `markedit.exe` and the three required runtime DLLs: `Scintilla.dll`, `Lexilla.dll`, and `WebView2Loader.dll`;
- the preview shell, Mermaid, Highlight.js, GitHub Markdown CSS, and the complete `katex/` asset tree needed for offline rendering; and
- `LICENSE`, `THIRD_PARTY_NOTICES.md`, and the referenced component licence/version files under `third_party/`.

Do not package `markedit.ini`, `webview2_data/`, debug symbols, import libraries, compiler objects, or temporary archives. The settings file and WebView2 browser profile are user-specific and are created on first run. The Inno Setup and GitHub Actions manifests mirror this runtime-and-licences layout.

## Project layout

```
markedit.c              application source
markdown.c / .h         cmark-gfm rendering adapter
cmark-gfm-sources.rsp   vendored renderer source list and compile flags
markedit.rc / .ico       Win32 resources (icon, version info)
markedit.manifest        DPI-awareness and long-path declarations
preview_shell.html       HTML/JS shell hosting the WebView2 preview + Mermaid
github-markdown.css      GitHub-flavored preview stylesheet (light + dark)
mermaid.min.js           bundled Mermaid renderer
highlight*.js / .css     bundled Highlight.js renderer and light/dark themes
katex/                    bundled KaTeX renderer, stylesheet, and fonts
make_icon.ps1            regenerates markedit.ico
build.bat                size-optimized build script (GCC / G++ / windres)
premake5.lua             alternative Makefile generator for the same build
markedit.iss             Inno Setup installer script
dist/                    clean unpacked release staging folder (generated, not tracked)
test.bat / tests/        parser regression tests
third_party/webview2/    WebView2 headers + import libs
third_party/scintilla/   Scintilla headers, licence, and built Scintilla.dll
third_party/lexilla/     Lexilla 5.5.1 lexer sources and licence, trimmed to the supported languages
third_party/cmark-gfm/   cmark-gfm 0.29.0.gfm.13 source and BSD-2-Clause licence
third_party/highlightjs/ Highlight.js 11.11.1 licence and provenance
third_party/katex/       KaTeX 0.17.0 licence and provenance
```

## GitHub-style preview

The preview uses GitHub-flavored CSS (`github-markdown.css`, checked in next to `preview_shell.html`) for a more authentic look. Its light and dark variants are synchronized with the matching GitHub editor palette. Mermaid, Highlight.js, and KaTeX assets are also local, so rendering works offline and does not silently fetch executable content, styles, or fonts from a CDN.

Build outputs such as `markedit.exe` and `Lexilla.dll`, the assembled `dist/` folder, `markedit.ini` (saved settings), and `webview2_data/` (WebView2's browser profile/cache) are generated locally and are not tracked in git.

## Security and Markdown scope

MarkEdit uses cmark-gfm without its unsafe-rendering option, so raw HTML is
omitted and dangerous URL schemes are scrubbed rather than executed in the
preview. KaTeX runs with trusted commands disabled and bounded expansion and
output sizing. Links using HTTP, HTTPS, and `mailto:` open in the system handler;
relative links resolve from the Markdown document's directory.

## Licence

MarkEdit is available under the MIT License. See `LICENSE` and
`THIRD_PARTY_NOTICES.md` for bundled-component licences and provenance.
