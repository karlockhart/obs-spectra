; OBS-Spectra installer (Inno Setup 6).
;
; Built by .github/scripts/Package-Spectra-Windows.ps1 from a release tree
; (what the regular zip holds):
;
;   ISCC.exe /DSourceDir=<tree> /DVersion=<tag> /DNumericVersion=a.b.c.d
;            /DEdition=Full|Lite|Ultralight /DOutputDir=<dir>
;            [/DRedist=<vc_redist.x64.exe>] [/DSign "/Sspectra=<command> $f"]
;            OBS-Spectra.iss
;
; Installs for the current user (no administrator rights) by default, or for
; all users in Program Files. Each edition has its own AppId and folder, so
; they can sit side by side, and settings stay in %USERPROFILE%\Spectra
; either way: the installer never makes a portable copy.
;
; Updating: Spectra's in-app updater downloads the new installer and runs it
; with /SILENT /WAITPID=<pid> /RELAUNCH (see SpectraUpdateJob::LaunchInstaller).
; The installer waits for Spectra to exit, replaces the program folders
; (bin, core, data, obs-plugins) so nothing of the old version is left to
; load, and starts Spectra again as the user who ran it. Running a newer
; installer by hand does the same, interactively.

#ifndef SourceDir
  #error Define SourceDir: the release tree to install
#endif
#ifndef Version
  #define Version "0.0.0-dev"
#endif
#ifndef NumericVersion
  #define NumericVersion "0.0.0.0"
#endif
#ifndef Edition
  #define Edition "Full"
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

#if Edition == "Full"
  #define AppName "OBS-Spectra"
  #define AppIdName "OBS-Spectra"
  #define NameSuffix ""
#elif Edition == "Lite"
  #define AppName "OBS-Spectra Lite"
  #define AppIdName "OBS-Spectra-Lite"
  #define NameSuffix "-Lite"
#elif Edition == "Ultralight"
  #define AppName "OBS-Spectra Ultralight"
  #define AppIdName "OBS-Spectra-Ultralight"
  #define NameSuffix "-Ultralight"
#else
  #error Edition must be Full, Lite or Ultralight
#endif

#define AppExe "bin\64bit\obs-spectra.exe"
#define RepoUrl "https://github.com/karlockhart/obs-spectra"
; The single-instance mutex of a non-portable OBS-Spectra (platform-windows.cpp)
#define AppMutexName "OBSSpectraCore"

[Setup]
; SpectraUpdateJob looks the install up under Uninstall\<AppId>_is1: keep in step
AppId={#AppIdName}
AppName={#AppName}
AppVersion={#Version}
AppVerName={#AppName} {#Version}
AppPublisher=OBS-Spectra
AppPublisherURL={#RepoUrl}
AppSupportURL={#RepoUrl}/issues
AppUpdatesURL={#RepoUrl}/releases
VersionInfoVersion={#NumericVersion}
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} Setup
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableWelcomePage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
OutputDir={#OutputDir}
OutputBaseFilename=OBS-Spectra-{#Version}-Windows-x64{#NameSuffix}-Setup
SetupIconFile={#SourcePath}..\cmake\windows\obs-studio.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
LZMANumBlockThreads=4
WizardStyle=modern
; Spectra is closed by /WAITPID or by the user; the Restart Manager catches
; anything else still holding a file (a Lucida Viewer, say)
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
#ifdef Sign
SignTool=spectra
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
english.LaunchApp=Start {#AppName}
english.CloseApp={#AppName} is running. Close it, then click OK to continue.
english.InstallingRedist=Installing the Microsoft Visual C++ runtime...
english.LucidaViewer=Lucida Viewer

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
; The program folders are the new version's alone: an old module left in core
; would still be loaded. Settings are in %USERPROFILE%\Spectra, never here. Only in a
; folder that holds OBS-Spectra already, never one someone else's bin is in.
Type: filesandordirs; Name: "{app}\bin"; Check: IsSpectraFolder
Type: filesandordirs; Name: "{app}\core"; Check: IsSpectraFolder
Type: filesandordirs; Name: "{app}\data"; Check: IsSpectraFolder
Type: filesandordirs; Name: "{app}\obs-plugins"; Check: IsSpectraFolder

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "portable_mode*,obs_portable_mode*,*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs
#ifdef Redist
Source: "{#Redist}"; DestDir: "{tmp}"; DestName: "vc_redist.x64.exe"; Flags: deleteafterinstall; Check: RedistNeeded
#endif

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}\bin\64bit"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; WorkingDir: "{app}\bin\64bit"; Tasks: desktopicon
#if Edition == "Full"
Name: "{autoprograms}\{#AppName} {cm:LucidaViewer}"; Filename: "{app}\bin\64bit\lucida-viewer.exe"; WorkingDir: "{app}\bin\64bit"; Check: FileExists(ExpandConstant('{app}\bin\64bit\lucida-viewer.exe'))
#endif

[Run]
#ifdef Redist
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "{cm:InstallingRedist}"; Flags: waituntilterminated; Check: RedistNeeded
#endif
; Interactive: the usual checkbox on the last page
Filename: "{app}\{#AppExe}"; WorkingDir: "{app}\bin\64bit"; Description: "{cm:LaunchApp}"; Flags: nowait postinstall skipifsilent
; An update from inside Spectra (/RELAUNCH): start it again, as the user, never elevated
Filename: "{app}\{#AppExe}"; WorkingDir: "{app}\bin\64bit"; Flags: nowait postinstall runasoriginaluser skipifnotsilent; Check: Relaunch

[UninstallRun]
; OBS-Spectra registers its virtual camera (for all users) the first time it is
; started; only an all-users uninstall has the rights to take it out again
Filename: "{sys}\regsvr32.exe"; Parameters: "/u /s ""{app}\core\win-dshow\data\obs-virtualcam-module64.dll"""; Flags: runhidden; Check: IsAdminInstallMode and FileExists(ExpandConstant('{app}\core\win-dshow\data\obs-virtualcam-module64.dll')); RunOnceId: "VirtualCam64"
Filename: "{syswow64}\regsvr32.exe"; Parameters: "/u /s ""{app}\core\win-dshow\data\obs-virtualcam-module32.dll"""; Flags: runhidden; Check: IsAdminInstallMode and FileExists(ExpandConstant('{app}\core\win-dshow\data\obs-virtualcam-module32.dll')); RunOnceId: "VirtualCam32"

[UninstallDelete]
; the installed files go by themselves; settings stay in %USERPROFILE%\Spectra
Type: dirifempty; Name: "{app}"

[Code]
const
  SYNCHRONIZE = $00100000;
  WAIT_TIMEOUT = $00000102;

function OpenProcess(Access: DWORD; Inherit: BOOL; Pid: DWORD): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(Handle: THandle; Milliseconds: DWORD): DWORD;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): BOOL;
  external 'CloseHandle@kernel32.dll stdcall';

function HasSwitch(const Name: String): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), Name) = 0 then
    begin
      Result := True;
      Exit;
    end;
end;

function IsSpectraFolder(): Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\{#AppExe}'));
end;

function Relaunch(): Boolean;
begin
  Result := HasSwitch('/RELAUNCH');
end;

{ The Visual C++ 2015-2026 runtime OBS-Spectra is built against }
function RedistNeeded(): Boolean;
var
  Installed, Major, Minor: Cardinal;
begin
  Result := True;
  if RegQueryDWordValue(HKLM64, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed) and
     (Installed = 1) and
     RegQueryDWordValue(HKLM64, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Major', Major) and
     RegQueryDWordValue(HKLM64, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Minor', Minor) then
    Result := (Major < 14) or ((Major = 14) and (Minor < 50));
end;

{ /WAITPID=<pid>: the Spectra that started this update; it exits on its own }
procedure WaitForCaller();
var
  Pid: Integer;
  Handle: THandle;
begin
  Pid := StrToIntDef(ExpandConstant('{param:WAITPID|0}'), 0);
  if Pid <= 0 then
    Exit;
  Handle := OpenProcess(SYNCHRONIZE, False, Pid);
  if Handle = 0 then
    Exit;
  Log(Format('Waiting for OBS-Spectra (process %d) to exit', [Pid]));
  if WaitForSingleObject(Handle, 120000) = WAIT_TIMEOUT then
    Log('OBS-Spectra is still running after two minutes');
  CloseHandle(Handle);
end;

function InitializeSetup(): Boolean;
var
  Tries: Integer;
begin
  Result := True;
  WaitForCaller();
  { Another copy may still be open: ask (or, silently, wait a little) }
  Tries := 0;
  while CheckForMutexes('{#AppMutexName}') do
  begin
    if WizardSilent() then
    begin
      Tries := Tries + 1;
      if Tries > 30 then
      begin
        Log('OBS-Spectra is still running; giving up');
        Result := False;
        Exit;
      end;
      Sleep(1000);
    end
    else if MsgBox(CustomMessage('CloseApp'), mbError, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  while CheckForMutexes('{#AppMutexName}') do
    if MsgBox(CustomMessage('CloseApp'), mbError, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
end;
