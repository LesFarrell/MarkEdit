# MarkEdit

A tiny, native Win32 Markdown editor with live HTML preview.

- **Left pane** — a [Scintilla](https://www.scintilla.org/) source editor for your Markdown, with line numbers, current-line highlighting, and real Tab/Shift+Tab block indent.
- **Right pane** — a WebView2-hosted HTML preview, re-rendered on every keystroke from a small built-in Markdown-to-HTML converter (no external Markdown library).
- Preview supports [Mermaid](https://mermaid.js.org/) diagrams (fenced ` ```mermaid ` blocks) with zoom/pan controls, and GFM-style tables, task lists, strikethrough, links/images, and nested lists.

Launching with no arguments shows just the editor; launching with a file path shows just the rendered preview (`markedit.exe notes.md`).

## Features

- Headings, emphasis (bold/italic/strikethrough), inline code, code fences, blockquotes, ordered/unordered/nested lists, task list checkboxes, tables, horizontal rules, links, images, and autolinks.
- Mermaid diagram rendering with a zoom/pan toolbar.
- Recently opened files (File → Recent Files), persisted across runs and pruned automatically if a file no longer exists.
- Editor font is configurable (Format → Font) and remembered between sessions.
- Find/Replace, Undo/Redo, standard clipboard shortcuts.
- A plain-text Markdown syntax reference (Help → Markdown Reference).

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

Requires the [zig](https://ziglang.org/) toolchain (used as the C/C++ compiler, linker, and resource compiler).

```
build.bat
```

This compiles `markedit.c`, links against the WebView2 loader and Scintilla, and copies the runtime DLLs (`WebView2Loader.dll`, `Scintilla.dll`) next to `markedit.exe`.

Running `markedit.exe` also requires the [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) to be installed (present by default on current Windows 10/11).

## Project layout

```
markedit.c              application source
markedit.rc / .ico       Win32 resources (icon, version info)
preview_shell.html       HTML/JS shell hosting the WebView2 preview + Mermaid
mermaid.min.js           bundled Mermaid renderer
build.bat                build script
third_party/webview2/    WebView2 headers + import libs
third_party/scintilla/   Scintilla editor component (source + built Scintilla.dll)
```

Files such as `markedit.exe`, `markedit.ini` (saved settings), and `webview2_data/` (WebView2's browser profile/cache) are generated at build or run time and are not tracked in git.

## To do / known gaps

- Line breaks via trailing double-space or backslash aren't rendered.
- Loose vs. tight list paragraph spacing isn't distinguished.
- Raw HTML embedded in Markdown source is escaped rather than passed through.
"# MarkEdit" 
