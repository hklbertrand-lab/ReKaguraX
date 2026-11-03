; Installateur de ReKaguraX (Ikaruga recompilé) — compilé par tools/package.sh
; Installé dans le profil de l'utilisateur : le jeu écrit ikaruga.ini et saves\ dans son dossier.

[Setup]
AppId={{6B2F3E1A-7C44-4E2B-9A51-2D8C0F4A9E13}
AppName=ReKaguraX
AppVersion=0.1
AppPublisher=hklbertrand-lab
DefaultDirName={localappdata}\Programs\ReKaguraX
DefaultGroupName=ReKaguraX
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=ReKaguraX-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\ikaruga.exe

[Languages]
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\dist\ReKaguraX\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Ikaruga"; Filename: "{app}\ikaruga.exe"; WorkingDir: "{app}"
Name: "{group}\Désinstaller ReKaguraX"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Ikaruga"; Filename: "{app}\ikaruga.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\ikaruga.exe"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,Ikaruga}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; réglages créés par le jeu ; les sauvegardes (saves\) sont conservées
Type: files; Name: "{app}\ikaruga.ini"
