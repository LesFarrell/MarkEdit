@echo off
rem Make the x64 MSVC command-line tools available to the calling script.
if /I "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    where cl.exe >nul 2>nul && where link.exe >nul 2>nul && where rc.exe >nul 2>nul && exit /b 0
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio Installer's vswhere.exe was not found.
    echo Install Visual Studio with the Desktop development with C++ workload.
    exit /b 1
)

set "VS_INSTALL="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"
if not defined VS_INSTALL (
    echo No Visual Studio installation with the x64 C++ tools was found.
    exit /b 1
)

if not exist "%VS_INSTALL%\Common7\Tools\VsDevCmd.bat" (
    echo Visual Studio developer environment was not found under "%VS_INSTALL%".
    exit /b 1
)

call "%VS_INSTALL%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1

where cl.exe >nul 2>nul || (echo cl.exe was not found after initializing Visual Studio. & exit /b 1)
where link.exe >nul 2>nul || (echo link.exe was not found after initializing Visual Studio. & exit /b 1)
where rc.exe >nul 2>nul || (echo rc.exe was not found after initializing Visual Studio. & exit /b 1)
exit /b 0
