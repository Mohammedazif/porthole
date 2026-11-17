; Porthole installer only. The built exe is {#BuildDir}\porthole.exe.
#ifndef BuildDir
#define BuildDir "."
#endif

[Setup]
AppId={{8F4C1A2E-6B7D-4E19-9C30-5A1D7E2B64F8}
AppName=Porthole
AppVersion=0.1.0
AppPublisher=Porthole
DefaultDirName={autopf}\Porthole
DefaultGroupName=Porthole
DisableProgramGroupPage=yes
OutputBaseFilename=porthole-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64
PrivilegesRequired=admin

[Files]
Source: "{#BuildDir}\porthole.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Porthole"; Filename: "{app}\porthole.exe"
Name: "{group}\Uninstall Porthole"; Filename: "{uninstallexe}"

[Tasks]
Name: "startwithwindows"; Description: "Start with Windows"; GroupDescription: "Additional tasks:"; Flags: unchecked

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Porthole"; ValueData: """{app}\porthole.exe"""; Tasks: startwithwindows; Flags: uninsdeletevalue

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Porthole"" dir=in action=allow protocol=TCP localport=7000,7001,7100"; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Porthole"" dir=in action=allow protocol=UDP localport=6000,6001,7011"; Flags: runhidden

[UninstallRun]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Porthole"""; Flags: runhidden
