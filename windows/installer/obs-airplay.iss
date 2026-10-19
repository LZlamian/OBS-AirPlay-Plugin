; SPDX-License-Identifier: GPL-3.0-or-later
; OBS AirPlay installer for Windows x64 (Inno Setup 6). Built by
; scripts/package-windows.sh, which passes AppVersion, SourceDir (the staged
; obs-airplay folder), OutputDir and OutputBaseName.
;
; Installs the plugin where OBS Studio 30+ looks for per-machine plugins,
;   %ProgramData%\obs-studio\plugins\obs-airplay
; and allows obs64.exe through Windows Defender Firewall on private networks
; (AirPlay senders connect to the receiver on TCP 7000 and UDP ports).

#ifndef AppVersion
  #error AppVersion is not set (run scripts/package-windows.sh)
#endif

[Setup]
AppId={{6E1E9B4C-7C1F-4E4E-9C55-0B3A3D2C5A11}
AppName=OBS AirPlay
AppVersion={#AppVersion}
AppVerName=OBS AirPlay {#AppVersion}
AppPublisher=LZlamian and contributors
AppPublisherURL=https://github.com/LZlamian/OBS-AirPlay-Plugin
AppSupportURL=https://github.com/LZlamian/OBS-AirPlay-Plugin/issues
LicenseFile={#SourceDir}\LICENSE.txt
DefaultDirName={commonappdata}\obs-studio\plugins\obs-airplay
DisableDirPage=yes
DisableProgramGroupPage=yes
UsePreviousAppDir=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; OBS 30+ (the folder above) runs on Windows 10 and later.
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; OBS holds the DLL open while it runs.
CloseApplications=yes
RestartApplications=no
UninstallDisplayName=OBS AirPlay
UninstallDisplayIcon={sys}\shell32.dll,22
VersionInfoVersion={#AppVersion}
VersionInfoDescription=OBS AirPlay plugin installer

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; Files of an earlier version that this one no longer ships.
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\data"
Type: filesandordirs; Name: "{app}\licenses"

[Tasks]
Name: firewall; Description: "Allow OBS through Windows Firewall on private networks (needed for AirPlay)"

[Run]
; One rule for obs64.exe, replaced on every install (no duplicates).
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""OBS AirPlay"""; Flags: runhidden; Tasks: firewall
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""OBS AirPlay"" dir=in action=allow enable=yes profile=private program=""{code:ObsExecutable}"" description=""AirPlay receiver in OBS Studio (OBS AirPlay plugin)"""; Flags: runhidden; Tasks: firewall; Check: ObsFound

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""OBS AirPlay"""; Flags: runhidden; RunOnceId: "RemoveFirewallRule"

[Code]
var
  ObsPath: String;

// /WAITPID=<pid>: started by the plugin's updater from inside OBS; install
// once that OBS process has quit (it holds obs-airplay.dll open).
function OpenProcess(DesiredAccess: Cardinal; InheritHandle: Cardinal; ProcessId: Cardinal): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(Handle: THandle; Milliseconds: Cardinal): Cardinal;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Cardinal;
  external 'CloseHandle@kernel32.dll stdcall';

function InitializeSetup(): Boolean;
var
  ProcessId: Cardinal;
  Process: THandle;
begin
  ProcessId := StrToIntDef(ExpandConstant('{param:WAITPID|0}'), 0);
  if ProcessId <> 0 then begin
    Process := OpenProcess($00100000 { SYNCHRONIZE }, 0, ProcessId);
    if Process <> 0 then begin
      Log(Format('Waiting for OBS (process %d) to quit', [ProcessId]));
      WaitForSingleObject(Process, $FFFFFFFF);
      CloseHandle(Process);
    end;
  end;
  Result := True;
end;

// obs64.exe of the installed OBS Studio: its installer records the folder
// as the default value of HKLM\SOFTWARE\OBS Studio.
function ObsExecutable(Param: String): String;
var
  Folder: String;
begin
  if ObsPath = '' then begin
    if RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Folder) then
      ObsPath := AddBackslash(Folder) + 'bin\64bit\obs64.exe';
    if (ObsPath = '') or not FileExists(ObsPath) then
      ObsPath := ExpandConstant('{commonpf64}\obs-studio\bin\64bit\obs64.exe');
  end;
  Result := ObsPath;
end;

function ObsFound(): Boolean;
begin
  Result := FileExists(ObsExecutable(''));
end;

function LegacyCopy(): String;
begin
  // <OBS>\bin\64bit\obs64.exe -> <OBS>\obs-plugins\64bit\obs-airplay.dll
  Result := ExtractFileDir(ExtractFileDir(ExtractFileDir(ObsExecutable(''))))
            + '\obs-plugins\64bit\obs-airplay.dll';
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpReady) and not ObsFound() then
    MsgBox('OBS Studio was not found in the usual place. The plugin is installed anyway ' +
           '(OBS 30 or later loads it from ProgramData), but the firewall rule is not added: ' +
           'allow OBS through Windows Firewall yourself if devices cannot connect.',
           mbInformation, MB_OK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  // A copy installed by hand next to OBS's own plugins would load as well.
  if (CurStep = ssPostInstall) and FileExists(LegacyCopy()) then
    MsgBox('Another copy of obs-airplay.dll is in OBS''s own plugin folder:' + #13#10 +
           LegacyCopy() + #13#10 +
           'Delete it, or OBS loads two copies.', mbInformation, MB_OK);
end;
