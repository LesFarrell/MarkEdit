@echo off
setlocal
where zig >nul 2>nul || (echo zig was not found on PATH. & exit /b 1)
set "PS=pwsh"
where pwsh >nul 2>nul || set "PS=powershell"
%PS% -NoProfile -ExecutionPolicy Bypass -File check_structure.ps1
if %errorlevel% neq 0 exit /b 1
set "TEST_BIN_DIR=build\tests"
if not exist "%TEST_BIN_DIR%\" mkdir "%TEST_BIN_DIR%" || exit /b 1
rem The tests load ..\Scintilla.dll beside build\tests; take it from the vendored
rem copy so tests run on a fresh checkout before build.bat has run.
copy /y third_party\scintilla\bin\Scintilla.dll build\Scintilla.dll >nul || exit /b 1
rem release-flags.rsp is build.bat's code generation (-Os, unwind tables, ...),
rem so the tests exercise what ships; -Werror keeps them warning-free.
set "TEST_CFLAGS=-std=c11 @release-flags.rsp -Wall -Wextra -Werror"

rem cmark-gfm is compiled on its own, as build.bat does, so its
rem -Wno-unused-parameter stays off the test and MarkEdit sources.
rem cmark-gfm-objects.rsp starts with the -D/-I lines src\markdown.c needs.
set "CMARK_OBJ_DIR=%TEST_BIN_DIR%\cmark-gfm"
if not exist "%CMARK_OBJ_DIR%\" mkdir "%CMARK_OBJ_DIR%" || exit /b 1
findstr /r /b "[-]" cmark-gfm-sources.rsp > %CMARK_OBJ_DIR%\cmark-gfm-flags.rsp
copy /y %CMARK_OBJ_DIR%\cmark-gfm-flags.rsp %CMARK_OBJ_DIR%\cmark-gfm-objects.rsp >nul || exit /b 1
for /f "usebackq delims=" %%S in (`findstr /r /v /b "[-]" cmark-gfm-sources.rsp`) do (
    call :compile_cmark "%%S" || exit /b 1
)

rem Each test is tests\<name>.c plus the sources and libraries listed after
rem its name. premake5.lua's test_project() calls mirror these lines.
call :run_test test_parser src\markdown.c @%CMARK_OBJ_DIR%\cmark-gfm-objects.rsp || exit /b 1
call :run_test test_editor_commands src\editor_commands.c -luser32 || exit /b 1
call :run_test test_text_search src\text_search.c -luser32 || exit /b 1
call :run_test test_language src\editor_language.c src\theme.c -luser32 -lshlwapi -lgdi32 -luxtheme || exit /b 1
call :run_test test_outline src\outline_minimap.c src\theme.c src\text_codec.c -luser32 -lcomctl32 -lgdi32 -lmsimg32 || exit /b 1
call :run_test test_theme src\theme.c @plugin-sources.rsp @lua-sources.rsp -luser32 -lgdi32 -luxtheme || exit /b 1
call :run_test test_core src\text_codec.c src\workspace_text.c src\file_io.c src\path_utils.c src\plugin_settings.c || exit /b 1
call :run_test test_output src\output_text.c src\output_process.c || exit /b 1
call :run_test test_git_status src\git_status.c || exit /b 1
call :run_test test_git_workflow || exit /b 1
call :run_test test_terminal_grid src\terminal_grid.c || exit /b 1
call :run_test test_terminal_input src\terminal_input.c -luser32 || exit /b 1
call :run_test test_terminal_process src\terminal_process.c -luser32 || exit /b 1
call :run_test test_recovery src\recovery.c src\file_io.c src\text_codec.c src\path_utils.c -lole32 || exit /b 1
call :run_test test_plugin_host @plugin-sources.rsp @lua-sources.rsp -luser32 || exit /b 1
call :run_test test_assemblers @lua-sources.rsp || exit /b 1
call :run_test test_lua_format @lua-sources.rsp || exit /b 1
rem test_menus and test_formatting read src\menus.lua and src\lua\*.lua, so
rem they run from the repository root, as everything here does.
call :run_test test_menus src\menu_commands.c src\app_accelerators.c @plugin-sources.rsp @lua-sources.rsp -luser32 || exit /b 1
call :run_test test_formatting src\menu_commands.c @plugin-sources.rsp @lua-sources.rsp -luser32 || exit /b 1

where node >nul 2>nul
if %errorlevel% neq 0 (
    echo Node.js not found; preview bridge tests skipped.
    exit /b 0
)
node tests\test_preview.js
exit /b %errorlevel%

rem Builds tests\%1.c with the remaining arguments, warning-free, then runs it.
:run_test
set "TEST_NAME=%~1"
shift
set "TEST_INPUTS="
:collect_inputs
if "%~1"=="" goto build_test
set "TEST_INPUTS=%TEST_INPUTS% %1"
shift
goto collect_inputs
:build_test
zig cc %TEST_CFLAGS% -o %TEST_BIN_DIR%\%TEST_NAME%.exe tests\%TEST_NAME%.c%TEST_INPUTS%
if %errorlevel% neq 0 exit /b 1
%TEST_BIN_DIR%\%TEST_NAME%.exe
exit /b %errorlevel%

rem Compiles one cmark-gfm source (%1) into %CMARK_OBJ_DIR% and lists it.
:compile_cmark
zig cc %TEST_CFLAGS% -Wno-unknown-pragmas -Wno-unused-parameter @%CMARK_OBJ_DIR%\cmark-gfm-flags.rsp -c %1 -o "%CMARK_OBJ_DIR%\%~n1.o"
if %errorlevel% neq 0 exit /b 1
echo %CMARK_OBJ_DIR%\%~n1.o>>%CMARK_OBJ_DIR%\cmark-gfm-objects.rsp
exit /b 0
