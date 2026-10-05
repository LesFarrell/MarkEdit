@echo off
setlocal
where zig >nul 2>nul || (echo zig was not found on PATH. & exit /b 1)
set "MARKEDIT_TEST_DIR=%TEMP%\markedit-tests"
if not exist "%MARKEDIT_TEST_DIR%" mkdir "%MARKEDIT_TEST_DIR%"
zig cc -std=c11 -O2 -Wall -Wextra -Werror -o "%MARKEDIT_TEST_DIR%\test_parser.exe" tests\test_parser.c markdown.c @cmark-gfm-sources.rsp
if %errorlevel% neq 0 exit /b 1
"%MARKEDIT_TEST_DIR%\test_parser.exe"
if %errorlevel% neq 0 exit /b 1
zig cc -std=c11 -O2 -Wall -Wextra -Werror -o tests\test_editor_commands.exe tests\test_editor_commands.c editor_commands.c -luser32
if %errorlevel% neq 0 exit /b 1
tests\test_editor_commands.exe
if %errorlevel% neq 0 exit /b 1
zig cc -std=c11 -O2 -Wall -Wextra -Werror -o "%MARKEDIT_TEST_DIR%\test_core.exe" tests\test_core.c json_formatter.c text_codec.c workspace_text.c file_io.c path_utils.c
if %errorlevel% neq 0 exit /b 1
"%MARKEDIT_TEST_DIR%\test_core.exe"
if %errorlevel% neq 0 exit /b 1
zig cc -std=c11 -O2 -Wall -Wextra -Werror -o "%MARKEDIT_TEST_DIR%\test_recovery.exe" tests\test_recovery.c recovery.c file_io.c text_codec.c path_utils.c -lole32
if %errorlevel% neq 0 exit /b 1
"%MARKEDIT_TEST_DIR%\test_recovery.exe"
if %errorlevel% neq 0 exit /b 1
where node >nul 2>nul
if %errorlevel% neq 0 (
    echo Node.js not found; preview bridge tests skipped.
    exit /b 0
)
node tests\test_preview.js
exit /b %errorlevel%
