# Native Windows Bluetooth HID helper

`porthole-ble-winrt.exe` runs as a child process of Porthole. It uses Windows'
`GattServiceProvider` to publish HID over GATT (service 1812), with the same
keyboard, 16-bit relative mouse and consumer report descriptor as the C app.
It requires Windows 10 version 2004 (build 19041) or later, a BLE radio, and a
Windows driver that supports the BLE **peripheral** role. A radio that supports
ordinary Bluetooth accessories does not necessarily support this role.

Windows advertises the PC's system Bluetooth name. Pair that PC from the iPhone's
Bluetooth Settings and accept the Windows pairing prompt. Enable iPhone
AssistiveTouch for mouse control. Porthole's Wi-Fi receiver name can differ from
the PC's Bluetooth name. The helper does not rename the computer or alter radio
settings. Encryption-required HID attributes let Windows handle pairing and
bonding; Device Information, GAP and GATT services are left to Windows.

The HID service supplies keyboard/mouse/consumer input Reports, matching Report
References, Report Map, HID Information, keyboard LED output, Report Protocol
Mode, and HID Control Point. Windows creates each notification CCCD. Input
starts only when one phone has subscribed to all three input Reports. Missing
subscriptions, mixed peers and closed sessions invalidate dispatch tokens;
reports cannot be redirected to a newly connected phone. After the first fully
ready connection loses readiness, the helper exits and Porthole starts a fresh
process and input pipe. This prevents unread old pipe bytes being assigned to a
reconnected phone; the existing wire format does not carry generation IDs.
Notifications target
the captured subscribed client, never all clients. Keyboard and consumer frames
run independently from paced mouse/delay frames. Windows owns the actual radio
connection interval; a protocol interval frame changes software dispatch pacing.
The `sync` reply confirms dispatch completion, not phone rendering.

## Build and diagnostic commands

Use the .NET 10 SDK (a self-contained publish also bundles the .NET runtime):

```powershell
dotnet publish helpers/ble-winrt/Porthole.Ble.WinRT.csproj -c Release -r win-x64 --self-contained true --artifacts-path build/dotnet -o build/ble-winrt
.\build\ble-winrt\porthole-ble-winrt.exe --probe
```

`--probe` checks hardware/access and actually creates HID 1812 and all its
characteristics, without advertising or connecting. A successful probe prints
`probe ok peripheral=true hid_service=1812` and exits. It does not prove iPhone
pairing, subscription or input delivery. Test those on the Windows host through
Porthole after the probe. An unavailable peripheral role is a driver/hardware
limitation: Wi-Fi viewing remains available.

`--descriptor` prints the report map as lowercase hexadecimal without touching
Bluetooth. The portable Core tests compare this descriptor to
`src/hid/hid_reports.c`, test fragmented binary stdin frames, lane ordering,
backpressure, cancellation and reconnection generations.

Without arguments, stdin is the existing binary Porthole helper protocol;
stdout contains only protocol status/replies and stderr contains diagnostics.
