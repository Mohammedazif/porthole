# Porthole

**Use your iPhone inside Windows.** Porthole shows your iPhone's screen in a window on your PC and
lets you control it with your mouse and keyboard, over Wi-Fi or USB. Think *scrcpy, but for iPhone*.

Free and open source (GPL-3.0). Nothing to install on the iPhone: it uses built-in iOS features
(Screen Mirroring, Bluetooth mouse/keyboard and AssistiveTouch).

> **Status: Linux prototype.** Wi-Fi mirroring and keyboard forwarding run in Porthole's own
> window. Keyboard shortcuts and wheel scrolling have been tested on the iPhone. Continuous
> pointer tracking, taps, secondary clicks and drags have been verified with the C controller
> on the live iPhone. Integrated mouse, click, drag and scroll checks also pass in the app.
> Audio playback through the PC is implemented and verified on the live iPhone.
> A full Windows Wi-Fi mirror now builds with native discovery and portable dependencies.
> The user confirmed mirroring with the Windows preview on the host. Windows Bluetooth
> control now pairs, subscribes and acknowledges reports through a native WinRT helper.
> Pointer alignment, delayed clicks/drags and release after failure remain under investigation;
> those reports are not a complete Windows usability pass. Local validation includes paced
> input, stale/delayed ACKs, frozen video, held-input teardown and sanitizer checks.
> USB capture remains a separate roadmap item.
> See [PLAN.md](PLAN.md) for the full plan, feature list and roadmap.
>
> | Spike | Question | Result |
> |---|---|---|
> | S1 | Does Wi-Fi mirroring work with iOS 26.6.1? | ✅ Works |
> | S2 | Can Windows act as a Bluetooth mouse/keyboard for the iPhone? | ⏳ |
> | S3 | Can a click on the PC land on the exact spot on the iPhone? | ✅ Within 5 px (tested in the Linux VM) |
> | S4 | Can we capture the screen over USB on Windows? | ⏳ |
> | S5 | Can we trigger Home, Control Center, etc.? | ✅ Home, Spotlight, typing, right-click, App Switcher, Control Center, Notification Center |

## How it works

| Direction | Wi-Fi | USB |
|---|---|---|
| Screen (iPhone → PC) | PC acts as an AirPlay receiver (built on [UxPlay](https://github.com/FDH2/UxPlay)'s core) | Apple's USB screen-capture protocol |
| Control (PC → iPhone) | PC acts as a Bluetooth mouse + keyboard; AssistiveTouch shows a pointer | same |

## Building

Clone with submodules:

```sh
git clone --recursive <repo-url>
# or, in an existing clone:
git submodule update --init --recursive
```

### Linux (Debian / Ubuntu / Linux Mint)

```sh
sudo ./scripts/install-deps-linux.sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
./build/debug/src/porthole --version
```

Run the prototype with `./build/debug/src/porthole --verbose`, then select **Porthole** in the
iPhone's Screen Mirroring list. Bluetooth control requires a Bluetooth adapter available to
Linux (pass it through to the guest when using VMware), pairing with Porthole, AssistiveTouch
on, and Dwell Control off. Use `--no-bluetooth` for mirroring only.
If Bluetooth control is unavailable, the app retries every five seconds while mirroring
continues. Input is discarded until control is ready.

**Pointer modes.** On Linux Porthole uses an **absolute pointer** by default: it presents an
absolute HID mouse, and iOS (verified on iOS 26.6.1) places the AssistiveTouch pointer exactly
where the PC mouse is. No calibration, video tracking or button mapping is needed. Clicks are
sent 120 ms after the pointer reaches its spot, because iOS ignores the position in a report that
changes a button. The HID wheel does not scroll iOS, so wheel notches become short drags (F4 feed
mode: one swipe per wheel burst). `--relative-pointer` keeps the older relative mouse with video
tracking, which Windows still uses until its helper publishes the absolute map. The iPhone caches
the mouse layout: after switching modes, choose **Forget This Device** on the iPhone (and
`bluetoothctl remove <address>` on Linux), then pair again from the iPhone.

Left Alt shortcuts: **H** Home, **F** Spotlight, **S** App Switcher, **C** Control Center,
**N** Notification Center, **P** Screenshot, **←** Back, **↑/↓** Volume, **D** double-tap,
**L** long press, **PgUp/PgDn/U** and **Shift+arrows** swipes. In absolute mode, H/S/C/N are exact
edge swipes and P sends ⌘⇧3, so no AssistiveTouch button assignment is needed. In relative mode,
buttons 3–6 must be assigned to App Switcher, Control Center, Notification Center and Screenshot
in AssistiveTouch's device settings; check them again after re-pairing. PC Ctrl acts as Command
on the iPhone; click a text field before using editing shortcuts such as Ctrl+A. Use Alt+H to
return Home. **F1** opens local controls and setup help; input is paused while help
is open. The waiting screen shows mirroring and Bluetooth setup steps, and a status banner
identifies when the mirror is available without controls.

**F2** opens guided setup: confirm AssistiveTouch/Dwell settings on the iPhone, wait for
Bluetooth and mirroring readiness, then check three pointer targets. The check sends no
taps and saves calibration only after all targets are confirmed; cancellation or failure
restores the previous calibration. First-run setup opens automatically until completed.
Use `--setup` to reopen it on launch, or `--no-setup` to skip automatic setup.

Additional Bluetooth controls:

| Input | Action |
|---|---|
| Middle and side mouse buttons | Forward buttons 3–8; assign their actions in iPhone AssistiveTouch device settings |
| Shift + wheel | Horizontal scrolling; fractional trackpad movement accumulates rather than disappearing |
| Alt+D | Timed double-tap at the PC pointer's position |
| Alt+L | Hold at the pointer for at least one second after the press acknowledgement |
| Alt+Page Down / Page Up | Swipe upward / downward through the picture |
| Alt+Shift+arrow | Swipe in that direction |
| Alt+U | Swipe upward from near the bottom; panel dismissal depends on the iPhone's gesture handling |
| F4 | Toggle feed wheel mode; wheel down/up requests an upward/downward swipe |
| Escape | Cancel a running gesture or release held mouse input |
| Media keys | Forward play/pause, previous/next track, stop, mute, volume and keyboard-eject actions |

Feed mode is explicit and shown in the window title. It accepts one swipe per wheel burst,
with a 500 ms idle gap before another; wheel activity during a gesture does not queue
additional swipes. Turn it off with F4 for comments, ordinary lists and text fields.
A swipe is not proof that an app advanced exactly one Reel/Short. These app-specific
outcomes still need live verification. Media Next Track is separate from feed navigation.

Gestures use the existing video-confirmed Bluetooth pointer. Motion feedback can extend
their duration; they do not provide inertial flicks or multi-touch. Double-tap requests
abandon a delayed second tap rather than replaying it much later. Escape, focus loss,
disconnection and picture-size changes cancel pending gestures and release held buttons.
Ordinary PC double-clicks still use the normal click path; Alt+D selects the explicitly
timed double-tap path. Unmodified F1–F4 belong to Porthole; modified function keys are
forwarded, with release ownership retained if modifiers are released first.

**Photos and external presentations:** a full-screen photo can send a separate
landscape presentation to AirPlay while the iPhone keeps its portrait controls.
The Photos library grid mirrors normally on the tested phone. Porthole cannot
reconstruct UI that the iPhone does not send. With the absolute pointer, the
mouse, wheel, and keyboard keep working on that picture: a click is that fraction
of the phone screen. Plain clicks and drags always go to the phone. To take a photo
or video's original file, hold Ctrl and drag it out of the window: that press never
reaches the phone. Porthole asks the phone app at the press, so the drag card only
appears for a photo or video from the iPhone's library; on the home screen or an app
page the window says so and nothing is dragged. Porthole takes the photo under the
pointer when it can find its edges, and otherwise the whole picture, such as a
screenshot or a photo that fills the screen. Dropped before the phone has answered, the card stays where it was
let go and the file lands there; Escape gives up. The drag uses the newest frame and
sends the phone app the picture at 32 and 64 cells a side. The phone app (2.5 or
later) takes copies of one picture as the largest file, decides close calls on
sharper copies, and checks videos frame by frame around their closest moments. When
nothing can tell, choose the item in the phone app with Choose original. A failed
match saves nothing. Ctrl+drag takes one item. To take several, select them in Photos, tap Share and
choose Porthole: the phone app (2.9 or later) sends those files themselves, and the PC
saves them straight into Downloads.
F1 shows the address and
code the phone app uses. The relative pointer still pauses, and the window says
"Return to phone view", because it has to see the AssistiveTouch dot. Returning
to a matching phone view restores it without re-pairing. This shape check cannot
detect separate content with the same aspect ratio, or establish the phone shape
before a portrait frame arrives.
Photos double-tap zoom has **not** passed a live check. The timed two-press sequence
passes transport tests, which do not prove iOS recognizes it as a native double-tap.
Apps can choose separate content for an external display
([Apple documentation](https://developer.apple.com/library/archive/documentation/WindowsViews/Conceptual/WindowAndScreenGuide/UsingExternalDisplay/UsingExternalDisplay.html)).

**F3** shows measured local diagnostics: displayed video FPS, receive-to-decode/present
time, accepted-input handoff time, helper dispatch ACK time, and queue/drop counters.
Use `--diagnostics` to show it on launch and log summaries. An idle phone can report zero
video FPS because AirPlay stops sending unchanged pictures. Radio interval and iPhone
capture latency remain explicitly unmeasured; helper ACK is not an iPhone response time.

Mirrored iPhone audio plays through the PC's default speakers. AAC-ELD, AAC-LC and ALAC
are decoded with FFmpeg and played through SDL; AirPlay volume changes are respected.
Use `--no-audio` to mute playback on the PC. The playback queue is bounded to avoid
accumulating audible lag; precise audio/video synchronization remains on the roadmap.

Some streaming apps restrict mirroring of their video. Netflix explicitly does not support
iPhone/iPad AirPlay or Screen Mirroring; use Netflix directly on the PC for those videos
([Netflix support](https://help.netflix.com/en/node/11634)). This is separate from picture
orientation: native landscape frames keep their full width even during black frames,
and a confirmed portrait crop is retained through same-size dark transitions.

Mouse movement over the phone picture now drives a continuous pointer controller. Left click
queues a tap, right click queues a secondary click, and holding and dragging queues a swipe.
Video feedback confirms the pointer position before a button is pressed; losing focus or
disconnecting cancels queued actions and releases held buttons. Live controller checks reached
four targets within 1.4 pixels, entered `7 + 8 = 15` in Calculator, opened a native secondary-click
menu and performed a vertical drag. Integrated window tests also verified real PC mouse
alignment, Calculator clicks, right-click, dragging in both directions and scrolling to
page edges. The two live drag endpoints were within 1 pixel, and both completed their
button releases. Wheel positioning accepts a small confirmed offset so scrolling need not
wait for click-level precision; taps and held drags retain their stricter alignment.
Scrolling now watches the native iOS scroll indicator and stops continued input at an edge;
normal wheel travel is faster, with smaller steps near the boundary. A stopped edge stays
blocked through the bounce animation and mouse repositioning; reverse the wheel to move
away. A new gesture after a pause rechecks the bounds with a one-unit probe, so navigation
on the phone or a changing device list cannot leave an old edge permanently latched.
One unchanged frame cannot prove an edge. Navigation clears the learned bounds.
The release build reached both actual ends of the long Bluetooth device list and stopped
sustained input at each edge. The final Settings stress check also reached both ends with
183 PC wheel events in each direction, including cursor movement after stopping, and sent
no wheel reports after either stop. Custom scroll views without an indicator and longer
sessions still need broader live testing.

Pointer recovery retains outstanding helper acknowledgements before accepting replacement
actions, caps each movement batch at 24 reports and restarts an unresponsive helper while
video continues. Small corrections converge across the tested tracking sensitivities;
the final live setup check confirmed all three targets and saved calibration.

The mouse controller uses the previous video-confirmed movement path. Each move is checked
against the mirrored pointer before the next correction or button press. Keyboard and
consumer reports keep their separate queue, and the faster input loop and rendering changes
are retained. Experimental streamed hover and automatic curve fitting have been withdrawn
after unstable calibration during scrolling; their findings are preserved in the M5 section
of [PLAN.md](PLAN.md).

The restored controller reads and saves per-axis calibration in `pointer-scale` (on Linux,
`~/.config/porthole/pointer-scale`); it ignores the experimental `pointer-curve` file.
Keep AssistiveTouch Tracking Sensitivity consistent, or recalibrate after changing it.
For everyday use build the optimized `release` preset (`cmake --preset release`);
`debug` is compiled without optimization.

### Windows (MSYS2)

1. Install [MSYS2](https://www.msys2.org) and open an **MSYS2 UCRT64** terminal.
2. Install the [.NET 10 SDK](https://dotnet.microsoft.com/download/dotnet/10.0) and reopen
   the UCRT64 terminal. The SDK builds the separate WinRT Bluetooth helper; the portable
   bundle includes its runtime, so users need no .NET installation.
3. Install dependencies and build:

```sh
./scripts/install-deps-msys2.sh
cmake --preset windows-release -DPORTHOLE_WARNINGS_AS_ERRORS=ON
cmake --build --preset windows-release
ctest --preset windows-release
./build/windows-release/src/porthole.exe --name "Porthole Windows"
```

The Windows mirror uses native DNS-SD (`DnsServiceRegister`), with no Bonjour installation.
It requires Windows 10 version 1903 or newer, or Windows 11. Keep the phone and PC on the
same Private Wi-Fi network, then choose **Porthole Windows** in Screen Mirroring.

Bluetooth controls require **Windows 10 version 2004 (build 19041) or later**, plus an
adapter/driver supporting **Bluetooth LE peripheral mode**. Ordinary Bluetooth accessory
support alone is insufficient. Controls start by default; `--no-bluetooth` keeps viewing
available. Keep the generated `ble-winrt/` folder beside `porthole.exe`. For an explicit
mirror-only build, configure `-DPORTHOLE_WITH_WINDOWS_BLUETOOTH=OFF`.

Close Porthole before running the hardware probe in PowerShell:

```powershell
.\build\windows-release\src\ble-winrt\porthole-ble-winrt.exe --probe
```

The probe creates the real HID service and characteristics without advertising. On
success, launch Porthole, enable AssistiveTouch and disable Dwell Control on iPhone,
then pair **the PC's Windows Bluetooth name** in iPhone Bluetooth settings. Accept any
Windows pairing prompt. This name can differ from the Screen Mirroring name. F2 guides
pairing, video readiness and calibration; all three report subscriptions are required
before controls activate. A probe pass still requires a live iPhone input check.

See [the Windows helper guide](helpers/ble-winrt/README.md) for its protocol, diagnostics
and adapter requirements. The helper uses the same keyboard/mouse/consumer report map
and paced lanes as Linux, with notifications targeted to a single subscribed phone.

Create a portable bundle from a clean committed build with
`bash scripts/package-windows.sh`. It gathers runtime DLLs, licenses, exact dependency
metadata and matching source archives. The CI full Windows job builds/tests this preset
and uploads the portable bundle and sources. The no-AirPlay presets remain available as
minimal build checks.

For a one-file preview, [build the portable launcher](scripts/portable-windows/README.md)
from that bundle. Users run one executable, which prepares its complete files under
Windows TEMP and launches Porthole. The Bluetooth helper also embeds its own .NET runtime;
copying it cannot omit separate runtime DLLs. This portable file installs no drivers or
system services. Windows advertising and report subscriptions have been confirmed;
live input accuracy, responsiveness and failure recovery still need validation.

If the Windows firewall blocks discovery or mirroring, the bundle includes a reviewed
`windows-firewall.ps1` helper for the mirror executable and fixed ports on Private networks.
Installer/uninstaller work remains with the separate polish workstream.

### Build presets

| Preset | What it builds |
|---|---|
| `debug` / `release` | Everything, including the AirPlay core (needs all dependencies) |
| `windows-release` | Full Windows Wi-Fi mirror with native DNS-SD (MSYS2 UCRT64) |
| `debug-no-airplay` / `release-no-airplay` | Without the AirPlay core; needs only a C compiler and CMake |

## Repository layout

```
src/            one source tree for the Windows and Linux builds
  core/         logging, version info, local configuration
  control/      keyboard state, shortcuts, pointer actions and wheel routing
  hid/          HID reports and the framed Bluetooth helper connection
  airplay/      Wi-Fi receiver
  video/        decoding, phone-screen cropping, cursor and scroll-indicator detection
  ui/           SDL mirror window, branded bar, help and setup, shared by both builds
    generated/  embedded brand pixels, produced from branding/porthole
helpers/        Bluetooth helpers; the window does not fork per platform
  ble-bluez/    Linux helper
  ble-winrt/    Windows helper
tests/          unit tests (ctest)
cmake/          build helpers (warnings, UxPlay integration)
scripts/        dependency installers for Linux and Windows (MSYS2)
third_party/    UxPlay (git submodule, pinned to a release tag)
PLAN.md         plan, features, architecture and roadmap
```

## License

GPL-3.0-or-later; see [LICENSE](LICENSE). Porthole reuses UxPlay's AirPlay receiver core, which is
GPL-3.0.

Porthole is not affiliated with Apple. iPhone, iOS and AirPlay are trademarks of Apple Inc.
