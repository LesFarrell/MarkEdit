# MarkEdit

A tiny, native Win32 Markdown editor with live HTML preview.

- **Left pane** — a [Scintilla](https://www.scintilla.org/) source editor for your Markdown, with line numbers, current-line highlighting, and real Tab/Shift+Tab block indent.
- **Right pane** — a WebView2-hosted HTML preview, refreshed shortly after edits and rendered by the vendored [cmark-gfm](https://github.com/github/cmark-gfm) library.
- Preview supports [Mermaid](https://mermaid.js.org/) diagrams (fenced ` ```mermaid ` blocks) with zoom/pan controls, [KaTeX](https://katex.org/) mathematics, and GFM-style tables, task lists, strikethrough, links/images, and nested lists.
- GitHub Light and GitHub Dark themes (View → Theme) apply to both the editor and preview, are remembered between sessions, and the preview starts directly in the selected theme (no wrong-theme flash).

Launching with no arguments shows just the editor; launching with a file path shows just the rendered preview (`markedit.exe notes.md`).

## Features

- CommonMark plus GFM tables, task lists, strikethrough, bare-URL autolinks, and footnotes, along with code-fence language classes used by syntax-aware preview features.
- GitHub-style note, tip, important, warning, and caution alerts.
- Local syntax highlighting for named code fences, with matching light and dark themes.
- Stable heading permalinks and a floating document outline for documents with multiple headings.
- Inline (`$...$`) and display (`$$...$$` or fenced `math`) mathematical expressions.
- YAML front matter delimited by `---` (and closed by `---` or `...`) is treated as document metadata and omitted from the preview.
- Mermaid diagram rendering with a zoom/pan toolbar.
- Recently opened files (File → Recent Files), persisted across runs and pruned automatically if a file no longer exists.
- Editor font is configurable (Format → Font) and remembered between sessions.
- GitHub Light and GitHub Dark themes (View → Theme), remembered between sessions.
- Find/Replace, Undo/Redo, standard clipboard shortcuts.
- A plain-text Markdown syntax reference (Help → Markdown Reference).
- Unicode filenames, long Windows paths, drag-and-drop, and document-relative links/images.
- Failure-safe saves: content is flushed to a temporary file before atomically replacing the destination.
- Preview hardening for untrusted documents: raw HTML and dangerous URLs are disabled, Mermaid runs in strict mode, and the shell uses a restrictive content-security policy.

### Keyboard shortcuts

| Shortcut | Action |
|---|---|
| Ctrl+N | New |
| Ctrl+O | Open |
| Ctrl+S | Save |
| Ctrl+Shift+S | Save As |
| Ctrl+Z / Ctrl+Y | Undo / Redo |
| Ctrl+X / Ctrl+C / Ctrl+V | Cut / Copy / Paste |
| Ctrl+A | Select All |
| Ctrl+F | Find |
| Ctrl+H | Replace |
| F3 | Find Next |
| Tab / Shift+Tab | Indent / outdent line or selection |
| Ctrl+Shift+V | Toggle preview pane |
| Ctrl+Shift+E | Toggle editor pane |

## Building

Requires a MinGW toolchain ([w64devkit](https://github.com/skeeto/w64devkit) or similar) with `gcc` and `windres` on `PATH`.

```
build.bat
```

This compiles `markedit.rc` with `windres`, compiles/links the application and vendored cmark-gfm sources with `gcc`, and copies the runtime DLLs (`WebView2Loader.dll`, `Scintilla.dll`) next to `markedit.exe`.

Run the parser regression tests with:

```
test.bat
```

The same test and build commands run in GitHub Actions. Successful workflow
runs produce a self-contained Windows artifact.

Running `markedit.exe` also requires the [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) to be installed (present by default on current Windows 10/11).

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
build.bat                build script
test.bat / tests/        parser regression tests
third_party/webview2/    WebView2 headers + import libs
third_party/scintilla/   Scintilla headers, licence, and built Scintilla.dll
third_party/cmark-gfm/   cmark-gfm 0.29.0.gfm.13 source and BSD-2-Clause licence
third_party/highlightjs/ Highlight.js 11.11.1 licence and provenance
third_party/katex/       KaTeX 0.17.0 licence and provenance
```

## GitHub-style preview

The preview uses GitHub-flavored CSS (`github-markdown.css`, checked in next to `preview_shell.html`) for a more authentic look. Its light and dark variants are synchronized with the matching GitHub editor palette. Mermaid, Highlight.js, and KaTeX assets are also local, so rendering works offline and does not silently fetch executable content, styles, or fonts from a CDN.

Files such as `markedit.exe`, `markedit.ini` (saved settings), and `webview2_data/` (WebView2's browser profile/cache) are generated at build or run time and are not tracked in git.

## Security and Markdown scope

MarkEdit uses cmark-gfm without its unsafe-rendering option, so raw HTML is
omitted and dangerous URL schemes are scrubbed rather than executed in the
preview. KaTeX runs with trusted commands disabled and bounded expansion and
output sizing. Links using HTTP, HTTPS, and `mailto:` open in the system handler;
relative links resolve from the Markdown document's directory.

## Licence

MarkEdit is available under the MIT License. See `LICENSE` and
`THIRD_PARTY_NOTICES.md` for bundled-component licences and provenance.
