@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

echo.
echo  Building Dental Macros...
echo.

rem ---- locate the Microsoft C++ compiler -------------------------------
rem  If cl.exe is already on PATH we are inside a Developer Command Prompt
rem  and there is nothing to set up. Otherwise vswhere finds the newest
rem  Visual Studio (or Build Tools) install that actually carries the x64
rem  toolset, and its vcvars64.bat puts cl.exe, rc.exe and link.exe on PATH.
where cl.exe >nul 2>&1
if not errorlevel 1 goto :havecl

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :nocompiler

set "VCVARS="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto :nocompiler

rem  2>&1 as well as >nul: vcvars64.bat shells out to vswhere.exe itself and
rem  grumbles to stderr when it is not on PATH, which is noise we did not cause
rem  and which does not stop it working.
call "%VCVARS%" >nul 2>&1
where cl.exe >nul 2>&1
if errorlevel 1 goto :nocompiler

:havecl
for /f "delims=" %%V in ('cl 2^>^&1 ^| findstr /c:"Version"') do (
  echo  Compiler: %%V
  goto :gotver
)
:gotver

if not exist build\obj mkdir build\obj

rem  /MD links the C++ runtime dynamically, so DentalMacros.exe needs
rem  msvcp140.dll, vcruntime140.dll and vcruntime140_1.dll beside it. package.bat
rem  copies those three out of the Visual Studio redistributable into the zip.
rem  /W3 is MSVC's practical equivalent of -Wall; /Wall here means "every
rem  warning including the ones the Windows headers themselves trip", which is
rem  thousands of lines of noise.
rem  NOMINMAX is required, not cosmetic: windows.h defines min() and max() as
rem  macros, which turns every std::max(a,b) in the source into std::(a)>(b)...
rem  and fails to compile. MinGW's headers are laxer about this, so the G++
rem  build never needed it.
set CXXFLAGS=/nologo /std:c++17 /O2 /EHsc /MD /W3 /GS /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /D_WIN32_WINNT=0x0A00 /D_CRT_SECURE_NO_WARNINGS /Isrc /Fobuild\obj\ /Fdbuild\obj\

rem  /SUBSYSTEM:WINDOWS is -mwindows: no console window. The entry point is
rem  wWinMain, which the linker picks up on its own for a Unicode GUI app.
rem  /MANIFEST:NO matters - app.rc already supplies RT_MANIFEST id 1, and
rem  without this the linker generates a second one and the build fails with
rem  a duplicate resource.
set LDFLAGS=/link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OPT:REF /OPT:ICF /INCREMENTAL:NO
set LIBS=gdiplus.lib comctl32.lib comdlg32.lib shlwapi.lib shell32.lib dwmapi.lib uxtheme.lib msimg32.lib gdi32.lib user32.lib ole32.lib oleaut32.lib advapi32.lib psapi.lib hid.lib

set SOURCES=src\main.cpp src\app.cpp src\minideck.cpp src\iconlib.cpp src\iconpicker.cpp src\dialogs.cpp src\ui_common.cpp src\theme.cpp src\engine.cpp src\config.cpp src\import_yaml.cpp src\winman.cpp src\vision.cpp src\actions.cpp src\device.cpp src\keys.cpp src\json.cpp src\util.cpp

echo  Compiling resources...
rc /nologo /fo build\app.res src\app.rc
if errorlevel 1 goto :failed

echo  Compiling and linking (this takes a minute)...
cl %CXXFLAGS% %SOURCES% build\app.res /Febuild\DentalMacros.exe %LDFLAGS% %LIBS%
if errorlevel 1 goto :failed

if not exist build\images mkdir build\images
if not exist build\layouts mkdir build\layouts

echo.
echo  Built:  build\DentalMacros.exe
echo.
echo  Start it with:   build\DentalMacros.exe
echo.
echo  Note: this is a /MD build. Running it straight out of build\ works on
echo  this machine because Visual Studio is installed. The copy you hand to
echo  someone else must be the one package.bat makes, which brings the three
echo  runtime DLLs with it.
echo.
exit /b 0

:nocompiler
echo  ERROR: no Microsoft C++ compiler found.
echo.
echo  Install the C++ build tools with this command, then run build.bat again:
echo.
echo      winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
echo.
echo  Or open a "x64 Native Tools Command Prompt" and run build.bat there.
echo.
exit /b 1

:failed
echo.
echo  BUILD FAILED - see the errors above.
exit /b 1
