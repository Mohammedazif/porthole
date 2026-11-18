# Clipboard and file transfer that fit Porthole

Scope is the owner's iPhone 14 Pro on iOS 26.6.1, used from a Windows PC, developed on Ubuntu 24.04. No jailbreak. No Mac in the daily loop. Same Wi-Fi only, no internet relay. Date of this pass: 2026-11-18.

Porthole today already mirrors over AirPlay and sends a Bluetooth HID keyboard and absolute mouse. Its only clipboard feature types the PC clipboard into the focused iPhone field as US QWERTY keystrokes (Alt+V). That is input, not a pasteboard read. Sources for that behavior are the tree itself: [src/control/paste.h](/home/spidewol/Documents/ios-windows/src/control/paste.h), [src/control/paste.c](/home/spidewol/Documents/ios-windows/src/control/paste.c), [src/polish/runtime.c](/home/spidewol/Documents/ios-windows/src/polish/runtime.c). [PLAN.md](/home/spidewol/Documents/ios-windows/PLAN.md) still lists "Paste PC clipboard text" as an open checkbox; the code path above is what actually exists. Nothing in that path, and no source found below, shows AirPlay or BLE HID reading the iOS pasteboard.

## For each candidate, what is done once, what works every day, and what breaks on iOS 26?

### Takeaway

Two no-app USB paths cover most of what the owner can have: lockdown AFC for photos and file-sharing folders, and the CoreDevice pasteboard for plain text and plain images both ways after Developer Mode and a mounted Developer Disk Image. A personal iOS app or Shortcuts is only needed for arbitrary files while unplugged. HID typing stays the zero-setup way to put ASCII into a focused field. iOS 26 does not remove AFC or the pasteboard service, but a stale disk image hides the pasteboard, and the iOS 27 screen-share clipboard UI does not run on 26.6.1.

### Cited Findings

**HID typing only (what Porthole does now)**

- Alt+V reads the PC clipboard with SDL and encodes it as HID key-downs. Bytes at or above 0x80 are skipped. Shift is the only modifier. The buffer passed in is 8192 bytes. The help line calls this "V paste clipboard". — [src/control/paste.c](/home/spidewol/Documents/ios-windows/src/control/paste.c), [src/polish/runtime.c](/home/spidewol/Documents/ios-windows/src/polish/runtime.c), [src/main.c](/home/spidewol/Documents/ios-windows/src/main.c)
- The comment on the encoder says it is "Clipboard text as US QWERTY key-downs" and that the caller sends each key release. — [src/control/paste.h](/home/spidewol/Documents/ios-windows/src/control/paste.h)
- PC Ctrl is Command on the phone, so Ctrl+C and Ctrl+V stay inside iOS rather than being stolen by the PC. — [README.md](/home/spidewol/Documents/ios-windows/README.md)
- No source in this pass describes an AirPlay mirror stream or a Bluetooth HID keyboard report that carries the iOS pasteboard. The open-source pasteboard implementation that does exist talks to `com.apple.coredevice.pasteboardservice` over a developer tunnel, which is a different channel. — [pymobiledevice3 issue 1744](https://github.com/doronz88/pymobiledevice3/issues/1744), [CLI recipes, screen streaming](https://doronz88.github.io/pymobiledevice3/latest/guides/cli-recipes/)

**USB AFC / libimobiledevice**

- libimobiledevice talks to iOS "using native protocols", "does not require jailbreaking", is cross-platform including Linux and Windows, and is LGPL-2.1. Its own feature list includes accessing "the filesystem of a device" and "documents of file sharing apps". The GitHub org was pushing the library on 2026-11-18. — [libimobiledevice README](https://github.com/libimobiledevice/libimobiledevice), [org repositories](https://github.com/orgs/libimobiledevice/repositories)
- The AFC service name is `com.apple.afc`. — [afc.h](https://docs.libimobiledevice.org/libimobiledevice/latest/afc_8h.html)
- `afcclient` "allows access to parts of the filesystem". `--documents <appid>` is the Documents directory and `--container <appid>` is the container. `--network` is "not recommended, since the connection might be terminated at any time". Symbolic links are noted as disabled on newer iOS. — [afcclient man page](https://www.mankier.com/1/afcclient)
- pymobiledevice3's AFC shell is documented as the "media directory", and `AfcService` "gives access to the device's media directory". — [CLI recipes](https://doronz88.github.io/pymobiledevice3/latest/guides/cli-recipes/), [Python API guide](https://github.com/doronz88/pymobiledevice3/blob/master/docs/guides/python-api.md)
- A libimobiledevice maintainer, in July 2025: full filesystem access "was only possible on jailbroken phone with afc2", and that service "is not working since ios11 or around that". Without that, "you get only limited content with afc (which is where you can see DCIM folder with photos)", or a document-sharing app. Rootless jailbreaks do not restore this through ifuse. — [issue 1679](https://github.com/libimobiledevice/libimobiledevice/issues/1679)
- Since iOS 8, house arrest only works for apps whose installation record has `UIFileSharingEnabled`. That is the same limit iTunes used for the document-sharing section. Maintainers said this in 2015 and the 2025 comment above matches it. — [issue 244](https://github.com/libimobiledevice/libimobiledevice/issues/244), [issue 1679](https://github.com/libimobiledevice/libimobiledevice/issues/1679)
- Pairing is a separate once-per-host step. Apple: the first time the phone is connected, it asks whether you trust the computer. Trusted computers "can sync with your device and access your device's photos, videos, contacts, and other content" and stay trusted until you change that list or erase the device. On iOS 16 and later the alert also appears when you back up, and if automatic backups are on it appears every time you connect. You unlock the phone and confirm on the device; Don't Trust refuses access. Reset Location & Privacy clears the trusted-computer list. — [Apple Support 109054](https://support.apple.com/en-us/109054)
- Apple's pairing model: the user must unlock and accept; on iOS 9 or later they also enter the passcode; the two sides exchange 2048-bit RSA keys; the host receives a 256-bit key that can unlock an escrow keybag; protected services start only inside the resulting SSL session. Wi-Fi use of that session requires a previous pairing over USB or Thunderbolt. On iOS 11 or later a pairing record that has not been used for more than 30 days expires. Some services, including `com.apple.mobile.pcapd`, are USB-only. — [Apple Platform Security, pairing model](https://support.apple.com/en-au/guide/security/secadb5b6434/1/web/1)
- usbmuxd stores those records in `/var/lib/lockdown` on Linux and `/var/db/lockdown` on macOS. — [usbmuxd README](https://github.com/libimobiledevice/usbmuxd)
- On Windows, Apple's stack is "Apple Mobile Service" from an iTunes installation, speaking the usbmux API over TCP port 27015. — [pymobiledevice3 protocol notes](https://github.com/doronz88/pymobiledevice3/blob/master/misc/understanding_idevice_protocol_layers.md)
- pymobiledevice3 tells Windows and Linux users to install "iTunes or usbmuxd" before the first command, then connect by USB, unlock, and accept Trust. — [installation](https://doronz88.github.io/pymobiledevice3/installation/), [Python API guide](https://github.com/doronz88/pymobiledevice3/blob/master/docs/guides/python-api.md)
- AFC is a lockdown service, not a developer service. The Python guide says classic lockdown services (explicitly including AFC) work over USB on every iOS version, while developer services moved behind RemoteServiceDiscovery on iOS 17. — [Python API guide](https://doronz88.github.io/pymobiledevice3/guides/python-api/)
- Linux AFC sessions have been reported to die while the phone is still charging (about 147 seconds in one Ubuntu/Mint report, about 103 minutes in a later one). A commenter said it did not disconnect on iOS 26.6.0 and that, historically, a crashed AFC service stayed dead until the cable was replugged. A fresh Mint install with the same package versions held for 10 minutes and survived replug. This is not a clean iOS 26 regression. — [libimobiledevice issue 1723](https://github.com/libimobiledevice/libimobiledevice/issues/1723)
- `ifuse --documents` failing to start `com.apple.mobile.house_arrest` was reported on iOS 15.8 even when `idevicepair validate` succeeded and `ifuse --list-apps` listed file-sharing apps. That is an open client bug report, not a statement that house arrest was removed. — [ifuse issue 118](https://github.com/libimobiledevice/ifuse/issues/118)

**Developer-mode CoreDevice pasteboard (the "DVT clipboard" candidate)**

- Developer services on iOS 16 and later need Developer Mode. Apple: it "protects people from inadvertently installing potentially harmful software" and "reduces attack vectors exposed by developer-only functionality". It does not change App Store or TestFlight installs. Turning it on shows an alert that Developer Mode reduces the security of the device, then a restart, then a second confirmation and the device passcode. After that, Xcode does not ask again unless it is turned off or the device is erased. The switch is Settings, Privacy & Security, Developer Mode. — [Apple Developer Documentation](https://developer.apple.com/documentation/xcode/enabling-developer-mode-on-a-device)
- pymobiledevice3: `developer dvt` on iOS 17 and later needs only Developer Mode. `developer core-device` always needs the Developer Disk Image as well. `amfi enable-developer-mode` restarts the device and "only works when no passcode is set"; with a passcode the user turns the switch on in Settings. `mounter auto-mount` "is needed again after every reboot" and downloads the image, so a Mac with Xcode is not required. Cache is `~/.pymobiledevice3` or `$XDG_DATA_HOME/pymobiledevice3`. — [CLI recipes, Developer Mode and DDI](https://doronz88.github.io/pymobiledevice3/latest/guides/cli-recipes/)
- Image choice: iOS below 17 uses a classic DeveloperDiskImage; iOS 17.0 through 26.x uses a PersonalizedDMG through the image mounter; iOS 27 and later uses a Cryptex1 image. iOS will install the image as a cryptex only from 26.4; below that, `cryptex auto-install` refuses and `mounter auto-mount` uses the PersonalizedDMG. Devices absent from a PersonalizedDMG build manifest (the note's example is the iPhone 18 series, which ship with iOS 27) cannot use that image. — [CLI recipes](https://doronz88.github.io/pymobiledevice3/latest/guides/cli-recipes/)
- On iOS 17.4 and later, developer commands on Linux and Windows build a no-root in-process userspace tunnel themselves. iOS 17.0 through 17.3 on Linux needs a root `tunneld`; on Windows that range needs an Administrator `tunneld` and a third-party driver the project does not ship. iOS 26 is outside that old range. — [iOS 17+ tunnels](https://github.com/doronz88/pymobiledevice3/blob/master/docs/guides/ios17-tunnels.md), [installation](https://doronz88.github.io/pymobiledevice3/installation/)
- `com.apple.coredevice.pasteboardservice` was missing from the RemoteServiceDiscovery list on an iPhone 13 running iOS 26.2 and on an iPhone SE running iOS 17.5.1 while an old iOS 17-era image was mounted. Mounting a current image (the reporter used build 27A5194q) made the service appear, and "copy/paste round-trip works perfectly — including Chinese text and emoji". — [issue 1744](https://github.com/doronz88/pymobiledevice3/issues/1744)
- The library API is `PasteboardService` plus `read_pasteboard()` / `PasteboardMonitor`. They watch the Darwin notification `com.apple.pasteboard.notify.changed` and read the primary type. Subscribing to the daemon's own change push makes `dtpasteboardd` load every representation first. A Notes copy advertises `public.rtf`, which fails only after about 64 seconds, and Apple's own `devicectl device pasteboard monitor` showed the same delay. A picture that exists only inside a Notes web archive is skipped unless `allow_full_pull=True`. Text and plain images are not affected. The project tells users to copy pictures from Photos or from a screenshot. The stall was reproduced on an iPhone 18,4 running iOS 27.0; the fix shipped as v11.15.2 on 2026-09-17. — [PR 1961](https://github.com/doronz88/pymobiledevice3/pull/1961), [v11.15.2 notes](https://newreleases.io/project/github/doronz88/pymobiledevice3/release/v11.15.2), [commit 3146d1a](https://github.com/doronz88/pymobiledevice3/commit/3146d1a98e85d5f8a968af1fe36198faa9886752)
- `serve-web` and `serve-vnc` can sync that pasteboard both ways, but both "need iOS 27 or later: older devices never send the stream, so these commands refuse to start on them". `serve-web` syncs text and images and needs a secure browser context. `serve-vnc --share-clipboard` is text only and the server-side decode is macOS-only. Binding either to `0.0.0.0` means "whoever reaches the port can both watch and control the device". — [CLI recipes, screen streaming](https://doronz88.github.io/pymobiledevice3/latest/guides/cli-recipes/)
- pymobiledevice3 is GPL-3.0-or-later, pure Python, and runs on Windows, Linux, and macOS. PyPI showed 11.59.0 on 2026-11-18. The maintainers say there will be no commercial license exception. Screen mirroring "with touch, buttons, keyboard, audio and clipboard" is advertised for iOS 27 and later, which matches the serve-web limit and is not the same claim as the pasteboard service on iOS 26. — [GitHub](https://github.com/doronz88/pymobiledevice3), [docs home](https://doronz88.github.io/pymobiledevice3/latest/), [PyPI](https://pypi.org/project/pymobiledevice3/)

**Shortcuts only**

- A third-party shortcut-plus-Python-server project (AirDropPlus) sends files from the iOS share sheet to a PC on the same LAN or a hotspot, pulls files from the PC clipboard when the user runs the shortcut, and claims text, image, and file clipboard sync. It tells the user to enable Settings, Apps, Shortcuts, Advanced, Allow Sharing Large Amounts of Data, or multi-image sends fail. The user configures the PC address in the shortcut. Nothing in that design runs a background daemon. — [AirDropPlus](https://github.com/yeyt97/AirDropPlus)
- A separate RoutineHub shortcut from 2023 posts files, photos, video, or clipboard text to a LAN HTTP server and says "Data is transmitted unencrypted" and "For use on private network only". That warning belongs to that shortcut, not to AirDropPlus. — [RoutineHub 17314](https://routinehub.co/shortcut/17314/)
- iOS 27's Shortcuts release notes add many actions and three automations (News notifications, keyboard connected, screenshot saved). They do not add an unattended clipboard or file-sync automation. The phone in this project is on iOS 26, not 27. — [9to5Mac, 2026-09-29](https://9to5mac.com/2026/09/29/ios-27s-shortcuts-app-adds-35-new-improved-actions-heres-whats-new/)

**Small personal iOS app plus LAN**

- LocalSend is an App Store / Windows / Linux app, Apache-2.0, "share files and messages with nearby devices" over a local network "without needing an internet connection", using a REST API and HTTPS, with no third-party server. iOS minimum is 13.0, and sharing from other apps requires iOS 14 or later. The project recommends the store build because the app does not auto-update. — [LocalSend](https://github.com/localsend/localsend)
- Porthole's own constraint is that an iPhone app, if any, is built in Xcode on a Mac, and normal use installs nothing. — [README.md](/home/spidewol/Documents/ios-windows/README.md)
- Wireless pairing from Xcode without a cable is described, by a secondary guide, as requiring iOS 27 or later and Xcode 27. Cable pairing remains the documented path below that. — [Technobezz](https://www.technobezz.com/how-to-turn-on-developer-mode-on-an-iphone). This is not Apple's wording; Apple's own page still describes enabling Developer Mode by running from Xcode over the connection you already have. — [Apple Developer Documentation](https://developer.apple.com/documentation/xcode/enabling-developer-mode-on-a-device)

### Inferences

- Once, for HID: Bluetooth pair, AssistiveTouch on. Every day: focus a field and press Alt+V. Survives iOS 26 because it is an ordinary keyboard. It cannot grow into images, files, or a read of the phone clipboard. Non-ASCII is already dropped in `ph_paste_encode`.
- Once, for AFC: a data cable, the phone unlocked, Trust, and the passcode (or whatever the trust UI offers that day). On Windows, install Apple's device support once so port 27015 exists. Every day: plug in and copy the media directory, or the Documents folder of an app that already has file sharing. No Developer Mode, no phone app, no Mac. iOS 26 does not restore the full filesystem and does not make AFC a pasteboard. Treat Linux idle drops as a helper bug to test on this phone, not as a reason to reject AFC.
- Once, for the pasteboard: the same Trust, plus Developer Mode (Settings, restart, Enable, passcode; the no-passcode CLI will not apply to a normal phone), plus one `mounter auto-mount`. Every day: USB, and after each reboot mount the image again before clipboard calls work. On 26.6.1 the tunnel should be the no-root userspace one. Call `read_pasteboard` / `PasteboardMonitor`. Do not start `serve-web` and expect a picture; that stream is refused before iOS 27. An iOS update that needs a newer PersonalizedDMG looks like a missing service until auto-mount is run again, which is exactly issue 1744.
- Shortcuts are already on the phone, so they avoid a Mac and a signing cycle, but every transfer is a manual run. They are a fallback, not the daily clipboard.
- A personal app can do share-sheet files and an explicit "send clipboard" button over the LAN. It cannot be the first slice: it needs a Mac to build, and this pass did not confirm how long a free provisioning profile lives. LocalSend from the App Store is the no-Mac way to get arbitrary files both ways, with a person accepting each send.

### Gaps

- No source in this pass tested an iPhone 14 Pro on iOS 26.6.1. The closest pasteboard report is iOS 26.2 after a matching disk image. The closest AFC comment is that 26.6.0 did not show the short idle disconnect.
- This pass did not confirm that the current PersonalizedDMG build manifest lists the iPhone 14 Pro board. The cryptex-only failure is described for devices missing from that manifest, with the iPhone 18 series as the example, not the 14 Pro.
- No recipe line found for a standalone `pymobiledevice3` pasteboard subcommand. The supported surface found here is the Python API (`read_pasteboard`, `PasteboardService`, `PasteboardMonitor`) and the iOS 27 viewer toggle.
- Not verified: whether writing a file into DCIM makes it appear in Photos on iOS 26; iCloud-optimized and edited assets; whether QuickTime USB capture and usbmux AFC can be open on the same cable.
- Not verified: current UIPasteboard rules for a background third-party app, the free Apple Developer signing lifetime, or KDE Connect's iOS clipboard limits.
- Apple's trust article still says to enter the passcode. A 2024 report says iOS 18.2 can confirm trust with Face ID. A 2026 forensic blog says Stolen Device Protection can require a biometric away from a familiar place and that Apple does not list pairing among the documented Stolen Device Protection actions. — [9to5Mac](https://9to5mac.com/2024/11/06/ios-18-2-trust-computer-face-id/), [ElcomSoft](https://blog.elcomsoft.com/2026/06/forensic-implications-of-apple-stolen-device-protection/). Which of those the owner's phone shows at home was not tested.
- USB Restricted Mode (data blocked after the phone has been locked) was not re-read from Apple's current security guide in this pass.

## Can Porthole stay useful with zero software on the phone, and which half is impossible?

### Takeaway

Yes. With nothing installed, the owner keeps the mirror, the mouse and keyboard, ASCII typed into the focused field, and, after one USB trust, photos and videos out of the camera-roll media folder plus files in and out of apps that already enable file sharing. The impossible half, until Developer Mode or some phone-side action exists, is reading the iOS pasteboard, writing an image onto it, and moving files that live outside that media share.

### Cited Findings

- The README's product rule is "Nothing to install on the iPhone" for normal use, using Screen Mirroring, Bluetooth, and AssistiveTouch. — [README.md](/home/spidewol/Documents/ios-windows/README.md)
- The implemented paste path only consumes the PC clipboard. It never queries the phone. — [src/control/paste_sdl.c](/home/spidewol/Documents/ios-windows/src/control/paste_sdl.c), [src/control/paste.c](/home/spidewol/Documents/ios-windows/src/control/paste.c)
- Trusted-computer access, without a third-party app, is what Apple describes as sync of photos, videos, contacts, and other content. — [Apple Support 109054](https://support.apple.com/en-us/109054)
- The non-jailbreak file surface is the AFC media directory (DCIM is the photo folder the maintainer names) and Documents of apps with `UIFileSharingEnabled`. Everything else in the filesystem was the old jailbreak `afc2` service. — [issue 1679](https://github.com/libimobiledevice/libimobiledevice/issues/1679), [issue 244](https://github.com/libimobiledevice/libimobiledevice/issues/244)
- Pasteboard read and write in the open-source tools is `com.apple.coredevice.pasteboardservice`, which showed up only after a matching Developer Disk Image was mounted, on both iOS 17.5.1 and iOS 26.2. — [issue 1744](https://github.com/doronz88/pymobiledevice3/issues/1744)
- Mounting that image is not installing an app, but it is Developer Mode plus an Apple-signed image, and core-device services always need the image on iOS 17 and later. — [CLI recipes](https://doronz88.github.io/pymobiledevice3/latest/guides/cli-recipes/)
- Shortcuts can move files and clipboard text without a new app, but only when the user runs the shortcut. — [AirDropPlus](https://github.com/yeyt97/AirDropPlus)

### Inferences

- "Zero software" should be split in the product, or the claim will be wrong. Zero installs and zero Developer Mode still gives PC text into a field, and files that are photos or already-shared app documents. Zero installs with Developer Mode adds plain text and plain images both ways on the real pasteboard. Arbitrary files in both directions, with the cable unplugged, need either a manual Shortcut or an app such as LocalSend.
- The half that stays impossible if the owner refuses both Developer Mode and any share action is: iPhone clipboard to PC, images onto the iPhone clipboard, and files that are not in the AFC media tree or a file-sharing app. HID cannot be extended to that half. AirPlay cannot either, on any source found here.
- Writing the pasteboard does not focus a field or press Paste. Even after slice 2, text that must land in a specific field can still go through Alt+V, and an image still needs the user to paste inside the phone app (Ctrl on the PC is already Command).

### Gaps

- Whether the built-in Files app exposes any `UIFileSharingEnabled` container of its own on iOS 26 was not established. Assume it does not, until a listing on this phone says otherwise.
- No measurement of how much of the Photos library AFC actually returns when iCloud Photos is optimizing storage.

## What open-source libraries can a C/C++ or Python helper on Linux and Windows call in 2026?

### Takeaway

Call libimobiledevice for AFC and pairing, and pymobiledevice3 for the pasteboard and the disk image. Both run on Linux and Windows without a Mac. Both were current in November 2026. pymobiledevice3 is GPL-3.0-or-later, which matches Porthole; libimobiledevice is LGPL-2.1.

### Cited Findings

- libimobiledevice, LGPL-2.1, C, Linux, macOS, Windows, and Android. Utilities include `afcclient` (AFC and house arrest) and `idevicepair`. Updated 2026-11-18. It does not use proprietary libraries and does not require a jailbreak. — [README](https://github.com/libimobiledevice/libimobiledevice)
- libusbmuxd and usbmuxd are the multiplexer. Pairing records live in `/var/lib/lockdown` on Linux. Socket on Linux is `/var/run/usbmuxd`. — [usbmuxd README](https://github.com/libimobiledevice/usbmuxd)
- ifuse mounts AFC through FUSE. That is a Linux desktop tool, and its documents mount is unreliable in at least one iOS 15.8 report. It is a poor fit for the Windows app. — [ifuse issue 118](https://github.com/libimobiledevice/ifuse/issues/118)
- pymobiledevice3, GPL-3.0-or-later, Python 3, Windows, Linux, and macOS. PyPI 11.59.0 dated 2026-11-18. Public classes include `AfcService` on a lockdown client and the developer stack on `PreferredRsdTunnel`. `read_pasteboard(rsd)` pulls the general pasteboard. Install with pip or uv inside a helper venv. Prerequisite on the host is iTunes/Apple Mobile Device Service or usbmuxd. — [GitHub](https://github.com/doronz88/pymobiledevice3), [PyPI](https://pypi.org/project/pymobiledevice3/), [Python API](https://github.com/doronz88/pymobiledevice3/blob/master/docs/guides/python-api.md), [commit 3146d1a](https://github.com/doronz88/pymobiledevice3/commit/3146d1a98e85d5f8a968af1fe36198faa9886752)
- The same project mounts the iOS 26 PersonalizedDMG with `mounter auto-mount`, with no local Xcode, and builds the iOS 17.4+ tunnel in-process without root on Linux and Windows. — [CLI recipes](https://doronz88.github.io/pymobiledevice3/latest/guides/cli-recipes/), [tunnels guide](https://github.com/doronz88/pymobiledevice3/blob/master/docs/guides/ios17-tunnels.md)
- LocalSend is Apache-2.0 and already has Windows and Linux builds plus an iOS app. It is a product the owner can run, not a C library for Porthole to link. Its protocol is a local HTTPS REST API. — [LocalSend](https://github.com/localsend/localsend)
- Porthole is GPL-3.0. pymobiledevice3's maintainers say a closed-source app cannot get an exception because copyright is spread across contributors and the tunnel stack is GPL as well. Releasing the caller as GPL is the allowed path. — [docs home, license FAQ](https://doronz88.github.io/pymobiledevice3/latest/)

### Inferences

- Slice 1 can be a small C helper linked to libimobiledevice, started the same way Porthole already starts a helper, talking only to `com.apple.afc` and house arrest. That keeps Developer Mode out of the file feature.
- Slice 2 should be a separate GPL Python helper that vendors nothing and calls pymobiledevice3. Reimplementing RemoteXPC, the personalized image, and `dtpasteboardd`'s type policy would repeat bugs that were still being fixed in September 2026 (the 64-second Notes stall).
- Do not shell out to `ifuse` on Windows. Do not use `backup2`, even though pymobiledevice3 implements it, as the file pipe. The recipes show it can pull messages, WhatsApp, contacts, and call history; that is a different and broader copy than the user asked for. — [CLI recipes, backup](https://doronz88.github.io/pymobiledevice3/latest/guides/cli-recipes/)
- A Linux dev VM can test both helpers. The Windows build needs the Apple mobile-device service present on the host; the helper should detect a missing usbmux listener and say so, not bundle a driver.

### Gaps

- go-ios and other 2026 language bindings were not reviewed. No second implementation of `com.apple.coredevice.pasteboardservice` was confirmed.
- Not checked: whether Ubuntu 24.04's apt libimobiledevice is new enough for iOS 26, or whether the helper must build upstream. The upstream tree moved on 2026-11-18; the distro package might be older.
- LocalSend's protocol was not read closely enough to say whether Porthole should speak it. The recommendation is to run the existing apps first.
- Windows driver question left open: pymobiledevice3 says iTunes or usbmuxd. It also says Apple's service is TCP 27015. Whether a stock WinUSB/libusb driver plus upstream usbmuxd is enough on Windows in 2026, without Apple Devices, was not proven here.

## Security for a personal tool: trust prompt, pairing records, and not exposing the phone on the LAN

### Takeaway

USB trust is the right gate: one explicit prompt, then a pairing record that is a secret. Leave Wi-Fi lockdown off. Do not bind a developer screen or a file receiver to the whole LAN. The phone should not advertise a new service for this feature.

### Cited Findings

- Trust is an on-device decision. The computer cannot complete it alone. Trusted hosts can sync photos, videos, contacts, and other content. The list survives until Reset Location & Privacy or erase. Automatic local backups on iOS 16 and later make the alert return on every connect. — [Apple Support 109054](https://support.apple.com/en-us/109054)
- The record is not a flag. It is 2048-bit RSA public keys plus a 256-bit key that unlocks an escrow keybag on the device. Protected services, including file transfer and development services, require that SSL session. A Wi-Fi session is allowed only after a physical pairing. Unused records expire after 30 days on iOS 11 and later. — [Apple Platform Security](https://support.apple.com/en-au/guide/security/secadb5b6434/1/web/1)
- usbmuxd writes the records under `/var/lib/lockdown` on Linux. A public write-up of the plist shows `HostPrivateKey`, `EscrowBag`, certificates, `HostID`, and `SystemBUID`. Deleting the files makes the next connection ask again. — [usbmuxd README](https://github.com/libimobiledevice/usbmuxd), [Jon Gabilondo, lockdown walkthrough](https://jon-gabilondo-angulo-7635.medium.com/understanding-usbmux-and-the-ios-lockdown-service-7f2a1dfd07ae)
- `afcclient --network` is documented as not recommended because the connection can be terminated at any time. — [afcclient man page](https://www.mankier.com/1/afcclient)
- pymobiledevice3 can turn lockdown Wi-Fi connections on, and a paired phone is then reachable as `_apple-mobdev2._tcp` with the same lockdown client. The protocol notes say the MAC in that advertisement is the phone's private Wi-Fi address, not the MAC stored in the pair record. — [CLI recipes](https://doronz88.github.io/pymobiledevice3/latest/guides/cli-recipes/), [protocol notes](https://github.com/doronz88/pymobiledevice3/blob/master/misc/understanding_idevice_protocol_layers.md)
- The project itself warns that `serve-web` or `serve-vnc` bound to `0.0.0.0` lets anyone who reaches the port watch and control the device. — [CLI recipes](https://doronz88.github.io/pymobiledevice3/latest/guides/cli-recipes/)
- Developer Mode "reduces the security of your device" by design, because it opens developer-only functionality. It is not a jailbreak and it does not change App Store installs. — [Apple Developer Documentation](https://developer.apple.com/documentation/xcode/enabling-developer-mode-on-a-device)
- One 2023 shortcut author states that their LAN HTTP file post is unencrypted and is for a private network only. — [RoutineHub 17314](https://routinehub.co/shortcut/17314/)
- LocalSend's published design stays on the local network and uses HTTPS, with no account and no external server. — [LocalSend](https://github.com/localsend/localsend)
- A Windows how-to for the Apple Devices app says the Wi-Fi sync checkbox is "Show this iPhone when on Wi-Fi", applied from the General tab after a cable trust, and that a Public network profile or a skipped Apply leaves it off. Resetting Location & Privacy is how that author clears a stale trust. — [Techdows](https://techdows.com/apple-devices-app-for-windows-11-fails-to-enable-show-this-iphone-when-on-wi-fi-feature.html). Treat the checkbox name as that article's, not as a sentence from Apple.

### Inferences

- Default the feature to USB. Do not call `lockdown wifi-connections on`, and do not tick Wi-Fi sync, unless a later setting says in plain language that this PC will be able to reach the phone on the network with no second prompt. Apple's model is that the second prompt does not exist.
- Keep the pairing record where usbmuxd already puts it. Do not copy it into the Porthole config, a screenshot, or a synced folder. The host private key and escrow bag are enough to start protected services for as long as the record is valid.
- The Porthole window should name the phone it paired and refuse a different UDID instead of trusting whatever is plugged in. The owner asked for "the user's own phone accepted without a prompt" only in the sense of not asking again after the Apple trust. The Apple prompt itself cannot be skipped.
- If a LAN receiver is added for Shortcuts or LocalSend, bind it to the PC, require a long shared token, and do not discover the phone. The phone opens the connection. A shortcut that posts to `http://` on the LAN is the weak version; do not ship that without TLS or an equivalent authenticated channel.
- Developer Mode stays off unless the owner turns the clipboard slice on. The UI should repeat Apple's warning and tell them they can turn the switch off later, at the cost of losing pasteboard sync. Remounting a disk image after reboot is a foot-gun, not a reason to also enable wireless debugging.
- Do not open PCAP, syslog, or backup services as part of this feature. They are in the same libraries and they are much broader than clipboard and files.
- iOS 16's "trust again on every backup" behavior can look like the feature broke. If automatic computer backups are on, say so in the helper error, and do not work around it by enabling Wi-Fi sync.

### Gaps

- Exact Windows directory was not read from usbmuxd source in this pass. Secondary write-ups say `%ALLUSERSPROFILE%\Apple\Lockdown` or `C:\ProgramData\Apple\Lockdown`. The Linux path is the one usbmuxd's README states.
- How long a pairing record remains valid when it is used often is the Apple rule (expire only after 30 days unused), but this phone was not checked.
- LocalSend's accept and PIN behavior was not re-read from the protocol spec. Do not describe it as silent.
- Whether Developer Mode on iOS 26 exposes any lockdown service on the LAN by itself, without wireless debugging or `wifi-connections on`, was not established. The safe implementation never turns those on.

## A concrete recommended plan: behavior, components, and what not to build

### Takeaway

Build in three slices, and stop when the owner has enough. Keep Alt+V. Add a USB file drawer with libimobiledevice and no Developer Mode. Add an opt-in USB pasteboard, both ways for plain text and plain images, through a pymobiledevice3 helper. Use LocalSend or a manual Shortcut only for arbitrary files off the cable. Do not build a jailbreak path, a Wi-Fi lockdown path, an AirPlay clipboard, or a custom iOS app until those three are insufficient.

### Cited Findings

- Today's paste is HID, US QWERTY, 8192-byte buffer, non-ASCII dropped, and it requires the keystrokes to land in whatever field is focused. — [paste.c](/home/spidewol/Documents/ios-windows/src/control/paste.c), [runtime.c](/home/spidewol/Documents/ios-windows/src/polish/runtime.c)
- AFC media and file-sharing Documents are available after Trust, without Developer Mode, on Linux and Windows, from a maintained C library. — [libimobiledevice](https://github.com/libimobiledevice/libimobiledevice), [issue 1679](https://github.com/libimobiledevice/libimobiledevice/issues/1679), [Python API](https://github.com/doronz88/pymobiledevice3/blob/master/docs/guides/python-api.md)
- Plain text and plain images both ways were demonstrated on the CoreDevice pasteboard once a current disk image was mounted, including on iOS 26.2, and the September 2026 client stops the Notes rich-text stall by not reading secondary representations. The iOS 27 browser mirror is a different feature and does not apply to this phone. — [issue 1744](https://github.com/doronz88/pymobiledevice3/issues/1744), [PR 1961](https://github.com/doronz88/pymobiledevice3/pull/1961), [CLI recipes](https://doronz88.github.io/pymobiledevice3/latest/guides/cli-recipes/)
- The disk image must be mounted again after every reboot. The tunnel on this iOS version does not need root. — [CLI recipes](https://doronz88.github.io/pymobiledevice3/latest/guides/cli-recipes/), [tunnels](https://github.com/doronz88/pymobiledevice3/blob/master/docs/guides/ios17-tunnels.md)
- LocalSend already covers same-LAN file and message transfer, with apps for iOS and Windows, without a cloud server. — [LocalSend](https://github.com/localsend/localsend)
- Apple says Developer Mode reduces security, and that a Wi-Fi lockdown session rides on the USB pairing with no further prompt described. — [Developer Mode](https://developer.apple.com/documentation/xcode/enabling-developer-mode-on-a-device), [pairing model](https://support.apple.com/en-au/guide/security/secadb5b6434/1/web/1)

### Inferences

User-visible behavior, in the order to build it:

1. **Leave Alt+V alone.** Label it "Type clipboard into the focused field". It remains the path that works with the cable unplugged and Developer Mode off. Do not promise Unicode or images on this key.
2. **First slice: "Phone files" when a cable is connected.** One-time setup text: unlock, tap Trust, enter the passcode. Windows setup text: install Apple Devices or iTunes so the phone is visible, then replug. Buttons, not a background sync: "Copy new photos and videos here" into a chosen PC folder, and "Copy these files to the phone" into the Documents folder of a file-sharing app the helper lists. If the list is empty, say that iOS only allows this for apps that turn on file sharing, and do not invent a drop box. Show the paired device name. If Trust is missing, stop and say so. Never enable Wi-Fi sync from this button.
3. **Second slice, off by default: "Shared clipboard".** Setup text quotes the consequence: Developer Mode reduces security, the phone restarts, and the owner confirms with the passcode. The helper runs `mounter auto-mount`, then watches the pasteboard. Plain text and ordinary images copied on the phone become the PC clipboard. Text and images copied on the PC are written to the phone pasteboard; the status line says "On the iPhone, paste with Ctrl+V". Do not pull Notes' rich representations. If the service is missing, run auto-mount again rather than failing permanently; that is the iOS 26 failure mode from issue 1744. After a phone reboot, the same remount is required before this slice works. HID Alt+V still types text when the pasteboard slice is off or the cable is out.
4. **Later, only if the owner still lacks a transfer:** arbitrary files with no cable. Prefer installing LocalSend on the phone from the App Store and on the PC, same Wi-Fi, each send accepted on the receiver. Second choice is one Shortcut the owner already has to build by hand: share sheet posts a file to a receiver inside Porthole, with a token, PC-side listener only. A custom Porthole iOS app is the last option, and only after checking that its signature will not expire weekly, because building it needs a Mac and daily use is not allowed to need one.

Components:

- Slice 1: GPL C helper, libimobiledevice plus the system usbmuxd or Apple Mobile Device Service. Commands limited to pair-validate, AFC listdir/get/put, and house-arrest Documents for bundles whose `UIFileSharingEnabled` is true. No network flag.
- Slice 2: separate GPL Python helper, pymobiledevice3 pinned to a 2026 release that contains the pasteboard stall fix (11.15.2 or later; 11.59.0 was on PyPI on the day of this note). USB only. `PreferredRsdTunnel` or the CLI's default no-root tunnel. `read_pasteboard` with `allow_full_pull` left false. Disk-image files cached under the pymobiledevice3 data dir, not committed to the repo.
- UI stays in the existing Porthole window. No second mirror and no browser.

What not to build:

- Any jailbreak, `afc2`, or SSH file server.
- A claim, a button, or a protocol parser that tries to read the pasteboard from AirPlay or BLE HID.
- `serve-web` / `serve-vnc` as Porthole's clipboard. Wrong iOS version for the stream, and dangerous if bound widely.
- `lockdown wifi-connections on`, wireless debugging, or "Show this iPhone when on Wi-Fi" as a default or as a silent retry when USB fails.
- An HTTP shortcut receiver with no authentication, or any internet relay.
- `backup2` as a sneaky way to copy the photo library or chat attachments.
- A from-scratch RemoteXPC client.
- A custom iOS app in the first two slices.
- Continuous automatic copy of the whole camera roll. Make it an explicit action so a trusted PC is not silently duplicating the library.

### Gaps

- No live check on this iPhone 14 Pro that auto-mount plus `read_pasteboard` succeeds on 26.6.1, or that AFC and the existing USB screen-capture session can run together.
- Signing lifetime for a future sideloaded app was not sourced, so slice 4 must not assume a free Apple ID stays valid.
- A written accept/PIN description for LocalSend was not pulled from its protocol document.
