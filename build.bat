@echo off
setlocal
set "PROJECT_DIR=%~dp0"
set "DIST_DIR=%~dp0dist"
set "DIST_BACKUP=%~dp0dist.previous"
pushd "%PROJECT_DIR%" || (echo Could not enter the project directory. & exit /b 1)

call scripts\msvc-env.bat || exit /b 1
set "PS=pwsh"
where pwsh >nul 2>nul || set "PS=powershell"
set "OBJ_DIR=build\msvc"
%PS% -NoProfile -ExecutionPolicy Bypass -File scripts\build_msvc.ps1 -OutputDirectory "%OBJ_DIR%"
if errorlevel 1 exit /b 1

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
