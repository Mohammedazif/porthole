# Native Windows Bluetooth HID helper

`porthole-ble-winrt.exe` runs as a child process of Porthole. It uses Windows'
`GattServiceProvider` to publish HID over GATT (service 1812), with the same
keyboard, 16-bit relative mouse and consumer report descriptor as the C app.
With `PORTHOLE_HID_ABSOLUTE=replace` (Porthole's default) report 2 is the absolute
mouse instead, X/Y 0..32767, byte for byte the map `helpers/ble-bluez` publishes, so
the iPhone pointer lands where the PC mouse is. The iPhone caches the report map:
after switching between the two, Forget This Device on the iPhone, remove the iPhone
in Windows Bluetooth settings, and pair again.
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
starts only when one phone has subscribed to all three input Reports and the
corresponding Windows sessions are active. Raw subscriptions are observed before
filtering for activity, so a session that activates after its CCCD event still
refreshes readiness. A snapshot after advertising starts also picks up restored
subscriptions. Diagnostics show per-report raw/active counts, session ordinals,
missing subscriptions and suspend state without logging Bluetooth addresses. Missing
subscriptions, mixed peers and closed sessions invalidate dispatch tokens;
reports cannot be redirected to a newly connected phone. After the first fully
ready connection loses readiness, the helper exits and Porthole starts a fresh
process and input pipe. This prevents unread old pipe bytes being assigned to a
reconnected phone; the existing wire format does not carry generation IDs.
Before retiring a ready connection, the helper captures the original report targets
and their session-registration generations. It stops normal input, queues only neutral
reports to those same still-active registrations, and waits up to 200 ms before
unregistering. This cleanup remains available after logical readiness is revoked; it
cannot retarget releases to a replacement phone. Actual radio delivery still needs a
Windows/iPhone test. The helper never removes the Windows bond.
HID Control Point Suspend/Exit Suspend records the host's power mode separately.
It preserves active subscriptions, the current dispatch epoch, cached held reports
and the input pipe. This powered desktop continues reporting input to the same active
peer; it does not advertise any additional remote-wake capability. Real subscription
or connection loss still requires a fresh pipe, even while the host is suspended.
Notifications target
the captured subscribed client, never all clients. Keyboard and consumer frames
run independently from paced mouse/delay frames. Windows owns the actual radio
connection interval; a protocol interval frame changes software dispatch pacing.
The `sync` reply confirms dispatch completion, not phone rendering.

## Build and diagnostic commands

Use the .NET 10 SDK to create the Windows x64 single-file helper. The executable
contains the .NET runtime, WinRT assemblies, helper assemblies and runtime
configuration; running it does not require a separate .NET installation or
adjacent runtime DLLs. The helper loads managed assemblies directly from the bundle.
The runtime extracts any bundled native libraries that require a file into the
current user's `%TEMP%\.net` cache. Trimming is
disabled to preserve the WinRT projections.

```powershell
dotnet publish helpers/ble-winrt/Porthole.Ble.WinRT.csproj -c Release -r win-x64 --self-contained true --artifacts-path build/winrt-single-dotnet -o build/ble-winrt-single
.\build\ble-winrt-single\porthole-ble-winrt.exe --probe
```

Publish into a fresh output directory when changing from an older multi-file
package. The output also contains `licenses/`, `COMPONENTS.txt` and this README;
keep those notices in the distributed package. They are not runtime dependencies.
The helper's name remains `porthole-ble-winrt.exe`, so the main app continues to
launch it at `ble-winrt/porthole-ble-winrt.exe` inside a portable Porthole package.

`--probe` checks hardware/access and actually creates HID 1812 and all its
characteristics, without advertising or connecting. A successful probe prints
`probe ok peripheral=true hid_service=1812` and exits. It does not prove iPhone
pairing, subscription or input delivery. Close Porthole before running a standalone
helper probe, so two processes do not compete to publish the same service.

To test the next step, run:

```powershell
.\build\ble-winrt-single\porthole-ble-winrt.exe --probe-advertising
```

This mode creates the full HID service, starts connectable/discoverable advertising,
and waits for two seconds of established advertising before printing
`probe ok advertising=true stable_seconds=2 hid_service=1812`. The PC can briefly
appear in nearby devices during this check. It then stops advertising and exits;
Bluetooth pairing and input delivery still require Porthole's live connection test.

Some Windows drivers emit an initial `Aborted` event with `Error=Success` before
`Started`. The helper keeps that initial event pending within the existing
12-second startup deadline; it prints `advertising` only after `Started` or
`StartedWithoutAllAdvertisementData`. A real error, timeout, or advertising loss
after startup remains fatal. Diagnostic lines include elapsed startup time and
both the event's status and the provider's current status.

Test pairing, subscriptions and input delivery on the Windows host through
Porthole after the probes. An unavailable peripheral role is a driver/hardware
limitation: Wi-Fi viewing remains available.

`--descriptor` prints the report map as lowercase hexadecimal without touching
Bluetooth. The portable Core tests compare this descriptor to
`src/hid/hid_reports.c`, test fragmented binary stdin frames, lane ordering,
backpressure, cancellation and reconnection generations.

Without arguments, stdin is the existing binary Porthole helper protocol;
stdout contains only protocol status/replies and stderr contains diagnostics.
