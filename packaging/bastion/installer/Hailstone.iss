#ifndef SetupAppId
  #define SetupAppId "Hailstone.Client"
#endif
#ifndef SetupFileDescription
  #define SetupFileDescription "Hailstone Setup"
#endif

[Setup]
AppId={#SetupAppId}
AppName=Hailstone
AppVersion={#ClientVersion}
AppPublisher=Project Hailstone
VersionInfoVersion={#NumericVersion}
VersionInfoTextVersion={#NumericVersion} ({#BuildRevision})
VersionInfoProductTextVersion={#ClientVersion}
VersionInfoDescription={#SetupFileDescription}
MinVersion=10.0.18362
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
DefaultDirName=C:\hailstone
DefaultGroupName=Hailstone
DisableDirPage=no
DisableProgramGroupPage=yes
DisableWelcomePage=no
AllowNoIcons=yes
DisableStartupPrompt=yes
UninstallDisplayIcon={app}\hailstone.exe
RestartApplications=no
CloseApplicationsFilter=*.exe,*.dll
Compression=lzma2/fast
SolidCompression=yes
SetupIconFile={#SetupIcon}
WizardStyle=modern dark
WizardImageFile={#Artwork}\WizardImage.png
WizardSmallImageFile={#Artwork}\WizardSmallImage.png

[Messages]
WelcomeLabel1=Welcome to Hailstone
WelcomeLabel2=Set up Hailstone {#ClientVersion} for your Windows account.%n%nOn first launch, choose the folder that contains your game files. Hailstone reads them in place.%n%nYou need a GPU and driver that support Vulkan 1.3.
FinishedHeadingLabel=Hailstone is ready
FinishedLabel=Open Hailstone to choose your game folder and sign in to your server.
FinishedLabelNoIcons=Open Hailstone from its installation folder to get started.

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts"; Flags: unchecked

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{group}\Hailstone"; Filename: "{app}\hailstone.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\Hailstone"; Filename: "{app}\hailstone.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\hailstone.exe"; Description: "Open Hailstone"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[Code]
function DestinationError(const Destination: String): String;
var
  Folder, Parent, PreviousFolder: String;
begin
  Result := '';
  Folder := RemoveBackslashUnlessRoot(Destination);
  repeat
    if FileExists(AddBackslash(Folder) + 'eqgame.exe') or
       FileExists(AddBackslash(Folder) + 'spells_us.txt') or
       FileExists(AddBackslash(Folder) + 'eqclient.ini') then
    begin
      Result := 'Choose a Hailstone folder outside the folder that contains your game files.';
      Exit;
    end;
    Parent := ExtractFileDir(Folder);
    if (Parent = '') or (Parent = Folder) then
      Break;
    Folder := Parent;
  until False;

  PreviousFolder := GetPreviousData('InstallFolder', '');
  if (PreviousFolder <> '') and
     (CompareText(RemoveBackslashUnlessRoot(Destination), PreviousFolder) <> 0) then
    Result := 'Hailstone is already installed. Use the existing folder: ' + PreviousFolder;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  ErrorText: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    ErrorText := DestinationError(WizardDirValue);
    if ErrorText <> '' then
    begin
      Log(ErrorText);
      SuppressibleMsgBox(ErrorText, mbError, MB_OK, IDOK);
      Result := False;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := DestinationError(WizardDirValue);
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'InstallFolder', RemoveBackslashUnlessRoot(WizardDirValue));
end;
