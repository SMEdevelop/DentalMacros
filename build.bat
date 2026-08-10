@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

echo.
echo  Building MacroDeck...
echo.

rem ---- locate a C++ compiler -------------------------------------------
set "GPP="
where g++ >nul 2>&1 && set "GPP=g++"

if "%GPP%"=="" (
  for %%D in (
    "%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
    "%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.MSVCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
    "C:\msys64\ucrt64\bin"
    "C:\msys64\mingw64\bin"
    "C:\mingw64\bin"
    "C:\ProgramData\mingw64\mingw64\bin"
  ) do (
    if exist "%%~D\g++.exe" (
      set "PATH=%%~D;!PATH!"
      set "GPP=g++"
      goto :found
    )
  )
)
:found

if "%GPP%"=="" (
  echo  ERROR: no C++ compiler found.
  echo.
  echo  Install one with this command, then run build.bat again:
  echo.
  echo      winget install BrechtSanders.WinLibs.POSIX.UCRT
  echo.
  exit /b 1
)

for /f "delims=" %%V in ('g++ --version') do (
  echo  Compiler: %%V
  goto :gotver
)
:gotver

if not exist build\resobj mkdir build\resobj

set CXXFLAGS=-std=c++17 -O2 -municode -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -Wall -Isrc

rem -static is what makes the result a genuinely single file. Without it the
rem exe imports libstdc++-6.dll, libgcc_s_seh-1.dll and libwinpthread-1.dll
rem from the compiler's own bin folder, so it starts on the machine that built
rem it and nowhere else. -s strips symbols; the static build still ends up
rem smaller than the dynamic exe plus those three DLLs.
set LDFLAGS=-static -static-libgcc -static-libstdc++ -s
set LIBS=-lgdiplus -lcomctl32 -lcomdlg32 -lshlwapi -lshell32 -ldwmapi -luxtheme -lmsimg32 -lgdi32 -luser32 -lole32 -loleaut32 -ladvapi32 -lpsapi

set SOURCES=src\main.cpp src\app.cpp src\dialogs.cpp src\ui_common.cpp src\theme.cpp src\engine.cpp src\config.cpp src\import_yaml.cpp src\winman.cpp src\vision.cpp src\actions.cpp src\device.cpp src\keys.cpp src\json.cpp src\util.cpp

echo  Compiling resources...
rem GCC's link spec always pulls in its own "default-manifest.o", which
rem collides with ours. Building an empty stand-in of that name into
rem build\resobj and pointing -B at it (a RELATIVE path - a path with spaces
rem breaks -B) lets app.res supply the real manifest instead.
windres src\empty.rc -O coff -o build\resobj\default-manifest.o
if errorlevel 1 goto :failed
windres src\app.rc -O coff -o build\app.res
if errorlevel 1 goto :failed

echo  Compiling and linking (this takes a minute)...
g++ %CXXFLAGS% -Bbuild/resobj/ %LDFLAGS% %SOURCES% build\app.res -o build\MacroDeck.exe -mwindows %LIBS%
if errorlevel 1 goto :failed

if not exist build\images mkdir build\images
if not exist build\layouts mkdir build\layouts

echo.
echo  Built:  build\MacroDeck.exe
echo.
echo  Start it with:   build\MacroDeck.exe
echo.
exit /b 0

:failed
echo.
echo  BUILD FAILED - see the errors above.
exit /b 1
