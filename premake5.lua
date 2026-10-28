-- Premake5 script for MarkEdit.
-- Premake supplies project, files, filter and its other functions as
-- globals, which a Lua language server doesn't know about.
---@diagnostic disable: undefined-global, lowercase-global
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

-- The non-blank lines of a repo-root *.rsp response file, trimmed.
function rsp_lines(path)
    local lines = {}
    for line in io.lines(path) do
        local entry = line:match("^%s*(.-)%s*$")
        if entry ~= "" then table.insert(lines, entry) end
    end
    return lines
end

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
        -- The code-generation flags build.bat and test.bat also read, so every
        -- project here (the tests too) compiles like the release build. They
        -- include the unwind tables Lua's longjmp needs on x64 Windows while
        -- it crosses plugin-host frames.
        buildoptions(rsp_lines("release-flags.rsp"))
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
-- that build.bat and test.bat hand to the compiler. Reading them here keeps
-- Premake and the batch builds in step; the .rsp also carry -D/-I flags,
-- which are filtered out (only .c/.cc/.cpp/.cxx entries are returned). This
-- is the one place a compiled-source list is read, so a new file only has to
-- be added to its .rsp.
function sources_from_rsp(path)
    local sources = {}
    for _, entry in ipairs(rsp_lines(path)) do
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
    -- Scoped to cmark-gfm's own files (as build.bat compiles them apart), so
    -- MarkEdit's sources keep the unused-parameter warning.
    filter { "files:"..CMARK_GFM_DIR.."/**.c", "toolset:gcc or clang" }
        buildoptions { "-Wno-unused-parameter" }
    filter { "files:"..CMARK_GFM_DIR.."/**.c", "toolset:msc" }
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
    "build/bin/markedit.exe",
    "preview_shell.html", "github-markdown.css", "mermaid.min.js", "highlight.min.js",
    "highlight-powershell.min.js", "highlight-github.min.css", "highlight-github-dark.min.css",
    "LICENSE", "THIRD_PARTY_NOTICES.md",
}
DIST_LICENCES = {
    ["cmark-gfm"]   = { CMARK_GFM_DIR.."/COPYING", CMARK_GFM_DIR.."/VERSION.txt" },
    ["gdb"]         = {
        "third_party/gdb/COPYING.txt", "third_party/gdb/COPYING.MinGW-w64-runtime.txt",
        "third_party/gdb/VERSION.txt", "third_party/gdb/SOURCE.md", "third_party/gdb/SHA256SUMS.txt",
    },
    ["github-markdown-css"] = {
        "third_party/github-markdown-css/LICENSE", "third_party/github-markdown-css/VERSION.txt",
    },
    ["highlightjs"] = { "third_party/highlightjs/LICENSE", "third_party/highlightjs/VERSION.txt" },
    ["katex"]       = { "third_party/katex/LICENSE", "third_party/katex/VERSION.txt" },
    ["lexilla"]     = { LEXILLA_DIR.."/License.txt", LEXILLA_DIR.."/version.txt" },
    ["lua"]         = { LUA_DIR.."/LICENSE", LUA_DIR.."/VERSION.txt" },
    ["mermaid"]     = { "third_party/mermaid/LICENSE", "third_party/mermaid/VERSION.txt" },
    ["scintilla"]   = { SCINTILLA_DIR.."/License.txt", SCINTILLA_DIR.."/version.txt" },
    ["webview2"]    = { WEBVIEW2_DIR.."/LICENSE.txt", WEBVIEW2_DIR.."/VERSION.txt" },
}
-- The two runtime DLLs go straight into dist/ from where they're vendored,
-- like markedit.exe from build/bin, so the repo root holds no build output.
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

    mkdir("dist/katex")
    mkdir("dist/plugins")
    for _, dll in ipairs(RUNTIME_DLLS) do copy(dll, "dist/") end
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
    -- No -fno-exceptions/-fno-rtti, as in build.bat: lexlib's
    -- CharacterCategory.cxx uses try/catch.

project "markedit"
    kind "WindowedApp"
    language "C"
    cdialect "C11"
    targetname "markedit"
    targetdir "build/bin"
    objdir "build/obj/markedit"

    files(sources_from_rsp("markedit-sources.rsp"))
    -- Not compiled on their own: the unity build's *.inc components, the
    -- headers, and the scripts and help markedit.rc embeds. Wildcards, so a
    -- new header or component shows up in the IDE without editing this list.
    files {
        "src/internal/*.inc", "src/*.h",
        "src/menus.lua", "src/lua/*.lua", "src/help/*",
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
    links { "lexilla", "gdi32", "msimg32", "user32", "comdlg32", "comctl32", "shlwapi", "shell32", "ole32", "mpr", "windowscodecs", "uxtheme", "dwmapi" }

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

-- ---- test programs ----
-- Each one is tests/<name>.c plus the app sources it exercises, built
-- warning-free like the app itself, mirroring its test.bat line.
--   sources  the extra source (and header) files
--   links    the Windows libraries it needs
--   uses     "plugins" for the plugin host (plugin-sources.rsp and Lua),
--            "lua" for Lua alone, "cmark" for the Markdown renderer
function test_project(name, sources, links_list, uses)
    project(name)
        kind "ConsoleApp"
        language "C"
        cdialect "C11"
        targetname(name)
        -- Beside test.bat's builds; the tests load ..\Scintilla.dll, so
        -- each one copies it into build/ first.
        targetdir "build/tests"
        objdir("build/obj/" .. name)
        files { "tests/" .. name .. ".c" }
        files(sources)
        if uses == "plugins" then
            files(sources_from_rsp("plugin-sources.rsp"))
            lua_settings()
        elseif uses == "lua" then
            lua_settings()
        elseif uses == "cmark" then
            cmark_gfm_settings()
        end
        includedirs { "." }
        links(links_list)
        warnings "Extra"
        optimize "Size"
        filter "toolset:gcc or clang"
            buildoptions { "-Werror" }
            prebuildcommands { "cp -f ../"..SCINTILLA_DIR.."/bin/Scintilla.dll ." }
        filter "toolset:msc"
            buildoptions { "/WX" }
            prebuildcommands { 'copy /Y "..\\'..SCINTILLA_DIR:gsub("/", "\\")..'\\bin\\Scintilla.dll" . >nul' }
        filter {}
end

test_project("test_parser", { "src/markdown.c" }, {}, "cmark")
test_project("test_editor_commands", { "src/editor_commands.c", "src/editor_commands.h" }, { "user32" })
test_project("test_text_search", { "src/text_search.c", "src/text_search.h" }, { "user32" })
test_project("test_language", { "src/editor_language.c", "src/editor_language.h", "src/theme.c" },
             { "user32", "shlwapi", "gdi32", "uxtheme" })
test_project("test_outline", { "src/outline_minimap.c", "src/outline_minimap.h", "src/theme.c", "src/text_codec.c" },
             { "user32", "comctl32", "gdi32" })
test_project("test_theme", { "src/theme.c", "src/theme.h", "src/plugin_host.h" },
             { "user32", "gdi32", "uxtheme" }, "plugins")
test_project("test_core", { "src/text_codec.c", "src/workspace_text.c", "src/file_io.c", "src/path_utils.c",
                            "src/plugin_settings.c", "src/plugin_settings.h" }, {})
test_project("test_output", { "src/output_text.c", "src/output_text.h", "src/output_process.c",
                              "src/output_process.h" }, {})
test_project("test_git_status", { "src/git_status.c", "src/git_status.h" }, {})
test_project("test_terminal_grid", { "src/terminal_grid.c", "src/terminal_grid.h" }, {})
test_project("test_terminal_input", { "src/terminal_input.c", "src/terminal_input.h" }, {})
test_project("test_terminal_process", { "src/terminal_process.c", "src/terminal_process.h" }, { "user32" })
test_project("test_recovery", { "src/recovery.c", "src/file_io.c", "src/text_codec.c", "src/path_utils.c" },
             { "ole32" })
test_project("test_plugin_host", { "src/plugin_host.h", "src/lua_editor_api.h" }, { "user32" }, "plugins")
test_project("test_assemblers", {}, {}, "lua")
test_project("test_lua_format", {}, {}, "lua")
-- These two read src/menus.lua and src/lua/formatting_*.lua, so run them
-- from the repository root (test.bat does).
test_project("test_menus", { "src/menu_commands.c", "src/menu_commands.h", "src/plugin_host.h", "src/lua_editor_api.h" },
             { "user32" }, "plugins")
test_project("test_formatting", { "src/menu_commands.c", "src/menu_commands.h", "src/plugin_host.h",
                                  "src/lua_editor_api.h" }, { "user32" }, "plugins")
