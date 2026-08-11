@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

rem ---- locate the Microsoft C++ compiler (same dance as build.bat) ------
where cl.exe >nul 2>&1
if not errorlevel 1 goto :havecl

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :nocompiler

set "VCVARS="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto :nocompiler
call "%VCVARS%" >nul 2>&1
where cl.exe >nul 2>&1
if errorlevel 1 goto :nocompiler
:havecl

if not exist build mkdir build
if not exist build\obj-test mkdir build\obj-test
if not exist build\obj-preview mkdir build\obj-preview

rem  /O1 keeps the test build quick. NOMINMAX for the same reason build.bat
rem  needs it - windows.h's min/max macros break std::min/std::max.
rem  These are console programs, so the default /SUBSYSTEM:CONSOLE and main()
rem  entry point are what we want; no /SUBSYSTEM:WINDOWS here.
set TESTFLAGS=/nologo /std:c++17 /O1 /EHsc /MD /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /D_WIN32_WINNT=0x0A00 /D_CRT_SECURE_NO_WARNINGS /Isrc

echo  Building the self-test...
rem engine.cpp brings in device.cpp and actions.cpp. The tests only ever run
rem steps with no side effects, so nothing here types, clicks or launches
rem anything - but the dispatch table and error paths do get exercised.
cl %TESTFLAGS% /Fobuild\obj-test\ /Fdbuild\obj-test\ ^
  tests\selftest.cpp src\config.cpp src\import_yaml.cpp src\json.cpp ^
  src\keys.cpp src\util.cpp src\vision.cpp src\winman.cpp ^
  src\engine.cpp src\actions.cpp src\device.cpp ^
  /Febuild\selftest.exe ^
  /link /INCREMENTAL:NO gdiplus.lib gdi32.lib user32.lib shell32.lib ole32.lib advapi32.lib psapi.lib
if errorlevel 1 ( echo  BUILD FAILED & exit /b 1 )

rem Pass a macrokb macros.yaml as the first argument to also test the importer.
set "YAML=%~1"
if "%YAML%"=="" if exist "..\Macros\macros.yaml" set "YAML=..\Macros\macros.yaml"

echo.
build\selftest.exe "%YAML%"
set "TESTRC=%errorlevel%"

echo.
echo  Rendering the drawing preview...
cl %TESTFLAGS% /Fobuild\obj-preview\ /Fdbuild\obj-preview\ ^
  tests\render_preview.cpp src\theme.cpp src\config.cpp src\import_yaml.cpp ^
  src\json.cpp src\keys.cpp src\util.cpp ^
  /Febuild\preview.exe ^
  /link /INCREMENTAL:NO gdiplus.lib gdi32.lib user32.lib shell32.lib ole32.lib advapi32.lib
if not errorlevel 1 build\preview.exe

rem  Keep a committed copy: build\ is ignored by git, and README.md shows
rem  this image, so it has to live somewhere that survives a clean checkout.
if exist build\ui-preview.png (
  if not exist docs mkdir docs
  copy /y build\ui-preview.png docs\ui-preview.png >nul
)

exit /b %TESTRC%

:nocompiler
echo  ERROR: no Microsoft C++ compiler found - see build.bat
exit /b 1
