@echo off
setlocal
where zig >nul 2>nul || (echo zig was not found on PATH. & exit /b 1)
set "TEST_BIN_DIR=build\tests"
if not exist "%TEST_BIN_DIR%\" mkdir "%TEST_BIN_DIR%" || exit /b 1
copy /y Scintilla.dll build\Scintilla.dll >nul || exit /b 1
zig cc -std=c11 -O2 -Wall -Wextra -Werror -o %TEST_BIN_DIR%\test_parser.exe tests\test_parser.c src\markdown.c @cmark-gfm-sources.rsp
if %errorlevel% neq 0 exit /b 1
%TEST_BIN_DIR%\test_parser.exe
if %errorlevel% neq 0 exit /b 1
zig cc -std=c11 -O2 -Wall -Wextra -Werror -o %TEST_BIN_DIR%\test_editor_commands.exe tests\test_editor_commands.c src\editor_commands.c -luser32
if %errorlevel% neq 0 exit /b 1
%TEST_BIN_DIR%\test_editor_commands.exe
if %errorlevel% neq 0 exit /b 1
zig cc -std=c11 -O2 -Wall -Wextra -Werror -o %TEST_BIN_DIR%\test_core.exe tests\test_core.c src\text_codec.c src\workspace_text.c src\file_io.c src\path_utils.c src\plugin_settings.c
if %errorlevel% neq 0 exit /b 1
%TEST_BIN_DIR%\test_core.exe
if %errorlevel% neq 0 exit /b 1
zig cc -std=c11 -O2 -Wall -Wextra -Werror -o %TEST_BIN_DIR%\test_recovery.exe tests\test_recovery.c src\recovery.c src\file_io.c src\text_codec.c src\path_utils.c -lole32
if %errorlevel% neq 0 exit /b 1
%TEST_BIN_DIR%\test_recovery.exe
if %errorlevel% neq 0 exit /b 1
zig cc -std=c11 -O2 -Wall -Wextra -Werror -o %TEST_BIN_DIR%\test_plugin_host.exe tests\test_plugin_host.c src\plugin_host.c src\lua_editor_api.c src\file_io.c src\text_codec.c src\path_utils.c @lua-sources.rsp -luser32
if %errorlevel% neq 0 exit /b 1
%TEST_BIN_DIR%\test_plugin_host.exe
if %errorlevel% neq 0 exit /b 1
zig cc -std=c11 -O2 -Wall -Wextra -Werror -o %TEST_BIN_DIR%\test_menus.exe tests\test_menus.c src\menu_commands.c src\plugin_host.c src\lua_editor_api.c src\file_io.c src\text_codec.c src\path_utils.c @lua-sources.rsp -luser32
if %errorlevel% neq 0 exit /b 1
%TEST_BIN_DIR%\test_menus.exe
if %errorlevel% neq 0 exit /b 1
zig cc -std=c11 -O2 -Wall -Wextra -Werror -o %TEST_BIN_DIR%\test_formatting.exe tests\test_formatting.c src\menu_commands.c src\plugin_host.c src\lua_editor_api.c src\file_io.c src\text_codec.c src\path_utils.c @lua-sources.rsp -luser32
if %errorlevel% neq 0 exit /b 1
%TEST_BIN_DIR%\test_formatting.exe
if %errorlevel% neq 0 exit /b 1
where node >nul 2>nul
if %errorlevel% neq 0 (
    echo Node.js not found; preview bridge tests skipped.
    exit /b 0
)
node tests\test_preview.js
exit /b %errorlevel%
