; Inno Setup script (draft) for Telemy v1.2

#define MyAppName "Telemy"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "Telemy"
#define MyAppExeName "obs-telemetry-bridge.exe"

[Setup]
AppId={{A9B9A1D1-5C57-4B6D-9F2C-3C4A0C8E0E7F}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={pf}\{#MyAppName}
DefaultGroupName={#MyAppName}
OutputBaseFilename=TelemySetup
Compression=lzma
SolidCompression=yes

[Files]
Source: "..\target\release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\config.example.toml"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\grafana-dashboard-template.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\assets\telemy.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch Telemy"; Flags: nowait postinstall skipifsilent
