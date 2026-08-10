@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

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

echo  Building the self-test...
rem engine.cpp brings in device.cpp and actions.cpp. The tests only ever run
rem steps with no side effects, so nothing here types, clicks or launches
rem anything - but the dispatch table and error paths do get exercised.
g++ -std=c++17 -O1 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -Isrc ^
  tests\selftest.cpp src\config.cpp src\import_yaml.cpp src\json.cpp ^
  src\keys.cpp src\util.cpp src\vision.cpp src\winman.cpp ^
  src\engine.cpp src\actions.cpp src\device.cpp ^
  -o build\selftest.exe ^
  -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -ladvapi32 -lpsapi
if errorlevel 1 ( echo  BUILD FAILED & exit /b 1 )

rem Pass a macrokb macros.yaml as the first argument to also test the importer.
set "YAML=%~1"
if "%YAML%"=="" if exist "..\Macros\macros.yaml" set "YAML=..\Macros\macros.yaml"

echo.
build\selftest.exe "%YAML%"
set "TESTRC=%errorlevel%"

echo.
echo  Rendering the drawing preview...
g++ -std=c++17 -O1 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -Isrc ^
  tests\render_preview.cpp src\theme.cpp src\config.cpp src\import_yaml.cpp ^
  src\json.cpp src\keys.cpp src\util.cpp ^
  -o build\preview.exe -lgdiplus -lgdi32 -luser32 -lshell32 -lole32
if not errorlevel 1 build\preview.exe

rem  Keep a committed copy: build\ is ignored by git, and README.md shows
rem  this image, so it has to live somewhere that survives a clean checkout.
if exist build\ui-preview.png (
  if not exist docs mkdir docs
  copy /y build\ui-preview.png docs\ui-preview.png >nul
)

exit /b %TESTRC%
