@echo off
setlocal
call scripts\msvc-env.bat || exit /b 1
set "PS=pwsh"
where pwsh >nul 2>nul || set "PS=powershell"
%PS% -NoProfile -ExecutionPolicy Bypass -File check_structure.ps1
if %errorlevel% neq 0 exit /b 1
%PS% -NoProfile -ExecutionPolicy Bypass -File scripts\test_msvc.ps1
exit /b %errorlevel%
