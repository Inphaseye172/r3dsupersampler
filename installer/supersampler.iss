; Inno Setup script for Supersampler.
; Build: ISCC.exe /DSourceDir=<cmake install prefix> /DAppVersion=<x.y.z> installer\supersampler.iss
#ifndef SourceDir
  #define SourceDir "..\dist"
#endif
#ifndef AppVersion
  #define AppVersion "0.2.0"
#endif

[Setup]
AppId={{5D7E3C2A-9B41-4F6E-8A1D-2C3B4E5F6A70}
AppName=Supersampler
AppVersion={#AppVersion}
AppPublisher=Anchorpoint Studio
DefaultDirName={autopf}\Supersampler
DefaultGroupName=Supersampler
OutputDir=Output
OutputBaseFilename=Supersampler-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
LicenseFile={#SourceDir}\LICENSE
UninstallDisplayIcon={app}\bin\Supersampler.exe

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
Name: "{group}\Supersampler"; Filename: "{app}\bin\Supersampler.exe"
Name: "{group}\Documentation"; Filename: "{app}\docs"
Name: "{group}\Uninstall Supersampler"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Supersampler"; Filename: "{app}\bin\Supersampler.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}\bin"; Tasks: addtopath; Check: NeedsAddPath(ExpandConstant('{app}\bin'))

[Run]
Filename: "{app}\bin\Supersampler.exe"; Description: "Launch Supersampler"; Flags: nowait postinstall skipifsilent

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
