@echo off
setlocal enabledelayedexpansion
set "PROJECT_DIR=%~dp0"
set "DIST_DIR=%~dp0dist"
set "DIST_BACKUP=%~dp0dist.previous"
pushd "%PROJECT_DIR%" || (echo Could not enter the project directory. & exit /b 1)

where zig >nul 2>nul || (echo zig was not found on PATH. & exit /b 1)

rem Lua reports script errors with longjmp. On 64-bit Windows that unwind must
rem cross valid function tables; stripping them makes any plugin error fail-fast
rem in ntdll with STATUS_BAD_FUNCTION_TABLE before MarkEdit can show it.
rem The code-generation flags (including those unwind tables) live in
rem release-flags.rsp, which test.bat and premake5.lua read too, so the tests
rem exercise the same code generation as the release build.
set "COMMON_FLAGS=@release-flags.rsp"
set "LINK_FLAGS=-Wl,--gc-sections -Wl,--strip-all -Wl,--build-id=none"
rem Intermediate files go here rather than the project root. build\ is shared
rem with the premake5-generated build, which uses build\obj.
set "OBJ_DIR=build\zig"
if not exist "%OBJ_DIR%\" mkdir "%OBJ_DIR%" || (echo Failed to create %OBJ_DIR%. & exit /b 1)

zig rc markedit.rc %OBJ_DIR%\markedit.res
if %errorlevel% neq 0 (
    echo Resource compile failed.
    exit /b 1
)

rem Lexilla is linked straight into markedit.exe (static object files)
rem rather than shipped as its own DLL beside it. WebView2Loader stays a
rem dynamic DLL: Microsoft's WebView2LoaderStatic.lib is MSVC-ABI-only (MSVC
rem name mangling, /GS stack-cookie symbols, MSVC CRT thread-init guards) and
rem can't be linked into a GNU/MinGW-targeted build like this one - the
rem dynamic loader's DLL boundary is plain C, so it's the only ABI-compatible
rem option here. Scintilla.dll also stays external, since it self-registers
rem its window class at runtime via LoadLibrary and isn't linked at build
rem time at all.
rem
rem Each Lexilla source is compiled individually into a same-named .o in
rem %OBJ_DIR% (zig's cc/c++ driver, unlike plain clang, can't emit
rem multiple named object files from one -c invocation targeting
rem COFF/Windows) - the resulting .o list is written to
rem %OBJ_DIR%\lexilla-objects.rsp for the final link below.
type nul > %OBJ_DIR%\lexilla-objects.rsp
for /f "usebackq delims=" %%S in (`findstr /offline /i ".cxx" lexilla-sources.rsp`) do (
    zig c++ -std=c++17 %COMMON_FLAGS% -DNDEBUG -Ithird_party\lexilla\include -Ithird_party\lexilla\lexlib -Ithird_party\scintilla\include -c "%%S" -o "%OBJ_DIR%\%%~nS.o"
    if !errorlevel! neq 0 (
        echo Lexilla build failed compiling %%S.
        exit /b 1
    )
    echo %OBJ_DIR%\%%~nS.o>>%OBJ_DIR%\lexilla-objects.rsp
)

rem cmark-gfm is compiled the same way, so its -Wno-unused-parameter (the
rem vendored code leaves many parameters unused) stays off MarkEdit's own sources.
rem cmark-gfm-objects.rsp starts with the .rsp's -D/-I lines, which
rem src\markdown.c needs to include the cmark-gfm headers, then lists the
rem objects.
findstr /offline /r /b "[-]" cmark-gfm-sources.rsp > %OBJ_DIR%\cmark-gfm-flags.rsp
copy /y %OBJ_DIR%\cmark-gfm-flags.rsp %OBJ_DIR%\cmark-gfm-objects.rsp >nul
for /f "usebackq delims=" %%S in (`findstr /offline /r /v /b "[-]" cmark-gfm-sources.rsp`) do (
    zig cc -std=c11 %COMMON_FLAGS% -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-parameter @%OBJ_DIR%\cmark-gfm-flags.rsp -c "%%S" -o "%OBJ_DIR%\%%~nS.o"
    if !errorlevel! neq 0 (
        echo cmark-gfm build failed compiling %%S.
        exit /b 1
    )
    echo %OBJ_DIR%\%%~nS.o>>%OBJ_DIR%\cmark-gfm-objects.rsp
)

rem zig c++ (not zig cc) so the C++ object code compiled above still gets its
rem runtime auto-linked; the MarkEdit and Lua sources are .c files and are
rem still compiled as C - the driver choice only changes runtime auto-linking
rem defaults, not per-file language detection.
rem Lexilla.cxx __declspec(dllexport)s its API even inside an exe, so the
rem linker also writes an import library; --out-implib keeps it in %OBJ_DIR%.
zig c++ -std=c11 %COMMON_FLAGS% -Wall -Wextra -Wno-unknown-pragmas ^
    -o %OBJ_DIR%\markedit.exe @markedit-sources.rsp @%OBJ_DIR%\cmark-gfm-objects.rsp @lua-sources.rsp @%OBJ_DIR%\lexilla-objects.rsp %OBJ_DIR%\markedit.res ^
    -Wl,--out-implib,%OBJ_DIR%\markedit.lib ^
    third_party\webview2\x64\WebView2Loader.dll.lib ^
    -isystem third_party\webview2\include -isystem third_party\lexilla\include ^
    -lgdi32 -lmsimg32 -luser32 -lcomdlg32 -lcomctl32 -lshlwapi -lshell32 -lole32 -lmpr ^
    -lwindowscodecs -luxtheme -ldwmapi -Wl,--subsystem,windows %LINK_FLAGS%
if %errorlevel% neq 0 (
    echo Build failed.
    exit /b 1
)
rem Resolve and verify the destructive target before replacing the release tree.
for %%I in ("%PROJECT_DIR%.") do set "PROJECT_FULL=%%~fI"
for %%I in ("%DIST_DIR%") do set "DIST_FULL=%%~fI"
for %%I in ("%DIST_BACKUP%") do set "DIST_BACKUP_FULL=%%~fI"
if /I not "%DIST_FULL%"=="%PROJECT_FULL%\dist" (
    echo Refusing to clear unexpected distribution path: "%DIST_FULL%"
    exit /b 1
)
if /I not "%DIST_BACKUP_FULL%"=="%PROJECT_FULL%\dist.previous" (
    echo Refusing to rotate to unexpected backup path: "%DIST_BACKUP_FULL%"
    exit /b 1
)

if exist "%DIST_DIR%\" rmdir /s /q "%DIST_DIR%"
if exist "%DIST_DIR%\" (
    rem A previously-run packaged app may leave its WebView cache locked.
    if exist "%DIST_BACKUP%\" rmdir /s /q "%DIST_BACKUP%"
    if exist "%DIST_BACKUP%\" (
        echo Failed to clear the previous distribution backup. Close applications using it.
        exit /b 1
    )
    move "%DIST_DIR%" "%DIST_BACKUP%" >nul
    if errorlevel 1 (
        echo Failed to rotate the locked distribution folder.
        exit /b 1
    )
)
mkdir "%DIST_DIR%" || (echo Failed to create the distribution folder. & exit /b 1)

rem The exe and runtime DLLs go straight into dist\ from where they are built
rem or vendored, so the project root holds no build output.
for %%F in (
    %OBJ_DIR%\markedit.exe
    third_party\scintilla\bin\Scintilla.dll
    third_party\webview2\x64\WebView2Loader.dll
    preview_shell.html
    github-markdown.css
    mermaid.min.js
    highlight.min.js
    highlight-powershell.min.js
    highlight-github.min.css
    highlight-github-dark.min.css
    LICENSE
    THIRD_PARTY_NOTICES.md
) do (
    if not exist "%%F" (echo Missing distribution file: %%F & exit /b 1)
    copy /y "%%F" "%DIST_DIR%\" >nul || exit /b 1
)

xcopy /e /i /y /q katex "%DIST_DIR%\katex" >nul
if %errorlevel% neq 0 (echo Failed to stage KaTeX assets. & exit /b 1)
xcopy /e /i /y /q plugins "%DIST_DIR%\plugins" >nul
if %errorlevel% neq 0 (echo Failed to stage plugins. & exit /b 1)

for %%D in (cmark-gfm gdb github-markdown-css highlightjs katex lexilla lua mermaid scintilla scowl webview2) do (
    mkdir "%DIST_DIR%\third_party\%%D" >nul 2>nul
)
mkdir "%DIST_DIR%\third_party\gdb\bin" >nul 2>nul
copy /y third_party\gdb\bin\gdb.exe "%DIST_DIR%\third_party\gdb\bin\" >nul || exit /b 1
for %%F in (COPYING.txt COPYING.MinGW-w64-runtime.txt VERSION.txt SOURCE.md SHA256SUMS.txt) do (
    copy /y "third_party\gdb\%%F" "%DIST_DIR%\third_party\gdb\" >nul || exit /b 1
)
copy /y third_party\cmark-gfm\COPYING "%DIST_DIR%\third_party\cmark-gfm\" >nul || exit /b 1
copy /y third_party\cmark-gfm\VERSION.txt "%DIST_DIR%\third_party\cmark-gfm\" >nul || exit /b 1
copy /y third_party\github-markdown-css\LICENSE "%DIST_DIR%\third_party\github-markdown-css\" >nul || exit /b 1
copy /y third_party\github-markdown-css\VERSION.txt "%DIST_DIR%\third_party\github-markdown-css\" >nul || exit /b 1
copy /y third_party\highlightjs\LICENSE "%DIST_DIR%\third_party\highlightjs\" >nul || exit /b 1
copy /y third_party\highlightjs\VERSION.txt "%DIST_DIR%\third_party\highlightjs\" >nul || exit /b 1
copy /y third_party\katex\LICENSE "%DIST_DIR%\third_party\katex\" >nul || exit /b 1
copy /y third_party\katex\VERSION.txt "%DIST_DIR%\third_party\katex\" >nul || exit /b 1
copy /y third_party\lexilla\License.txt "%DIST_DIR%\third_party\lexilla\" >nul || exit /b 1
copy /y third_party\lexilla\version.txt "%DIST_DIR%\third_party\lexilla\" >nul || exit /b 1
copy /y third_party\lua\LICENSE "%DIST_DIR%\third_party\lua\" >nul || exit /b 1
copy /y third_party\lua\VERSION.txt "%DIST_DIR%\third_party\lua\" >nul || exit /b 1
copy /y third_party\mermaid\LICENSE "%DIST_DIR%\third_party\mermaid\" >nul || exit /b 1
copy /y third_party\mermaid\VERSION.txt "%DIST_DIR%\third_party\mermaid\" >nul || exit /b 1
copy /y third_party\scintilla\License.txt "%DIST_DIR%\third_party\scintilla\" >nul || exit /b 1
copy /y third_party\scintilla\version.txt "%DIST_DIR%\third_party\scintilla\" >nul || exit /b 1
copy /y third_party\scowl\COPYING "%DIST_DIR%\third_party\scowl\" >nul || exit /b 1
copy /y third_party\scowl\VERSION.txt "%DIST_DIR%\third_party\scowl\" >nul || exit /b 1
copy /y third_party\webview2\LICENSE.txt "%DIST_DIR%\third_party\webview2\" >nul || exit /b 1
copy /y third_party\webview2\VERSION.txt "%DIST_DIR%\third_party\webview2\" >nul || exit /b 1

if exist "%DIST_BACKUP%\" rmdir /s /q "%DIST_BACKUP%"
if exist "%DIST_BACKUP%\" echo Warning: dist.previous is still locked and will be retried on the next build.

for %%I in ("%DIST_DIR%\markedit.exe") do echo Build succeeded: markedit.exe ^(%%~zI bytes^)
echo Distribution staged in: "%DIST_DIR%"
