@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

rem  Builds MacroDeck and produces everything needed to hand it to someone
rem  else, in dist\:
rem
rem      MacroDeck-<version>.zip          portable copy
rem      MacroDeck-Setup-<version>.exe    installer (needs Inno Setup)
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

rem ---- version: src\app.rc is the one place it is declared -------------
set "VERSION="
for /f tokens^=4^ delims^=^" %%V in ('findstr /c:"VALUE \"FileVersion\"" src\app.rc') do set "VERSION=%%V"
if "%VERSION%"=="" (
  echo  ERROR: could not read FileVersion out of src\app.rc
  exit /b 1
)
rem 1.0.0.0 reads better as 1.0.0 in a file name
for /f "tokens=1,2,3 delims=." %%a in ("%VERSION%") do set "SHORTVER=%%a.%%b.%%c"
echo  Version:  %SHORTVER%   (from src\app.rc)

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

set "ZIP=dist\MacroDeck-%SHORTVER%.zip"
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
set "SRCSTAGE=dist\srcstage\MacroDeck-%SHORTVER%"
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

set "SRCZIP=dist\MacroDeck-%SHORTVER%-source.zip"
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
"!ISCC!" /Q "/DAppVersion=%SHORTVER%" "packaging\installer.iss"
if errorlevel 1 (
  echo  ERROR: the installer failed to compile.
  exit /b 1
)
call :sign "dist\MacroDeck-Setup-%SHORTVER%.exe"
if errorlevel 1 exit /b 1
echo  Built:  dist\MacroDeck-Setup-%SHORTVER%.exe

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
