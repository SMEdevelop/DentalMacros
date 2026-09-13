@echo off
setlocal
cd /d "%~dp0.."

rem  Builds and runs the raw-input dump. See tools\inputdump.cpp for what it
rem  is for. Optional argument narrows it to one device, e.g. a VID:
rem
rem      tools\inputdump.bat 046D

where cl.exe >nul 2>&1
if not errorlevel 1 goto :havecl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :nocompiler
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto :nocompiler
call "%VCVARS%" >nul 2>&1
where cl.exe >nul 2>&1
if errorlevel 1 goto :nocompiler
:havecl

if not exist build mkdir build
cl /nologo /std:c++17 /O1 /EHsc /MD /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX ^
  /D_WIN32_WINNT=0x0A00 /D_CRT_SECURE_NO_WARNINGS ^
  /Fobuild\obj-dump\ /Fdbuild\obj-dump\ tools\inputdump.cpp ^
  /Febuild\inputdump.exe /link /INCREMENTAL:NO user32.lib hid.lib
if errorlevel 1 ( echo  BUILD FAILED & exit /b 1 )

echo.
build\inputdump.exe %1
exit /b 0

:nocompiler
echo  ERROR: no Microsoft C++ compiler found - see build.bat
exit /b 1
