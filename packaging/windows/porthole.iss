; Point BuildDir at the extracted and verified Windows portable payload.
#ifndef BuildDir
#define BuildDir "."
#endif
#ifndef PortholeVersion
#define PortholeVersion "0.1.0"
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
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.18362
PrivilegesRequired=admin

[Files]
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
Name: "startwithwindows"; Description: "Start with Windows for all users"; GroupDescription: "Additional tasks:"; Flags: unchecked
Name: "localnetwork"; Description: "Allow Porthole on Private networks"; GroupDescription: "Local network:"; Flags: unchecked

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "Porthole"; Flags: dontcreatekey uninsdeletevalue

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Porthole Installed TCP"" dir=in action=allow program=""{app}\porthole.exe"" protocol=TCP localport=7000,7001,7100,47731,47732,47733 profile=private remoteip=localsubnet"; Tasks: localnetwork; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Porthole Installed UDP"" dir=in action=allow program=""{app}\porthole.exe"" protocol=UDP localport=6000,6001,7011 profile=private remoteip=localsubnet"; Tasks: localnetwork; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Porthole Installed mDNS"" dir=in action=allow program=""{sys}\svchost.exe"" service=Dnscache protocol=UDP localport=5353 profile=private remoteip=localsubnet"; Tasks: localnetwork; Flags: runhidden
Filename: "{app}\porthole.exe"; WorkingDir: "{app}"; Description: "Open Porthole"; Flags: postinstall nowait skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Porthole Installed TCP"" program=""{app}\porthole.exe"""; Flags: runhidden; RunOnceId: "removeTcpFirewall"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Porthole Installed UDP"" program=""{app}\porthole.exe"""; Flags: runhidden; RunOnceId: "removeUdpFirewall"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Porthole Installed mDNS"" program=""{sys}\svchost.exe"" service=Dnscache"; Flags: runhidden; RunOnceId: "removeMdnsFirewall"
