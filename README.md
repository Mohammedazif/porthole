# Porthole

**Use your iPhone inside Windows.** Porthole shows your iPhone's screen in a window on your PC and
lets you control it with your mouse and keyboard, over Wi-Fi or USB. Think *scrcpy, but for iPhone*.

Free and open source (GPL-3.0). Nothing to install on the iPhone: it uses built-in iOS features
(Screen Mirroring, Bluetooth mouse/keyboard and AssistiveTouch).

> **Status: early development (Phase 0, feasibility).** It doesn't do anything useful yet.
> See [PLAN.md](PLAN.md) for the full plan, feature list and roadmap.
>
> | Spike | Question | Result |
> |---|---|---|
> | S1 | Does Wi-Fi mirroring work with iOS 26.6.1? | ✅ Works |
> | S2 | Can Windows act as a Bluetooth mouse/keyboard for the iPhone? | ⏳ |
> | S3 | Can a click on the PC land on the exact spot on the iPhone? | ✅ Within 5 px (tested in the Linux VM) |
> | S4 | Can we capture the screen over USB on Windows? | ⏳ |
> | S5 | Can we trigger Home, Control Center, etc.? | 🟡 Home, Spotlight and typing work; other actions untested |

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
  core/         logging, version info
  hid/          the virtual Bluetooth mouse/keyboard (HID report descriptor + reports)
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
