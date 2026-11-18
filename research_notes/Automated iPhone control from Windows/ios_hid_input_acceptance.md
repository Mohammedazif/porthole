# iOS HID Input Acceptance (BLE / USB) and Exact Pointer Positioning on iPhone

Scope note: research date 2026-11-18. iOS 27 is now shipping; the project's target phone runs iOS 26.6.1. The strongest new evidence (late Oct / early Nov 2026) comes from one open-source project, `esp32-iphone-agent`, and was tested on **iOS 27 on an iPhone 17 Pro**, not on iOS 26. No source I found tests absolute input on iOS 26 specifically. That one point decides whether the approach works for Porthole, so it needs a device test.

## Q1. Does iPhone accept absolute pointer (Generic Desktop X/Y absolute), Digitizer page 0x0D (touch screen / pen / multi-touch) over BLE HOGP, classic BT or USB?

### Takeaway
Yes for the absolute mouse, as of iOS 27. A Generic Desktop Mouse collection with absolute 16-bit X/Y (0..32767) maps straight to screen fractions while AssistiveTouch is on. Two reports show it: over USB, 10/10 taps landed within 0.001 of the target; over BLE HOGP, a later device test confirmed it, with no calibration. Digitizer touch screen, multi-touch and pen descriptors got **no response** on iPhone (iOS 27, USB). Older reports (iOS 13/14, 2020, iPad) found that absolute BLE mice did not move the pointer, so the behaviour on iOS 26 is unverified.

### Cited Findings
- **The spike that tested all four descriptors** (`spikes/abs-touch`): one firmware image held a single-touch touch screen (Digitizer / Touch Screen: tip, in range, X/Y 0-32767), a multi-touch screen (contact id, contact count, Feature max contacts), a pen (Digitizer / Pen) and an absolute mouse (Generic Desktop / Mouse, left button, X/Y). It ran over USB on an iPhone 17 Pro with iOS 27, in "pure mode" (the tested collection alone in the report map). Results: "Touch screen, multi-touch, pen: nothing / nothing"; "Absolute mouse: works, delivered as `touch`" with AssistiveTouch on, and "nothing" with AssistiveTouch off. — [chenshaorong0118/esp32-iphone-agent-ble spikes/abs-touch/README.md](https://github.com/chenshaorong0118/esp32-iphone-agent-ble/tree/spike/abs-touch/spikes/abs-touch)
- Accuracy in that spike: "ten taps over two loops all landed within 0.001 of the target (screen fractions). Tested inside Safari only." With `pure 2` (absolute mouse + keyboard + consumer control, no relative mouse) "the taps land the same". A wheel event arrived at the right position "but with deltaY 0, so scrolling is unconfirmed." — [same README](https://github.com/chenshaorong0118/esp32-iphone-agent-ble/tree/spike/abs-touch/spikes/abs-touch)
- Issue #98 (2026-11-06), translated from Chinese: the USB absolute mouse works on iPhone (iOS 27.3, StickS3, AssistiveTouch on). Taps land at the screen-proportional position, all 10 accurate with error under 0.001, and swipes are accurate too. "Touch screen and stylus descriptors got no response." Only Safari was tested; system UI, other apps and Bluetooth were not. — [onehumanbeing/esp32-iphone-agent #98](https://github.com/onehumanbeing/esp32-iphone-agent/issues/98). Caveat: "iOS 27.3" is an unusually high point release for November 2026. It may be a beta or a mislabel; the spike README just says "iOS 27".
- **Critical quirk:** "iOS ignores X/Y in a report that changes a button." A tap is therefore: move, wait about 60 ms, press, release. The firmware uses `ABS_SETTLE_MS = 60`. — [PR #99](https://github.com/onehumanbeing/esp32-iphone-agent/pull/99); [abs_mouse.h / hid_bridge.ino in onehumanbeing/esp32-iphone-agent](https://github.com/onehumanbeing/esp32-iphone-agent/blob/main/hardware/hid_bridge/abs_mouse.h)
- **Drag quirk:** "a jump is a flick to iOS, and the list flies." Drags must glide in small steps at roughly relative-mouse speed (8 ms reports, step about 245 of 32767 by default). A Home or Back flick has to be fast (about 0.1 s), or it opens the App Switcher. — [abs_mouse.h](https://github.com/onehumanbeing/esp32-iphone-agent/blob/main/hardware/hid_bridge/abs_mouse.h); [PR #99](https://github.com/onehumanbeing/esp32-iphone-agent/pull/99)
- **Working USB descriptor** (report ID 10): Usage Page Generic Desktop, Usage Mouse, Application; Pointer, Physical; Buttons 1-3 (3 bits) plus 5 bits of padding; X, Y with Logical Min 0, Logical Max 0x7FFF, Physical Min 0, Physical Max 0x7FFF, 16-bit ×2, Input (Data, Var, **Abs**) `0x81 0x02`; Wheel int8 relative `0x81 0x06`. Report bytes: `[buttons, xLo, xHi, yLo, yHi, wheel]`. The header comment says: "iOS maps X/Y straight to screen fractions while AssistiveTouch is on, so no homing or calibration." — [abs_mouse.h](https://github.com/onehumanbeing/esp32-iphone-agent/blob/main/hardware/hid_bridge/abs_mouse.h)
- **Over BLE HOGP:** the BLE_HID build uses one HOGP service (0x1812) with three report IDs: keyboard (1); mouse (2), with 5 buttons, absolute X/Y 0..32767 when `ABSOLUTE_POINTER` is set, a wheel and AC Pan; and consumer control (3), a 10-bit array. The device test on 2026-10-27 (ESP32-S3, iPhone, board powered but not plugged into the phone) reported: "Absolute pointer over BLE: yes — `at x y` lands where it should without calibration, `click` opens the tapped icon." The test did not state the iOS version. Pairing is "Just Works" in Settings > Bluetooth, HOGP needs an encrypted, bonded link, and iOS shares one LE connection between system HID and the controller app. — [jinwon-int/esp32-iphone-agent #104](https://github.com/jinwon-int/esp32-iphone-agent/issues/104); [ble_hid.h](https://github.com/jinwon-int/esp32-iphone-agent/blob/main/hardware/hid_bridge/ble_hid.h)
- **Benchmark on iOS 27** (3 tasks, 1 run each, before the later refactor): success was relative 2/3 vs absolute 3/3; median single click 1.02 s vs 0.46 s; slowest click 4.95 s vs 0.56 s. — [PR #99](https://github.com/onehumanbeing/esp32-iphone-agent/pull/99)
- **Older contrary evidence (2020, iOS 13, iPad):** an nRF52832 BLE mouse worked with AssistiveTouch in relative mode. After changing the report to `HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE)`, "the mouse stopped moving on the iPad." Another developer reported the same (Oct 2020), and there was no Apple reply. Note: the descriptor change only flipped the Input flag and did not widen the logical range. — [Apple Developer Forums thread 652700](https://developer.apple.com/forums/thread/652700)
- **Older contrary evidence (2020, iOS 14):** the NicoHood/HID absoluteMouse example on an Arduino Micro (USB) worked on Ubuntu but "_not_ with Android 8.0/9.0 nor ios14." A digitizer sketch on "IOS14+ (ipad 7th generation): does not work (normal as your HID is a digitizer and not supported since ios14)". A Sept 2026 question about iOS 27 went unanswered. — [NicoHood/HID #123](https://github.com/NicoHood/HID/issues/123)
- Other ESP32 BLE mouse libraries (T-vK/ESP32-BLE-Mouse) have open, unresolved requests for absolute mode and touchscreen mode, and iPad is not on their compatibility list. — [T-vK/ESP32-BLE-Mouse #19](https://github.com/T-vK/ESP32-BLE-Mouse/issues/19), [#5](https://github.com/T-vK/ESP32-BLE-Mouse/issues/5), [#33](https://github.com/T-vK/ESP32-BLE-Mouse/issues/33)
- **HID reports never reach apps:** iOS consumes HOGP in the system, not in apps (an app cannot read HID-profile data). — [Apple Developer Forums 45989](https://developer.apple.com/forums/thread/45989); [Apple Developer Forums 725238](https://developer.apple.com/forums/thread/725238)

### Inferences
- For Porthole, the practical answer is an **absolute mouse** collection (Generic Desktop Mouse, X/Y Input Abs, 16-bit, 0..32767), not a Digitizer. It can be added to the existing BLE HOGP composite report map, either replacing the relative mouse or as a separate report ID. The relative mouse is not needed as a "control" (`pure 2` worked).
- The 2020 failures may have been caused by iOS versions before 15–27, by the iPad, or by the descriptor itself (8-bit signed range with only the Abs flag flipped). The sources cannot separate these causes. Because the 2026 success used a clean 16-bit 0..32767 range with Physical Min/Max, copy that descriptor exactly.
- Digitizer usages (page 0x0D) appear to be ignored on iPhone. Synthesizing real touches is not available, so input still goes through the AssistiveTouch pointer, which iOS then turns into touches ("delivered as `touch`" in Safari pointer events).
- Classic BT HID (BR/EDR): the project memory notes iPhone listing needed LE-only. I found no source testing absolute input over classic BT.

### Gaps
- **No test of absolute mouse on iOS 26.x** (Porthole's target, 26.6.1). The evidence covers iOS 13/14 failures (iPad) and iOS 27 successes (iPhone 17 Pro). Whether the change came in 27 or earlier is unknown. This is the single most important item to test on the user's device.
- The BLE absolute test (#104) did not state the iOS version or phone model. It is presumably the same iOS 27 setup, but that is not confirmed.
- No test in system UI or other apps was published, although the BLE test opened a tapped home-screen icon.
- The "AssistiveTouch off → nothing" result is marked unconfirmed by its author.
- Not covered: multiple monitors or orientation (whether 0..32767 follows rotation in landscape) and Stage Manager or external display.
- No iPadOS result for third-party HID digitizers (a non-Apple Pencil HID pen or touch) was found.

## Q2. Does AssistiveTouch need to be on for mouse input on iPhone? Does iPhone have native pointer support without AssistiveTouch on iOS 26? How does iPadOS differ?

### Takeaway
On iPhone, mouse input runs through AssistiveTouch. Apple's own instructions say to turn it on, and the absolute-mouse spike saw no response with it off. iPadOS has native pointer support with its own Pointer Control settings. iPhone keeps pointer appearance under AssistiveTouch > Pointer Style.

### Cited Findings
- Apple: "With your pointer connected, turn on AssistiveTouch." Wired mice, trackpads or eye trackers plug into the Lightning or USB-C port ("If you're using USB-A devices, you'll need an adapter"). Bluetooth devices pair via Settings > Accessibility > Touch > AssistiveTouch > Devices > Bluetooth Devices. Buttons are customized under AssistiveTouch > Devices > [device]. — [Apple Support 111775](https://support.apple.com/en-us/111775)
- Apple, on the iPad difference: pointer appearance is under Settings > Accessibility > Pointer Control on iPad, but under Touch > AssistiveTouch > Pointer Style on iPhone. "Eye tracking is currently only supported on iPad." — [Apple Support 111775](https://support.apple.com/en-us/111775)
- AbilityNet's iOS 26 guide covers iPhone and iPad together: Settings > General > Trackpad & Mouse > Tracking Speed; Settings > Accessibility > Accessories > Pointer Control, which "only appears when a pointing device is connected". — [AbilityNet iOS 26 mouse guide](https://mcmw.abilitynet.org.uk/how-to-use-a-mouse-or-trackpad-with-your-iphone-or-ipad-in-ios-26)
- In the iOS 27 USB spike, the absolute mouse with AssistiveTouch off gave "nothing", though the author marks this unconfirmed. PR #99 says "AssistiveTouch must stay on". — [abs-touch README](https://github.com/chenshaorong0118/esp32-iphone-agent-ble/tree/spike/abs-touch/spikes/abs-touch); [PR #99](https://github.com/onehumanbeing/esp32-iphone-agent/pull/99)

### Inferences
- Porthole should keep requiring AssistiveTouch on iPhone. Moving to absolute mode does not remove that dependency, but it does remove dead reckoning and acceleration from the problem.
- iPadOS native pointer support (since iPadOS 13.4, from general knowledge, not re-verified here) is a different code path. Results on iPad do not transfer to iPhone, and the reverse also holds.

### Gaps
- No Apple document states outright "AssistiveTouch is required on iPhone". The evidence is Apple's procedure plus one unconfirmed test with AssistiveTouch off.

## Q3. Pointer acceleration on iOS AssistiveTouch: can it be disabled, is the curve known, and does slamming into a corner give a reliable origin?

### Takeaway
I found no setting to disable pointer acceleration on iPhone or iPad in iOS/iPadOS 26. Only the Tracking Speed slider exists. Users on iPadOS 26 complain about forced acceleration, and Apple published no fix. I found no documented or reverse-engineered curve. Absolute mode avoids the problem entirely.

### Cited Findings
- Tracking Speed is at Settings > General > Trackpad & Mouse, and it adjusts "how quickly the pointer moves across the screen". — [AbilityNet iOS 26](https://mcmw.abilitynet.org.uk/how-to-use-a-mouse-or-trackpad-with-your-iphone-or-ipad-in-ios-26); [Apple Support 111775](https://support.apple.com/en-us/111775)
- Apple Community threads (Sept 2025), titled "iPadOS 26 Mouse acceleration (still) feels horrible", ask for a way to disable acceleration. They were closed without a fix. A separate thread reports that the Pointer Animations toggle is broken on iPadOS 26. — [Apple Community 256142488](https://communities.apple.com/de/thread/256142488); [Apple Discussions 256240307](https://discussions.apple.com/thread/256240307); [older: Apple Discussions 254543173](https://discussions.apple.com/thread/254543173?page=1)
- The relative-mouse approach needs re-homing and correction before taps because drift accumulates. — [DEV Community write-up on esp32-iphone-agent](https://dev.to/numbpill3d/an-esp32-and-a-local-vision-model-drive-my-iphone-4nh3)
- With a relative mouse, a single click had a median of 1.02 s and a worst case of 4.95 s, versus 0.46 s and 0.56 s with an absolute mouse. — [PR #99](https://github.com/onehumanbeing/esp32-iphone-agent/pull/99)

### Inferences
- Slamming the pointer into a corner with large negative deltas should clamp it at the screen edge, which gives a usable origin. Acceleration still makes the next move's displacement non-linear, so corner-homing helps only together with closed-loop visual correction (Porthole's current approach). This is an inference; I found no source measuring it on iOS.

### Gaps
- No published acceleration curve for iOS or AssistiveTouch, and no "Pointer Acceleration" toggle found for iPhone or iPad in iOS 26. I found no reverse-engineering work.

## Q4. USB-C (iPhone 15+) and Lightning + Camera Connection Kit: direct USB HID mouse/touch/digitizer devices? Any differences from Bluetooth?

### Takeaway
Yes. iPhone accepts USB HID mice and keyboards directly through USB-C, and through Lightning with an adapter, all via AssistiveTouch. The iOS 27 tests behave the same over USB and BLE: absolute mouse works, digitizer/touch/pen descriptors are ignored. USB powers the device from the phone, and the project reported a USB-only stuck-pointer problem.

### Cited Findings
- Apple: plug a mouse, trackpad or eye-tracking device "into the Lightning or USB-C port". USB-A devices need an adapter. — [Apple Support 111775](https://support.apple.com/en-us/111775)
- An ESP32-S3 (TinyUSB via arduino-esp32 `USBHID`) acting as a USB composite mouse + keyboard + consumer control works on iPhone 17 Pro (iOS 27). Its absolute mouse works, and its touch screen and pen descriptors do not. — [abs-touch README](https://github.com/chenshaorong0118/esp32-iphone-agent-ble/tree/spike/abs-touch/spikes/abs-touch); [hid_bridge.ino](https://github.com/onehumanbeing/esp32-iphone-agent/blob/main/hardware/hid_bridge/hid_bridge.ino)
- USB-specific problems: a loose cable "stops everything". After certain drags, the pointer's button clicks still work but pop-overs stop opening, and only unplugging and replugging clears it. Whether BLE has the same problem is unknown. A USB device is powered by the phone. — [jinwon-int/esp32-iphone-agent #104](https://github.com/jinwon-int/esp32-iphone-agent/issues/104)

### Inferences
- For a PC-hosted design, the PC cannot itself be a USB device to the iPhone without device-mode USB hardware. A microcontroller bridge (RP2040, ESP32-S3, Teensy) on USB-C works, but it adds hardware. Porthole's BLE path should already be sufficient if absolute works over BLE on iOS 26.

### Gaps
- No specific test of Lightning + Camera Connection Kit with a custom HID found. Apple only says Lightning wired pointers work.
- No USB test on iOS 26 or earlier with absolute descriptors, apart from the iOS 14 failure (NicoHood #123), which did not say whether it was iPad or iPhone, or USB or BT.

## Q5. Consumer usages and keyboard shortcuts iOS honors (AC Home, Search, Globe/Fn shortcuts); can they replace AssistiveTouch custom buttons? Full Keyboard Access as an alternative?

### Takeaway
With Full Keyboard Access enabled, iPhone has documented keyboard commands for Home (Fn-H), App Switcher (Fn-Up), Control Center (Fn-C) and Notification Center (Fn-N), and they can be remapped. That covers most system navigation without AssistiveTouch menu buttons. Reports on consumer AC Home (0x0223) conflict on iOS 27.

### Cited Findings
- Apple iPhone User Guide, "Control iPhone with an external keyboard" (version selector covers iOS 17, 18, 26 and 27): turn it on at Settings > Accessibility > Keyboards & Typing > Full Keyboard Access, which "highlights the item on the screen that has focus". Default commands: Tab-H view commands, Tab / Shift-Tab move, Space activate, **Fn-H Home Screen, Fn-Up Arrow App Switcher, Fn-C Control Center, Fn-N Notification Center**. "You can change the keyboard shortcuts associated with Full Keyboard Access commands" (Commands screen). — [Apple iPhone User Guide](https://support.apple.com/en-bn/guide/iphone/ipha4375873f/ios)
- Globe-key system shortcuts (Globe+H Home, Globe+C Control Center, Globe+N Notification Center, Globe+Up App Switcher) are documented for **iPadOS 15+**. I found no confirmation for iPhone outside Full Keyboard Access. — [jeffperry.blot.im, The Globe Key and iPadOS 15](https://jeffperry.blot.im/2021/06/21/the-globe-key.html); [Six Colors](https://sixcolors.com/post/2021/06/think-globally-the-ipads-new-universal-keyboard-shortcuts/)
- **AC Home 0x0223 (conflicting, both from the same project):**
  - PR #99 (USB, iOS 27.x) says "iOS does not respond to AC Home". Animated pages fooled the success check, so `home` was switched to a swipe-up gesture. — [PR #99](https://github.com/onehumanbeing/esp32-iphone-agent/pull/99)
  - The firmware comment says AC Home makes "iOS go to the Home Screen". — [hid_bridge.ino](https://github.com/onehumanbeing/esp32-iphone-agent/blob/main/hardware/hid_bridge/hid_bridge.ino)
  - The BLE test reported "consumer Home work[s]". The BLE map comment labels Home as usage 0x0040, which is not the same usage as AC Home 0x0223. — [#104](https://github.com/jinwon-int/esp32-iphone-agent/issues/104); [ble_hid.h](https://github.com/jinwon-int/esp32-iphone-agent/blob/main/hardware/hid_bridge/ble_hid.h)
- **Consumer Eject:** "iOS toggles the on-screen keyboard once per Consumer Control Eject press." — [hid_bridge.ino](https://github.com/onehumanbeing/esp32-iphone-agent/blob/main/hardware/hid_bridge/hid_bridge.ino)
- **Keyboard combos:** the BLE test reported that `type`, key combos and consumer Home work. The firmware supports `key cmd+h`. — [#104](https://github.com/jinwon-int/esp32-iphone-agent/issues/104); [hid_bridge.ino](https://github.com/onehumanbeing/esp32-iphone-agent/blob/main/hardware/hid_bridge/hid_bridge.ino)
- Apple: AssistiveTouch pointer buttons can be mapped to actions via AssistiveTouch > Devices > [device] > button. — [Apple Support 111775](https://support.apple.com/en-us/111775)

### Inferences
- Full Keyboard Access shortcuts (Fn-H, Fn-Up, Fn-C, Fn-N) are the most reliable documented way to trigger system navigation from the Porthole keyboard report. Fn in HID is not a standard keyboard usage; Apple keyboards send it as a vendor or AC usage. Porthole would likely have to remap the commands to standard modifier combos in the FKA Commands screen. A swipe-up gesture with the absolute pointer is a fallback for Home that the project validated.
- FKA has a side effect: it draws a focus ring and captures Tab and Space. That may interfere with text entry, so test it alongside pointer use.

### Gaps
- No authoritative list of HID consumer usages honored by iOS 26. AC Search (0x0221), Globe (0x029D) and the exact usage that works for Home on iPhone are unverified.
- No test of whether Fn-based FKA defaults can be triggered by a non-Apple HID keyboard without remapping.

## Q6. Other tricks for absolute positioning: Switch Control point scanning, Voice Control grid, FKA

### Takeaway
None of these is as good as the absolute HID mouse. Voice Control's numbered grid gives coarse, recursive absolute targeting but needs spoken commands. Switch Control point scanning is timing-based. FKA works through focus, not coordinates.

### Cited Findings
- Voice Control "Show grid" shows a numbered overlay. Saying a number zooms into that cell; "Tap <number>" taps the cell centre; "Show grid continuously" keeps it up. Grid size is set under Settings > Accessibility > Voice Control > Overlay. — [Apple Support 111778](https://support.apple.com/en-us/111778); [iPhoneLife](https://iphonelife.com/node/278486); [a11ysupport.io](https://a11ysupport.io/learn/at/vc_ios)
- FKA navigates by focus (Tab / Space) and has no coordinate-tap command in the default list. — [Apple iPhone User Guide](https://support.apple.com/en-bn/guide/iphone/ipha4375873f/ios)

### Inferences
- Voice Control would need synthetic audio sent to the phone's microphone, or a route through AirPlay or a headset. That is impractical and slow, but it could serve as a fallback for coarse targeting.
- Switch Control point scanning (crosshair sweeps, using keyboard keys as switches) depends on sweep timing. It is neither exact nor fast.

### Gaps
- No source found on Switch Control point-scanning precision or on driving it with HID keys on iOS 26.
- No search hit for "scrcpy for iOS", ios-remote or TouchHID-style projects achieving true touch injection without jailbreak. The esp32-iphone-agent project (absolute mouse through AssistiveTouch) is the closest current equivalent found.
