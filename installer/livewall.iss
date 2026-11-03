; LiveWall installer (Inno Setup 6).
;
; Per-user install: no admin rights required, everything lands in
; %LOCALAPPDATA%\Programs\LiveWall, the autostart entry goes to HKCU.
;
; Build with:
;   ISCC /DAppVersion=0.1.0 /DSourceDir=build\dist /DOutputDir=build\dist installer\livewall.iss

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\build\dist"
#endif
#ifndef OutputDir
  #define OutputDir "..\build\dist"
#endif

#define AppName "LiveWall"
#define AppExe "livewall.exe"
#define AppUrl "https://github.com/ldima8286-cmd/Livewall"
#define RunKey "Software\Microsoft\Windows\CurrentVersion\Run"
#define MutexName "Local\LiveWall.SingleInstance"

[Setup]
AppId={{B6F5F0D4-2C8E-4F4B-9E3A-7C1D5A9B2E60}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=LiveWall
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir={#OutputDir}
OutputBaseFilename=LiveWall-{#AppVersion}-setup
SetupIconFile=..\resources\livewall.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes
MinVersion=10.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE
AppMutex={#MutexName}
CloseApplications=yes
RestartApplications=no
CloseApplicationsFilter=*.exe
ShowLanguageDialog=auto

[Languages]
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "autostart"; Description: "Запускать LiveWall при входе в Windows"; GroupDescription: "Дополнительно:"; Languages: russian; Flags: checkedonce
Name: "autostart"; Description: "Start LiveWall when Windows starts"; GroupDescription: "Additional:"; Languages: english; Flags: checkedonce
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\livewall.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "{#RunKey}"; ValueType: string; ValueName: "{#AppName}"; ValueData: """{app}\{#AppExe}"" --autostart"; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\{#AppExe}"; Parameters: "--quit"; Flags: runhidden waituntilterminated; RunOnceId: "StopLiveWall"

[UninstallDelete]
; Settings and the log. Portable installs keep them next to the exe and the
; {app} folder is removed anyway.
Type: filesandordirs; Name: "{localappdata}\LiveWall"
