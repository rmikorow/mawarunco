; Installer for the Fate/unlimited codes rollback netplay build of PCSX2 (Inno Setup 6).
;
; Built by tools\rollback\make-netplay-package.ps1 -Installer, which passes:
;   /DSourceDir=<staged package folder>  /DOutputDir=<dist>  /DAppVersion=<yyyy.MM.dd>  /DRepoDir=<repo root>
;
; Installs per user (no admin rights) to %LOCALAPPDATA%\Programs\FUC-Netplay. A custom page asks for the
; player's own BIOS (copied into the "bios" folder) and disc image (only its path is kept; 4 GB are not
; copied). Running the installer again updates the program and keeps settings, button config and BIOS.
; It never contains a BIOS, a disc image or any game data.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{6E0F2C7A-6B53-4C55-9E7B-3F1D2A7C9B41}
AppName=Fate/unlimited codes Netplay
AppVersion={#AppVersion}
AppVerName=Fate/unlimited codes Netplay {#AppVersion}
AppPublisher=Unofficial PCSX2 rollback fork
DefaultDirName={localappdata}\Programs\FUC-Netplay
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=FUC-Netplay-Setup-{#AppVersion}
SetupIconFile={#RepoDir}\pcsx2\windows\AppIcon.ico
UninstallDisplayIcon={app}\pcsx2-qtx64-clang.exe
LicenseFile={#RepoDir}\tools\rollback\installer\LICENSE-NOTICE.txt
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
; Program files (replaced on update).
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "inis\*,bios\*,game\*,memcards\*,sstates\*,logs\*"; Flags: ignoreversion recursesubdirs createallsubdirs
; Settings, placeholders and data folders: only on the first installation, so an update keeps the player's
; controller mapping, button config, name, delay, display settings and BIOS choice.
Source: "{#SourceDir}\inis\*"; DestDir: "{app}\inis"; Flags: onlyifdoesntexist recursesubdirs createallsubdirs
Source: "{#SourceDir}\bios\*"; DestDir: "{app}\bios"; Flags: onlyifdoesntexist
Source: "{#SourceDir}\game\*"; DestDir: "{app}\game"; Flags: onlyifdoesntexist
Source: "{#SourceDir}\memcards\*"; DestDir: "{app}\memcards"; Flags: onlyifdoesntexist
Source: "{#SourceDir}\sstates\*"; DestDir: "{app}\sstates"; Flags: onlyifdoesntexist
Source: "{#SourceDir}\logs\*"; DestDir: "{app}\logs"; Flags: onlyifdoesntexist

[Icons]
Name: "{userprograms}\Fate unlimited codes Netplay"; Filename: "{app}\pcsx2-qtx64-clang.exe"; WorkingDir: "{app}"
Name: "{userdesktop}\Fate unlimited codes Netplay"; Filename: "{app}\pcsx2-qtx64-clang.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[UninstallDelete]
; Files the program or the installer created. The "game", "sstates" and "memcards" folders stay: players may
; keep their own files there.
Type: filesandordirs; Name: "{app}\bios"
Type: filesandordirs; Name: "{app}\logs"
Type: filesandordirs; Name: "{app}\inis"
Type: filesandordirs; Name: "{app}\cache"
Type: filesandordirs; Name: "{app}\covers"
Type: filesandordirs; Name: "{app}\gamesettings"
Type: filesandordirs; Name: "{app}\inputprofiles"
Type: filesandordirs; Name: "{app}\snaps"
Type: filesandordirs; Name: "{app}\textures"
Type: filesandordirs; Name: "{app}\videos"
Type: filesandordirs; Name: "{app}\cheats"
Type: filesandordirs; Name: "{app}\patches"

[Run]
Filename: "{app}\pcsx2-qtx64-clang.exe"; WorkingDir: "{app}"; Description: "Start Fate/unlimited codes Netplay"; Flags: nowait postinstall skipifsilent

[Code]
const
  BIOS_SIZE = 4194304;

var
  FilesPage: TInputFileWizardPage;

procedure InitializeWizard;
begin
  FilesPage := CreateInputFilePage(wpSelectDir,
    'Your game files',
    'Choose your own PS2 BIOS and your own disc image of the game.',
    'The BIOS is copied into the program folder; the disc image stays where it is. ' +
    'Both can be changed later in the start window ("Browse..."). You may leave a field empty.');
  FilesPage.Add('PS2 BIOS file (4 MB, from your own console):',
    'PS2 BIOS (*.bin;*.rom;*.rom0)|*.bin;*.rom;*.rom0|All files (*.*)|*.*', '.bin');
  FilesPage.Add('Disc image of Fate/unlimited codes (SLPM-55108):',
    'Disc images (*.iso;*.chd;*.cso;*.zso;*.cue)|*.iso;*.chd;*.cso;*.zso;*.cue|All files (*.*)|*.*', '.iso');
  // Optional command line values (silent installs, tests): /BIOS="<file>" /GAME="<file>".
  FilesPage.Values[0] := ExpandConstant('{param:BIOS|}');
  FilesPage.Values[1] := ExpandConstant('{param:GAME|}');
end;

function FileSizeOf(const Path: String): Int64;
var
  Size: Int64;
begin
  if FileSize64(Path, Size) then
    Result := Size
  else
    Result := -1;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Bios, Game: String;
begin
  Result := True;
  if CurPageID <> FilesPage.ID then
    exit;
  Bios := Trim(FilesPage.Values[0]);
  Game := Trim(FilesPage.Values[1]);
  if (Bios <> '') and (not FileExists(Bios)) then
  begin
    MsgBox('The BIOS file does not exist.', mbError, MB_OK);
    Result := False;
  end
  else if (Bios <> '') and (FileSizeOf(Bios) <> BIOS_SIZE) then
  begin
    MsgBox('This does not look like a PS2 BIOS (it should be exactly 4 MB).', mbError, MB_OK);
    Result := False;
  end
  else if (Game <> '') and (not FileExists(Game)) then
  begin
    MsgBox('The disc image does not exist.', mbError, MB_OK);
    Result := False;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Bios, Game, BiosName: String;
  Lines: TArrayOfString;
  Count: Integer;
begin
  if CurStep <> ssPostInstall then
    exit;
  Bios := Trim(FilesPage.Values[0]);
  Game := Trim(FilesPage.Values[1]);
  Count := 0;
  SetArrayLength(Lines, 2);
  if Bios <> '' then
  begin
    BiosName := ExtractFileName(Bios);
    ForceDirectories(ExpandConstant('{app}\bios'));
    if FileCopy(Bios, ExpandConstant('{app}\bios\') + BiosName, False) then
    begin
      Lines[Count] := 'BIOS=' + BiosName;
      Count := Count + 1;
    end
    else
      MsgBox('Could not copy the BIOS. You can choose it later in the start window.', mbError, MB_OK);
  end;
  if Game <> '' then
  begin
    Lines[Count] := 'GamePath=' + Game;
    Count := Count + 1;
  end;
  if Count > 0 then
  begin
    // The start window takes these over on its next start (UTF-8, so any path works).
    SetArrayLength(Lines, Count);
    SaveStringsToUTF8File(ExpandConstant('{app}\inis\launcher-setup.txt'), Lines, False);
  end;
end;
