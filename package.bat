@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

rem  Builds MacroDeck and produces everything needed to hand it to someone
rem  else, in dist\:
rem
rem      MacroDeck-<version>.zip          portable copy
rem      MacroDeck-Setup-<version>.exe    installer (needs Inno Setup)
rem
rem  Since the move to MSVC the build links the C++ runtime dynamically (/MD),
rem  so both of those carry msvcp140.dll, vcruntime140.dll and
rem  vcruntime140_1.dll beside MacroDeck.exe. Without them the app dies on
rem  startup on any machine without Visual Studio installed, with a dialog
rem  naming the missing DLL. They are copied out of the Visual Studio
rem  redistributable folder, which is the supported way to ship them ("local
rem  deployment" - see Microsoft's "Deploying Visual C++ applications").
rem
rem  Signing is optional and off unless you point it at a certificate. Set
rem  ONE of these before running:
rem
rem      set MACRODECK_SIGN_PFX=C:\path\to\cert.pfx
rem      set MACRODECK_SIGN_PASS=...            (optional)
rem
rem  or, for anything that is not a plain .pfx - a hardware token, or a cloud
rem  service such as Azure Trusted Signing - give a command that takes the
rem  file to sign as its one argument:
rem
rem      set MACRODECK_SIGN_CMD=powershell -File C:\path\to\sign.ps1

echo.
echo  Packaging MacroDeck...
echo.

rem ---- MSVC environment ------------------------------------------------
rem  Done here rather than leaning on build.bat because we need
rem  VCToolsRedistDir, and build.bat's setlocal throws its environment away
rem  when it returns. With cl.exe already on PATH, build.bat below skips
rem  straight past its own detection.
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

rem ---- the three runtime DLLs -----------------------------------------
rem  The folder is named for the toolset (Microsoft.VC145.CRT today), so match
rem  it with a wildcard instead of hard-coding a number that changes with
rem  every Visual Studio upgrade.
set "CRTDIR="
for /d %%D in ("%VCToolsRedistDir%x64\Microsoft.VC*.CRT") do set "CRTDIR=%%D"
if "%CRTDIR%"=="" (
  echo  ERROR: could not find the Visual C++ redistributable DLLs.
  echo         Looked under: %VCToolsRedistDir%x64\
  echo         Install the "C++ x64/x86 build tools" component in the Visual
  echo         Studio Installer, which is what brings that folder in.
  exit /b 1
)
echo  Runtime DLLs: %CRTDIR%

rem ---- version: src\app.rc is the one place it is declared -------------
set "VERSION="
for /f tokens^=4^ delims^=^" %%V in ('findstr /c:"VALUE \"FileVersion\"" src\app.rc') do set "VERSION=%%V"
if "%VERSION%"=="" (
  echo  ERROR: could not read FileVersion out of src\app.rc
  exit /b 1
)
rem 0.7.0.0 reads better as 0.7.0 in a file name
for /f "tokens=1,2,3 delims=." %%a in ("%VERSION%") do set "SHORTVER=%%a.%%b.%%c"

rem  Pre-release marker, carried in the file names so a beta is never mistaken
rem  for a final build sitting in the same folder. Clear it for a real release.
set "PRERELEASE=beta"
if defined PRERELEASE (set "FULLVER=%SHORTVER%-%PRERELEASE%") else (set "FULLVER=%SHORTVER%")
echo  Version:  %FULLVER%   (from src\app.rc)

rem ---- build ----------------------------------------------------------
rem  Full path, not "build.bat": when NoDefaultCurrentDirectoryInExePath is
rem  set, or the script is launched from another shell, the current directory
rem  is not searched.
call "%~dp0build.bat"
if errorlevel 1 (
  echo  Packaging stopped: the build failed.
  exit /b 1
)

rem ---- locate signtool, if we might need it ---------------------------
set "SIGNTOOL="
if defined MACRODECK_SIGN_PFX (
  for /f "delims=" %%S in ('where signtool.exe 2^>nul') do set "SIGNTOOL=%%S"
  if "!SIGNTOOL!"=="" (
    for /f "delims=" %%S in ('dir /b /s /o-n "%ProgramFiles(x86)%\Windows Kits\10\bin\*\x64\signtool.exe" 2^>nul') do (
      if "!SIGNTOOL!"=="" set "SIGNTOOL=%%S"
    )
  )
)

if not exist dist mkdir dist

rem ---- sign the executable -------------------------------------------
call :sign "build\MacroDeck.exe"
if errorlevel 1 exit /b 1

rem ---- portable zip ---------------------------------------------------
echo.
echo  Building the portable zip...
set "STAGE=dist\stage"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%\MacroDeck"
copy /y "build\MacroDeck.exe"     "%STAGE%\MacroDeck\" >nul
copy /y "packaging\INSTALL.txt"   "%STAGE%\MacroDeck\" >nul
copy /y "README.md"               "%STAGE%\MacroDeck\" >nul
copy /y "LICENSE"                 "%STAGE%\MacroDeck\" >nul

rem  The runtime. Copied one by one and checked, because a zip that is missing
rem  one of these looks perfectly fine here - this machine has them in
rem  System32 - and fails only on the machine you sent it to.
for %%L in (msvcp140.dll vcruntime140.dll vcruntime140_1.dll) do (
  if not exist "%CRTDIR%\%%L" (
    echo  ERROR: %%L is not in %CRTDIR%
    exit /b 1
  )
  copy /y "%CRTDIR%\%%L" "%STAGE%\MacroDeck\" >nul
  if errorlevel 1 (
    echo  ERROR: could not copy %%L into the package.
    exit /b 1
  )
  echo    + %%L
)

set "ZIP=dist\MacroDeck-%FULLVER%.zip"
if exist "%ZIP%" del /q "%ZIP%"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Compress-Archive -Path '%STAGE%\MacroDeck' -DestinationPath '%ZIP%' -Force"
if errorlevel 1 (
  echo  ERROR: could not create the zip.
  exit /b 1
)
rmdir /s /q "%STAGE%"
echo  Built:  %ZIP%

rem ---- source snapshot ------------------------------------------------
rem  Everything needed to rebuild from scratch, and nothing that is either
rem  generated or belongs to whoever ran the app. Suitable for attaching to
rem  a GitHub release.
echo.
echo  Building the source zip...
set "SRCSTAGE=dist\srcstage\MacroDeck-%FULLVER%"
if exist "dist\srcstage" rmdir /s /q "dist\srcstage"
mkdir "%SRCSTAGE%"
xcopy /e /i /q /y "src"       "%SRCSTAGE%\src"       >nul
xcopy /e /i /q /y "tests"     "%SRCSTAGE%\tests"     >nul
xcopy /e /i /q /y "tools"     "%SRCSTAGE%\tools"     >nul
xcopy /e /i /q /y "packaging" "%SRCSTAGE%\packaging" >nul
xcopy /e /i /q /y ".vscode"   "%SRCSTAGE%\.vscode"   >nul
if exist "docs" xcopy /e /i /q /y "docs" "%SRCSTAGE%\docs" >nul
for %%F in (README.md LICENSE .gitignore build.bat run_tests.bat package.bat) do (
  copy /y "%%F" "%SRCSTAGE%\" >nul
)

set "SRCZIP=dist\MacroDeck-%FULLVER%-source.zip"
if exist "%SRCZIP%" del /q "%SRCZIP%"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Compress-Archive -Path '%SRCSTAGE%' -DestinationPath '%SRCZIP%' -Force"
if errorlevel 1 (
  echo  ERROR: could not create the source zip.
  exit /b 1
)
rmdir /s /q "dist\srcstage"
echo  Built:  %SRCZIP%

rem ---- installer ------------------------------------------------------
echo.
rem  winget installs Inno per-user by default, so %LOCALAPPDATA% has to be
rem  checked as well as the two Program Files locations.
set "ISCC="
for %%D in (
  "%LOCALAPPDATA%\Programs\Inno Setup 6"
  "%ProgramFiles(x86)%\Inno Setup 6"
  "%ProgramFiles%\Inno Setup 6"
) do (
  if "!ISCC!"=="" if exist "%%~D\ISCC.exe" set "ISCC=%%~D\ISCC.exe"
)
if "!ISCC!"=="" for /f "delims=" %%S in ('where iscc.exe 2^>nul') do set "ISCC=%%S"

if "!ISCC!"=="" (
  echo  Skipping the installer: Inno Setup is not installed.
  echo.
  echo      winget install JRSoftware.InnoSetup
  echo.
  echo  The portable zip above is finished and usable either way.
  goto :summary
)

echo  Building the installer...
rem  CrtDir is passed in so installer.iss can pick up the same three DLLs
rem  without repeating the lookup.
"!ISCC!" /Q "/DAppVersion=%SHORTVER%" "/DAppVersionFull=%FULLVER%" "/DCrtDir=%CRTDIR%" "packaging\installer.iss"
if errorlevel 1 (
  echo  ERROR: the installer failed to compile.
  exit /b 1
)
call :sign "dist\MacroDeck-Setup-%FULLVER%.exe"
if errorlevel 1 exit /b 1
echo  Built:  dist\MacroDeck-Setup-%FULLVER%.exe

:summary
echo.
echo  ---------------------------------------------------------------
dir /b dist\*.zip dist\*.exe 2>nul
echo  ---------------------------------------------------------------
if not defined MACRODECK_SIGN_PFX if not defined MACRODECK_SIGN_CMD (
  echo.
  echo  NOT SIGNED. Anyone you send this to will meet SmartScreen's
  echo  "Windows protected your PC" box and have to click through it,
  echo  and Smart App Control may refuse the file outright.
  echo  See the top of this script for how to turn signing on.
)
echo.
exit /b 0

:nocompiler
echo  ERROR: no Microsoft C++ compiler found - see build.bat
exit /b 1

rem ---------------------------------------------------------------------
rem  :sign <file>   - signs in place, or does nothing if no cert is set up
rem ---------------------------------------------------------------------
:sign
if defined MACRODECK_SIGN_CMD (
  echo  Signing %~nx1 ...
  call %MACRODECK_SIGN_CMD% "%~1"
  if errorlevel 1 (
    echo  ERROR: signing %~nx1 failed.
    exit /b 1
  )
  exit /b 0
)
if not defined MACRODECK_SIGN_PFX exit /b 0
if "!SIGNTOOL!"=="" (
  echo  ERROR: MACRODECK_SIGN_PFX is set but signtool.exe was not found.
  echo         Install the Windows SDK, or use MACRODECK_SIGN_CMD instead.
  exit /b 1
)
echo  Signing %~nx1 ...
set "PFXPASS="
if defined MACRODECK_SIGN_PASS set "PFXPASS=/p "%MACRODECK_SIGN_PASS%""
rem  /tr + /td timestamp the signature, so it stays valid after the
rem  certificate itself expires.
"!SIGNTOOL!" sign /fd SHA256 /td SHA256 /tr http://timestamp.digicert.com ^
  /f "%MACRODECK_SIGN_PFX%" !PFXPASS! "%~1"
if errorlevel 1 (
  echo  ERROR: signing %~nx1 failed.
  exit /b 1
)
exit /b 0
