; Pitel Instant Replay - Windows installer
; Copyright (C) 2026 Alexander Pitel

#define ProductName "Pitel Instant Replay for OBS Studio"
#ifndef MyVersion
  #define MyVersion "1.0.0"
#endif
#define ProductPublisher "videolab666"
#define ProductURL "https://github.com/videolab666/obinstrp"

[Setup]
AppName={#ProductName}
AppVersion={#MyVersion}
AppPublisher={#ProductPublisher}
AppPublisherURL={#ProductURL}
AppSupportURL={#ProductURL}
DefaultDirName={code:ResolveOBSDirectory}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
DisableReadyPage=no
UninstallDisplayName={#ProductName}
UninstallDisplayIcon={app}\bin\64bit\obs64.exe
OutputBaseFilename=pitel-instant-replay-{#MyVersion}-windows-installer
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
LicenseFile=LICENSE.txt

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Files]
Source: "pitel-instant-replay\bin\64bit\pitel-instant-replay.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "pitel-instant-replay\data\locale\*"; DestDir: "{app}\data\obs-plugins\pitel-instant-replay\locale"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
Type: filesandordirs; Name: "{app}\data\obs-plugins\pitel-instant-replay"

[Code]
function ResolveOBSDirectory(Param: String): String;
var
  InstalledPath: String;
begin
  InstalledPath := '';
  if RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', InstalledPath) and (InstalledPath <> '') then
    Result := InstalledPath
  else if RegQueryStringValue(HKLM, 'SOFTWARE\OBS Studio', '', InstalledPath) and (InstalledPath <> '') then
    Result := InstalledPath
  else
    Result := ExpandConstant('{commonpf}\obs-studio');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  ContinueInstall: Integer;
begin
  Result := True;
  if CurPageID <> wpSelectDir then
    Exit;

  if FileExists(ExpandConstant('{app}\bin\64bit\obs64.exe')) then
    Exit;

  ContinueInstall := MsgBox(
    'The selected directory does not look like an OBS Studio installation.' + #13#10 +
    'Expected: bin\64bit\obs64.exe' + #13#10#13#10 +
    'Install to this directory anyway?',
    mbConfirmation, MB_YESNO);
  Result := ContinueInstall = IDYES;
end;
