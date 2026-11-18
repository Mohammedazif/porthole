# Developer-tool touch injection into iPhone from Windows/Linux (WDA/XCUITest, CoreDevice HID, alternatives)

Context: Porthole mirrors via AirPlay and controls via BLE HID relative mouse; target phone runs iOS 26.6.1. The question is whether a developer-tool path can give exact-coordinate taps, swipes and text from a Windows host.

Headline finding (dated Nov 2026): there are now two developer-tool paths, not one.
1. **WebDriverAgent (XCUITest runner)**: works on iOS 17 through 26 from Windows. Setup is heavy (free-Apple-ID signing with a 7-day expiry, Developer Mode, an iOS 17+ tunnel). It is slow, at about 0.6 to 0.9 s per confirmed tap.
2. **CoreDevice "universal HID" service** (`com.apple.coredevice.hid.universalhidservice`): new, and only on **iOS 27+**. It takes absolute, normalized touch, multi-touch, keyboard and hardware buttons over the RemoteXPC tunnel. It needs no on-device runner and no signing, but requires Developer Mode and a mounted DDI. pymobiledevice3 (Python, cross-platform) and go-ios (merged Sep 2026) both implement it. **It does not work on iOS 26.6.1**: below 27 the service is not advertised.

---

## Q1. How WDA and alternatives inject touches, and what latency, throughput and feature limits apply

### Takeaway
WDA injects through XCTest's private event-synthesis daemon path. A standard `/wda/tap` takes about 0.6 to 0.9 s and a drag about 1.7 to 2.7 s, because XCTest waits for confirmation and quiescence. An unmerged "fast" patch cuts this to about 263 ms per tap and about 466 ms per drag. Continuous, scrcpy-style dragging through WDA is therefore not realistic. CoreDevice HID (iOS 27+) is fire-and-forget raw HID reports with no acknowledgement, and is much better suited to live control.

### Cited Findings
**WebDriverAgent (appium/WebDriverAgent, Appium XCUITest driver)**
- Measured on an iPhone 15 Plus with WDA 16.14.0, n=10, Settings app, HTTP reply latency. Defaults: `/wda/tap` **918 ms**, `/wda/dragfromtoforduration` **2662 ms**. With `animationCoolOffTimeout=0` and `waitForIdleTimeout=0`: tap 898 ms, drag 2288 ms. With the proposed `fast=true` route: tap **263 ms**, drag **466 ms**. The table's device ran iOS 27.2. — [appium/appium-ios PR #87](https://github.com/appium/appium-ios/pull/87)
- Earlier measurements on the same setup (USB, iOS 27.0, Xcode 26 beta): confirmed path **0.62 to 0.94 s per tap**, **1.73 s per 0.4 s drag**, W3C `/actions` tap 0.94 s. — [PR #87](https://github.com/appium/appium-ios/pull/87)
- Mechanism (per the PR author): most of the cost is the XCTest confirmation wait inside `-[XCSynthesizedEventRecord synthesizeWithError:]`, which waits on the testmanager daemon. Pre-event quiescence adds to it. No setting reaches this wait. Zeroing the idle and cool-off settings changed taps by only about 2% and drags by about 14%. A prototype that called the daemon's `synthesizeEvent:completion:` off-thread reached 2.5 to 4.5 ms per tap but was unstable and skipped coordinate and orientation resolution. A "fast" reply means the event was queued, not confirmed. — [PR #87](https://github.com/appium/appium-ios/pull/87)
- PR status: opened Oct 18, 2026, **open and not merged**. A maintainer review on Nov 16, 2026 raised a possible stuck queue, reordering between fast and confirmed gestures, and missing tests. — [PR #87](https://github.com/appium/appium-ios/pull/87)
- Appium's slowness guide names animations and idle waits as causes, with `waitForIdleTimeout` and `animationCoolOffTimeout` as tuning knobs. — [Appium XCUITest driver: WDA slowness](https://appium.github.io/appium-xcuitest-driver/10.43/troubleshooting/wda-slowness/); [Settings reference](https://appium.github.io/appium-xcuitest-driver/latest/reference/settings/)
- Older reports put part of the slowness in XCUIElement snapshot and window lookup. Reducing `snapshotMaxDepth` helps element-based calls, though coordinate taps avoid most of this. — [TesterHome (2021, Chinese)](https://testerhome.com/topics/29820); [facebookarchive/WebDriverAgent #675](https://github.com/facebookarchive/WebDriverAgent/issues/675)
- A real-world Windows WDA harness (SideTap) reports these figures:
  - Keeping one connection to WDA open cuts **46%** off each call.
  - About **0.6 s per call** when warm, against **2 to 4 s cold**.
  - The first gesture after the phone slept used to stall about 16 s and now takes 0.59 s.
  - Settle detection dropped from 877 ms to 318 ms.
  - The WDA MJPEG viewer (:9100) streams at about **34 fps**.
  - "WebDriverAgent's touches never reach the home-indicator zone," so the real App Switcher could not be opened from the PC.
  - Home Screen icons only drag in jiggle mode.
  - [ucsandman/SideTap README](https://github.com/ucsandman/SideTap)

**DeviceKit (mobile-next): an XCUITest-based WDA alternative**
- DeviceKit "runs as an XCUITest" with "private API wrappers (touch synthesis, accessibility)". It exposes JSON-RPC on port 12004 with tap(x,y), swipe and long-press. It also offers H.264 streaming: screenshot-based `GET /h264` at 1 to 60 fps (default 30) and a ReplayKit broadcast extension sending raw NAL units on port 12005 at up to 60 fps. It supports iOS 14+. The build needs Xcode. It is licensed **FSL-1.1 (Apache-2.0 future)**. No latency numbers are published. — [mobile-next/devicekit-ios](https://github.com/mobile-next/devicekit-ios); [ecosyste.ms entry](https://awesome.ecosyste.ms/projects/github.com%2Fmobile-next%2Fdevicekit-ios)
- go-ios forks list `ui install devicekit -p12file ... -profile ...` and `ui tap -x 100 -y 200`, with a `--driver auto` mode that picks DeviceKit (12004) or WDA (8100). Upstream go-ios's README lists `ui` as "control the UI through WebDriverAgent or DeviceKit", with `ui download` and `ui install`. — [bingyuegong/go-ios fork docs](https://pkg.go.dev/github.com/bingyuegong/go-ios@v1.0.5); [go-ios README](https://github.com/danielpaulus/go-ios/blob/main/README.md?plain=1)

**CoreDevice universal HID (iOS 27+, no on-device runner)**
- Service: `com.apple.coredevice.hid.universalhidservice`, served by `dtuhidd`, which ships in the Developer Disk Image. It is "iOS 27 and above" and not advertised in RSD below 27. An iOS 18.7.3 device refused with CoreDevice error 9021. It was verified on an iPhone running iOS 27.0. — [go-ios PR #849](https://github.com/danielpaulus/go-ios/pull/849)
- Semantics:
  - Touch uses `TouchDown`/`TouchMove`/`TouchUp` with uint16 coordinates packed into a 58-byte report.
  - "A nil error means the report was written, not that anything happened." The device never acknowledges reports.
  - **A media (screen) stream must be running, or the device "throws every report away."**
  - Connecting to the HID service takes about 1 s once per session.
  - `Close` lifts a held contact.
  - The PR was merged Sep 15, 2026. It covers touch only; button and keyboard support was dropped from it.
  - [go-ios PR #849](https://github.com/danielpaulus/go-ios/pull/849)
- pymobiledevice3 commands under `developer core-device universal-hid-service`:
  - `tap`, `drag`, `swipe`, `move`, `multi-tap` (up to 5 fingers), `multi-drag` (pinch and zoom), and a `session` mode reading batched commands from stdin.
  - `type 'Hello'` (keyboard text) and `key backspace --count 2`.
  - `developer core-device hid button home|lock|volume-up|volume-down|mute|siri press` (down, up and cancel states supported).
  - Raw HID via `raw-button` and `send-report`.
  - Coordinates are normalized 0 to 65535, with (32768, 32768) at the centre.
  - [pymobiledevice3 CLI recipes](https://doronz88.github.io/pymobiledevice3/latest/guides/cli-recipes/)
- The same service powers pymobiledevice3's `display serve-web`, which streams the screen to a browser via WebCodecs with touch, buttons and keyboard, and is cross-platform. `serve-vnc` is macOS-only because it decodes HEVC with VideoToolbox. Screen streaming requires **iOS 27+**. — [CLI recipes](https://doronz88.github.io/pymobiledevice3/latest/guides/cli-recipes/); [pymobiledevice3 PR #2154](https://github.com/doronz88/pymobiledevice3/pull/2154); [v11.51.0 release](https://github.com/doronz88/pymobiledevice3/releases/tag/v11.51.0)
- CoreDevice feature IDs seen via `devicectl` on an iOS 27.0 iPhone 17 Pro: HID Digitizer (`com.apple.coredevice.feature.remote.hid.digitizer`), HID Keyboard, HID Button, HID Scroll, Universal HID, and View Device Screen. The issue states that "On iOS 26+ these sit behind a consent toggle" (Settings › Developer › UI Automation › Enable UI Automation). It has no accessibility tree, so automation is screenshot-driven only. — [getsentry/MobileBuildMCP #519](https://github.com/getsentry/MobileBuildMCP/issues/519)

### Inferences
- For Porthole on iOS 26.6.1, WDA is the only exact-coordinate developer path available today. At about 0.9 s per confirmed tap (about 0.26 s with an unmerged patch you would have to build), it suits discrete click-to-tap, not live drag-following. For scrolling, it is better to send one swipe gesture (start, end, duration) when the mouse button is released than to stream moves.
- WDA could run as a hybrid with the BLE HID mouse: BLE HID for continuous pointer and hover, WDA for "snap tap at exact (x,y)" and for text entry.
- If the user moves to iOS 27, CoreDevice universal HID is clearly the better backend. It gives absolute multi-touch, keyboard and buttons, needs no signing, and gives streamed HID reports with no confirmation wait. That is scrcpy-like by design. The requirement to keep a CoreDevice media stream open means Porthole would hold that stream open (and possibly discard it) alongside AirPlay, or switch mirroring to it.
- WDA cannot reliably do Home or App Switcher gestures from the home-indicator zone (per SideTap). XCUIDevice `pressButton` (home, volume) is the usual workaround, but see Gaps.

### Gaps
- No published per-tap latency for CoreDevice HID or DeviceKit. No measurements found over Wi-Fi versus USB for WDA tap latency.
- I found no source confirming WDA hover or pointer (XCPointerEventPath mouse-move) support on iPhone. XCUIDevice `pressButton` options and Siri on iOS 26 were not verified from a primary source in this pass.
- The PR #87 numbers were measured on iOS 27.x betas. I found no iOS 26-specific WDA latency numbers, though the mechanism is the same.

---

## Q2. Signing and install requirements: Xcode, free Apple ID, Developer Mode, DDI

### Takeaway
A pre-built WDA IPA can be installed from Windows with a free Apple ID. However, Sideloadly re-signs only the host app and leaves `WebDriverAgentRunner.xctest` unsigned, so the runner never launches. The working fix re-signs the whole IPA, including the nested xctest bundle, with `go-ios ios sign app`, using Sideloadly's certificate and the profile pulled from the phone. The signature expires every 7 days. Developer Mode, the Enable UI Automation toggle and a personalized DDI mounted from Windows via pymobiledevice3 or go-ios are also required.

### Cited Findings
- Sideloadly "signs the host app and leaves `PlugIns/WebDriverAgentRunner.xctest` unsigned," so iOS rejects it.
- SideTap's `fix-input` builds a `.p12` from Sideloadly's cert and key and pulls the provisioning profile from `/var/MobileDevice/ProvisioningProfiles/` via pymobiledevice3. It then re-signs the whole IPA with `go-ios ios sign app` (same Team ID) and installs it.
- Free Apple ID only; "no paid developer account."
- "Free Apple ID signatures expire every 7 days." Apple pins re-signs to the App ID's original 7-day window, so re-signing early does not extend it.
- The phone must be unlocked during installs.
- [SideTap README](https://github.com/ucsandman/SideTap); [SideTap issue #14 "Unable to Load WDA"](https://github.com/ucsandman/SideTap/issues/14)
- A separate report describes a manually re-signed WDA IPA with a free account that installed and was trusted but still failed to launch. — [appium-device-farm discussion #1927](https://github.com/AppiumTestDistribution/appium-device-farm/discussions/1927)
- Appium Device Farm publishes a prebuilt WDA IPA "for you to download and use for signing," but its documented re-sign flow uses Xcode (macOS). — [devicefarm.org iOS signing](https://devicefarm.org/ios-signing/)
- Sideloadly: free-account apps are valid for 7 days. — [Sideloadly](https://sideloadly.io/index.html); [Corellium Sideloadly docs](https://support.corellium.com/features/apps/sideloadly)
- go-ios `sign app`: "Sign an app with a P12 and profile; run ui download first for WDA or DeviceKit artifacts." So go-ios can download the WDA and DeviceKit artifacts itself. — [go-ios README](https://github.com/danielpaulus/go-ios/blob/main/README.md?plain=1)
- Developer Mode is required on iOS 16+. On iOS 17+, `developer dvt` needs only Developer Mode, while `developer core-device` always needs Developer Mode and the DDI. — [pymobiledevice3 CLI recipes](https://doronz88.github.io/pymobiledevice3/latest/guides/cli-recipes/)
- DDI: `pymobiledevice3 mounter auto-mount` must be re-run after every reboot. The image depends on the iOS version:
  - Below 17: classic DMG.
  - **17.0 to 26.x: PersonalizedDMG** via the image mounter.
  - 27.0+: Cryptex1 DDI via `cryptexd`.
  - A PersonalizedDMG works only for boards in the build manifest. Cryptex installation needs iOS 26.4+. — [CLI recipes](https://doronz88.github.io/pymobiledevice3/latest/guides/cli-recipes/)
- go-ios: `image auto` auto-downloads and mounts the developer image. — [go-ios README](https://github.com/danielpaulus/go-ios/blob/main/README.md?plain=1)
- "Enable UI Automation" (Settings › Developer) must be on before WDA runs. Midscene's iOS guide shows WDA ≥ 7.0.0 working, with `/status` reporting sdkVersion 26.0. — [Midscene.js iOS guide](https://midscenejs.com/ios-getting-started); [Tricentis device prep](https://documentation.tricentis.com/tricentis_mobile_agent/content/user_manual/prepare_your_devices.htm)
- On first WDA start, iOS may prompt "Touch ID for "XCTest": Enable UI Automation." — [SideTap PR #13](https://github.com/ucsandman/SideTap/pull/13)
- iPASide is a newer open-source free-Apple-ID sideloader for Windows (tested on iOS 16.7.15). It is not documented as handling the WDA runner problem. — [pwnapplehat/iPASide](https://github.com/pwnapplehat/iPASide)

### Inferences
- Xcode or a Mac is **not** strictly required: go-ios `ui download` or a device-farm prebuilt IPA, plus `go-ios sign app` with a free-ID certificate, avoids it. Porthole would have to automate the weekly re-sign and warn ahead of expiry, as SideTap does 48 h ahead. A free account's 3-app and 10-App-ID limits (standard Apple policy, not verified in this pass) would count WDA as one app.
- A paid Apple Developer account ($99/yr) would give 1-year profiles, removing the weekly re-sign.

### Gaps
- I did not verify AltStore or SideStore handling of nested `.xctest` bundles, or whether zsign signs PlugIns correctly.
- The free-account app and App ID limits were not re-verified from Apple sources in this pass.

---

## Q3. Launching the XCUITest runner without Xcode on iOS 17+ (tunnels, Windows, Wi-Fi, iOS 26)

### Takeaway
On Windows, iOS 17.4+ works with pymobiledevice3's **userspace tunnel (no admin, no driver)** over USB. go-ios's tunnel uses wintun.dll and admin rights, or a userspace mode. iOS 17.0 to 17.3 needs a third-party driver on Windows that is not provided. Wi-Fi works but needs one-time USB pairing, and on Windows it needs the classic iTunes (Bonjour/usbmuxd) rather than the Store "Apple Devices" app. A Windows WDA-over-tunnel setup has been run on iPadOS 26.5.2, over both USB and Wi-Fi.

### Cited Findings
- pymobiledevice3 tunnels:
  - The **userspace tunnel** (default, no root) is an in-process PyTCP stack. It works on **iOS 17.4+ over USB via CoreDeviceProxy**. Its address exists only inside the Python process, so external tools cannot use it.
  - `tunneld` is privileged. It runs from an Administrator terminal on Windows and serves 127.0.0.1:49151, unauthenticated.
  - iOS 17.0 to 17.3 needs `tunneld` plus a third-party driver "the project does not ship or endorse."
  - "iOS 17.4+ works with no driver and no Administrator."
  - [pymobiledevice3 iOS 17 tunnels guide](https://doronz88.github.io/pymobiledevice3/latest/guides/ios17-tunnels/); [Discussion #981 (Windows tunneld for iOS 17.3)](https://github.com/doronz88/pymobiledevice3/discussions/981)
- Wi-Fi: run `pymobiledevice3 lockdown wifi-connections on` once over USB. On Windows, Wi-Fi discovery requires the classic iTunes installer; the Microsoft Store "Apple Devices" app does not discover devices over Wi-Fi. `--mobdev2` is a fallback for discovery. — [tunnels guide](https://doronz88.github.io/pymobiledevice3/latest/guides/ios17-tunnels/)
- A TCP tunnel is required for iOS 18.2 and later ("where QUIC is gone"). That needs Python ≥3.13. — [SideTap PR #13](https://github.com/ucsandman/SideTap/pull/13)
- go-ios: iOS 17+ needs `ios tunnel start` running ("Start iOS 17+ tunnel and pair if needed"). On Windows you must download `wintun.dll` and copy it to `C:\Windows\system32`. A `--userspace-port` option exists. `runwda`, `runtest` and `runxctest` launch XCUITest bundles without Xcode. — [go-ios README](https://github.com/danielpaulus/go-ios/blob/main/README.md?plain=1); [pkg.go.dev go-ios](https://pkg.go.dev/github.com/danielpaulus/go-ios)
- SideTap (Windows, Oct 2026):
  - Tunnel setup:
    - tunneld runs as an admin logon task with `--no-wifi --no-mobdev2 --no-usb`.
    - Wi-Fi tunnels are requested on demand via `/start-tunnel` (USB first, then Wi-Fi).
    - A one-time USB pairing is needed for Wi-Fi.
  - Validated on iPadOS 26.5.2 only:
    - Wi-Fi-only, USB-only and unplug-to-Wi-Fi switching all worked.
    - A 1 h Wi-Fi soak showed no adapter flapping.
    - The iPhone and multiple devices were untested.
  - Timing and failure behaviour:
    - A WDA start after idle takes **10 to 20 s**.
    - A dead tunnel stays listed for about 20 s, and a gesture sent then fails after about 20 s.
    - WDA is stopped after 10 min idle.
  - Security issues:
    - The admin logon task, together with user-writable script paths, is a privilege-escalation risk.
    - tunneld is unauthenticated.
    - go-ios forwards WDA ports on 0.0.0.0, and WDA has no authentication.
  - [SideTap PR #13](https://github.com/ucsandman/SideTap/pull/13); [SideTap README](https://github.com/ucsandman/SideTap)
- SideTap's README notes that the "iOS 17+ tunnel needs wintun.dll once (admin) if userspace mode fails." — [SideTap README](https://github.com/ucsandman/SideTap)

### Inferences
- On iOS 26.6.1 the confirmed Windows chain is: pymobiledevice3 or go-ios tunnel, then `mounter auto-mount` (PersonalizedDMG), then `go-ios runwda` or pymobiledevice3's XCUITest launch, then a port-forward of 8100 (and 9100 MJPEG). This chain is confirmed working on iPadOS 26.5.2 and on iOS 26 with WDA ≥7 per Midscene. It is a heavy multi-process dependency for a C app, since Porthole would have to embed or ship Python ≥3.13 or the go-ios binary.
- Over Wi-Fi, Porthole already uses the network for AirPlay. A WDA tunnel over Wi-Fi is feasible after one USB pairing, but expect 10 to 20 s of cold-start delay and roughly 20 s to notice a dead tunnel.

### Gaps
- No primary source found on tidevice status for iOS 17+. It is widely understood to support only iOS <17 via the lockdown testmanagerd path, but that was not verified in this pass.
- facebook/idb: not researched in this pass. Its companion historically needs a macOS host, which was not verified here.
- No measured overhead for the userspace PyTCP tunnel versus a kernel (wintun) tunnel.

---

## Q4. Commercial and open-source scrcpy-like iOS controllers from Windows

### Takeaway
The closest open-source analog to "scrcpy for iPhone on Windows" is **SideTap**: Windows, free Apple ID, WDA plus go-ios and pymobiledevice3, a viewer at about 34 fps via WDA MJPEG, and about 0.6 s per warm call. For iOS 27+, pymobiledevice3's `display serve-web` already provides browser-based mirror-plus-touch, keyboard and buttons with no on-device runner.

### Cited Findings
- SideTap: "Your iPhone, driven from Windows: LLM agents tap, type, and read the screen over USB." Figures and limits are as given in Q1 to Q3. — [SideTap](https://github.com/ucsandman/SideTap)
- pymobiledevice3 `display serve-web` (iOS 27+): screen to browser via WebCodecs, with touch, buttons and keyboard. It follows the active display on foldables. — [CLI recipes](https://doronz88.github.io/pymobiledevice3/latest/guides/cli-recipes/); [v11.51.0](https://github.com/doronz88/pymobiledevice3/releases/tag/v11.51.0); [PR #2188](https://github.com/doronz88/pymobiledevice3/pull/2188)
- DeviceKit with go-ios `ui` and `--driver auto`, and mobile-next's mobile-mcp, which uses it. — [devicekit-ios](https://github.com/mobile-next/devicekit-ios); [mobile-mcp ios.ts](https://glama.ai/mcp/servers/@mobile-next/mobile-mcp/blob/72e405098909acd3a1a182df5759256751185328/src/ios.ts)
- UiPath documents iOS local-device automation. — [UiPath iOS local devices](https://docs.uipath.com/activities/other/latest/ui-automation/ios-local-devices)

### Inferences
- None of these tools publish end-to-end tap-to-photon latency comparable to scrcpy. The WDA-based ones are an order of magnitude slower per tap than BLE HID.

### Gaps
- 3uTools, iMazing, "Vysor for iOS" and Appium Inspector were not researched in this pass. As far as I know, none offers coordinate touch control of an un-jailbroken iPhone from Windows, but that is unverified.

---

## Q5. Alternatives: jailbreak IOHIDEvent, iPhone Mirroring internals, AX audit daemon, instruments DTX

### Takeaway
The supported non-runner path is the iOS 27 CoreDevice HID and display service, which appears to be the developer-facing relative of the device-screen-viewing features. On iOS 26 there is no documented no-runner coordinate-touch service.

### Cited Findings
- On iOS 27.0, `devicectl` exposes the CoreDevice features HID Digitizer, Keyboard, Button, Scroll, Universal HID and View Device Screen. — [MobileBuildMCP #519](https://github.com/getsentry/MobileBuildMCP/issues/519)
- An iOS 18.7.3 device returned CoreDevice error 9021 for the HID service. It is not advertised below iOS 27. — [go-ios PR #849](https://github.com/danielpaulus/go-ios/pull/849)
- go-ios has `ax` ("Accessibility inspector features") and `ax audit`. — [go-ios README](https://github.com/danielpaulus/go-ios/blob/main/README.md?plain=1)

### Inferences
- The AX audit/inspector service may support focus movement (the Accessibility Inspector can step focus) but not coordinate touches. That would give at best element-level "activate" rather than exact taps (inference, unverified).

### Gaps
- Not researched in this pass:
  - jailbreak IOHIDEvent injection (no public jailbreak for iOS 26 is assumed);
  - the internals of macOS iPhone Mirroring;
  - whether `com.apple.accessibility.axAuditDaemon` can activate elements;
  - any instruments DTX UI-input service.

---

## Q6. Practical stability: crashes, keep-alive, 7-day re-sign, battery

### Takeaway
WDA-on-Windows is workable but needs a supervisor. It has to:
- restart WDA when it stops accepting connections,
- keep the tunnel alive,
- avoid replaying gestures that may already have landed,
- re-sign weekly,
- handle the first-gesture-after-sleep stall and 10 to 20 s cold starts.

### Cited Findings
- SideTap's relay restarts WDA on the current tunnel when WDA refuses connections. It also restarts tunneld, never resends requests that may have reached WDA, stops WDA after 10 min idle, and treats keep-alive connections idle more than 2 s as stale. Agent calls retry for 75 s with a 30 s timeout. — [SideTap PR #13](https://github.com/ucsandman/SideTap/pull/13)
- Free signatures expire every 7 days, and when touch input dies the fix is to re-run `fix-input`. Unplugging requires a link restart. The phone can stay locked between tasks if a passcode is supplied. — [SideTap README](https://github.com/ucsandman/SideTap)
- An earlier SideTap version woke a sleeping device every few seconds for about 3 s each time, and the Wi-Fi tunnel adapter flapped about once a minute. Both were fixed by on-demand tunnels. — [SideTap PR #13](https://github.com/ucsandman/SideTap/pull/13)
- XCTest taps can intermittently hang at event synthesis and then wait for app idle. — [Apple Developer Forums thread 13427](https://forums.developer.apple.com/thread/13427)
- CoreDevice HID: reports are unacknowledged, and they are silently dropped without an active media stream. — [go-ios PR #849](https://github.com/danielpaulus/go-ios/pull/849)

### Inferences
- A running XCUITest runner plus MJPEG adds battery and thermal load (not measured anywhere I found). Porthole should start WDA only when "precise tap" mode is used, as SideTap's idle-stop does.

### Gaps
- No published battery or thermal measurements for long-running WDA sessions.
- No mean-time-between-crash data for WDA on iOS 26.
