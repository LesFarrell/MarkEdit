@echo off
setlocal
where zig >nul 2>nul || (echo zig was not found on PATH. & exit /b 1)
zig cc -std=c11 -O2 -Wall -Wextra -Werror -o tests\test_parser.exe tests\test_parser.c markdown.c @cmark-gfm-sources.rsp
if %errorlevel% neq 0 exit /b 1
tests\test_parser.exe
exit /b %errorlevel%
