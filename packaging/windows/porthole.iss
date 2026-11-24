; Point BuildDir at the extracted and verified Windows portable payload.
#ifndef BuildDir
#define BuildDir "."
#endif
#ifndef PortholeVersion
#define PortholeVersion "0.1.1"
#endif

[Setup]
AppId={{8F4C1A2E-6B7D-4E19-9C30-5A1D7E2B64F8}
AppName=Porthole
AppVersion={#PortholeVersion}
AppPublisher=Porthole
DefaultDirName={autopf}\Porthole
DefaultGroupName=Porthole
DisableProgramGroupPage=yes
OutputBaseFilename=Porthole-{#PortholeVersion}-Windows-x64-Setup
SetupIconFile=..\..\branding\porthole\app-icons\dark\porthole.ico
UninstallDisplayIcon={app}\porthole.exe
WizardStyle=modern dynamic windows11
WizardSmallImageFile=..\..\branding\porthole\png\on-light\porthole-icon-64.png, ..\..\branding\porthole\png\on-light\porthole-icon-128.png, ..\..\branding\porthole\png\on-light\porthole-icon-256.png
WizardSmallImageFileDynamicDark=..\..\branding\porthole\png\on-dark\porthole-icon-64.png, ..\..\branding\porthole\png\on-dark\porthole-icon-128.png, ..\..\branding\porthole\png\on-dark\porthole-icon-256.png
WizardImageFile=wizard-light.png
WizardImageFileDynamicDark=wizard-dark.png
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.18362
PrivilegesRequired=admin

[Files]
Source: "switch-*.png"; Flags: dontcopy
Source: "{#BuildDir}\porthole.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\ble-winrt\*"; DestDir: "{app}\ble-winrt"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#BuildDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Porthole"; Filename: "{app}\porthole.exe"; WorkingDir: "{app}"
Name: "{group}\Uninstall Porthole"; Filename: "{uninstallexe}"
Name: "{commonstartup}\Porthole"; Filename: "{app}\porthole.exe"; WorkingDir: "{app}"; Tasks: startwithwindows

[Tasks]
Name: "localnetwork"; Description: "Allow connections on private networks"
Name: "startwithwindows"; Description: "Start Porthole at sign-in for all users"; Flags: unchecked

[Messages]
WizardSelectTasks=Installation options
SelectTasksDesc=Choose how Porthole connects and starts.
ReadyMemoTasks=Installation options
FinishedHeadingLabel=Porthole is installed
FinishedLabel=Open Porthole to pair your iPhone.

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "Porthole"; Flags: dontcreatekey uninsdeletevalue

[InstallDelete]
Type: files; Name: "{commonstartup}\Porthole.lnk"; Tasks: not startwithwindows

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Porthole Installed TCP"" program=""{app}\porthole.exe"""; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Porthole Installed UDP"" program=""{app}\porthole.exe"""; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Porthole Installed mDNS"" program=""{sys}\svchost.exe"" service=Dnscache"; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Porthole Installed TCP"" dir=in action=allow program=""{app}\porthole.exe"" protocol=TCP localport=7000,7001,7100,47731,47732,47733 profile=private remoteip=localsubnet"; Tasks: localnetwork; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Porthole Installed UDP"" dir=in action=allow program=""{app}\porthole.exe"" protocol=UDP localport=6000,6001,7011 profile=private remoteip=localsubnet"; Tasks: localnetwork; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Porthole Installed mDNS"" dir=in action=allow program=""{sys}\svchost.exe"" service=Dnscache protocol=UDP localport=5353 profile=private remoteip=localsubnet"; Tasks: localnetwork; Flags: runhidden
Filename: "{app}\porthole.exe"; WorkingDir: "{app}"; Description: "Open Porthole"; Flags: postinstall nowait skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Porthole Installed TCP"" program=""{app}\porthole.exe"""; Flags: runhidden; RunOnceId: "removeTcpFirewall"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Porthole Installed UDP"" program=""{app}\porthole.exe"""; Flags: runhidden; RunOnceId: "removeUdpFirewall"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Porthole Installed mDNS"" program=""{sys}\svchost.exe"" service=Dnscache"; Flags: runhidden; RunOnceId: "removeMdnsFirewall"

[Code]
const
  NetworkTitle = 'Allow connections on private networks';
  StartupTitle = 'Start Porthole at sign-in for all users';

var
  NetworkSwitch, StartupSwitch: TBitmapButton;
  SwitchOn, SwitchOff: TPngImage;

procedure UpdateSwitch(Button: TBitmapButton; const Task, Title: String);
begin
  if WizardIsTaskSelected(Task) then begin
    Button.PngImage.Assign(SwitchOn);
    Button.Caption := Title + ': On';
  end else begin
    Button.PngImage.Assign(SwitchOff);
    Button.Caption := Title + ': Off';
  end;
end;

procedure OptionClick(Sender: TObject);
var
  Task, Title: String;
begin
  if Sender = NetworkSwitch then begin
    Task := 'localnetwork';
    Title := NetworkTitle;
  end else begin
    Task := 'startwithwindows';
    Title := StartupTitle;
  end;

  if WizardIsTaskSelected(Task) then
    WizardSelectTasks('!' + Task)
  else
    WizardSelectTasks(Task);
  UpdateSwitch(TBitmapButton(Sender), Task, Title);
end;

function AddOption(const Title, Description: String; Top: Integer): TBitmapButton;
var
  Heading, Detail: TNewStaticText;
  Page: TNewNotebookPage;
begin
  Page := WizardForm.SelectTasksPage;
  Result := TBitmapButton.Create(WizardForm);
  Result.Parent := Page;
  Result.SetBounds(Page.ClientWidth - ScaleX(54), Top + ScaleY(3), ScaleX(54), ScaleY(32));
  Result.Anchors := [akTop, akRight];
  Result.Stretch := True;
  Result.TabStop := True;
  Result.Cursor := crHand;
  Result.OnClick := @OptionClick;

  Heading := TNewStaticText.Create(WizardForm);
  Heading.Parent := Page;
  Heading.Caption := Title;
  Heading.Left := ScaleX(4);
  Heading.Top := Top;
  Heading.Font.Style := [fsBold];
  Heading.FocusControl := Result;

  Detail := TNewStaticText.Create(WizardForm);
  Detail.Parent := Page;
  Detail.AutoSize := False;
  Detail.WordWrap := True;
  Detail.Caption := Description;
  Detail.SetBounds(Heading.Left, Top + ScaleY(20), Result.Left - ScaleX(22), ScaleY(34));
  Detail.Anchors := [akLeft, akTop, akRight];
  WizardForm.AdjustLabelHeight(Detail);
end;

procedure InitializeWizard;
var
  Theme: String;
begin
  WizardForm.RunList.MinItemHeight := ScaleY(32);
  if HighContrastActive then
    Exit;
  if IsDarkInstallMode then
    Theme := 'dark'
  else
    Theme := 'light';
  ExtractTemporaryFile('switch-' + Theme + '-on.png');
  ExtractTemporaryFile('switch-' + Theme + '-off.png');
  SwitchOn := TPngImage.Create;
  SwitchOff := TPngImage.Create;
  SwitchOn.LoadFromFile(ExpandConstant('{tmp}\switch-' + Theme + '-on.png'));
  SwitchOff.LoadFromFile(ExpandConstant('{tmp}\switch-' + Theme + '-off.png'));
  WizardForm.TasksList.Visible := False;
  WizardForm.SelectTasksLabel.Visible := False;
  NetworkSwitch := AddOption(NetworkTitle,
    'Lets your iPhone connect over Wi-Fi or Ethernet on a private Windows network.', ScaleY(14));
  StartupSwitch := AddOption(StartupTitle,
    'Opens Porthole automatically when anyone signs in to this PC.', ScaleY(90));
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpSelectTasks) and Assigned(NetworkSwitch) then begin
    UpdateSwitch(NetworkSwitch, 'localnetwork', NetworkTitle);
    UpdateSwitch(StartupSwitch, 'startwithwindows', StartupTitle);
  end;
end;

procedure DeinitializeSetup;
begin
  SwitchOn.Free;
  SwitchOff.Free;
end;
