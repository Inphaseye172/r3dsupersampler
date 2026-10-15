; Inno Setup script for r3dss (preview; product name not chosen, see docs/IMPLEMENTATION_PLAN.md D7).
; Build: ISCC.exe /DSourceDir=<cmake install prefix> /DAppVersion=<x.y.z> installer\r3dss.iss
#ifndef SourceDir
  #define SourceDir "..\dist"
#endif
#ifndef AppVersion
  #define AppVersion "0.2.0"
#endif

[Setup]
AppId={{3F1A6C8E-2D47-4B9A-9E15-7C0B2A4D8E63}
AppName=r3dss (preview)
AppVersion={#AppVersion}
AppPublisher=Anchor Point Studio
DefaultDirName={autopf}\r3dss
DefaultGroupName=r3dss
OutputDir=Output
OutputBaseFilename=r3dss-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
LicenseFile={#SourceDir}\LICENSE
UninstallDisplayIcon={app}\bin\r3dss-gui.exe

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked
Name: "addtopath"; Description: "Add r3dss-cli to the user PATH"; GroupDescription: "Command line:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\bin\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs
Source: "{#SourceDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\NOTICE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\PATENTS.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\r3dss"; Filename: "{app}\bin\r3dss-gui.exe"
Name: "{group}\Documentation"; Filename: "{app}\docs"
Name: "{group}\Uninstall r3dss"; Filename: "{uninstallexe}"
Name: "{autodesktop}\r3dss"; Filename: "{app}\bin\r3dss-gui.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}\bin"; Tasks: addtopath; Check: NeedsAddPath(ExpandConstant('{app}\bin'))

[Run]
Filename: "{app}\bin\r3dss-gui.exe"; Description: "Launch r3dss"; Flags: nowait postinstall skipifsilent

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
