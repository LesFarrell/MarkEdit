-- Premake5 script for MarkEdit.
-- Alternative to the Zig-based build.bat / test.bat scripts, using a native
-- MinGW-w64 GCC toolchain.
-- Generate Makefiles with:  premake5 gmake
-- Then build with:          make config=release_x64
-- (builds markedit.exe, Lexilla.dll, and test_parser).
--
-- premake5 --help for other actions (e.g. `premake5 vs2022`), but this
-- project's headers/libs are laid out for MinGW-w64-compatible toolchains, so
-- gmake is the supported Premake action.

workspace "MarkEdit"
    configurations { "Release" }
    platforms { "x64" }
    location "build"

    filter "platforms:x64"
        architecture "x86_64"

    filter "toolset:gcc"
        buildoptions {
            "-ffunction-sections",
            "-fdata-sections",
            "-fno-asynchronous-unwind-tables",
            "-fno-unwind-tables",
            "-fno-ident",
            "-fmerge-all-constants",
        }
        linkoptions { "-Wl,--gc-sections", "-Wl,--strip-all", "-Wl,--build-id=none" }
    filter {}

CMARK_GFM_DIR = "third_party/cmark-gfm"
WEBVIEW2_DIR = "third_party/webview2"
SCINTILLA_DIR = "third_party/scintilla"
LEXILLA_DIR = "third_party/lexilla"

CMARK_GFM_SOURCES = {
    CMARK_GFM_DIR.."/src/arena.c",
    CMARK_GFM_DIR.."/src/blocks.c",
    CMARK_GFM_DIR.."/src/buffer.c",
    CMARK_GFM_DIR.."/src/cmark.c",
    CMARK_GFM_DIR.."/src/cmark_ctype.c",
    CMARK_GFM_DIR.."/src/commonmark.c",
    CMARK_GFM_DIR.."/src/footnotes.c",
    CMARK_GFM_DIR.."/src/houdini_href_e.c",
    CMARK_GFM_DIR.."/src/houdini_html_e.c",
    CMARK_GFM_DIR.."/src/houdini_html_u.c",
    CMARK_GFM_DIR.."/src/html.c",
    CMARK_GFM_DIR.."/src/inlines.c",
    CMARK_GFM_DIR.."/src/iterator.c",
    CMARK_GFM_DIR.."/src/latex.c",
    CMARK_GFM_DIR.."/src/linked_list.c",
    CMARK_GFM_DIR.."/src/man.c",
    CMARK_GFM_DIR.."/src/map.c",
    CMARK_GFM_DIR.."/src/node.c",
    CMARK_GFM_DIR.."/src/plaintext.c",
    CMARK_GFM_DIR.."/src/plugin.c",
    CMARK_GFM_DIR.."/src/references.c",
    CMARK_GFM_DIR.."/src/registry.c",
    CMARK_GFM_DIR.."/src/render.c",
    CMARK_GFM_DIR.."/src/scanners.c",
    CMARK_GFM_DIR.."/src/syntax_extension.c",
    CMARK_GFM_DIR.."/src/utf8.c",
    CMARK_GFM_DIR.."/src/xml.c",
    CMARK_GFM_DIR.."/extensions/autolink.c",
    CMARK_GFM_DIR.."/extensions/core-extensions.c",
    CMARK_GFM_DIR.."/extensions/ext_scanners.c",
    CMARK_GFM_DIR.."/extensions/strikethrough.c",
    CMARK_GFM_DIR.."/extensions/table.c",
    CMARK_GFM_DIR.."/extensions/tagfilter.c",
    CMARK_GFM_DIR.."/extensions/tasklist.c",
}

function cmark_gfm_settings()
    files(CMARK_GFM_SOURCES)
    includedirs { CMARK_GFM_DIR.."/src", CMARK_GFM_DIR.."/extensions" }
    defines { "CMARK_GFM_STATIC_DEFINE", "CMARK_GFM_EXTENSIONS_STATIC_DEFINE" }
    filter "toolset:gcc"
        buildoptions { "-Wno-unused-parameter" }
    filter {}
end

-- Trimmed down from Lexilla's full catalogue to the languages supported by
-- MarkEdit; keep this list aligned with lexilla-sources.rsp.
project "lexilla"
    kind "SharedLib"
    language "C++"
    cppdialect "C++17"
    targetname "Lexilla"
    targetdir "."
    objdir "build/obj/lexilla"

    files {
        LEXILLA_DIR.."/src/Lexilla.cxx",
        LEXILLA_DIR.."/lexers/LexMarkdown.cxx",
        LEXILLA_DIR.."/lexers/LexJSON.cxx",
        LEXILLA_DIR.."/lexers/LexHTML.cxx",
        LEXILLA_DIR.."/lexers/LexYAML.cxx",
        LEXILLA_DIR.."/lexers/LexCPP.cxx",
        LEXILLA_DIR.."/lexers/LexPython.cxx",
        LEXILLA_DIR.."/lexers/LexBash.cxx",
        LEXILLA_DIR.."/lexers/LexPowerShell.cxx",
        LEXILLA_DIR.."/lexers/LexLua.cxx",
        LEXILLA_DIR.."/lexlib/Accessor.cxx",
        LEXILLA_DIR.."/lexlib/CharacterCategory.cxx",
        LEXILLA_DIR.."/lexlib/CharacterSet.cxx",
        LEXILLA_DIR.."/lexlib/DefaultLexer.cxx",
        LEXILLA_DIR.."/lexlib/InList.cxx",
        LEXILLA_DIR.."/lexlib/LexAccessor.cxx",
        LEXILLA_DIR.."/lexlib/LexerBase.cxx",
        LEXILLA_DIR.."/lexlib/LexerModule.cxx",
        LEXILLA_DIR.."/lexlib/LexerSimple.cxx",
        LEXILLA_DIR.."/lexlib/PropSetSimple.cxx",
        LEXILLA_DIR.."/lexlib/StyleContext.cxx",
        LEXILLA_DIR.."/lexlib/WordList.cxx",
    }
    includedirs { LEXILLA_DIR.."/include", LEXILLA_DIR.."/lexlib", SCINTILLA_DIR.."/include" }
    defines { "NDEBUG" }
    optimize "Size"

    filter "toolset:gcc"
        buildoptions { "-fno-exceptions", "-fno-rtti" }
    filter {}

project "markedit"
    kind "WindowedApp"
    language "C"
    cdialect "C11"
    targetname "markedit"
    targetdir "."
    objdir "build/obj/markedit"

    files {
        "markedit.c",
        "path_utils.c", "path_utils.h",
        "text_codec.c", "text_codec.h",
        "json_formatter.c", "json_formatter.h",
        "markdown.c", "markdown.h",
        "settings.c", "settings.h",
        "help_reference.c", "help_reference.h",
        "file_explorer.c", "file_explorer.h",
        "markedit.rc",
    }
    cmark_gfm_settings()

    includedirs { "." }
    -- windres (used to compile markedit.rc) doesn't understand gcc's -isystem,
    -- which externalincludedirs would otherwise add to its command line too;
    -- scope it to the C sources only.
    filter "files:not **.rc"
        externalincludedirs { WEBVIEW2_DIR.."/include", SCINTILLA_DIR.."/include", LEXILLA_DIR.."/include" }
    filter {}
    libdirs { WEBVIEW2_DIR.."/x64", "." }
        links { "gdi32", "user32", "comdlg32", "comctl32", "shlwapi", "shell32", "ole32", "windowscodecs", "uxtheme", "dwmapi", "Lexilla" }
    dependson { "lexilla" }

    warnings "Extra"
    optimize "Size"

    filter "toolset:gcc"
        buildoptions { "-Wno-unknown-pragmas" }
        -- This vendor import library is named WebView2Loader.dll.lib rather
        -- than libWebView2Loader.a, so pass its path directly to GNU ld.
        linkoptions { "../"..WEBVIEW2_DIR.."/x64/WebView2Loader.dll.lib" }
    filter {}

    -- {COPY}'s generated `IF EXIST (...) ELSE (...)` batch syntax only runs
    -- under cmd.exe; this project's gmake2 build runs under the MinGW/
    -- w64devkit sh, so copy with a plain POSIX cp instead. Postbuild commands
    -- run with cwd = the gmake2 build/ directory (not targetdir), so both the
    -- source (vendor dir) and destination (repo root, alongside markedit.exe)
    -- are given relative to build/.
    --
    -- The remaining commands mirror build.bat's dist/ staging (same file
    -- list, same third_party/<component> licence/version layout) so this
    -- build path also produces a complete, ready-to-run folder instead of
    -- leaving binaries and sources mixed together in the repo root. Unlike
    -- build.bat, this doesn't wipe dist/ first - mkdir -p/cp -f only add or
    -- overwrite, so a stale file from a since-removed dependency could
    -- linger; that tradeoff matches this script's role as the lighter-weight
    -- alternative to the primary Zig/build.bat release path.
    postbuildcommands {
        "cp -f ../"..WEBVIEW2_DIR.."/x64/WebView2Loader.dll ..",
        "cp -f ../"..SCINTILLA_DIR.."/bin/Scintilla.dll ..",
        "mkdir -p ../dist/katex ../dist/third_party/cmark-gfm ../dist/third_party/highlightjs "..
            "../dist/third_party/katex ../dist/third_party/lexilla ../dist/third_party/scintilla "..
            "../dist/third_party/webview2",
        "cp -f ../markedit.exe ../Lexilla.dll ../Scintilla.dll ../WebView2Loader.dll "..
            "../preview_shell.html ../github-markdown.css ../mermaid.min.js ../highlight.min.js "..
            "../highlight-powershell.min.js ../highlight-github.min.css ../highlight-github-dark.min.css "..
            "../LICENSE ../THIRD_PARTY_NOTICES.md ../dist/",
        "cp -rf ../katex/. ../dist/katex/",
        "cp -f ../"..CMARK_GFM_DIR.."/COPYING ../dist/third_party/cmark-gfm/",
        "cp -f ../third_party/highlightjs/LICENSE ../third_party/highlightjs/VERSION.txt ../dist/third_party/highlightjs/",
        "cp -f ../third_party/katex/LICENSE ../third_party/katex/VERSION.txt ../dist/third_party/katex/",
        "cp -f ../"..LEXILLA_DIR.."/License.txt ../"..LEXILLA_DIR.."/version.txt ../dist/third_party/lexilla/",
        "cp -f ../"..SCINTILLA_DIR.."/License.txt ../"..SCINTILLA_DIR.."/version.txt ../dist/third_party/scintilla/",
        "cp -f ../"..WEBVIEW2_DIR.."/LICENSE.txt ../dist/third_party/webview2/",
    }

project "test_parser"
    kind "ConsoleApp"
    language "C"
    cdialect "C11"
    targetname "test_parser"
    targetdir "tests"
    objdir "build/obj/test_parser"

    files { "tests/test_parser.c", "markdown.c" }
    cmark_gfm_settings()

    includedirs { "." }
    warnings "Extra"
    optimize "Size"

    filter "toolset:gcc"
        buildoptions { "-Werror" }
    filter {}
