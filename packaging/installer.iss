; Inno Setup script for Dental Macros.
;
; Compile it through package.bat, not directly: that script reads the version
; out of src\app.rc and passes it in, so the version is declared in exactly
; one place.
;
; Per-user install by design. Dental Macros writes macros.json, backups\,
; layouts\ and snipped images next to its own executable, so it has to land
; somewhere the signed-in user can write. Installing to Program Files would
; mean every settings change failed unless the app ran as administrator.
; Installing under %LOCALAPPDATA% also means no UAC prompt at install time.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
; AppVersionFull carries the pre-release marker ("0.7.0-beta") where
; AppVersion is only ever digits, because VersionInfoVersion below refuses
; anything else. package.bat passes both in.
#ifndef AppVersionFull
  #define AppVersionFull AppVersion
#endif
; Where the three Visual C++ runtime DLLs are, passed in by package.bat.
#ifndef CrtDir
  #define CrtDir ""
#endif

#define AppName      "Dental Macros"
#define AppExeName   "DentalMacros.exe"
#define AppPublisher "SMEdevelop"

[Setup]
; Never change AppId - it is how Windows recognises an existing install and
; offers an upgrade instead of a second copy.
AppId={{8F3C1A62-5D47-4E9B-9C2A-71B0E6D4A153}
AppName={#AppName}
AppVersion={#AppVersionFull}
AppVerName={#AppName} {#AppVersionFull}
AppPublisher={#AppPublisher}
; Digits only - Inno rejects "0.7.0-beta" here, so this one stays numeric.
VersionInfoVersion={#AppVersion}

; An upgrade from MacroDeck - same AppId - reuses that install's folder
; (Programs\MacroDeck), because UsePreviousAppDir is on by default. That is
; what keeps someone's macros.json, which lives beside the exe, in place.
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
LicenseFile=LICENSE

; "lowest" keeps the whole install inside the user's own profile: no admin
; prompt, and nothing lands anywhere the app cannot later write to.
PrivilegesRequired=lowest

; Paths below are relative to the repository root, not to this file.
SourceDir=..
OutputDir=dist
OutputBaseFilename=DentalMacros-Setup-{#AppVersionFull}

Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExeName}
; So Setup.exe itself carries the app's icon rather than Inno's default -
; a generic icon on a downloaded installer reads as untrustworthy.
SetupIconFile=src\app.ico

; Signs the setup engine, the second program Setup unpacks to %TEMP% and
; runs - see "the setup engine" in package.bat for why it matters. Only
; signed builds pass SignEngine: in this mode the first compile for each new
; version stops until the engine file it writes out has been signed, which
; an unsigned build would never get past.
#ifdef SignEngine
SignedUninstaller=yes
SignedUninstallerDir=build\signed-engine
#endif

; The app allows only one instance (a second one would fight over the same
; keyboard hook). Naming its mutex here makes Setup notice a running copy and
; ask the user to close it, instead of failing on a locked file. The second
; name is the mutex the app held when it was still called MacroDeck.
AppMutex=DentalMacros.SingleInstance,MacroDeck.SingleInstance

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "startup"; Description: "Run {#AppName} when I sign in to Windows"; GroupDescription: "Startup:"
Name: "desktopicon"; Description: "Create a shortcut on the desktop"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "build\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.md";           DestDir: "{app}"; DestName: "README.md"; Flags: ignoreversion
Source: "LICENSE";             DestDir: "{app}"; Flags: ignoreversion
Source: "packaging\INSTALL.txt"; DestDir: "{app}"; Flags: ignoreversion
; The Visual C++ runtime, installed beside the exe rather than into the
; system. Dental Macros is a per-user install with PrivilegesRequired=lowest, so
; it cannot write to System32 and must not try - and this way uninstalling
; takes its own copy with it and cannot break another program's. Without
; these three the app fails to start on any machine that has never had
; Visual Studio or the VC++ Redistributable on it.
Source: "{#CrtDir}\msvcp140.dll";      DestDir: "{app}"; Flags: ignoreversion
Source: "{#CrtDir}\vcruntime140.dll";  DestDir: "{app}"; Flags: ignoreversion
Source: "{#CrtDir}\vcruntime140_1.dll"; DestDir: "{app}"; Flags: ignoreversion

; Left behind by versions from before the rename to Dental Macros. The old
; exe would otherwise sit beside the new one, and its shortcuts would start
; a program that is no longer the current one.
[InstallDelete]
Type: files; Name: "{app}\MacroDeck.exe"
Type: files; Name: "{autoprograms}\MacroDeck.lnk"
Type: files; Name: "{autodesktop}\MacroDeck.lnk"

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\{#AppName}";  Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Registry]
; Written in exactly the form Settings -> "Run Dental Macros when I sign in to
; Windows" writes it (same value name, same quoted path), so the checkbox
; inside the app agrees with what the installer did and can turn it back off.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "DentalMacros"; \
    ValueData: """{app}\{#AppExeName}"""; \
    Flags: uninsdeletevalue; Tasks: startup

[Run]
Filename: "{app}\{#AppExeName}"; Description: "Start {#AppName} now"; \
    Flags: nowait postinstall skipifsilent

[Code]
const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';

// True when a Run entry starts a program inside this install's folder.
function RunsFromApp(const Cmd: String): Boolean;
begin
  Result := Pos(Lowercase(ExpandConstant('{app}\')), Lowercase(Cmd)) > 0;
end;

// Before the rename the app registered itself under the value "MacroDeck",
// pointing at MacroDeck.exe - which [InstallDelete] has just removed. Move
// that entry to the new name and exe, so someone who had "run at sign-in"
// on still has it, rather than a startup entry for a file that is gone.
procedure CurStepChanged(CurStep: TSetupStep);
var
  Old: String;
begin
  if CurStep <> ssPostInstall then
    Exit;
  if RegQueryStringValue(HKCU, RunKey, 'MacroDeck', Old) and RunsFromApp(Old) then
  begin
    RegWriteStringValue(HKCU, RunKey, 'DentalMacros',
                        '"' + ExpandConstant('{app}\{#AppExeName}') + '"');
    RegDeleteValue(HKCU, RunKey, 'MacroDeck');
  end;
end;

// Uninstalling removes the program. Whether it also removes the user's
// macros is their call, not ours - so ask, and default to keeping them.
procedure CurUninstallStepChanged(CurStep: TUninstallStep);
var
  AppDir: String;
  Gap: String;
  Msg: String;
  Cmd: String;
begin
  // The [Registry] entry only covers a startup entry the installer itself
  // wrote. One the app wrote from Settings, or one moved over from MacroDeck
  // above, would otherwise outlive the program it starts.
  if (CurStep = usUninstall) and
     RegQueryStringValue(HKCU, RunKey, 'DentalMacros', Cmd) and RunsFromApp(Cmd) then
    RegDeleteValue(HKCU, RunKey, 'DentalMacros');

  if CurStep <> usPostUninstall then
    Exit;

  AppDir := ExpandConstant('{app}');
  if not FileExists(AppDir + '\macros.json') then
  begin
    RemoveDir(AppDir);
    Exit;
  end;

  // Built up in a variable rather than one long expression: Inno's
  // preprocessor reads a '#' at the start of a line as a directive, so a
  // wrapped line may never begin with #13#10.
  Gap := Chr(13) + Chr(10) + Chr(13) + Chr(10);
  Msg := 'Also delete your macros and settings?' + Gap +
         'This removes macros.json, your saved window layouts, snipped ' +
         'template images and backups from:' + Gap + AppDir + Gap +
         'Choose No to keep them.';

  // SuppressibleMsgBox, not MsgBox: a silent uninstall (/VERYSILENT
  // /SUPPRESSMSGBOXES, which is what upgrade and deployment tooling uses)
  // never shows this, and plain MsgBox answers such prompts affirmatively.
  // That would delete someone's macros without ever asking. The explicit
  // IDNO default means the quiet path keeps their data.
  if SuppressibleMsgBox(Msg, mbConfirmation, MB_YESNO or MB_DEFBUTTON2,
                        IDNO) = IDYES then
  begin
    DeleteFile(AppDir + '\macros.json');
    DelTree(AppDir + '\backups', True, True, True);
    DelTree(AppDir + '\layouts', True, True, True);
    DelTree(AppDir + '\images', True, True, True);
    RemoveDir(AppDir);
  end;
end;
