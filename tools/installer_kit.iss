; Installateur communautaire de ReKaguraX : AUCUN fichier du jeu. Compilé par tools/make_kit.sh.
; rkx-setup demande le paquet LIVE de l'utilisateur, le vérifie, l'extrait et recompile le jeu sur sa machine.
; Installé dans le profil de l'utilisateur : le jeu écrit ikaruga.ini et saves\ dans son dossier.

[Setup]
AppId={{9D41C7B2-3E58-4A6F-B812-5C0E7A3F2D94}
AppName=ReKaguraX
AppVersion=0.2
AppPublisher=hklbertrand-lab
DefaultDirName={localappdata}\Programs\ReKaguraX
DefaultGroupName=ReKaguraX
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=ReKaguraX-communaute-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\rkx-setup.exe

[Languages]
Name: "fr"; MessagesFile: "compiler:Languages\French.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\dist\ReKaguraX-kit\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Ikaruga"; Filename: "{app}\ikaruga.exe"; WorkingDir: "{app}"
Name: "{group}\Installer le jeu (fournir mon paquet)"; Filename: "{app}\rkx-setup.exe"; WorkingDir: "{app}"
Name: "{group}\Désinstaller ReKaguraX"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Ikaruga"; Filename: "{app}\ikaruga.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\rkx-setup.exe"; WorkingDir: "{app}"; Description: "Fournir mon paquet Ikaruga et recompiler le jeu"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; fichiers produits sur la machine ; les sauvegardes (saves\) sont conservées
Type: files; Name: "{app}\ikaruga.ini"
Type: files; Name: "{app}\ikaruga.exe"
Type: filesandordirs; Name: "{app}\extracted"
Type: filesandordirs; Name: "{app}\work"
