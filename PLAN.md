# Porthole — Use your iPhone inside Windows

> Working name: **Porthole** (placeholder; avoid Apple trademarks like "iPhone"/"AirPlay" in the final product name).
> Goal: a **free, open-source** Windows app that shows your iPhone's screen in a window and lets you
> **control it with your PC mouse and keyboard**, over **Wi-Fi (priority)** and **USB** —
> "scrcpy for iPhone".
>
> Plan created: 2026-11-16 · Target device: iPhone on iOS 26.6.1 · Host: Windows PC with Bluetooth

---

## 1. Why this project

| Existing option | Shows screen | Control from PC | Free & open source |
|---|---|---|---|
| Apple iPhone Mirroring | ✅ | ✅ | ❌ Mac only |
| Microsoft Phone Link | ❌ (calls/SMS/notifications) | ❌ | ❌ |
| AirPlay receivers (UxPlay, LonelyScreen…) | ✅ Wi-Fi | ❌ | UxPlay only |
| iMyFone MirrorTo / ApowerMirror / Wormhole | ✅ | ✅ | ❌ paid, closed |
| scrcpy | ❌ Android only | — | ✅ |

**Gap:** no free/open-source app that does mirroring **and** control of an iPhone on Windows.

**Key advantage:** nothing needs to be installed on the iPhone. We use only built-in iOS features
(AirPlay Screen Mirroring + Bluetooth mouse/keyboard + AssistiveTouch).

---

## 2. How it works (core idea)

iOS does not let any app inject touches. So we use two channels that iOS already supports:

1. **Video (iPhone → PC)**
   - **Wi-Fi:** the PC pretends to be an AirPlay receiver (like an Apple TV). The iPhone sends its
     screen as an H.264/H.265 stream.
   - **USB:** the PC uses the same USB screen-capture protocol that QuickTime uses on a Mac to
     record an iPhone screen (reverse-engineered by open-source projects).
2. **Control (PC → iPhone)**
   - The PC pretends to be a **Bluetooth mouse + keyboard** (BLE HID). iOS AssistiveTouch shows a
     pointer; clicks become taps, drags become swipes, and typing goes to the focused text field.
   - We translate your clicks on the mirror window into pointer moves at the matching spot on the phone.

```
            ┌──────────────────────── Windows PC (Porthole.exe) ─────────────────────────┐
            │                                                                            │
 iPhone ──Wi-Fi──► AirPlay receiver ──┐                                                  │
 (iOS 26)   │      (mDNS + RTSP +     │                                                  │
            │       FairPlay + AES)   ├──► H.264/H.265 ──► Decoder ──► Mirror window     │
 iPhone ──USB────► QuickTime USB ─────┘     + audio       (FFmpeg,     (SDL2, scaling,   │
            │      capture (libusb)                        D3D11VA)     rotation)        │
            │                                                              │             │
            │                                           mouse/keyboard ◄───┘             │
            │                                                 │                          │
            │                                     Coordinate mapper + shortcut mapper    │
            │                                                 │                          │
 iPhone ◄──Bluetooth LE── BLE HID backend ◄───────────────────┘                          │
 (AssistiveTouch)         (WinRT GATT server  |  ESP32 dongle fallback)                  │
            └────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Features

### MVP (v0.1): Wi-Fi mirror and basic control
- [ ] PC appears automatically in iPhone **Control Center → Screen Mirroring** (Bonjour/mDNS)
- [ ] Live iPhone screen in a resizable window, aspect ratio kept, handles rotation
- [ ] One-time Bluetooth pairing: the PC shows up as a mouse/keyboard on the iPhone
- [ ] **Click = tap, click-drag = swipe, scroll wheel = scroll**, mapped to where you click in the window
- [ ] **Keyboard typing** into the iPhone (any text field)
- [ ] Shortcut keys / toolbar buttons: **Home, App Switcher, Control Center, Notification Center,
      Spotlight, Lock, Volume +/−, Screenshot**
- [ ] Setup wizard that walks the user through enabling AssistiveTouch and pairing

### v0.2: everyday use
- [ ] **Audio** forwarding (hear the iPhone through PC speakers), toggleable
- [ ] Fullscreen, always-on-top, 1:1 pixel size, remember window position
- [ ] **Screenshot** (PNG) and **screen recording** (MP4 without re-encoding the stream)
- [ ] **Paste PC clipboard text** into the iPhone (sent as keystrokes)
- [ ] Connection security: PIN code on first connect, allow-list of trusted iPhones
- [ ] Tray icon, start with Windows, auto-reconnect
- [ ] Windows installer that also adds firewall rules (MSI or Inno Setup)

### v0.3: USB
- [ ] **USB mirroring** with lower latency, no Wi-Fi needed
- [ ] Auto-select: use USB when plugged in, Wi-Fi otherwise
- [ ] Device picker for multiple iPhones/iPads

### Later / advanced
- [ ] **Precise touch mode** (optional, for power users): real multi-touch, pinch and long-press
      through Apple's developer test tool (WebDriverAgent) over USB. Needs iPhone Developer Mode.
- [ ] **ESP32 Bluetooth dongle** backend (≈$5), used if the PC's own Bluetooth can't act as a mouse
- [ ] H.265 / high-resolution (up to 4K) mirroring
- [ ] Drag-and-drop photos/files to the iPhone over USB
- [ ] iPad support (same pipeline, larger screen)
- [ ] Low-latency tuning mode, FPS/latency overlay
- [ ] Multiple languages

### Non-goals
- No jailbreak, no iPhone companion app required, no cloud/relay servers (everything stays on your LAN/cable)
- Not a remote-access tool over the internet (LAN and USB only, at least initially)

---

## 4. Technical design

### 4.1 Wi-Fi video: AirPlay receiver
- **Base:** reuse the receiver core from **UxPlay** (`lib/` — RTSP server, pairing, FairPlay
  handshake, stream decryption). It's mature and already works with recent iOS.
  → Project license: **GPL-3.0** (required by UxPlay).
- **Discovery:** advertise `_airplay._tcp` + `_raop._tcp`.
  - Windows: use the built-in mDNS API (`DnsServiceRegister`) so users don't need Apple Bonjour;
    fall back to a bundled mDNSResponder if needed.
  - Linux (dev): Avahi.
- **Ports:** TCP 7000/7001/7100, UDP 6000/6001/7011 (fixed, so the firewall can be configured).
- **Stream:** H.264 (default) or H.265; resolution advertised to the iPhone (e.g. 1920×1080,
  adjustable). Audio is AAC-ELD (mirroring) / ALAC.
- **Known limitation:** DRM apps (Netflix, etc.) show black when mirrored, same as on an Apple TV.

### 4.2 USB video: QuickTime screen-capture protocol
- A vendor USB request switches the iPhone into a hidden USB configuration that streams the
  screen (H.264 + audio as CoreMedia sample buffers).
- Implement in C with **libusb**, using the open-source reverse-engineering work
  (`quicktime_video_hack`) as a reference.
- **Windows challenge:** the Apple Mobile Device driver owns the iPhone. We need **WinUSB bound only
  to the screen-capture interface** (via libwdi, installed by our installer) without breaking iTunes
  or the Apple Devices app. → Research spike S4.

### 4.3 Control: Bluetooth LE HID
- **HID report descriptor** (one composite device):
  - Report 1: **Keyboard** (standard 104-key + modifiers)
  - Report 2: **Mouse**: 8 buttons, relative X/Y (16-bit), wheel, horizontal pan
  - Report 3: **Consumer control**: Volume, Play/Pause, AC Home, etc.
  - (Experiment) **absolute pointer/digitizer** report. If iOS accepts it, mapping becomes exact.
- **Windows backend:** WinRT `GattServiceProvider` publishing the HID-over-GATT (HOGP) service
  0x1812 + Battery + Device Info, with advertising. → Research spike S2 (risk: Windows may
  restrict the HID service).
- **Fallback backend:** ESP32-S3/C3 firmware (NimBLE HID) that gets reports from the PC over
  USB-serial. Works on any PC and avoids Windows Bluetooth quirks.
- **Linux backend (dev only):** BlueZ D-Bus GATT server.

### 4.4 Click → tap mapping
iOS mice are relative and pointer movement has acceleration, so a click at (x, y) in the window
has to be turned into the right pointer moves:
1. **Anchor:** push the pointer hard into the top-left corner to get a known (0, 0).
2. **Move:** send many small steps, which keeps acceleration close to linear, scaled by a
   calibrated factor.
3. **Closed-loop correction (preferred):** if the AssistiveTouch pointer shows up in the mirrored
   video, detect it in frames and correct drift automatically.
4. **Calibration wizard:** moves the pointer by known amounts and measures the result. It asks the
   user to set AssistiveTouch Tracking Speed to a fixed value.
5. Continuous mouse movement inside the window is forwarded live, so the user can "drive" the
   pointer directly when needed.

### 4.5 Shortcut actions
Most reliable approach: **extra mouse buttons**. In *AssistiveTouch → Devices → [Porthole] →
Customize Additional Buttons* the user assigns buttons 3–8 to Home, App Switcher, Control Center,
Notification Center, Lock Screen and Screenshot (the setup wizard guides this). Porthole then
"presses" those virtual buttons when you press the hotkey or toolbar button.
Also: HID keyboard shortcuts (Cmd+Space Spotlight, etc.) and consumer keys (volume).

### 4.6 App structure (C/C++, CMake)
```
porthole/
├─ src/
│  ├─ core/        app state, device sessions, config
│  ├─ airplay/     vendored UxPlay lib (GPL-3.0) + our glue
│  ├─ usbcap/      QuickTime USB capture (libusb)
│  ├─ decode/      FFmpeg (D3D11VA hardware decode on Windows, VAAPI on Linux)
│  ├─ render/      SDL2 mirror window, scaling, rotation, overlay toolbar
│  ├─ audio/       FFmpeg AAC-ELD/ALAC decode → SDL audio
│  ├─ input/       mouse/keyboard capture, coordinate mapper, shortcut mapper
│  ├─ hid/         backends: winrt_ble (C++/WinRT), esp32_serial, bluez (dev)
│  └─ ui/          tray, settings, setup wizard
├─ firmware/esp32-hid/   optional dongle firmware
├─ installer/            Windows installer + firewall rules + WinUSB (libwdi)
├─ docs/
├─ cmake/                build helpers (warnings, UxPlay integration)
├─ scripts/              dependency installers (Linux apt, Windows MSYS2)
├─ third_party/UxPlay/   git submodule pinned to a release tag (v1.73.7)
└─ .github/workflows/    Windows (MSYS2) and Linux CI builds
```
- **Dependencies:** OpenSSL, libplist, FFmpeg, SDL2, libusb.
- **Windows toolchain: MSYS2 UCRT64 (MinGW-w64 GCC)**, not Visual Studio. UxPlay's core uses POSIX
  threads and is built with MinGW upstream, so we match that. MSYS2 provides all dependencies via
  `pacman` (`scripts/install-deps-msys2.sh`). *(Decided 2026-11-16 during project setup.)*
- **Bluetooth on Windows:** the WinRT Bluetooth APIs are easiest to use from C# or C++/WinRT
  (MSVC), which don't mix well with MinGW. Plan: a small helper process (`porthole-ble.exe`)
  that owns the GATT server and receives HID reports from the main app over a pipe. The ESP32
  dongle backend uses the same message format over USB-serial.
- **Performance targets:** Wi-Fi latency < 150 ms, USB < 80 ms, 60 fps, < 10% CPU with hardware decode.

### 4.7 Development workflow
- Code lives in this Linux VM (`~/Documents/ios-windows`) under git, pushed to GitHub.
- **Linux build** in the VM for fast iteration on AirPlay, decoding, rendering and mapping. The VM's
  bridged network lets the iPhone find it.
- **Windows build** via GitHub Actions (MSYS2 UCRT64); the user tests the `.exe` on the Windows host.
  Until Porthole has its own Windows mDNS code, Windows CI builds without the AirPlay core
  (UxPlay needs Apple's Bonjour SDK there).
- **Bluetooth work happens on Windows** (the VM has no Bluetooth). Optionally, pass the host
  Bluetooth adapter through to the VM for BlueZ experiments.

---

## 5. Roadmap

### Phase 0: Feasibility spikes (do first, ~1–2 weeks)
| # | Question | How we test | Pass criteria |
|---|---|---|---|
| **S1** ✅ | Does AirPlay mirroring work with iOS 26.6.1? | Stock `uxplay` 1.68 in the VM; if it fails, build latest UxPlay from source | iPhone screen visible on PC. **Passed 2026-11-16** (in VMware the default video sink shows green lines; `-vs ximagesink` fixes it) |
| **S2** | Can Windows act as a BLE mouse/keyboard for the iPhone? | Small WinRT/C# or Python (`bless`) GATT server advertising HID 0x1812 | iPhone pairs; pointer moves; text types |
| **S3** ✅ | Can we hit an exact screen spot? | Corner-anchor + step moves; check whether the pointer appears in the AirPlay video; try an absolute-pointer descriptor | Tap lands within ~10 px of target. **Passed 2026-11-16**: Calculator `AC 7 + 8 =` → 15, every tap first attempt, ≤ 4.6 px error (absolute-pointer descriptor not tried yet) |
| **S4** | Can we grab USB video on Windows without breaking Apple's driver? | libusb + WinUSB on the capture interface only | Frames received over USB on Windows |
| **S5** ✅ (Lock not tested) | Do extra mouse buttons / consumer keys trigger Home, Control Center…? | Configure AssistiveTouch buttons, send HID reports | Each action works on iOS 26. **Passed 2026-11-16:** Home (0x223), ⌘+Space, typing, ⌘+[ (Back), Escape; buttons 2–5 → Secondary Click, App Switcher, Control Center, Notification Center. Button 6 → Screenshot works (saved to Photos; iOS just doesn't show the preview in the mirror). Lock Screen deliberately not tested |

If S2 fails → use the ESP32 dongle as the default control path. If S4 fails → ship Wi-Fi first and
keep researching USB.

#### Spike results so far (2026-11-16, Linux VM + BlueZ, iOS 26.6.1)
- **Pairing:** the iPhone only listed the device after (1) LE-only mode (adv flags 0x06) and
  (2) a **20–30 ms advertising interval**; BlueZ's 1.28 s default was invisible to iOS Settings.
  Then it paired first try ("Just Works") and subscribed to all 3 input reports. The HID
  descriptor in `src/hid/hid_reports.c` was accepted unchanged.
- **Reconnect:** a bonded iPhone doesn't re-subscribe after reconnecting; it expects the
  device to remember its subscriptions. Every backend must keep the GATT layout stable and
  persist notification state per bonded device.
- **S5:** consumer *AC Home* (0x223) → Home Screen ✅. ⌘+Space → Spotlight ✅, typing ✅,
  Enter opens the top hit ✅. Keys sent while iOS is still animating can be lost, so the app
  should wait for the screen to settle (it can see the screen) between steps.
- **S3:** the AssistiveTouch pointer **is visible in the AirPlay stream**, so closed-loop
  positioning works. iOS acceleration is deterministic during continuous movement:
  one report of *s* units ≈ `0.25·s + 0.055·s²` px of a 500-px-wide mirror (default
  tracking speed, 15 ms report interval). Taps landed within 2–8 px of target (Calculator
  buttons). Small corrective nudges after a pause were unreliable → correct by re-anchoring
  in the corner and redoing the move as one continuous burst.
- **Gotchas:** AssistiveTouch **Dwell Control** must be off (it auto-taps wherever the pointer
  rests); the setup wizard should check this. Never tap without first verifying the expected
  screen is showing.
- **Guarded Calculator retest (2026-11-16, 17:02): passed.** `AC 7 + 8 =` showed **15**; all
  five taps on the first attempt, 3.6–4.6 px from the button centre, model scale unchanged
  (1.000), so the open-loop model alone was accurate. The screen guard also proved itself: it
  stopped a run when an incoming call covered the Calculator (the `+` target sits next to
  *Accept*).
- **Why the earlier resumed run failed:** (1) after the call the iPhone reconnected and never
  re-subscribed, so reports went nowhere → the spike now re-registers the HID service whenever
  the iPhone reconnects without subscribing within 3 s, which makes iOS rediscover and
  subscribe again; (2) the UxPlay test viewer had drifted to **several seconds of lag**, so
  captures showed stale frames → restarting UxPlay brought it back to ~0.1 s. (`-vsync no`
  broke mirroring entirely in UxPlay 1.68 here.) The real app needs a frame-freshness check
  before trusting the video for positioning.
- **S5 (2026-11-16, 17:05–17:17):** configured entirely from the PC through the mirror
  (Spotlight → AssistiveTouch settings, wheel scrolling, screen-guarded taps, ⌘+[ for Back).
  *Customize Additional Buttons…* shows "Press a button on your pointer device"; pressing a
  button adds a "Button N" row (button 2 defaults to **Secondary Click**, which is iOS's real
  right-click: it opens app quick-action menus). Buttons 3–6 were set to App Switcher, Control
  Center, Notification Center, Screenshot; all of 2–6 verified working (the screenshot landed in
  Photos, though its preview never appears in the mirror). Other findings:
  - The **mouse wheel** scrolls, with natural direction (positive wheel moves content up) and
    only a few px per report, so the app must send many reports and invert/scale the PC wheel.
  - **Escape** dismisses menus (and is "clear" in Calculator); **⌘+[** goes Back in Settings.
  - The **AssistiveTouch floating button** sits on a screen edge and intercepts clicks near it
    (a right-click meant for the Calculator display opened the AssistiveTouch menu). The app
    should detect it in the video and keep clicks clear of it, or the wizard should suggest
    turning *Always Show Menu* off.
  - Spotlight's top hit can be a web suggestion: never press Enter without checking the
    highlighted result.
  - The action list includes **Restart** and **SOS**; the setup wizard must never be able to
    assign those by mistake.
- **Pointer appearance:** with Dwell off the pointer is a ~22 px grey dot (the ~55 px ring
  earlier was Dwell's countdown). It is still easy to detect on the black Calculator keypad
  (contrast 61 vs threshold 24).
- **Capture and tap guards:** keep the mirror visible above other windows; XWD captures of
  covered regions can contain stale pixels. Capture a new reference after resizing (the mirror
  changed from 500×1080 to 500×1095 during this session). The tap script now rejects a resized
  mirror, invalid coordinates, a missing pointer, and positioning errors over 5 px even after
  exhausting retries. Regression checks: `python3 -m unittest discover -s tests -p test_tap_test.py -v`.

### Linux first (decided 2026-11-16)
The complete app (Phases 1–2) gets built and working in the Linux VM first, then ported to
Windows. Platform-specific code stays behind small interfaces so the port only swaps backends.

| Milestone | Delivers | Linux backend | Windows backend (later) |
|---|---|---|---|
| **M1** Mirror window | `porthole` shows the iPhone screen: UxPlay core → FFmpeg decode → SDL2 window | Avahi mDNS | Windows mDNS API |
| **M2** Keyboard & shortcuts | typing, Home/App Switcher/Control Center/… hotkeys, scroll wheel, right-click | BLE helper: Python + BlueZ (from the spike) | BLE helper: WinRT |
| **M3** Pointer | the phone pointer follows the PC cursor over the window; clicks and drags land where you click; drift corrected from the decoded video | same helper | same helper |
| **M4** Polish | status/help overlay, reconnect handling, setup checks (AssistiveTouch on, Dwell off) | — | — |

**Bluetooth helper protocol:** the main app starts the helper and writes framed HID reports to
its stdin (`type, length, payload`; types = keyboard / mouse / consumer). The helper sends them
at a fixed 15 ms pace (the acceleration model depends on it) and prints status lines
(connected, subscribed) on stdout. The ESP32 dongle can reuse the same framing.

#### M2 input fixes and live results (2026-11-16)
- Keyboard handling now uses the current modifier snapshot, including Ctrl held before the
  window receives focus. Key releases are handled across Alt shortcuts, including releasing
  Alt before the shortcut key or before another modifier. Focus loss releases keyboard and
  shortcut mouse buttons.
- Extra-button shortcuts hold their button until key release and ignore duplicate key-down
  events. Live PC-input tests now verify **Alt+S keeps App Switcher open** and **Alt+C keeps
  Control Center open**, including captures several seconds after releasing the keys. The
  phone's saved button 3–6 assignments were inspected and are correct.
- The BlueZ sender now waits for each notification callback to finish before spacing the next
  report. Previously a busy GLib loop could flush queued reports together; the live trace
  showed two mouse reports only about 1 ms apart. Sync acknowledgements now follow actual
  dispatch. A regression test fails on the old sender and passes with this change.
- Bluetooth readiness now requires fresh **keyboard, mouse and consumer** subscriptions.
  Waiting only for the mouse could show control as "on" while keyboard reports went nowhere.
  Reconnect recovery checks all three reports and disconnects a stale link if re-registering
  alone fails. Ctrl+A was verified selecting Spotlight text with Ctrl held before window
  focus, then typing replaced the selected text.
- The earlier wheel-only routing used coordinates from each SDL wheel event and ignored
  positions outside the phone picture. It anchored once, followed changed targets relatively,
  and tolerated 20 model pixels of jitter. It did not handle ordinary mouse motion, so it
  could not keep the two pointers aligned. M3 replaces that position estimate with a shared
  continuous pointer controller and video feedback.
- **Overscroll correction (2026-11-16, 20:31):** reducing wheel sensitivity alone did not
  fix the reported problem: repeated input could stretch nearly the whole Settings page out
  of view, leaving empty space. Turning off Perform Touch Gestures did not prevent it; that
  setting was restored. Decoded frames now locate the native scroll indicator. A pinned or
  compressed indicator stops further input into that edge, drops pending movement, and
  allows reversal. Batches wait briefly for video feedback; a missing frame has a timeout.
  Normal vertical notches use three unit reports, falling to one near the approaching edge.
  Repeated notches no longer send artificial +/-1 pointer movements or a 60 ms delay.
  One notch is in flight, at most four wait, and reversal replaces old queued movement.
  The **actual C controller** was connected to the existing live Bluetooth helper for a
  sustained down/up test: it stopped at the bottom, reversed, and stopped at the top, without
  the large empty area. Small native bounce remains. Custom scroll views without a detectable
  indicator are not yet covered by the boundary guard. The updated app has been launched.
- After a fresh Bluetooth connection, the earlier **2.0 / 2.0 calibration was wrong**: it
  placed the pointer around halfway to the target. Re-measuring with **1.0 / 1.0** reached
  (251.5, 699.5) for a (250, 700) target; a subsequent guarded tap was 2.7 px from its target.
  The corrected values are saved in the local per-user
  `pointer-scale` file (two positive numbers, X then Y); absent a file, the S3 model uses 1.0.
  Changing tracking sensitivity requires recalibration. The M3 implementation below adds
  continuous correction; a guided calibration wizard remains pending.
- Bluetooth helper failures now retry every five seconds while the existing mirror window,
  receiver and decoder keep running. Keyboard input is discarded until Bluetooth is ready;
  reconnect begins with released reports so old key presses are not replayed. A headless
  integration test exits the first helper and confirms a second helper starts while the
  mirror components are created only once. Service restarts and adapter removal exit the
  helper so the app can retry; an unbuffered stdin reader avoids a Python shutdown crash.
- Validation before the M3 changes: full debug build with warnings as errors and all
  **10 CTest checks** pass;
  the build without AirPlay and its **8 CTest checks** also pass. Tests cover modifier
  snapshots, shortcut repetition/release order, focus loss, scrolling batches and calibration,
  sender ordering, partial subscriptions, helper shutdown and reconnect lifecycle. Added
  scrollbar detection (light/dark, cropped frames, faded and compressed indicators), edge
  suppression/reversal, frame pacing/timeouts, and wheel-only follow-up regressions.
- The VM restart interrupted testing, but the Intel AX211 adapter is now passed through and
  both mirroring and Bluetooth control are running. An initial advertising timeout was
  resolved by cycling the guest Bluetooth radio.

#### M3/M4 implementation and controller validation
- Added a continuous pointer controller for ordinary mouse motion, shared with wheel
  positioning. Decoded video locates the AssistiveTouch dot so the controller can correct
  drift instead of relying on the saved movement multiplier alone. A queued button press
  waits for a fresh position confirmation; missing feedback must not become an estimated tap.
- Left/right button events preserve their target coordinates while positioning completes.
  Queued down/up events support short clicks and drags, preserve a held button during motion,
  and release it on focus loss, cancellation or disconnection. Motion coalesces while a
  report batch is in flight. Cursor detection handles mixed icon/background edges; during a
  drag, a 40-pixel search region and movement plausibility checks prevent moving icons from
  being accepted as the pointer. Unit coverage includes landscape mapping and edge clamps.
- **Live C-controller checks:** attached the actual controller to the existing Bluetooth
  helper. Four target positions were within **1.0–1.4 px**. Calculator `AC 7 + 8 =` produced
  **15**, with all five taps **1.0–3.6 px** from their targets. A secondary click on the Phone
  dock icon opened its native context menu (**1.4 px** error). A vertical App Library drag
  from y=450 to y=850 ended at **(249, 850)** for a **(250, 850)** target (**1 px** error).
- Added local **F1 help**, a waiting/setup screen and a view-only connection banner without
  another font dependency. Help lists controls, shortcuts, Bluetooth pairing, AssistiveTouch,
  Dwell Control and button assignments. Opening help blocks phone coordinate mapping so
  clicks cannot pass through it. The window API exposes status, help toggling and help state.
- The window changes pass a debug build and isolated SDL renderer checks for readable help,
  resize/letterbox mapping, blocked mapping during help/unavailable controls, and restoration
  after help closes or controls return. All **14 full-build** and **10 minimal-build** checks
  pass. F1 help was also opened and closed through real SDL key events in the running app.
  Bluetooth reconnected after a guest radio reset and fresh subscriptions. The integrated
  SDL mouse-event checks are now complete. A guided multi-step pairing/calibration wizard
  remains part of the later roadmap; the current Linux mouse controls and local help work.
- The first integrated mouse check exposed an idle-video timing bug: the final pointer image
  can arrive before the HID acknowledgement, with no later decoded frame. Pointer feedback
  now polls the retained image after acknowledgement, rejects images predating movement,
  caches cursor searches, and requires a stable position before acting. A real-app SDL
  regression publishes only the pre-acknowledgement image and verifies an aligned click and
  release; removing the retained-image polling reproduces the timeout. This test uses an
  isolated configuration directory and also passes sanitizers.
- **Integrated window checks:** real PC mouse events aligned four positions within
  **0–3.2 px**, and five real PC clicks entered `7 + 8 = 15` in Calculator. Native secondary
  click opened the Phone context menu. Eighty wheel events in each direction reached the
  Touch settings boundaries and stopped without the large empty stretch. The held-cursor
  size contracts during dragging; detection now searches both hover and pressed sizes.
  Pressed-dot regressions cover odd/even sample positions, and a full-app drag test checks
  a validated button-up acknowledgement so timeout cleanup cannot count as a successful
  drag. The corrected running app passed real PC drags in both directions: (250,450) to
  (250,850) ended at (250,849), and the reverse ended at (251,450). Both were within **1 px**
  and completed a queued, acknowledged button release rather than timeout cleanup.

### Phase 1: Wi-Fi mirror app (v0.1-alpha)
- Project skeleton, CMake, CI for Linux + Windows
- Vendor UxPlay core, replace GStreamer renderer with FFmpeg + SDL2
- Windows mDNS, fixed ports, firewall rules
- **Exit:** `porthole.exe` on Windows shows the iPhone screen over Wi-Fi

### Phase 2: Control (v0.1)
- BLE HID backend on Windows (or ESP32 fallback)
- Coordinate mapper + calibration, keyboard forwarding, shortcut actions
- Setup wizard (AssistiveTouch, pairing, button mapping)
- **Exit:** you can open apps, scroll, swipe and type on the iPhone using only the PC

### Phase 3: Polish (v0.2)
- Audio, screenshot/recording, clipboard paste, PIN/allow-list, tray, installer

### Phase 4: USB (v0.3)
- QuickTime USB capture, WinUSB install, auto-switch USB/Wi-Fi

### Phase 5: Advanced
- Precise touch mode (WebDriverAgent), H.265/4K, iPad, file drop

---

## 6. Risks and mitigations
| Risk | Impact | Mitigation |
|---|---|---|
| An iOS update changes AirPlay/FairPlay | Mirroring breaks | Track upstream UxPlay closely; keep our changes to its core small |
| Windows won't publish the BLE HID service | No built-in control | ESP32 dongle backend |
| Pointer acceleration makes taps inaccurate | Bad UX | Small-step moves, calibration, closed-loop correction from video |
| USB driver conflict with Apple's driver | USB mode unusable | Bind WinUSB to the capture interface only; Wi-Fi remains primary |
| Firewall / router blocks mDNS or ports | iPhone can't find the PC | Installer adds rules; built-in diagnostics page; manual IP option |
| GPL-3.0 obligations | Licensing | The whole project is GPL-3.0 and open source |
| Trademarks | Naming | Don't use "iPhone"/"AirPlay" in the product name or logo |

---

## 7. One-time iPhone setup (what end users will do)
1. Same Wi-Fi as the PC (or plug in USB).
2. Settings → Accessibility → Touch → **AssistiveTouch: On**.
3. Settings → Bluetooth → pair with **Porthole** (once).
4. Control Center → **Screen Mirroring** → choose your PC.
5. (Wizard) assign AssistiveTouch extra buttons → Home / App Switcher / Control Center / etc.

---

## 8. Next immediate steps
1. ~~**S1:** run `uxplay` in the VM and mirror the iPhone (iOS 26.6.1).~~ ✅ Works.
2. ~~Initialize git repo + project skeleton.~~ ✅ Done (CMake, UxPlay submodule, HID reports, tests, CI).
3. ~~**S3:** guarded Calculator tap test.~~ ✅ `7 + 8 = 15`, ≤ 4.6 px error.
4. ~~**S5:** AssistiveTouch extra mouse buttons.~~ ✅ Buttons 2–6 verified (Lock Screen untested
   on purpose).
5. ~~Finish the **integrated app mouse-event check on Linux**.~~ ✅ Pointer alignment,
   taps, secondary-click, drags in both directions and wheel boundaries verified live.
   The current Linux controls are complete; a guided pairing/calibration wizard remains
   on the later roadmap before the Windows port.
6. **S2:** the same Bluetooth device on the Windows host with the WinRT GATT server (fast
   advertising, LE-only, re-subscribe handling), as part of the Windows port.
