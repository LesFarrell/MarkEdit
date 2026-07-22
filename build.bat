@echo off
setlocal
where gcc >nul 2>nul || (echo gcc was not found on PATH. & exit /b 1)
where windres >nul 2>nul || (echo windres was not found on PATH. & exit /b 1)
windres markedit.rc -O coff -o markedit.res
if %errorlevel% neq 0 (
    echo Resource compile failed.
    exit /b 1
)
gcc -std=c11 -O2 -Wall -Wextra -Wno-unknown-pragmas -o markedit.exe markedit.c markdown.c @cmark-gfm-sources.rsp markedit.res -isystem third_party\webview2\include -L third_party\webview2\x64 -lgdi32 -luser32 -lcomdlg32 -lshlwapi -lshell32 -lole32 -lWebView2Loader.dll -mwindows
if %errorlevel% neq 0 (
    echo Build failed.
    exit /b 1
)
copy /y third_party\webview2\x64\WebView2Loader.dll . >nul
if %errorlevel% neq 0 (echo Failed to copy WebView2Loader.dll. & exit /b 1)
copy /y third_party\scintilla\bin\Scintilla.dll . >nul
if %errorlevel% neq 0 (echo Failed to copy Scintilla.dll. & exit /b 1)
echo Build succeeded: markedit.exe
