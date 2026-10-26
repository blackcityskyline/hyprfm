; HyprFM Windows installer (Inno Setup 6). Built in CI:
;   iscc /DAppVersion=<version> /DStageDir=<cmake install prefix> hyprfm.iss
; Per-user by default, so no admin prompt; the dialog offers all users.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\..\stage"
#endif

[Setup]
AppId={{6F2A1C7E-4B8D-4E0A-9C61-2D5B7E3F9A10}
AppName=HyprFM
AppVersion={#AppVersion}
AppPublisher=HyprFM
AppPublisherURL=https://github.com/soyeb-jim285/hyprfm
DefaultDirName={autopf}\HyprFM
DefaultGroupName=HyprFM
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=HyprFM-{#AppVersion}-setup-x64
SetupIconFile=hyprfm.ico
UninstallDisplayIcon={app}\bin\hyprfm.exe
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\HyprFM"; Filename: "{app}\bin\hyprfm.exe"
Name: "{autodesktop}\HyprFM"; Filename: "{app}\bin\hyprfm.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\hyprfm.exe"; Description: "{cm:LaunchProgram,HyprFM}"; Flags: nowait postinstall skipifsilent
