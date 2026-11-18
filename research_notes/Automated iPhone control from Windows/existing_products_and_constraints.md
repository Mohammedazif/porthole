# Existing Products and Constraints for PC-to-iPhone Control (Windows/Linux)

Research date: 2026-11-18. Scope: how real products and projects let a PC mouse or keyboard control an iPhone, what setup they need, and the legal, policy and DMA constraints on an open-source "scrcpy for iPhone" ("Porthole": UxPlay AirPlay mirror plus a BLE HID mouse and keyboard emulated by the PC).

## Q1. Commercial mirroring and control apps: do they control the iPhone, how, and with what setup and limits?

### Takeaway
Every Windows product that claims to control an iPhone works the same way. AirPlay (or a USB capture path) mirrors the screen, and the PC's own Bluetooth adapter is paired to the iPhone as a BLE HID mouse, driving the AssistiveTouch pointer. This is Porthole's architecture. The vendors share the same pain points: BLE 5.0 adapter requirements, pairing that must start on the iPhone, duplicate device entries, inaccurate clicks from relative pointer motion, and weak or no keyboard support. They also all admit that iOS control is worse than Android control. Pure remote-desktop tools (TeamViewer, DeskIn) offer view-only access to iOS.

### Cited Findings

**ApowerMirror / Apowersoft (also behind the LetsView site's "control iPhone from PC" page)**
- Requirements: Windows 10 version 1803 or later and iOS 12.5 or later. The PC needs Bluetooth 5.0 or higher with BLE, "Based on module RTL8761B". An author comment on the same page says BLE 4.0 is enough, which contradicts the main text — [Apowersoft FAQ "How to control iOS"](https://d2t95nfi15v50s.cloudfront.net/faq/how-to-control-ios.html). The LetsView-hosted page says Windows 10 1703+ instead — [LetsView: control iPhone from PC](https://letsview.com/control-iphone-from-pc.html)
- Setup (Apowersoft FAQ): (1) install the app on the PC and iPhone, (2) mirror the iPhone to the PC, (3) turn on Settings > Accessibility > Touch > **AssistiveTouch**, (4) click the control icon on the PC toolbar (it turns blue), (5) turn on Bluetooth on Windows, (6) on the iPhone, go to Settings > Bluetooth and select the PC or an "Accessory" entry. "If two entries share the PC's name, connect both." (7) tap Pair on the iPhone, then Allow on the PC — [Apowersoft FAQ](https://d2t95nfi15v50s.cloudfront.net/faq/how-to-control-ios.html)
- Accuracy fixes: "Inaccurate clicks: set Settings > Trackpad & Mouse > Tracking Speed to 6. Leave sensitivity at default." Version 1.7.14.3+ shows a black dot, and Alt+C hides the PC mouse "for more accurate clicks" — [Apowersoft FAQ](https://d2t95nfi15v50s.cloudfront.net/faq/how-to-control-ios.html)
- Mechanism confirmed by a Japanese review: the iPhone recognizes the PC as a Bluetooth keyboard/mouse. On iOS 15.4 the cursor did not line up with the tapped spot, and the mirror window closes when the iPhone screen turns off — [iPhone Mania review](https://iphone-mania.jp/news-450686/)
- Control is limited in the free tier. VIP adds multi-device control — [AnyViewer roundup (secondary)](https://www.anyviewer.com/how-to/remote-control-iphone-from-pc-2996.html)

**AirDroid Cast**
- "AirDroid Cast can only control iPhones via Bluetooth connection." Control is a premium (Cast & Control) feature. Mirroring uses AirPlay (Control Center > Screen Mirroring) or USB. Windows pairing uses a built-in Bluetooth tool in v1.1.0+. "To control your iOS device better, you are highly recommended to turn on Assistive Touch and Portrait Orientation Lock." If control stays disabled after pairing, remove the device on both sides and re-pair. The direct help page returned 403, so this comes from search excerpts of [AirDroid help 1260803607109](https://help.airdroid.com/hc/articles/1260803607109) and [AirDroid help 1260804059449 (iOS–Windows Bluetooth pairing)](https://help.airdroid.com/hc/articles/1260804059449)

**iMyFone MirrorTo**
- The PC needs Bluetooth 5.0+ with BLE and Windows 10 (1803+) or 11. **macOS 12+ does not support the Bluetooth connection.** Mouse control is paid. Pairing: remove old pairings on both sides, restart MirrorTo, "Start the Bluetooth connection on your iPhone, not on the computer," then Pair on both pop-ups — [iMyFone MirrorTo FAQ](https://www.imyfone.com/support/imyfone-mirrorto/)
- Troubleshooting: if the phone can't see the PC, use the LightBlue BLE scanner app and toggle Bluetooth, then wait about 10 min. If two identically named Bluetooth devices appear, disable the built-in radio in Device Manager when using a dongle — [iMyFone FAQ](https://www.imyfone.com/support/imyfone-mirrorto/)
- "iOS mouse control cannot be as good as Android because of iOS system limits." MirrorTo offers "two mouse control methods." A "Focus Mode" hides the arrow for gaming (toggle "~") — [iMyFone FAQ](https://www.imyfone.com/support/imyfone-mirrorto/). A third-party guide also recommends Tracking Speed 6 — [Gizchina guide](https://www.gizchina.com/2024/05/07/full-guide-how-to-control-iphone-from-your-windows-mac/)

**Wondershare MirrorGo (iOS) / Dr.Fone Air**
- Requires AssistiveTouch plus Bluetooth pairing with the PC, then mouse control. Mirroring supports iOS 7.0+ over the same Wi-Fi — [Wondershare MirrorGo iOS guide](https://drfone.wondershare.com/guide/mirrorgo-ios.html)
- A review says iOS control is paid only. File transfer and the shared clipboard do not work with iPhone, and **keyboard control is not offered for iOS** — [FlashGet review of MirrorGo](https://cast.flashget.com/blog/mirrorgo-review). This is a competitor's review, so treat it with some caution.

**Douwan (Chinese tool shown on the LetsView page)**
- Wired Lightning mirroring, then Settings > Bluetooth on the iPhone > select the PC, then control. Same BT-HID approach — [LetsView page](https://letsview.com/control-iphone-from-pc.html)

**TeamViewer QuickSupport / DeskIn / remote-desktop tools**
- TeamViewer staff: "only screen-sharing is possible when connecting to iOS devices. Remote controlling the device is not possible." Sharing uses the iOS screen-broadcast feature (ReplayKit) started from Control Center — [TeamViewer community](https://community.teamviewer.com/English/discussion/123898/cant-remote-iphone-from-computer); [TeamViewer community thread 2](https://community.teamviewer.com/English/discussion/123762/can-i-remotely-control-my-ipad-with-my-iphone)
- DeskIn: "due to Apple's system restrictions, it does not support remote control of iOS devices… applies to all third-party remote support software" — [DeskIn FAQ](https://deskin.io/support/faq/cant-control-iphone-ipad)

**AirPlay receivers: Reflector 4, LonelyScreen, 5KPlayer**
- These are AirPlay receivers that display the iPhone screen on a PC. Reflector 4 is paid (7-day trial). 5KPlayer has a free receiver mode — [FlashGet Reflector 4 review](https://cast.flashget.com/blog/reflector-4-review); [codegenes LonelyScreen guide](https://www.codegenes.net/blog/lonelyscreen-airplay-receiver-download/); [ofzenandcomputing AirPlay-on-Windows guide](https://ofzenandcomputing.com/can-i-use-airplay-on-my-windows-pc/). None of these sources mention any input-control feature (see Gaps).

**Microsoft Phone Link / Intel Unison**
- Intel Unison (calls, texts, notifications, and files for iPhone and Android on Windows 11) was **discontinued 30 June 2025** — [Tom's Hardware](https://www.tomshardware.com/software/intel-discontinues-unison-app-for-connecting-pcs-and-smartphones); [Windows Latest](https://windowslatest.com/2025/04/03/intel-unison-support-ends-on-windows-11-in-june-killing-off-pc-android-ios-bridge-app)
- Phone Link on iPhone is limited. One user reports that it only sees incoming texts and cannot do group texts — [Apple Discussions](https://discussions.apple.com/thread/256083865). I found no source saying Phone Link offers iPhone screen mirroring or control.

### Inferences
- The commercial market has converged on Porthole's exact design: AirPlay receiver plus the PC acting as a BLE HID peripheral (Windows 10 1703/1803+ is when the WinRT GATT-server/peripheral role became usable, which matches the version floors). Porthole's differentiators would be open source, lower latency, better pointer accuracy (closed-loop homing), and working keyboard input. Those are the areas where vendors visibly struggle.
- Vendor advice about RTL8761B / BT 5.0 BLE adapters, starting pairing from the iPhone, and duplicate entries matches this project's own finding that iPhone only reliably lists the PC when it advertises LE-only with a 20–30 ms advertising interval. Expect user support load around adapter quirks. A "known-good dongle" list (RTL8761B-class) is worth publishing.
- "Tracking Speed 6" is the vendors' empirical fix for a non-linear iOS acceleration curve. Porthole should either document the same setting or calibrate it at runtime.
- Keyboard support is the weak spot across vendors (MirrorGo has none, and see the Q3 Shift bug), so working text entry would be a real differentiator.

### Gaps
- The AirDroid help pages returned 403, so the AirDroid details come from search excerpts only.
- No primary source confirms that Reflector 4, LonelyScreen or 5KPlayer lack control. This is inferred from their receiver-only descriptions.
- Vysor for iOS and AnyDesk for iOS were not researched directly. I expect them to be view-only or unsupported like TeamViewer, but this is unverified.
- No independent latency measurements (ms) were found for any vendor's BT-HID control path. Reviews are qualitative only.
- I could not confirm what MirrorTo's "two mouse control methods" are. They might be relative versus homing-based absolute, but that is unknown.

## Q2. Apple iPhone Mirroring (macOS Sequoia+): how input works, regional availability, why there is no Windows equivalent, and any reverse engineering

### Takeaway
iPhone Mirroring is a Continuity feature limited to the same Apple Account. It needs Bluetooth and Wi-Fi on both devices, a locked iPhone nearby, and macOS 15+/iOS 18+ on a Mac with Apple silicon or a T2 chip. It is still unavailable in the EU because of "DMA regulatory uncertainty." Commentators link that decision to a fear that the Commission could require Windows parity. I found no public reverse-engineering effort that drives it from a non-Mac.

### Cited Findings
- Requirements: the same Apple Account with 2FA, Bluetooth and Wi-Fi on for both devices, and the iPhone locked and nearby (it can be charging or in StandBy). In macOS Tahoe 26.4+ you choose which iPhone to use if there are several. Heavy Bluetooth or Wi-Fi use for other things can interfere — [Apple Support 120421](https://support.apple.com/120421)
- Minimums are macOS Sequoia / iOS 18 and an Apple silicon or T2 Mac — [MacRumors guide](https://www.macrumors.com/guide/iphone-mirroring)
- EU: Apple said iPhone Mirroring "will not be available in the European Union for the time being due to ongoing regulatory uncertainty." It confirmed no plans at the macOS Tahoe announcement. A reported concern is that the Commission could force it to be offered on Windows as well — [heise](https://heise.de/-10453123); [Cult of Mac](https://www.cultofmac.com/news/apple-wont-bring-iphone-mirroring-to-the-eu-with-macos-tahoe); [Thurrott](https://www.thurrott.com/?p=322065); [Apfelpatient (2026: still unavailable)](https://www.apfelpatient.de/en/news/apple-iphone-mirroring-feature-remains-unavailable-to-eu-users)

### Inferences
- iPhone Mirroring gets its input from a private, account-authenticated Continuity channel rather than HID. It is not reachable by third parties, and nothing indicates Apple will open it. The EU withholding shows that Apple prefers not shipping a feature over opening it.

### Gaps
- Apple does not document the exact transport or input protocol (AWDL/Bluetooth, event format). No authoritative technical teardown was found.
- I found no reverse-engineering project that uses iPhone Mirroring from Windows or Linux. Absence of evidence is not proof that none exists.

## Q3. Hardware and open-source approaches, and how they locate or position the pointer

### Takeaway
Apple's own DTS engineers say a standard USB or Bluetooth HID accessory is the only supported way to drive iOS input externally, and that "the simplest way to prototype is a computer simulating a Bluetooth HID device." The best open-source reference for pointer positioning is the ESP32 iPhone Agent, which uses a relative USB HID mouse plus AssistiveTouch, periodic corner resets, and vision-based cursor detection to correct drift. There is an open, unresolved iOS 26 bug where composite BLE mouse-plus-keyboard devices lose the Shift modifier. That bears directly on Porthole's single-radio design.

### Cited Findings
- **Apple DTS (developer forums):** iOS "does not provide any mechanism that would allow an app to 'inject' events into the system." On the other hand, "iOS supports HID, so a USB or Bluetooth HID accessory can control the device's interface… it's certainly possible to create a HID accessory that 'relays' events from some external source." Also: "nothing you've described would require an MFi accessory… For prototyping purposes, the simplest way… would be to use a computer to simulate a Bluetooth HID device." Full-screen capture is only possible through a user-started ReplayKit Broadcast Upload Extension — [Apple Developer Forums 794952](https://developer.apple.com/forums/thread/794952)
- **Composite BLE HID Shift bug (June–July 2026, iPhone 16 Plus, iOS 26):** an nRF52840 composite keyboard + consumer + mouse device (appearance HID_MOUSE) is adopted as an AssistiveTouch pointer, and unmodified keys and Cmd+Space work. **Shift is stripped from text** (`A`→`a`, `!`→`1`). Keyboard-only builds and a Magic Keyboard work, and the "Hardware Keyboard" settings row appears only without a mouse role. Splitting into two HID services on one device still fails, and a commercial USB combo fails too. DTS pointed to the Accessory Design Guidelines (GAP appearance, report-map structure) and said combos "should just work" if structured correctly. Feedback FB24023642 is filed and there is no confirmed workaround — [Apple Developer Forums 835175](https://developer.apple.com/forums/thread/835175)
- **ESP32 iPhone Agent** (MIT, github.com/onehumanbeing/esp32-iphone-agent): an ESP32-S3 plugged into the iPhone over USB acts as a USB HID mouse and keyboard, and AssistiveTouch shows a round grey cursor. "A mouse is a relative device." The app tracks the cursor, **periodically resets it to a screen corner**, and before precise taps **wiggles it and finds it in the screenshot to correct drift**. It checks whether the screen changed to detect missed taps. Screen capture comes from AirPlay to the ESP32's own hotspot (480×1040, ≤5 fps). It needs iOS 18+ and is limited to ASCII-only typing. Passcode entry, Apple Pay and DRM video can't be mirrored — [yuv.ai write-up](https://yuv.ai/blog/esp32-iphone-agent?lang=en); [Electronics For You](https://www.electronicsforu.com/news/this-esp32-lets-ai-control-a-real-iphone)
- Generic BLE HID mouse libraries: T-vK ESP32-BLE-Mouse (relative only) — [GitHub](https://www.Github.com/T-vK/ESP32-BLE-Mouse); asterics esp32_mouse_keyboard — [ecosyste.ms](https://awesome.ecosyste.ms/projects/github.com%2Fasterics%2Fesp32_mouse_keyboard). ESP32MouseTo implements absolute positioning by homing, assuming the pointer starts in the lower-right corner — [GitHub Youngv/ESP32MouseTo](https://github.com/Youngv/ESP32MouseTo). A forum thread proposes "position the pointer at 0,0 for each movement" for BLE absolute moves — [Arduino forum](https://forum.arduino.cc/t/ble-hid-mouse-with-cursor-absolute-positioning/1216238)
- A HID-extender project's compatibility table notes that iOS supports mouse control when AssistiveTouch is on. It is unclear whether absolute pointing works — [PC-HID-Extend](https://github.com/Cemu0/PC-HID-Extend)
- **Why scrcpy doesn't do iOS:** a contributor who discussed it with the author says "scrcpy is very specific to Android… mostly an adb link, a little ffmpeg player, and hacks and java payload." iOS "would be completely different," with "almost no code sharing." He lists alternatives: quicktime_video_hack (USB screen capture, no input), XCTest/WebDriverAgent-based tools (ios-vnc, controlfloor, iOS-Tagent, ws-scrcpy), and jailbreak tools — [scrcpy issue #1691](https://github.com/Genymobile/scrcpy/issues/1691)
- ws-scrcpy: "iOS support was always experimental… never completed properly," and on Windows it can only see the device — [ws-scrcpy issue 263](https://www.github.com/NetrisTV/ws-scrcpy/issues/263)
- KVM software: no source was found that says Deskflow, Barrier, Mouse Without Borders or Logitech Flow support iPhone. DeskIn's statement that no third-party remote software can control iOS is the closest evidence — [DeskIn FAQ](https://deskin.io/support/faq/cant-control-iphone-ipad)

### Inferences
- Porthole is on the Apple-sanctioned path: the PC emulates a standard HID accessory. Software-only KVM tools (Deskflow and others) cannot target iPhone because they rely on an agent app injecting events, which iOS forbids. Only HID-emitting hardware or radios work.
- Pointer-positioning playbook from prior art: (a) slam to a corner with large negative deltas to get a known origin, (b) move with deltas calibrated under a known tracking speed (vendors use 6), and (c) close the loop by detecting the AssistiveTouch cursor in the mirrored frame, which Porthole already receives via UxPlay. Pointer-acceleration non-linearity means open-loop absolute mapping will drift. Visual feedback from the AirPlay stream is Porthole's structural advantage over the BT-only vendors.
- The composite Shift bug is high-risk for Porthole's single-adapter mouse+keyboard. Options: two separate BLE peripherals/identities (pointer and keyboard), a keyboard-only mode for text entry that drops the mouse role, or a different report-map or appearance (keyboard appearance, or a Digitizer/trackpad collection). These need empirical testing on iOS 26.6.1. The thread notes Apple's guidelines mention trackpad only for iPadOS 14.5.
- USB-C HID into iPhone 15+ (as the ESP32 agent does) is a viable fallback transport with no Bluetooth quirks. It needs a microcontroller with USB device mode, since a Windows PC cannot act as a USB device itself.

### Gaps
- I did not find a GitHub project combining BLE HID, absolute positioning and corner reset specifically for iPhone, beyond ESP32 iPhone Agent (USB) and ESP32MouseTo (generic).
- No commercial "iPhone KVM" or Bluetooth HID relay box (for example USB-to-BT bridges) was verified with iPhone-specific documentation in this pass.
- Whether iOS accepts an absolute digitizer/touchscreen HID report from a BLE device on iPhone (not iPad) is unconfirmed. Projects default to relative mouse plus AssistiveTouch.
- No accessibility switch-interface products were researched.

## Q4. Legal, policy and DMA constraints (MFi, App Store, developer license, trademarks, GPL, EU DMA)

### Takeaway
A standard BLE HID mouse or keyboard does not require MFi. Apple DTS says so explicitly and points to the public Accessory Design Guidelines for report-map conformance. Porthole needs no iOS app at all, so App Store and WebDriverAgent signing constraints are avoided. The EU's March 2025 DMA interoperability decision opened nine connected-device features (notifications, proximity pairing, P2P Wi-Fi, NFC and others). None covers HID input or iPhone Mirroring, so the DMA does not currently enable Windows control. A request-based process exists that a developer could use to ask for more.

### Cited Findings
- **MFi:** "the MFi program is a hardware licensing program… If whatever you're trying to do isn't on that list, then it's very likely that the MFi program will not be useful to you"; "nothing you've described would require an MFi accessory" (about HID-based remote control) — [Apple Developer Forums 794952](https://developer.apple.com/forums/thread/794952)
- The Accessory Design Guidelines cover Bluetooth accessory requirements and are the reference for HID GAP appearance and report-map structure. Keyboard classification likely requires conformance — [Apple Accessory Design Guidelines (PDF)](https://developer.apple.com/accessories/Accessory-Design-Guidelines.pdf); [Apple Developer Forums 835175](https://developer.apple.com/forums/thread/835175)
- A community answer says standard Classic profiles (HID, HFP, A2DP) can be used without MFi — [Apple Developer Forums 756763](https://developer.apple.com/forums/thread/756763)
- **No in-app injection:** iOS offers no API for apps to inject system events. Screen capture by an app requires a user-started ReplayKit broadcast extension — [Apple Developer Forums 794952](https://developer.apple.com/forums/thread/794952)
- **DMA specification decision (19 March 2025):** it covers "nine iOS connectivity features, predominantly used for connected devices such as smartwatches, headphones or TVs." Examples are notifications on smartwatches, P2P Wi-Fi and NFC transfers, and pairing. A second decision improves the request-based process for developers seeking interoperability with other iOS features (documentation access, predictable review timeline). There is no mention of HID, input, or screen mirroring — [European Commission DMA page](https://digital-markets-act.ec.europa.eu/commission-provides-guidance-under-digital-markets-act-facilitate-development-innovative-products-2025-03-19_en); decision text [EUR-Lex C/2025/4646](https://eur-lex.europa.eu/eli/C/2025/4646/oj/eng/pdf)
- One outlet lists the features as including notifications, automatic Wi-Fi connection, AirPlay, AirDrop, NFC, and automatic audio switching — [TechRepublic](https://www.techrepublic.com/fr/article/news-apple-eu-dma/). Not verified against the decision text, so treat "AirPlay" as unconfirmed. 9to5Mac reports the reported timelines (proximity pairing beta late 2025, full by 1 June 2026) — [9to5Mac](https://9to5mac.com/2025/03/19/eu-order-apple-interoperability/)
- **iOS 26.3 (EU only):** AirPods-style proximity pairing for third-party accessories, and notification forwarding to one third-party wearable at a time (forwarding to it disables Apple Watch notifications) — [MacRumors](https://www.macrumors.com/2025/12/22/ios-26-3-dma-airpods-pairing/); [Engadget](https://www.engadget.com/mobile/apples-ios-263-will-introduce-proximity-pairing-to-third-party-devices-in-the-eu-133037696.html)
- Apple called the decisions "bad for our products and our users" — [MacRumors](https://www.macrumors.com/2025/03/19/apple-eu-interoperability-bad-for-products-users/). FSFE said the final decision was weaker than the draft — [FSFE](https://fsfe.org/news/2025/news-20250403-01.html)

### Inferences
- **No iOS app means no App Store, developer-program or WDA constraints.** Porthole's design (AirPlay via the built-in Screen Mirroring plus a standard HID peripheral) needs nothing installed on the iPhone. This avoids App Review, the yearly developer fee, 7-day free-provisioning re-signing, and the Apple Developer Program License Agreement limits that WebDriverAgent/XCTest-based tools face.
- DMA proximity pairing (iOS 26.3+, EU) could at most streamline Porthole's BLE pairing UX for EU users if a PC can act as a qualifying "connected device." It does not give a new input or mirroring channel. An EU developer could file a DMA interoperability request for iPhone Mirroring-equivalent access, but that is speculative and slow.
- Naming: the vendors call their features "screen mirroring" and "control iPhone." Avoiding "iPhone" or "AirPlay" in the product name itself (for example "Porthole — mirror and control for iOS devices") follows normal practice. A dedicated legal check is still needed (see Gaps).

### Gaps
- **Trademark rules:** Apple's trademark guidelines on "iPhone" and "AirPlay" in third-party product names were not fetched in this pass. A follow-up is needed (apple.com/legal/intellectual-property/guidelinesfor3rdparties.html).
- **GPL/FairPlay:** the UxPlay licence (believed to be GPL-3.0) and the legal status of its reverse-engineered FairPlay handshake were not verified in this pass. These need a separate check of UxPlay's LICENSE and README and of any DMCA/anti-circumvention risk discussions.
- The decision text (EUR-Lex PDF) was not fetched, so the authoritative list of the nine features and whether AirPlay/media casting is among them is unconfirmed.
- No evidence was found of anyone filing a DMA interoperability request for remote input or mirroring to non-Apple PCs.
- Bluetooth SIG qualification and listing obligations for a distributed product that only uses the host's already-qualified adapter were not researched.
