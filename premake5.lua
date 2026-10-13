-- Premake5 script for MarkEdit.
-- Alternative to the Zig-based build.bat / test.bat scripts, for three
-- toolchains (builds markedit.exe, statically linking Lexilla, and the
-- test programs, then stages dist/ the same way build.bat does):
--
--   MinGW-w64 GCC:  premake5 gmake          then  make -C build config=release_x64
--   Zig:            premake5 --zig gmake    then  make -C build config=release_x64
--   MSVC:           premake5 vs2022         then  build build/MarkEdit.sln
--                   (or any later vs20xx action Premake supports)
--
-- The gmake builds need GNU make running its recipes under a POSIX sh (as
-- w64devkit or MSYS2 provide): the post-build staging uses cp/mkdir. --zig
-- needs only zig on PATH besides make - zig cc / zig c++ / zig ar / zig rc
-- stand in for the whole GCC toolchain, including windres.

newoption {
    trigger = "zig",
    description = "With gmake: compile and link with zig cc / zig c++ instead of GCC",
}

workspace "MarkEdit"
    configurations { "Release" }
    platforms { "x64" }
    location "build"

    filter "platforms:x64"
        architecture "x86_64"

    -- Premake has no Zig toolset; its Clang one generates the right flags
    -- (zig cc is Clang), and these override the compiler commands it names.
    -- `override` beats both the makefile's own defaults and the make command
    -- line, so the build can't silently fall back to a system clang/gcc.
    filter "options:zig"
        toolset "clang"
        makesettings {
            "override CC = zig cc",
            "override CXX = zig c++",
            "override AR = zig ar",
        }

    filter "toolset:gcc or clang"
        buildoptions {
            "-ffunction-sections",
            "-fdata-sections",
            -- Lua errors use longjmp; x64 Windows requires valid unwind tables
            -- while that jump crosses plugin-host frames.
            "-funwind-tables",
            "-fasynchronous-unwind-tables",
            "-fno-ident",
            "-fmerge-all-constants",
        }
        linkoptions { "-Wl,--gc-sections", "-Wl,--strip-all", "-Wl,--build-id=none" }

    filter "toolset:msc"
        -- The sources are UTF-8 (wide literals such as L"—"); without
        -- /utf-8, MSVC reads them in the system code page and miscompiles
        -- every non-ASCII literal.
        buildoptions { "/utf-8" }
        defines { "_CRT_SECURE_NO_WARNINGS" }
        -- The GCC/Zig builds don't define UNICODE either; WinMain and the
        -- ...A/...W API calls are written for that.
        characterset "ASCII"
    filter {}

CMARK_GFM_DIR = "third_party/cmark-gfm"
WEBVIEW2_DIR = "third_party/webview2"
SCINTILLA_DIR = "third_party/scintilla"
LEXILLA_DIR = "third_party/lexilla"
LUA_DIR = "third_party/lua"

-- The compiled translation units live in the repo-root *.rsp response files
-- that build.bat hands to the compiler. Reading them here keeps Premake and
-- the batch build in step; the .rsp also carry -D/-I/-W flags, which are
-- filtered out (only .c/.cc/.cpp/.cxx entries are returned). This is the one
-- place a compiled-source list is read, so a new file only has to be added to
-- its .rsp.
function sources_from_rsp(path)
    local sources = {}
    for line in io.lines(path) do
        local entry = line:match("^%s*(.-)%s*$")
        if entry:match("%.[cC]$") or entry:match("%.[cC][xX][xX]$") or
           entry:match("%.[cC][cC]$") or entry:match("%.[cC][pP][pP]$") then
            table.insert(sources, entry)
        end
    end
    return sources
end

CMARK_GFM_SOURCES = sources_from_rsp("cmark-gfm-sources.rsp")

function cmark_gfm_settings()
    files(CMARK_GFM_SOURCES)
    includedirs { CMARK_GFM_DIR.."/src", CMARK_GFM_DIR.."/extensions" }
    defines { "CMARK_GFM_STATIC_DEFINE", "CMARK_GFM_EXTENSIONS_STATIC_DEFINE" }
    filter "toolset:gcc or clang"
        buildoptions { "-Wno-unused-parameter" }
    filter "toolset:msc"
        disablewarnings { "4100" } -- unreferenced parameter, as -Wno-unused-parameter
    filter {}
end

-- The Lua core plus the libraries plugins get; keep this list aligned with
-- lua-sources.rsp (third_party/lua/VERSION.txt says why io/os/package/debug
-- are left out).
LUA_SOURCES = sources_from_rsp("lua-sources.rsp")

function lua_settings()
    files(LUA_SOURCES)
    includedirs { LUA_DIR.."/src" }
end

-- ---- dist/ staging, mirroring build.bat (same file list, same
-- third_party/<component> licence/version layout), so every build path
-- produces a complete, ready-to-run folder. Unlike build.bat this doesn't
-- wipe dist/ first - it only adds or overwrites, so a stale file from a
-- since-removed dependency could linger; that tradeoff matches this
-- script's role as the lighter-weight alternative to build.bat.
-- All paths are relative to the repo root.
DIST_FILES = {
    "markedit.exe", "Scintilla.dll", "WebView2Loader.dll",
    "preview_shell.html", "github-markdown.css", "mermaid.min.js", "highlight.min.js",
    "highlight-powershell.min.js", "highlight-github.min.css", "highlight-github-dark.min.css",
    "LICENSE", "THIRD_PARTY_NOTICES.md",
}
DIST_LICENCES = {
    ["cmark-gfm"]   = { CMARK_GFM_DIR.."/COPYING" },
    ["gdb"]         = {
        "third_party/gdb/COPYING.txt", "third_party/gdb/COPYING.MinGW-w64-runtime.txt",
        "third_party/gdb/VERSION.txt", "third_party/gdb/SOURCE.md", "third_party/gdb/SHA256SUMS.txt",
    },
    ["highlightjs"] = { "third_party/highlightjs/LICENSE", "third_party/highlightjs/VERSION.txt" },
    ["katex"]       = { "third_party/katex/LICENSE", "third_party/katex/VERSION.txt" },
    ["lexilla"]     = { LEXILLA_DIR.."/License.txt", LEXILLA_DIR.."/version.txt" },
    ["lua"]         = { LUA_DIR.."/LICENSE", LUA_DIR.."/VERSION.txt" },
    ["scintilla"]   = { SCINTILLA_DIR.."/License.txt", SCINTILLA_DIR.."/version.txt" },
    ["webview2"]    = { WEBVIEW2_DIR.."/LICENSE.txt" },
}
-- The two runtime DLLs are copied next to markedit.exe first, so the build
-- runs in place as well as from dist/.
RUNTIME_DLLS = { WEBVIEW2_DIR.."/x64/WebView2Loader.dll", SCINTILLA_DIR.."/bin/Scintilla.dll" }

-- Post-build commands run with cwd = the build/ directory (where both the
-- makefiles and the Visual Studio projects live), so every repo path is
-- prefixed with "../". {COPYFILE} and friends aren't used: their expansion
-- follows the host shell, which for gmake on Windows is cmd syntax even when
-- make runs its recipes under sh. Each generator is spelled out instead.
local function staging_commands(posix)
    local commands = {}
    local function at(path)
        path = "../"..path
        return posix and path or path:gsub("/", "\\")
    end
    local function copy(source, destination)
        if posix then
            table.insert(commands, "cp -f "..at(source).." "..at(destination))
        else
            table.insert(commands, 'copy /Y "'..at(source)..'" "'..at(destination)..'" >nul')
        end
    end
    local function mkdir(path)
        if posix then
            table.insert(commands, "mkdir -p "..at(path))
        else
            table.insert(commands, 'if not exist "'..at(path)..'" mkdir "'..at(path)..'"')
        end
    end

    for _, dll in ipairs(RUNTIME_DLLS) do copy(dll, "") end
    mkdir("dist/katex")
    mkdir("dist/plugins")
    for _, file in ipairs(DIST_FILES) do copy(file, "dist/") end
    if posix then
        table.insert(commands, "cp -rf "..at("katex").."/. "..at("dist/katex").."/")
        table.insert(commands, "cp -rf "..at("plugins").."/. "..at("dist/plugins").."/")
    else
        table.insert(commands, 'xcopy /E /I /Y /Q "'..at("katex")..'" "'..at("dist/katex")..'" >nul')
        table.insert(commands, 'xcopy /E /I /Y /Q "'..at("plugins")..'" "'..at("dist/plugins")..'" >nul')
    end
    mkdir("dist/third_party/gdb/bin")
    copy("third_party/gdb/bin/gdb.exe", "dist/third_party/gdb/bin/")
    local components = {}
    for component in pairs(DIST_LICENCES) do table.insert(components, component) end
    table.sort(components) -- a stable command order from run to run
    for _, component in ipairs(components) do
        mkdir("dist/third_party/"..component)
        for _, licence in ipairs(DIST_LICENCES[component]) do copy(licence, "dist/third_party/"..component.."/") end
    end
    return commands
end

-- Trimmed down from Lexilla's full catalogue to the languages supported by
-- MarkEdit; keep this list aligned with lexilla-sources.rsp.
-- A static lib rather than a SharedLib: linked straight into markedit.exe
-- instead of shipped as its own Lexilla.dll (see the markedit project below).
project "lexilla"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    targetname "Lexilla"
    objdir "build/obj/lexilla"

    files(sources_from_rsp("lexilla-sources.rsp"))
    includedirs { LEXILLA_DIR.."/include", LEXILLA_DIR.."/lexlib", SCINTILLA_DIR.."/include" }
    defines { "NDEBUG" }
    optimize "Size"

    filter "toolset:gcc or clang"
        buildoptions { "-fno-exceptions", "-fno-rtti" }
    filter {}

project "markedit"
    kind "WindowedApp"
    language "C"
    cdialect "C11"
    targetname "markedit"
    targetdir "."
    objdir "build/obj/markedit"

    files(sources_from_rsp("markedit-sources.rsp"))
    files {
        "src/internal/preview.inc", "src/internal/documents.inc", "src/internal/dialogs.inc",
        "src/internal/help_and_menu.inc", "src/internal/ui_shell.inc", "src/internal/debugger.inc",
        "src/internal/outline_host.inc", "src/internal/editor_input.inc",
        "src/internal/workspace.inc",
        "src/internal/settings_dialog.inc", "src/internal/command_dispatch.inc",
        "src/internal/plugin_bridge.inc", "src/internal/control_theme.inc",
        "src/editor_commands.h",
        "src/editor_language.h",
        "src/preview_fallback.h",
        "src/path_utils.h",
        "src/file_io.h",
        "src/recovery.h",
        "src/text_codec.h",
        "src/workspace_text.h",
        "src/markdown.h",
        "src/settings.h", "src/settings_values.h", "src/settings_io.h", "src/find_state.h",
        "src/recent_list.h",
        "src/layout_metrics.h", "src/file_explorer.h",
        "src/toolbar_icons.h",
        "src/themed_scrollbar.h",
        "src/plugin_host.h",
        "src/plugin_settings.h",
        "src/lua_editor_api.h",
        "src/outline_minimap.h",
        "src/progress_dialog.h",
        "src/scintilla_config.h",
        "src/menu_commands.h", "src/command_ids.h",
        "src/menus.lua", "src/lua/formatting_base.lua",
        "src/lua/formatting_tables.lua", "src/lua/formatting_text.lua",
        "src/help/markdown-reference.md", "src/help/keyboard-shortcuts.lua",
        "src/theme.h",
        "markedit.rc",
    }
    cmark_gfm_settings()
    lua_settings()

    includedirs { "." }
    -- windres (used to compile markedit.rc) doesn't understand gcc's -isystem,
    -- which externalincludedirs would otherwise add to its command line too;
    -- scope it to the C sources only.
    filter "files:not **.rc"
        externalincludedirs { WEBVIEW2_DIR.."/include", SCINTILLA_DIR.."/include", LEXILLA_DIR.."/include" }
    filter {}
    libdirs { WEBVIEW2_DIR.."/x64" }
    -- The project name, not its "Lexilla" target name, so Premake treats it as
    -- the sibling project above: it links that project's output wherever it
    -- was built and orders the build after it, for both make and Visual Studio.
    links { "lexilla", "gdi32", "user32", "comdlg32", "comctl32", "shlwapi", "shell32", "ole32", "mpr", "windowscodecs", "uxtheme", "dwmapi" }

    warnings "Extra"
    optimize "Size"

    filter "toolset:gcc or clang"
        buildoptions { "-Wno-unknown-pragmas" }
        -- WebView2Loader.dll.lib rather than WebView2LoaderStatic.lib: the
        -- static loader is MSVC-ABI-only (MSVC name mangling, /GS stack-
        -- cookie symbols, MSVC CRT thread-init guards) and can't be linked
        -- into this GNU/MinGW toolchain's output - the dynamic loader's DLL
        -- boundary is plain C, so it's the only ABI-compatible option here.
        -- This vendor import library is named WebView2Loader.dll.lib rather
        -- than libWebView2Loader.a, so pass its path directly to the linker.
        linkoptions { "../"..WEBVIEW2_DIR.."/x64/WebView2Loader.dll.lib" }
        postbuildcommands(staging_commands(true))
    -- Lexilla is C++, but this project links with the C driver; name each
    -- toolchain's C++ runtime explicitly.
    filter "toolset:gcc"
        links { "stdc++" }
    filter "options:zig"
        links { "c++" }
        -- Zig has no windres for the gmake resource rule; zig rc compiles
        -- the script to a .res that lld links directly (as build.bat does).
        removefiles { "markedit.rc" }
        prebuildcommands { "zig rc ../markedit.rc markedit.res" }
        linkoptions { "markedit.res" }

    filter "toolset:msc"
        -- The same DLL-backed loader as the GCC/Zig builds, so every
        -- toolchain ships the same WebView2Loader.dll in dist/.
        links { "WebView2Loader.dll.lib" }
        -- markedit.rc already embeds markedit.manifest; a linker-generated
        -- manifest as well is a duplicate-resource link error.
        flags { "NoManifest" }
        -- Function/data pointer casts are how the WebView2 COM handler
        -- vtables are filled in from C.
        disablewarnings { "4152" }
        postbuildcommands(staging_commands(false))
    filter {}

project "test_parser"
    kind "ConsoleApp"
    language "C"
    cdialect "C11"
    targetname "test_parser"
    targetdir "tests"
    objdir "build/obj/test_parser"

    files { "tests/test_parser.c", "src/markdown.c" }
    cmark_gfm_settings()

    includedirs { "." }
    warnings "Extra"
    optimize "Size"

    filter "toolset:gcc or clang"
        buildoptions { "-Werror" }
    filter "toolset:msc"
        buildoptions { "/WX" }
    filter {}

project "test_editor_commands"
    kind "ConsoleApp"
    language "C"
    cdialect "C11"
    targetname "test_editor_commands"
    targetdir "tests"
    objdir "build/obj/test_editor_commands"
    files { "tests/test_editor_commands.c", "src/editor_commands.c", "src/editor_commands.h" }
    includedirs { "." }
    links { "user32" }
    warnings "Extra"
    optimize "Size"
    filter "toolset:gcc or clang"
        buildoptions { "-Werror" }
    filter "toolset:msc"
        buildoptions { "/WX" }
    filter {}

project "test_core"
    kind "ConsoleApp"
    language "C"
    cdialect "C11"
    targetname "test_core"
    targetdir "tests"
    objdir "build/obj/test_core"
    files { "tests/test_core.c", "src/text_codec.c", "src/workspace_text.c", "src/file_io.c", "src/path_utils.c", "src/plugin_settings.c", "src/plugin_settings.h" }
    includedirs { "." }
    warnings "Extra"
    optimize "Size"
    filter "toolset:gcc or clang"
        buildoptions { "-Werror" }
    filter "toolset:msc"
        buildoptions { "/WX" }
    filter {}

project "test_recovery"
    kind "ConsoleApp"
    language "C"
    cdialect "C11"
    targetname "test_recovery"
    targetdir "tests"
    objdir "build/obj/test_recovery"
    files { "tests/test_recovery.c", "src/recovery.c", "src/file_io.c", "src/text_codec.c", "src/path_utils.c" }
    includedirs { "." }
    links { "ole32" }
    warnings "Extra"
    optimize "Size"
    filter "toolset:gcc or clang"
        buildoptions { "-Werror" }
    filter "toolset:msc"
        buildoptions { "/WX" }
    filter {}

project "test_plugin_host"
    kind "ConsoleApp"
    language "C"
    cdialect "C11"
    targetname "test_plugin_host"
    targetdir "tests"
    objdir "build/obj/test_plugin_host"
    files { "tests/test_plugin_host.c", "src/plugin_host.h", "src/lua_editor_api.h" }
    files(sources_from_rsp("plugin-sources.rsp"))
    lua_settings()
    includedirs { "." }
    links { "user32" }
    warnings "Extra"
    optimize "Size"
    filter "toolset:gcc or clang"
        buildoptions { "-Werror" }
    filter "toolset:msc"
        buildoptions { "/WX" }
    filter {}

-- Run from the repository root: it reads src/menus.lua.
project "test_menus"
    kind "ConsoleApp"
    language "C"
    cdialect "C11"
    targetname "test_menus"
    targetdir "tests"
    objdir "build/obj/test_menus"
    files { "tests/test_menus.c", "src/menu_commands.c", "src/menu_commands.h", "src/plugin_host.h", "src/lua_editor_api.h" }
    files(sources_from_rsp("plugin-sources.rsp"))
    lua_settings()
    includedirs { "." }
    links { "user32" }
    warnings "Extra"
    optimize "Size"
    filter "toolset:gcc or clang"
        buildoptions { "-Werror" }
    filter "toolset:msc"
        buildoptions { "/WX" }
    filter {}

-- Run from the repository root: it reads src/lua/formatting_*.lua.
project "test_formatting"
    kind "ConsoleApp"
    language "C"
    cdialect "C11"
    targetname "test_formatting"
    targetdir "tests"
    objdir "build/obj/test_formatting"
    files { "tests/test_formatting.c", "src/menu_commands.c", "src/menu_commands.h", "src/plugin_host.h", "src/lua_editor_api.h" }
    files(sources_from_rsp("plugin-sources.rsp"))
    lua_settings()
    includedirs { "." }
    links { "user32" }
    warnings "Extra"
    optimize "Size"
    filter "toolset:gcc or clang"
        buildoptions { "-Werror" }
    filter "toolset:msc"
        buildoptions { "/WX" }
    filter {}
