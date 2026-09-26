; Inno Setup script — installs ac3spdif the way Windows apps install: into
; Program Files (or per-user, the wizard offers both), a Start Menu entry, an
; optional desktop icon, an optional PATH entry for the command-line tools,
; and a real uninstaller that undoes all of it, including the login service
; the app may have registered while it ran.
;
; The version and paths arrive as /D defines from scripts\build-installer.ps1,
; which greps the version out of Version.h — the same single source of truth
; every other artifact uses.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\build"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

[Setup]
; Fixed AppId so upgrades replace rather than duplicate.
AppId={{7E1A3F62-9C4B-4C0A-A1D8-5B6F0D2A64E1}
AppName=ac3spdif
AppVersion={#AppVersion}
AppPublisher=Mihir Joshi
AppPublisherURL=https://github.com/mihirj/ac3spdif
DefaultDirName={autopf}\ac3spdif
DefaultGroupName=ac3spdif
DisableProgramGroupPage=yes
LicenseFile={#SourceDir}\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename=ac3spdif-setup-{#AppVersion}
SetupIconFile={#SourceDir}\..\build\app.ico
UninstallDisplayIcon={app}\AC3SPDIFApp.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; Per-machine by default (Program Files), with the per-user alternative
; offered in the wizard rather than forbidden — no reason a machine without
; admin rights cannot run this.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; A running tray app holds its binaries open; let Restart Manager close it.
CloseApplications=yes
ChangesEnvironment=yes

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Shortcuts:"; Flags: unchecked
Name: "startupicon"; Description: "Start the tray app when &Windows starts"; GroupDescription: "Shortcuts:"; Flags: unchecked
Name: "addtopath"; Description: "Add the install folder to &PATH (for ac3spdif.exe and audioprobe.exe in a terminal)"; GroupDescription: "Command line:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\AC3SPDIFApp.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\ac3spdif.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\audioprobe.exe"; DestDir: "{app}"; Flags: ignoreversion
; The four FFmpeg libraries the binaries link against. Only FFmpeg DLLs live
; in build\, so the wildcard cannot pick up anything else.
Source: "{#SourceDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\FFMPEG-NOTICE.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LGPL-2.1.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\ac3spdif"; Filename: "{app}\AC3SPDIFApp.exe"
Name: "{autodesktop}\ac3spdif"; Filename: "{app}\AC3SPDIFApp.exe"; Tasks: desktopicon
; The Startup-folder shortcut launches the *tray app* at login. This is
; distinct from the app's own "Run as Background Service" item, which runs
; the engine headless — the two are the same split the macOS version keeps
; between login items and the LaunchAgent.
Name: "{autostartup}\ac3spdif"; Filename: "{app}\AC3SPDIFApp.exe"; Tasks: startupicon

[Run]
Filename: "{app}\AC3SPDIFApp.exe"; Description: "Launch ac3spdif"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Stop whatever is running before files disappear: the tray app, the headless
; service. Both hold the FFmpeg DLLs open, which would otherwise block their
; removal.
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM AC3SPDIFApp.exe"; Flags: runhidden; RunOnceId: "KillApp"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM ac3spdif.exe"; Flags: runhidden; RunOnceId: "KillCli"

[Code]
const
  EnvironmentKeyMachine = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';
  EnvironmentKeyUser = 'Environment';

function EnvironmentRootKey: Integer;
begin
  if IsAdminInstallMode then Result := HKEY_LOCAL_MACHINE
  else Result := HKEY_CURRENT_USER;
end;

function EnvironmentSubKey: String;
begin
  if IsAdminInstallMode then Result := EnvironmentKeyMachine
  else Result := EnvironmentKeyUser;
end;

// Append {app} to PATH, once. A blind append on every reinstall would grow
// PATH without bound, so the current value is checked first.
procedure AddInstallDirToPath;
var
  Path, Dir: String;
begin
  Dir := ExpandConstant('{app}');
  if not RegQueryStringValue(EnvironmentRootKey, EnvironmentSubKey, 'Path', Path) then
    Path := '';
  if Pos(';' + Lowercase(Dir) + ';', ';' + Lowercase(Path) + ';') > 0 then
    exit;
  if (Path <> '') and (Path[Length(Path)] <> ';') then
    Path := Path + ';';
  RegWriteExpandStringValue(EnvironmentRootKey, EnvironmentSubKey, 'Path', Path + Dir);
end;

procedure RemoveInstallDirFromPath;
var
  Path, Dir: String;
  Position: Integer;
begin
  Dir := ExpandConstant('{app}');
  if not RegQueryStringValue(EnvironmentRootKey, EnvironmentSubKey, 'Path', Path) then
    exit;
  Position := Pos(';' + Lowercase(Dir) + ';', ';' + Lowercase(Path) + ';');
  if Position = 0 then
    exit;
  // Position is within the padded copy; translate to the real string.
  Delete(Path, Position, Length(Dir) + 1);
  // The deletion above removes "dir;" or ";dir" depending on placement;
  // normalise any doubled or trailing separator it may leave.
  StringChangeEx(Path, ';;', ';', True);
  if (Path <> '') and (Path[Length(Path)] = ';') then
    SetLength(Path, Length(Path) - 1);
  if (Path <> '') and (Path[1] = ';') then
    Delete(Path, 1, 1);
  RegWriteExpandStringValue(EnvironmentRootKey, EnvironmentSubKey, 'Path', Path);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('addtopath') then
    AddInstallDirToPath;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    RemoveInstallDirFromPath;
    // The app's "Run as Background Service" writes a Run-key entry pointing
    // at the exe being removed; a login entry for a missing binary is the
    // kind of debris uninstallers exist to prevent. Settings under
    // HKCU\Software\ac3spdif are deliberately left, so a reinstall finds the
    // devices and buffer sizes it had before.
    RegDeleteValue(HKEY_CURRENT_USER,
      'Software\Microsoft\Windows\CurrentVersion\Run', 'ac3spdif');
  end;
end;
