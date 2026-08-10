@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0.."

rem  Regenerates src\app.ico from drawAppIcon() in src\theme.cpp.
rem
rem  The .ico is committed, so build.bat never needs this. Run it only after
rem  changing how the icon is drawn.

set "GPP="
where g++ >nul 2>&1 && set "GPP=g++"
if "%GPP%"=="" (
  for %%D in (
    "%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
    "C:\msys64\ucrt64\bin"
    "C:\mingw64\bin"
  ) do (
    if exist "%%~D\g++.exe" ( set "PATH=%%~D;!PATH!" & set "GPP=g++" & goto :found )
  )
)
:found
if "%GPP%"=="" ( echo  ERROR: no C++ compiler found - see build.bat & exit /b 1 )

if not exist build mkdir build

echo  Building the icon exporter...
g++ -std=c++17 -O1 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -Isrc ^
  tools\make_icon.cpp src\theme.cpp src\util.cpp ^
  -o build\make_icon.exe ^
  -lgdiplus -lgdi32 -luser32 -lole32 -ladvapi32 -lshell32
if errorlevel 1 ( echo  BUILD FAILED & exit /b 1 )

echo.
build\make_icon.exe
if errorlevel 1 exit /b 1

echo.
echo  Rebuild the app to pick it up:  build.bat
exit /b 0
