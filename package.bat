@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

rem  Builds Dental Macros and produces everything needed to hand it to someone
rem  else, in dist\:
rem
rem      DentalMacros-<version>.zip          portable copy
rem      DentalMacros-Setup-<version>.exe    installer (needs Inno Setup)
rem
rem  Since the move to MSVC the build links the C++ runtime dynamically (/MD),
rem  so both of those carry msvcp140.dll, vcruntime140.dll and
rem  vcruntime140_1.dll beside DentalMacros.exe. Without them the app dies on
rem  startup on any machine without Visual Studio installed, with a dialog
rem  naming the missing DLL. They are copied out of the Visual Studio
rem  redistributable folder, which is the supported way to ship them ("local
rem  deployment" - see Microsoft's "Deploying Visual C++ applications").
rem
rem  Signing is optional and off unless you point it at a certificate. Set
rem  ONE of these before running:
rem
rem      set DENTALMACROS_SIGN_PFX=C:\path\to\cert.pfx
rem      set DENTALMACROS_SIGN_PASS=...            (optional)
rem
rem  or, for anything that is not a plain .pfx - a hardware token, or a cloud
rem  service such as Azure Trusted Signing - give a command that takes the
rem  file to sign as its one argument:
rem
rem      set DENTALMACROS_SIGN_CMD=powershell -File C:\path\to\sign.ps1
rem
rem  That is for copies you build yourself. Official releases are built and
rem  signed by .github\workflows\release.yml, which calls this script as
rem
rem      package.bat --prebuilt
rem
rem  to package a build\DentalMacros.exe that has already come back signed,
rem  instead of building a fresh, unsigned one over the top of it.
rem
rem  The installer's setup engine needs a signature of its own - see "the
rem  setup engine" further down - so CI also runs
rem
rem      package.bat --engine-only
rem
rem  to write the engine out to be signed and stop there, and then
rem
rem      package.bat --prebuilt --signed-engine
rem
rem  to build the installer around the engine once it has come back signed.

set "PREBUILT="
set "ENGINEONLY="
set "SIGNEDENGINE="
for %%A in (%*) do (
  if /i "%%~A"=="--prebuilt"      set "PREBUILT=1"
  if /i "%%~A"=="--engine-only"   set "ENGINEONLY=1"
  if /i "%%~A"=="--signed-engine" set "SIGNEDENGINE=1"
)

echo.
echo  Packaging Dental Macros...
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

rem  --engine-only builds nothing and packages nothing.
if defined ENGINEONLY goto :installer

rem ---- build ----------------------------------------------------------
rem  Full path, not "build.bat": when NoDefaultCurrentDirectoryInExePath is
rem  set, or the script is launched from another shell, the current directory
rem  is not searched.
if defined PREBUILT (
  if not exist "build\DentalMacros.exe" (
    echo  ERROR: --prebuilt, but there is no build\DentalMacros.exe to package.
    exit /b 1
  )
  echo  Using the existing build\DentalMacros.exe  ^(--prebuilt^)
) else (
  call "%~dp0build.bat"
  if errorlevel 1 (
    echo  Packaging stopped: the build failed.
    exit /b 1
  )
)

rem ---- locate signtool, if we might need it ---------------------------
set "SIGNTOOL="
if defined DENTALMACROS_SIGN_PFX (
  for /f "delims=" %%S in ('where signtool.exe 2^>nul') do set "SIGNTOOL=%%S"
  if "!SIGNTOOL!"=="" (
    for /f "delims=" %%S in ('dir /b /s /o-n "%ProgramFiles(x86)%\Windows Kits\10\bin\*\x64\signtool.exe" 2^>nul') do (
      if "!SIGNTOOL!"=="" set "SIGNTOOL=%%S"
    )
  )
)

if not exist dist mkdir dist

rem ---- sign the executable -------------------------------------------
call :sign "build\DentalMacros.exe"
if errorlevel 1 exit /b 1

rem ---- portable zip ---------------------------------------------------
echo.
echo  Building the portable zip...
set "STAGE=dist\stage"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%\DentalMacros"
copy /y "build\DentalMacros.exe"     "%STAGE%\DentalMacros\" >nul
copy /y "packaging\INSTALL.txt"   "%STAGE%\DentalMacros\" >nul
copy /y "README.md"               "%STAGE%\DentalMacros\" >nul
copy /y "LICENSE"                 "%STAGE%\DentalMacros\" >nul

rem  The runtime. Copied one by one and checked, because a zip that is missing
rem  one of these looks perfectly fine here - this machine has them in
rem  System32 - and fails only on the machine you sent it to.
for %%L in (msvcp140.dll vcruntime140.dll vcruntime140_1.dll) do (
  if not exist "%CRTDIR%\%%L" (
    echo  ERROR: %%L is not in %CRTDIR%
    exit /b 1
  )
  copy /y "%CRTDIR%\%%L" "%STAGE%\DentalMacros\" >nul
  if errorlevel 1 (
    echo  ERROR: could not copy %%L into the package.
    exit /b 1
  )
  echo    + %%L
)

set "ZIP=dist\DentalMacros-%FULLVER%.zip"
if exist "%ZIP%" del /q "%ZIP%"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Compress-Archive -Path '%STAGE%\DentalMacros' -DestinationPath '%ZIP%' -Force"
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
set "SRCSTAGE=dist\srcstage\DentalMacros-%FULLVER%"
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

set "SRCZIP=dist\DentalMacros-%FULLVER%-source.zip"
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
:installer
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

if "!ISCC!"=="" if defined ENGINEONLY (
  echo  ERROR: --engine-only needs Inno Setup, and it is not installed.
  exit /b 1
)
if "!ISCC!"=="" (
  echo  Skipping the installer: Inno Setup is not installed.
  echo.
  echo      winget install JRSoftware.InnoSetup
  echo.
  echo  The portable zip above is finished and usable either way.
  goto :summary
)

rem ---- the setup engine -----------------------------------------------
rem  An Inno Setup installer unpacks a second program into %TEMP% and runs
rem  that: the setup engine, which is also the uninstaller. Signing the
rem  finished installer does not sign it, and Smart App Control refuses an
rem  unsigned engine whenever Microsoft's cloud has no verdict on that exact
rem  file - which, for a fresh build, is often. So every signed build gives
rem  the engine a signature of its own.
rem
rem  /DSignEngine turns on SignedUninstaller in installer.iss. The first
rem  compile for a given Inno Setup version, icon and app version then writes
rem  the engine to build\signed-engine\ and stops on purpose; once that file
rem  is signed, every compile after it embeds the signature.
set "SIGNENGINE="
if defined ENGINEONLY          set "SIGNENGINE=1"
if defined SIGNEDENGINE        set "SIGNENGINE=1"
if defined DENTALMACROS_SIGN_CMD set "SIGNENGINE=1"
if defined DENTALMACROS_SIGN_PFX set "SIGNENGINE=1"

rem  CrtDir is passed in so installer.iss can pick up the same three DLLs
rem  without repeating the lookup.
set ISCCARGS=/Q "/DAppVersion=%SHORTVER%" "/DAppVersionFull=%FULLVER%" "/DCrtDir=%CRTDIR%"
if defined SIGNENGINE (
  set ISCCARGS=!ISCCARGS! /DSignEngine
  if not exist "build\signed-engine" mkdir "build\signed-engine"
)
set "ISCCLOG=%TEMP%\dentalmacros-iscc-%RANDOM%.log"

if defined ENGINEONLY (echo  Writing out the setup engine...) else (echo  Building the installer...)
"!ISCC!" !ISCCARGS! "packaging\installer.iss" >"%ISCCLOG%" 2>&1
if not errorlevel 1 goto :installerbuilt

rem  A failed compile is either a real error or the deliberate stop above,
rem  which names the engine file on a line of its own.
set "ENGINE="
if defined SIGNENGINE (
  for /f "tokens=*" %%E in ('findstr /r /c:"signed-engine\\uninst-.*\.e32" "%ISCCLOG%"') do set "ENGINE=%%E"
)
if not defined ENGINE (
  type "%ISCCLOG%"
  del "%ISCCLOG%" 2>nul
  echo  ERROR: the installer failed to compile.
  exit /b 1
)
del "%ISCCLOG%" 2>nul
if defined ENGINEONLY (
  echo  Written, for signing:  !ENGINE!
  exit /b 0
)
if defined SIGNEDENGINE (
  echo  ERROR: --signed-engine, but this setup engine has not been signed:
  echo         !ENGINE!
  exit /b 1
)
call :sign "!ENGINE!"
if errorlevel 1 exit /b 1
"!ISCC!" !ISCCARGS! "packaging\installer.iss" >"%ISCCLOG%" 2>&1
if errorlevel 1 (
  type "%ISCCLOG%"
  del "%ISCCLOG%" 2>nul
  echo  ERROR: the installer failed to compile with the signed setup engine.
  exit /b 1
)

:installerbuilt
del "%ISCCLOG%" 2>nul
if defined ENGINEONLY (
  echo  This setup engine is already signed - nothing to write out.
  exit /b 0
)
call :sign "dist\DentalMacros-Setup-%FULLVER%.exe"
if errorlevel 1 exit /b 1
echo  Built:  dist\DentalMacros-Setup-%FULLVER%.exe

:summary
echo.
echo  ---------------------------------------------------------------
dir /b dist\*.zip dist\*.exe 2>nul
echo  ---------------------------------------------------------------
if not defined DENTALMACROS_SIGN_PFX if not defined DENTALMACROS_SIGN_CMD (
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
if defined DENTALMACROS_SIGN_CMD (
  echo  Signing %~nx1 ...
  call %DENTALMACROS_SIGN_CMD% "%~1"
  if errorlevel 1 (
    echo  ERROR: signing %~nx1 failed.
    exit /b 1
  )
  exit /b 0
)
if not defined DENTALMACROS_SIGN_PFX exit /b 0
if "!SIGNTOOL!"=="" (
  echo  ERROR: DENTALMACROS_SIGN_PFX is set but signtool.exe was not found.
  echo         Install the Windows SDK, or use DENTALMACROS_SIGN_CMD instead.
  exit /b 1
)
echo  Signing %~nx1 ...
set "PFXPASS="
if defined DENTALMACROS_SIGN_PASS set "PFXPASS=/p "%DENTALMACROS_SIGN_PASS%""
rem  /tr + /td timestamp the signature, so it stays valid after the
rem  certificate itself expires.
"!SIGNTOOL!" sign /fd SHA256 /td SHA256 /tr http://timestamp.digicert.com ^
  /f "%DENTALMACROS_SIGN_PFX%" !PFXPASS! "%~1"
if errorlevel 1 (
  echo  ERROR: signing %~nx1 failed.
  exit /b 1
)
exit /b 0
