[Setup]
AppId={{2E5C8B9E-F592-4C2F-A37D-FECAFA4F5342}
AppName=WWMM Remote
AppVersion={#AppVersion}
VersionInfoVersion={#AppVersion}.{#BuildNumber}
DefaultDirName={localappdata}\Programs\WWMM Remote
PrivilegesRequired=lowest
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
UninstallDisplayIcon={app}\WWMM Remote.exe
WizardStyle=modern
SetupIconFile={#AppIcon}
OutputDir={#Output}
OutputBaseFilename=install
ArchitecturesAllowed=x64compatible or arm64
ArchitecturesInstallIn64BitMode=x64compatible or arm64
Compression=lzma2/fast
SolidCompression=no
CloseApplications=no
RestartApplications=no
AppMutex=Local\WWMMRemote-Desktop

[Languages]
Name: "zh"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "en"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#X64}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: not IsArm64
Source: "{#Arm}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: IsArm64

[Icons]
Name: "{userprograms}\WWMM Remote"; Filename: "{app}\WWMM Remote.exe"
[Run]
Filename: "{app}\WWMM Remote.exe"; Description: "{cm:LaunchProgram,WWMM Remote}"; Flags: nowait postinstall skipifsilent
