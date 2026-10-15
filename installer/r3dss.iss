; Inno Setup script for R3D Supersampler.
; Build: ISCC.exe /DSourceDir=<cmake install prefix> /DAppVersion=<x.y.z> installer\r3dss.iss
#ifndef SourceDir
  #define SourceDir "..\dist"
#endif
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif

[Setup]
AppId={{8C1B9D2E-4C3B-4C8E-9F0B-5A2A3F6E7D11}
AppName=R3D Supersampler
AppVersion={#AppVersion}
AppPublisher=Anchorpoint Studio
DefaultDirName={autopf}\R3D Supersampler
DefaultGroupName=R3D Supersampler
OutputDir=Output
OutputBaseFilename=R3DSupersampler-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
LicenseFile={#SourceDir}\LICENSE
UninstallDisplayIcon={app}\bin\R3DSupersampler.exe

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked
Name: "addtopath"; Description: "Add r3dss-cli to the user PATH"; GroupDescription: "Command line:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\bin\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs
Source: "{#SourceDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\R3D Supersampler"; Filename: "{app}\bin\R3DSupersampler.exe"
Name: "{group}\Documentation"; Filename: "{app}\docs"
Name: "{group}\Uninstall R3D Supersampler"; Filename: "{uninstallexe}"
Name: "{autodesktop}\R3D Supersampler"; Filename: "{app}\bin\R3DSupersampler.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}\bin"; Tasks: addtopath; Check: NeedsAddPath(ExpandConstant('{app}\bin'))

[Run]
Filename: "{app}\bin\R3DSupersampler.exe"; Description: "Launch R3D Supersampler"; Flags: nowait postinstall skipifsilent

[Code]
function NeedsAddPath(Param: string): boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', OrigPath) then
  begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Uppercase(Param) + ';', ';' + Uppercase(OrigPath) + ';') = 0;
end;
