# Porthole

**Use your iPhone inside Windows.** Porthole shows your iPhone's screen in a window on your PC and
lets you control it with your mouse and keyboard, over Wi-Fi or USB. Think *scrcpy, but for iPhone*.

Free and open source (GPL-3.0). Nothing to install on the iPhone: it uses built-in iOS features
(Screen Mirroring, Bluetooth mouse/keyboard and AssistiveTouch).

> **Status: Linux prototype.** Wi-Fi mirroring and keyboard forwarding run in Porthole's own
> window. Keyboard shortcuts and wheel scrolling have been tested on the iPhone. Continuous
> pointer tracking, taps, secondary clicks and drags have been verified with the C controller
> on the live iPhone. Integrated mouse, click, drag and scroll checks also pass in the app.
> USB capture and the Windows receiver remain planned.
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

Left Alt shortcuts: **H** Home, **F** Spotlight, **S** App Switcher, **C** Control Center,
**N** Notification Center, **P** Screenshot, **←** Back, **↑/↓** Volume. Buttons 3–6 must be
assigned to the corresponding actions in AssistiveTouch's device settings. PC Ctrl acts as
Command on the iPhone. **F1** opens local controls and setup help; input is paused while help
is open. The waiting screen shows mirroring and Bluetooth setup steps, and a status banner
identifies when the mirror is available without controls.

Mouse movement over the phone picture now drives a continuous pointer controller. Left click
queues a tap, right click queues a secondary click, and holding and dragging queues a swipe.
Video feedback confirms the pointer position before a button is pressed; losing focus or
disconnecting cancels queued actions and releases held buttons. Live controller checks reached
four targets within 1.4 pixels, entered `7 + 8 = 15` in Calculator, opened a native secondary-click
menu and performed a vertical drag. Integrated window tests also verified real PC mouse
alignment, Calculator clicks, right-click, dragging in both directions and scrolling to
page edges. The two live drag endpoints were within 1 pixel, and both completed their
button releases. Wheel input uses the same target position before scrolling.
Scrolling now watches the native iOS scroll indicator and stops continued input at an edge;
normal wheel travel is faster, with smaller steps near the boundary. This was checked in
Settings; custom scroll views without an indicator still need testing.

The prototype reads pointer calibration from the per-user `pointer-scale` file (on Linux,
`~/.config/porthole/pointer-scale`). It contains X and Y movement multipliers, for example
`1.0 1.0`, and seeds the movement model at startup. Calibration depends on the phone's
AssistiveTouch tracking sensitivity; video feedback corrects placement, and a guided
calibration UI remains planned.

### Windows (MSYS2)

1. Install [MSYS2](https://www.msys2.org) and open an **MSYS2 UCRT64** terminal.
2. Install dependencies and build:

```sh
./scripts/install-deps-msys2.sh
cmake --preset release-no-airplay
cmake --build --preset release-no-airplay
./build/release-no-airplay/src/porthole.exe --version
```

The Windows build doesn't include the Wi-Fi receiver yet. UxPlay's core needs Apple's Bonjour SDK
on Windows; Porthole will use Windows' built-in mDNS instead (Phase 1).

### Build presets

| Preset | What it builds |
|---|---|
| `debug` / `release` | Everything, including the AirPlay core (needs all dependencies) |
| `debug-no-airplay` / `release-no-airplay` | Without the AirPlay core; needs only a C compiler and CMake |

## Repository layout

```
src/            Porthole's own code
  core/         logging, version info, local configuration
  control/      keyboard state, shortcuts, pointer actions and wheel routing
  hid/          HID reports and the framed Bluetooth helper connection
  airplay/      Wi-Fi receiver
  video/        decoding, phone-screen cropping, cursor and scroll-indicator detection
  ui/           SDL mirror window, connection guidance and local help
helpers/        platform-specific Bluetooth helper (BlueZ on Linux)
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
