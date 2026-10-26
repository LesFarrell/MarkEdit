@echo off
setlocal
rem Builds tests\%1.c with the remaining arguments, warning-free, then runs it.
rem TEST_CFLAGS and TEST_BIN_DIR are set by the repository-root test.bat.
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
