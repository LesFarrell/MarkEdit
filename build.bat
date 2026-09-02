@echo off
setlocal
set "PROJECT_DIR=%~dp0"
set "DIST_DIR=%~dp0dist"
set "DIST_BACKUP=%~dp0dist.previous"
pushd "%PROJECT_DIR%" || (echo Could not enter the project directory. & exit /b 1)

where zig >nul 2>nul || (echo zig was not found on PATH. & exit /b 1)

set "COMMON_FLAGS=-Os -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident -fmerge-all-constants"
set "LINK_FLAGS=-Wl,--gc-sections -Wl,--strip-all -Wl,--build-id=none"

zig rc markedit.rc markedit.res
if %errorlevel% neq 0 (
    echo Resource compile failed.
    exit /b 1
)

zig c++ -std=c++17 %COMMON_FLAGS% -shared -DNDEBUG %LINK_FLAGS% ^
    -Wl,--out-implib,libLexilla.dll.a -o Lexilla.dll @lexilla-sources.rsp
if %errorlevel% neq 0 (
    echo Lexilla build failed.
    exit /b 1
)

zig cc -std=c11 %COMMON_FLAGS% -Wall -Wextra -Wno-unknown-pragmas ^
    -o markedit.exe @markedit-sources.rsp @cmark-gfm-sources.rsp markedit.res ^
    third_party\webview2\x64\WebView2Loader.dll.lib libLexilla.dll.a ^
    -isystem third_party\webview2\include -isystem third_party\lexilla\include ^
    -lgdi32 -luser32 -lcomdlg32 -lcomctl32 -lshlwapi -lshell32 -lole32 ^
    -lwindowscodecs -luxtheme -ldwmapi -Wl,--subsystem,windows %LINK_FLAGS%
if %errorlevel% neq 0 (
    echo Build failed.
    exit /b 1
)
copy /y third_party\webview2\x64\WebView2Loader.dll . >nul
if %errorlevel% neq 0 (echo Failed to copy WebView2Loader.dll. & exit /b 1)
copy /y third_party\scintilla\bin\Scintilla.dll . >nul
if %errorlevel% neq 0 (echo Failed to copy Scintilla.dll. & exit /b 1)

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

for %%F in (
    markedit.exe
    Lexilla.dll
    Scintilla.dll
    WebView2Loader.dll
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

for %%D in (cmark-gfm highlightjs katex lexilla scintilla webview2) do (
    mkdir "%DIST_DIR%\third_party\%%D" >nul 2>nul
)
copy /y third_party\cmark-gfm\COPYING "%DIST_DIR%\third_party\cmark-gfm\" >nul || exit /b 1
copy /y third_party\highlightjs\LICENSE "%DIST_DIR%\third_party\highlightjs\" >nul || exit /b 1
copy /y third_party\highlightjs\VERSION.txt "%DIST_DIR%\third_party\highlightjs\" >nul || exit /b 1
copy /y third_party\katex\LICENSE "%DIST_DIR%\third_party\katex\" >nul || exit /b 1
copy /y third_party\katex\VERSION.txt "%DIST_DIR%\third_party\katex\" >nul || exit /b 1
copy /y third_party\lexilla\License.txt "%DIST_DIR%\third_party\lexilla\" >nul || exit /b 1
copy /y third_party\lexilla\version.txt "%DIST_DIR%\third_party\lexilla\" >nul || exit /b 1
copy /y third_party\scintilla\License.txt "%DIST_DIR%\third_party\scintilla\" >nul || exit /b 1
copy /y third_party\scintilla\version.txt "%DIST_DIR%\third_party\scintilla\" >nul || exit /b 1
copy /y third_party\webview2\LICENSE.txt "%DIST_DIR%\third_party\webview2\" >nul || exit /b 1

if exist "%DIST_BACKUP%\" rmdir /s /q "%DIST_BACKUP%"
if exist "%DIST_BACKUP%\" echo Warning: dist.previous is still locked and will be retried on the next build.

for %%I in (markedit.exe) do echo Build succeeded: markedit.exe ^(%%~zI bytes^)
echo Distribution staged in: "%DIST_DIR%"
