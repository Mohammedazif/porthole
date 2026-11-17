# Porthole — Use your iPhone inside Windows

> Working name: **Porthole** (placeholder; avoid Apple trademarks like "iPhone"/"AirPlay" in the final product name).
> Goal: a **free, open-source** Windows app that shows your iPhone's screen in a window and lets you
> **control it with your PC mouse and keyboard**, over **Wi-Fi (priority)** and **USB** —
> "scrcpy for iPhone".
>
> Plan created: 2026-11-16 · Target device: iPhone on iOS 26.6.1 · Host: Windows PC with Bluetooth

## Current status — 2026-11-18

### Mouse and keyboard follow-up — 2026-11-18

Implemented fractional two-axis wheel accumulation, Shift-wheel horizontal scrolling,
mouse buttons 3–8, held media keys, and correct release ownership for modified F1–F4.
Added a cancellable, ACK-paced gesture scheduler using the existing video-confirmed
pointer: timed double-tap, one-second hold, directional swipes, bottom-up swipe, and
explicit F4 feed-wheel mode with one swipe per burst. Bindings and limitations are in
[README.md](README.md). Existing Bluetooth recovery and overscroll handling remain.
This work does not implement or replace Grok's items 5–7.

The combined source passes **69/69 CTest checks** with warnings treated as errors and
address/undefined-behavior sanitizers (78.72 seconds); the optimized Linux build also
passes compilation. Local validation includes real SDL event routing, simulated paced report delivery,
delayed/missing ACKs, frozen video, interrupted holds and geometry changes under
address/undefined-behavior sanitizers. Two integration regressions were reproduced:
keyboard gestures used cached coordinates after moving the window underneath a still
mouse, and small mouse jitter discarded fractional wheel movement before a whole
notch accumulated. Both have regression tests; the cached-coordinate negative control
and the pre-fix fractional-wheel run fail as expected.
Disabling the display-layout guard also makes its SDL regression fail with 16 unwanted
nonneutral mouse reports, while the corrected test passes with zero.

Live Linux checks confirmed two accepted press/release pairs at the corrected Calculator
target. Calculator displayed one digit, so this does not establish native double-tap
recognition. The Photos zoom check failed: its full-screen image arrived as a separate
1920×1080 photo presentation without the portrait phone UI or pointer. The user confirmed
that returning to the library grid restores matching views. Mouse mapping now pauses
for a displayed shape inconsistent with either orientation of the learned phone shape,
without tearing down Bluetooth. Same-aspect external output and sessions without an
initial portrait frame cannot be identified by that check.

Still pending: native double-tap recognition in an app with matching mirrored UI;
live hold/swipe, Notification Center dismissal and Reel/Short outcomes; and native Windows
validation of this controls wave. A completed report sequence is not an app-level pass.

The final Linux preview log records the display-layout pause and restoration on real
Photos frames. The subsequent interactive grid check did not pass: after the preview
restart, BlueZ reported the bonded phone disconnected, so mouse readiness could not be
established. This is not evidence that Bluetooth or live control recovery is solved.

**Windows controls preview (source `5f58874`):** all 150 Release cross-build targets
compile with warnings as errors; 23/23 portable WinRT HID-core tests pass. The native
Win32 pipe/lifecycle test and 12 native unit/SDL control checks pass under Wine,
including presentation suppression and portrait recovery with a simulated phone.
These checks do not validate a real Windows radio or iPhone timing.
The initial packaging check caught the combined app's additional `avformat-63.dll`
dependency. Its complete recursive closure is now bundled: 103 runtime DLLs, exact
MSYS2 package hashes/source URLs/licenses, matching project/vendor sources and the
static-runtime .NET helper. Both payload and final embedded-EXE verification pass.
The locally generated file is `build/Porthole-Windows-Portable-5f58874.exe`
(100,826,112 bytes), SHA-256
`6400424b0a0147c3d2a638fdfb12f0db12b91cec938288ee7549c3bc43b04508`.
No installer, driver, firewall or pairing changes were applied by these checks.

The working controls run in the **Linux prototype in the VMware guest**. A full Windows
Wi-Fi mirror now cross-builds with native discovery and portable dependencies. The user
confirmed Windows host mirroring with the versioned preview. The item 4 WinRT Bluetooth
helper and native process connection are implemented. A follow-up platform check established
that the first reported Bluetooth/pointer checks used the Linux VM. Actual Windows logs
then confirmed video and full HID creation, but advertising aborted during startup.
The one-file preview now launches on the Windows host. Its actual host log confirms
advertising, all three active report subscriptions and a helper input acknowledgement.
Repeated control drops were traced to HID Suspend being mistaken for connection loss;
the correction preserves subscriptions and input epochs across this power-mode hint.
Live calibration, typing, wheel edges and reconnect still need Windows confirmation.
Windows CI is configured to build/test the full mirror and retain a minimal
smoke build; this workstream did not trigger a remote CI run.
The detailed M5 measurements later in this file include an archived experimental controller.

| Work | Current state |
|---|---|
| Claude's project foundation | C/CMake, GPL-3.0, pinned UxPlay core, HID descriptor, Linux dependency scripts, MSYS2 setup and Linux/Windows CI |
| Claude's M1 mirror | Working AirPlay discovery, H.264 → FFmpeg → SDL window, crop/aspect ratio, resize and rotation |
| Claude's M2 controls | BlueZ composite keyboard/mouse/consumer device; typing, Ctrl→Command, Home, Spotlight, Back, App Switcher, Control Center, Notification Center, volume and iPhone screenshot shortcuts |
| Claude's M3 pointer | Video-confirmed positioning, click, secondary click, held drag, wheel routing, rotation, focus-loss releases, per-axis calibration and automated/live checks |
| Claude's M4 usability | F1 controls/setup help, viewing-only status and helper retry; the later Codex workstream adds the guided F2 wizard |
| Claude's M5 work retained | Separate key/consumer queue, paced notification dispatch, complete release drain, faster busy input loop, renderer without vsync and slow-drag retry fix |
| Experimental M5 work archived | Streamed hover and automatic curve fitting withdrawn at the user's request after unstable scrolling calibration; previous controller active |
| Bluetooth follow-up | Registration/advertising ordering, stale-link and timer races, bonded subscription reuse, 19 helper regressions; actual adapter stall and stale pairing recovery documented below |
| Scroll follow-up | Continuous overscroll guard retained; new gestures discard stale bounds and probe gently; unchanged frames require confirmation instead of stopping a list early |
| Audio follow-up | PC playback implemented for AAC-ELD/AAC-LC/ALAC with volume, flush and bounded queue; `--no-audio` mutes PC output; live validation recorded below |

An earlier committed baseline passed 60 CTest executions across debug (23), release (23) and
no-AirPlay (14) builds, all with warnings treated as errors. Pointer, scroll, metrics, decoder
timing/reset, helper shutdown and native-discovery checks pass address/undefined sanitizers.
Live PC audio and both ends of the Bluetooth list were verified earlier. The final isolated
build confirmed all three guided calibration targets and saved the calibration. The
small-correction sensitivity case and its regressions are recorded below.

### Roadmap and delivery status

**Workstream ownership (2026-11-17):** Codex delivered the baseline for items **1–3**, using
parallel agents for control recovery, latency diagnostics and the Windows mirror. Grok owns
items **5–7** in parallel. The user subsequently authorized Codex to implement item **4**.
Shared application changes in this workstream are limited to setup, input recovery,
diagnostics and Windows mirror portability; audio/polish, USB and advanced features stay
with Grok.

| Item | Codex implementation and validation |
|---|---|
| 1 | Cancellation retains helper ACK fences; pointer batches are capped at 24 reports; missing ACKs restart the helper without blocking rendering. F2 guides preparation, pairing, mirroring and transactional three-target calibration. Rapid-cancel, modal-input and reconnect tests pass; all three live calibration targets confirmed and saved on the final build. |
| 2 | F3 and `--diagnostics` report local FPS, receive/decode/present, accepted input handoff, helper dispatch ACK and queue counters. Decoder generations prevent stale frames after reconnect. The actual Bluetooth radio interval remains unknown without a privileged trace. |
| 3 | Native Windows DNS-SD replaces Bonjour, preserves protocol identity/TXT records, and builds the complete SDL/FFmpeg mirror. All 85 UCRT cross-build targets compile with warnings as errors. Portable runtime/source packaging and full MSYS2 CI are in place; the user confirmed host mirroring with preview c18f7ac. |
| 4 | Native WinRT HID-over-GATT helper, Windows process/pipe transport, single-peer subscription gates, existing input protocol and guided setup integration. Actual host logs confirm advertising, all three active subscriptions and an input ACK. The repeated restart cause is HID Suspend being treated as subscription loss; the correction preserves the connection. Full host input/calibration and reconnect validation remain pending. |

1. **Linux controls — baseline delivered:** bounded pointer recovery, long-list edge checks,
   drag/release regressions and guided AssistiveTouch, pairing and calibration setup.
   Continue longer sessions, custom scroll views and additional sensitivity checks.
   Keep streaming/curve experiments isolated until proven.
2. **Responsiveness — diagnostics and queue fixes delivered:** F3 measures local frame stages,
   input handoff and helper acknowledgements. Follow up with a privileged Bluetooth trace
   and external visible-input measurements; radio interval and end-to-end latency remain
   unmeasured. Preserve confirmed positions and releases during later optimizations.
3. **Windows Wi-Fi mirror — preview delivered:** native mDNS, full AirPlay/FFmpeg/SDL build,
   portable dependencies/sources and Private-network firewall helper. User-confirmed host
   mirroring; repeat native MSYS2 CI and wider network/host testing before release.
4. **Windows control — native implementation delivered, host check pending:** WinRT
   GATT HID helper and process transport, encrypted pairing, per-phone subscriptions,
   paced keyboard/mouse/consumer input and F2 setup. HID creation, advertising and all
   report subscriptions passed on the actual host. Retest idle/resume after the Suspend
   correction, then validate typing, pointer/calibration, wheel edges and reconnect on
   Windows. Windows owns the radio interval; software pacing is not that interval.
   An ESP32 fallback remains future work for hosts without BLE peripheral support.
5. **Everyday-use polish (v0.2):** audio/video sync and audio UI, fullscreen/always-on-top/
   saved window placement, PNG screenshots, MP4 recording, clipboard text paste,
   connection PIN/allow-list, tray, diagnostics and installer/uninstaller.
6. **USB mirroring (v0.3):** QuickTime capture feasibility (S4), libusb capture, safe
   WinUSB binding to the capture interface, USB audio/video, latency measurements and
   automatic USB/Wi-Fi switching.
7. **Advanced:** optional precise touch via WebDriverAgent, H.265/4K, iPad support,
   multi-device sessions and file-drop workflows after the basic Windows release is solid.

This status summary describes implemented work and remaining delivery steps. The original
design, spike results and chronological implementation evidence follow.

### Windows Bluetooth delivery — item 4 (2026-11-17)

The native helper uses .NET 10 LTS and WinRT `GattServiceProvider`, leaving the mirror
on C/MSYS2. It publishes encrypted HID 1812 with the same 161-byte keyboard/mouse/consumer
descriptor as the C app. Windows owns pairing, the advertised PC name, CCCDs and radio
timing. The helper requires build 19041+ and a driver with BLE peripheral-role support.
Microsoft's [VirtualKeyboard sample](https://github.com/microsoft/BluetoothLEExplorer/blob/master/BluetoothLEExplorer/BluetoothLEExplorer/Models/VirtualKeyboard.cs)
demonstrates HID publication; its [GATT server documentation](https://learn.microsoft.com/en-us/windows/apps/develop/devices-sensors/gatt-server)
describes system-owned attributes and services.

Input activates only for one peer subscribed to all three Reports. Neutral reports
prime each dispatch epoch; queued/in-flight reports carry peer/epoch checks. Because
unread pipe bytes have no generation field, losing the first ready session permanently
ends that helper instance. The app restarts it with a fresh pipe and resets outstanding
fences after exit. Windows overlapped writes and a bounded FIFO keep a hung helper from
blocking rendering. F2 supplies Windows-specific pairing instructions and calibration.

Validation: all 91 Windows cross-build targets compile with warnings as errors;
the self-contained helper publishes with zero warnings/errors. The final Linux regression
suite passes 23/23. The portable .NET sender passes 10/10 checks, including malformed
frames, queue saturation, epoch isolation, real dispatch ACKs, pacing and bounded
release cleanup. Actual Wine execution of the native Windows pipe test writes a Win32
PASS marker and verifies Unicode/quoting, inherited-handle isolation, partial I/O,
hung stdin, early stdout EOF, descendant-held output, termination and no handle leaks.
Logs are `build/live/item4-final-{windows,linux,windows-link}.log`.

The initially reported probe and pointer/click successes were subsequently clarified to
have used **Porthole inside the Linux VM**. They do not validate the Windows backend.
The later actual Windows log confirms BLE peripheral support, full encrypted HID 1812
creation and 500x1080 live video, but shows `Aborted (Success)` about 20 ms after each
advertising request. The old helper exited immediately on that callback, preventing any
later `Started` event from being observed.

A [primary implementation's measured startup sequence](https://github.com/abhishek-raj/windows-ble-hid/blob/main/DEVELOPMENT.md#advertising-facts)
records an initial `Aborted` followed by `Started`. The fix keeps only initial
`Aborted/Success` pending within the original 12-second deadline. It never reports
advertising until `Started`/partial-start; actual errors, stops and established-session
aborts remain fatal, and failure wins a concurrent Started callback. The portable suite
now passes 11/11, including this sequence, deadline expiry and sticky failures.

`--probe` still checks creation without advertising. With Porthole closed,
`ble-winrt/porthole-ble-winrt.exe --probe-advertising` now tests two seconds of actual
HID advertising. Retest it on the Windows host, then check iPhone subscriptions, typing,
pointer/drag/wheel input and reconnect. Grok's items 5–7 remain excluded from this delivery.

The subsequent `hostpolicy.dll` errors came from running the seven-file helper update
in a separate directory without its runtime. The Windows folder listing confirmed those
runtime files were absent. At the user's request, delivery now uses a one-file portable
launcher and a genuinely self-contained Bluetooth helper. The helper's 43.97 MB executable
contains a statically linked .NET 10 runtime and 178 verified bundled files; no adjacent
runtime DLLs/configuration or installed .NET are needed.

The outer launcher embeds the full native app, 86 runtime DLLs, helper, sources and licenses.
It extracts into a unique user TEMP directory and opens Porthole automatically, with
private runtime cache and diagnostic logs. A recursive verifier checks native import
closure, safe paths, bundled assembly/configuration contents and the final executable's
RCDATA ZIP. Seven negative cases reject partial or damaged helpers and missing native
dependencies. Native Wine tests cover extraction/app startup success, extractor failure,
missing helper and app startup failure, with actual execution markers and both log streams.
The user subsequently ran the portable file on Windows and reached guided setup, confirming
real system PowerShell extraction and app launch. Pairing the PC name still left controls
waiting; that alone does not establish active HID report subscriptions.

The follow-up subscription fix observes every raw GATT session before filtering for
`Active`. Previously, a CCCD arriving before its session activated lost its only status
observer, and later activation could not update readiness. Observers now survive logical
peer resets, refresh subscriptions on activation, reject stale registration callbacks and
inspect restored CCCDs when advertising starts. The same-peer, all-three-report and fresh
pipe requirements remain. Diagnostics record raw/active counts and session transitions
without peer addresses. Windows setup now distinguishes service creation from established
advertising instead of showing both as waiting for the iPhone.

Validation: 12/12 portable HID checks pass, including delayed activation without a second
CCCD event, observer teardown and stale callbacks. All 91 native Windows cross-build targets
pass with warnings as errors. The four native launcher execution cases pass again, including
the adjacent-log assertion. The updated `Porthole-Windows-Portable-2.exe` writes its connection
log beside itself (with a private TEMP fallback for read-only folders). Windows HID
subscriptions, typing, pointer/calibration, wheel edges and reconnect remain unvalidated.

The user then reported that preview 2 connects but repeatedly drops controls during
checking. The screenshot shows `unavailable; retrying`; a Windows connection log was
requested to distinguish host link loss, notification failure and app-initiated restart.
Independent code review found a separate readiness race: replacing one partial report
snapshot with another one report at a time could temporarily combine old and new bits
into a false all-ready state. The helper now replaces the entire subscription snapshot
under one gate lock after validating all targets. Full readiness and genuine readiness
loss retain the existing epoch/fresh-pipe rules. Initial neutral-report failures identify
the report kind, and startup diagnostics include the helper build revision. These changes
did not establish the cause of the observed host disconnects without its log.
The portable HID suite now passes 14/14 checks, including partial-to-partial snapshot
replacement, stable readiness, real readiness loss, neutral-report failure for each report,
shared priming across both input lanes and bounded shutdown after a stalled first report.

The subsequent actual Windows log establishes the restart cause: at 491.277, 504.006 and
519.027 seconds, all three reports still have one active subscriber for the same peer,
but a HID Control Point `Suspend` write changes `suspended=True`. The old adapter turns
that power hint into three disabled subscriptions, triggering its mandatory fresh-pipe
restart. Advertising then starts successfully and the phone resubscribes, repeating the
cycle. The same log records a 372 ms helper ACK; this confirms dispatch progress, not
visible pointer accuracy or end-to-end latency. A focused excerpt is retained in
`build/live/windows-v2-suspend-evidence.txt`.

Suspend and Exit Suspend now record host power state independently from physical
readiness. They do not disable targets, rotate epochs, clear held-report values, emit
disconnect statuses or restart the helper while the peer and its CCCDs remain active.
Actual session closure, subscription loss, peer changes and notification failures retain
their invalidation and fresh-pipe rules. The HID descriptor and advertised wake flags are
unchanged. This follows the separation of power hints and report subscriptions in the
[HID over GATT profile, sections 4.8–4.10](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/HOGP_v1.0/out/en/index-en.html).
All 15 portable checks pass, including held keyboard/button releases and Sync across
Suspend without an epoch or status change, plus genuine subscription/disconnect loss
while suspended. The Windows .NET target builds with zero warnings/errors. The next
single-file preview is `Porthole-Windows-Portable-3.exe`; host idle/resume and calibration
remain to be retested with this correction.

The next Windows log identifies a separate native-app restart at 223.430 seconds:
`input helper stopped acknowledging queued reports`, followed by pointer Sync
2147483904 at 223.447 seconds. Reconnection and all three report subscriptions succeed;
the helper build is `acbc34e`, confirming that the Suspend correction was running.
Code review and a failing-before regression reproduce a false watchdog condition:
generic pointer/video/alignment failure queues a fresh held-button release fence, then
the app treats `failed && DRAINING` as a transport stall before that new fence can ACK.

The pointer now records actual ACK expiry separately from generic positioning failure.
Only an expired outstanding ACK sets the sticky transport fault; a current matching ACK
or explicit transport reset clears it. Ordinary cancellation, geometry reset and disable
cannot erase a real fault. Fresh release fences after positioning failure drain normally;
the real three-second timeout and missing-ACK recovery remain in place. Expiry checks
also reject reversed timestamps before unsigned subtraction.

ACK callbacks now process pointer and timestamped scroll acknowledgements before the
main loop advances timeout/pump work. Follow-up wheel batches and video waits start from
the actual ACK time, preventing an old clock or pre-ACK timeout from discarding queued
scrolling. Focused pointer and scroll regressions pass; pointer checks also pass
address/undefined sanitizers. The old pointer implementation fails the new watchdog
regressions. Windows live dragging and recovery still need testing in preview 4.
Validation of the isolated committed baseline plus this correction passes all 23 Linux
CTest cases, including real app/helper recovery when ACKs are genuinely absent. All 91
Windows cross-build targets pass with warnings treated as errors. Transcripts are
`build/live/ack-fix-{linux-build,linux-tests,windows-build}.log`.

### Local validation consolidation — 2026-11-17

After the user reported alignment error, lag, interrupted pointer motion, delayed
clicks/drags and a phone that still felt stuck after disconnecting, work moved to local
failure testing. No new Windows preview is produced in this pass. Grok's items 5–7
remain outside the workstream.

The app fixture now physically applies queued reports at 17 ms intervals, with delayed
video, stale ACKs and queued cancellation. Real subprocess tests distinguish a healthy
ACK-producing helper without fresh video from a genuinely missing ACK. Additional
cases cover held drags during mirroring disconnect, focus loss, close, F2, frozen video,
calibration cancellation and a 3.2-second renderer stall. Negative controls reproduce the
old false restart and filtered-event leaks. The full expanded Linux suite passes 37/37
with address/undefined sanitizers and leak detection; the added stale-scale microcase
also passes in the sanitized pointer suite.

This exposed two SDL event-payload leaks: filtered events returned zero from
`SDL_PushEvent`, and late helper exit events were left queued after the app loop stopped.
The app now frees rejected payloads and discards its remaining owned events after all
producers stop. Sanitizer failures in the original reconnect tests disappear with this
cleanup.

Seeded controller stress includes 320 actual held presses, 645 rejected false cursor
circles and 254 missing-ACK recoveries. Dynamic-content tests reproduce a premature
wheel stop when the scroll thumb shrinks while its centre continues in the requested
direction. A narrow fresh-frame heuristic rebases that possible growth/track resize;
existing toolbar compression, elastic-edge stops, one-unit probes and pinned-position
confirmation remain covered.

Cursor profiling measures 53.643 ms median for a 300-pixel search radius before the
change and 9.263 ms after precomputing sampling offsets once per search. All 320
before/after comparisons across 160 seeded scenes are identical, and existing cursor
recognition checks pass sanitizers. These are local CPU measurements, not phone latency.

Paced tests expose remaining controller latency: roughly 1.08 seconds for a simulated
cold click, 1.39 seconds for a cold drag and up to 2.87 seconds for a tracked release
across 11 confirmation plans with a perturbed acceleration curve. Faster fixed correction
fractions improve those examples but regress 96/163 previously successful cases in an
864-case small-residual/stale-scale matrix. Both changes were rejected; a representative
4.1-pixel residual is now a durable regression. The current matrix still has 40 extreme
stale-scale convergence failures, so passing safety checks is not a latency/accuracy pass.

The adapter was attached to the VM and mirroring connected, but Bluetooth repeatedly
connected without input-report subscriptions after switching hosts. No live input was
sent. The receiver subsequently closed and advertising was confirmed inactive. Fresh
phone/VM pairing is being repaired before controlled Calculator alignment/drag checks.
Evidence, reproduction scripts, timings and sanitizer logs are under
`build/live/local-validation/`.

Stateful helper tests also reproduce a held-input defect: native logical peer loss
invalidated the ordinary EOF release path while the physical phone could remain
connected with held keyboard/mouse state. The helper now captures immutable original
targets and registration generations, blocks normal sends, queues only neutral reports
to those same still-active registrations and waits independently for up to 200 ms before
unregistering. It does not remove bonds or deliver queued old input to a replacement
generation. Equivalent client wrappers remain usable; closed/reused registrations and
ready-target generation changes are rejected. Local tests cover the physical-state
model and typed Windows API usage; they do not establish actual Windows radio delivery.
The final helper suite passes 23/23 in five fresh processes, including same-bond idle,
restored-subscription and long-uptime models. Neutral enqueue ordering follows an
assumption used by Microsoft's [VirtualKeyboard sample](https://github.com/microsoft/BluetoothLEExplorer/blob/f0067f40ca3f2a0cd1475b1bd7b36b636d691ee1/BluetoothLEExplorer/BluetoothLEExplorer/Models/VirtualKeyboard.cs#L360-L365);
the API reference does not formally guarantee phone delivery order.

Live VM pairing exposed another Linux helper defect: a two-second startup cleanup
disconnected fresh, untrusted private-address devices and could affect unrelated
devices. BlueZ's [Device API](https://bluez.readthedocs.io/en/latest/device-api/) specifies
that Disconnect on an untrusted LE device disables incoming connections until Connect.
The blanket cleanup is removed. Device listeners and snapshots precede service
publication, GATT reads/writes associate previously unidentified clients, and actual
connection state is checked before late callbacks can rebind a peer. Recovery is scoped
to the selected adapter/current phone and requires a live bonded-and-trusted connection
after its discovery grace. Same-peer subscription ownership survives ordinary link loss;
unrelated device changes do not disable the active phone. All 28 Linux helper tests pass,
including 100 same-bond reconnect cycles, early CCCDs, late association and stale callbacks.
No code removes a bond during routine recovery. The earlier blocked attempt required one
adapter power cycle; the patched receiver is waiting for its first controlled live pairing.

### Live pointer, keyboard and orientation follow-up — 2026-11-17 evening

Fresh Linux pairing restored all three input reports. Subsequent helper/provider restarts
reconnected the same bond without another pairing request, but took about 37 seconds.
Recovery now waits for successful service registration, gives the replacement provider a
full discovery grace and permits one controlled reconnect for an entirely unsubscribed,
bonded/trusted peer. Partial subscriptions are left alone; the attempt cannot repeat in
the same helper lifetime. Forty helper tests cover these cases. Shutdown emits neutral
keyboard, mouse and consumer reports before unregistering and prevents later queued input
from following them. This does not yet establish long-session Windows reliability.

The real Calculator sequence AC, 7, +, 8, = produced 15. The measured first-movement to
button-release times were 557–1173 ms, so latency remains unfinished. Nine earlier live
pointer targets were within 3.54 pixels. ACK settling and candidate stability now overlap
without dropping either safety requirement, and a pending unpressed click may make one
neutral reveal retry when its tiny movement is invisible. A filtered SDL frame event can
also be retried instead of permanently suppressing later video. The consolidated sanitizer
suite passed 41/41 before the subsequent crop and cancellation regressions were added.

Focused App Library search accepted typing, Ctrl+A selected the complete query and the
next character replaced it. Calculator accepted 7 and 8 as 78. Home and Spotlight also
passed live checks. Alt+S/C/N emitted the expected extra-button reports but did not trigger
their actions during the controlled checks; the user reports correct device assignments.
That discrepancy remains open. Repeating Alt+N is not implemented as a close toggle.

The reported jump to a corner is reproduced in the live log: leaving during a move's
video-confirmation wait discards its endpoint, then each re-entry sends eight large
top-left reports. Cancellation now clears the old target/click while retaining feedback
for already-sent unpressed motion. It emits no probe while inactive; held-drag cancellation,
geometry reset and transport failure still invalidate the position. Four paced SDL tests
cover leave/focus loss before and after the ACK: old code sends a second corner sweep,
fixed code does not, and the subsequent click lands within 0.96 pixels. Controller stress
passes 25,600 actions with 320 actual presses and 254 missing-ACK recoveries. The final
integrated Linux ASAN/UBSAN/leak-detection suite passes 45/45. Live verification of this
latest cancellation change still follows; simulations do not establish its radio timing.

The follow-up live log exposed a gain-learning risk: a cancelled sweep's observation
changed vertical gain from about 4.42 to 2.01. Retained cancellation feedback now updates
position only, never speed calibration. A replay of that exact sample fails on the prior
source and passes with this guard; uninterrupted calibration still learns normally. The
pointer suite, 25,600-action stress and four SDL cancellation cases pass sanitizers after
the guard. Do not count the later failed synthetic clicks as controlled phone results:
SDL capture can inject edge motion when synthetic coordinates disagree with the real
desktop cursor. A stationary-cursor/window-placement driver passes an offline SDL check,
but the receiver received an external termination signal before its live use.

The last clean Linux restart restored all three input subscriptions in 2.82 seconds
without re-pairing; this is a successful case, not a general reconnect-time guarantee.

A live 1920x888 landscape stream exposed a separate crop error: dark frames repeatedly
became 412x888 portrait crops, resetting pointer coordinates. Native rotated geometry now
keeps the full frame, and ambiguous dark frames retain a confirmed crop only at the same
stream dimensions. Crop and app integration sanitizer checks pass 24/24. Netflix's iOS
AirPlay/Screen Mirroring restriction is independent of this orientation fix; see its
[official support page](https://help.netflix.com/en/node/11634).

The Windows Portable-4 artifact remains at 994cf42 and lacks the newer pointer, cursor,
cleanup and orientation fixes. Its performance cannot yet be compared with the updated
Linux receiver as the same version. The current portable WinRT core suite passes 23/23
locally; it does not verify Windows radio delivery. No additional Windows preview has
been sent during these live Linux checks. Grok's items 5–7 remain outside this work.

After those checks, one consolidated portable file was prepared from committed source
5e85aba: `build/Porthole-Windows-Portable-5e85aba.exe` (89,473,536 bytes). Its final source
archive, pinned vendor, embedded .NET runtime, DLL imports and payload corruption checks
pass verification. The final native pointer fixture and process/pipe fixture pass under
Wine, and the unchanged managed helper suite passes 23/23. The helper was built at
4314ae0 from sources byte-identical to 5e85aba; both revisions are recorded in build
metadata. This is ready for a real Windows host check, not a Windows Bluetooth latency
or reliability pass. The last Linux receiver received SIGTERM before the remaining live
device-settings check could finish; the source of that external termination is unknown.

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
Linux controls and the Windows viewing preview are implemented as described above.

- [x] PC appears automatically in iPhone **Control Center → Screen Mirroring** (Avahi/mDNS)
- [x] Live iPhone screen in a resizable window, aspect ratio kept, handles rotation
- [x] Bluetooth pairing: the PC shows up as a mouse/keyboard on the iPhone (BlueZ)
- [x] **Click = tap, click-drag = swipe, scroll wheel = scroll**, mapped to the window
- [x] **Keyboard typing** into the iPhone
- [x] Shortcut keys: **Home, App Switcher, Control Center, Notification Center,
      Spotlight, Back, Volume +/−, Screenshot**
- [ ] Toolbar buttons and Lock Screen verification
- [x] F2 setup wizard for AssistiveTouch, pairing, mirror readiness and three-target calibration

### v0.2: everyday use
- [x] **Basic audio** forwarding through PC speakers; `--no-audio` toggle
- [ ] Precise audio/video sync and audio settings UI
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
- [ ] Low-latency tuning mode (the FPS/latency overlay moved to v0.2, see M5)
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
4. **Calibration:** the restored controller uses the measured movement curve and adjusts
   per-axis scale from confirmed hover moves. Keep AssistiveTouch Tracking Sensitivity
   consistent or recalibrate when it changes. Automatic curve fitting remains experimental.
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
  The full mirror now uses native Windows DNS-SD; CI retains a separate minimal build.
- **Bluetooth controls currently run in the Linux VM**, using the passed-through Intel
  adapter and BlueZ. The native Windows WinRT helper now implements item 4, with its
  real host pairing/input gate pending.

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

| Milestone | Status | Delivers | Linux backend | Windows backend (later) |
|---|---|---|---|---|
| **M1** Mirror window | ✅ done | `porthole` shows the iPhone screen: UxPlay core → FFmpeg decode → SDL2 window | Avahi mDNS | Windows mDNS API |
| **M2** Keyboard & shortcuts | ✅ done | typing, Home/App Switcher/Control Center/… hotkeys, scroll wheel, right-click | BLE helper: Python + BlueZ (from the spike) | BLE helper: WinRT |
| **M3** Pointer | ✅ done | the phone pointer follows the PC cursor over the window; clicks and drags land where you click; drift corrected from the decoded video | same helper | same helper |
| **M4** Polish | ✅ help, status, reconnect and F2 guided setup implemented | actual connection gates, user-confirmed iPhone settings and video-confirmed calibration | Linux controls | Windows mirror-only guide |
| **M5** Responsiveness | 🔧 previous video-confirmed mouse controller restored 2026-11-17; Bluetooth and overscroll fixes retained | stable pointer and scroll edges, responsive typing, measured latency; streaming and curve fitting require further validation | same helper, faster Bluetooth connection interval | same helper; WinRT must request the same interval |

**Bluetooth helper protocol:** the main app starts the helper and writes framed HID reports to
its stdin (`type, length, payload`; types = keyboard / mouse / consumer). The helper sends them
at a fixed 15 ms pace (the acceleration model depends on it) and prints status lines
(connected, subscribed) on stdout. Since M5 there are two lanes, each paced on its own: mouse
reports, pauses and `sync` in one, keyboard and consumer reports in the other, so a key press
never waits behind queued pointer movement. The ESP32 dongle can reuse the same framing.

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

#### M5 responsiveness: measurements and changes (2026-11-16)
**Current implementation (2026-11-17):** the previous video-confirmed mouse controller from
`acea5fd` has been restored. It keeps the independently verified retry-counter fix for slow
continuous drags, and resumes `pointer-scale` load/save. Experimental streamed hover, curve
fitting and `pointer-curve` persistence are inactive. Claude's completed mirroring, keyboard,
shortcut, click/drag, rotation, cursor-detection and setup-help work is retained, along with
the separate key queue, fast input loop, rendering improvements, Bluetooth fixes and the
stronger overscroll guard. The experimental controller and all prior changes are backed up
under `build/live/`; restoration does not discard their implementation or findings.

The following movement-curve design and performance tables describe the **experimental M5
build**, not the restored controller. Active Bluetooth and overscroll follow-ups are recorded
below them. Restored-controller build and live verification results are recorded afterward.

After M3/M4 everything worked, but the pointer lagged behind the mouse, jumped, and input felt
slow. Everything in the first two tables was measured from the running app's `--verbose` log
(38 minutes of real use, iPhone 13 Pro Max, debug build in the VM).

**Finding 1: the phone's pointer speed changed twice during the session, and the fixed
movement model no longer matched it.** Fitting the travel of each step size to the
video-confirmed moves gives a clean curve for each period (median fit error 0.4 px):

| Pixels per report, by step size | 1 | 2 | 4 | 8 | 16 | 32 | ≈ curve |
|---|---|---|---|---|---|---|---|
| Model in the code (S3 measurement) | 0.30 | 0.72 | 1.88 | 5.52 | 18.08 | 64.32 | 0.25·s + 0.055·s² |
| Phone, minutes 4–5 | 0.24 | 0.74 | 1.88 | 5.54 | 18.02 | 64.04 | same |
| Phone, minutes 9–14 | 1.20 | 2.84 | 5.87 | 13.91 | 34.38 | 94.62 | 1.25·s + 0.053·s² |
| Phone, after reconnecting (minute 36 on) | 2.71 | 5.68 | 12.27 | 27.72 | 68.57 | 189.12 | 2.6·s + 0.10·s² |

Small steps ended up almost **nine times** longer than the model assumed and large ones three
times, so every move overshot, corrections overshot again, and the pointer oscillated until
the controller gave up and re-anchored through the corner. The old per-axis scale could not
follow: it is one number, it ignored moves under 20 px, and it rejected any change above 2.5×.
This is what a changed **AssistiveTouch → Tracking Sensitivity** looks like (the first change
happened inside one Bluetooth connection). It also explains the earlier "2.0 / 2.0 calibration
was wrong after a fresh connection" note. **Open question for the next session:** was that
slider moved on the phone? If not, something else changes the pointer speed and has to be
found (connection interval is the first suspect).

| Measured before M5 | Value |
|---|---|
| Model's prediction error per move, first 5 minutes / later / now | median 0.6 px / 26 px / 38 px (within 4 px: 99% / 5% / 1%) |
| One pointer hop, from sending to video confirmation | median **193 ms**, 90% under 379 ms (about 6 reports = 98 ms, then 77 ms waiting for video) |
| Phone pointer position updates while the mouse moves | one every **113 ms** (median); 10% of gaps over 313 ms |
| Corner re-anchors (the pointer visibly sweeps through the top-left corner) | 34 in the first ten minutes, 51 in all; 44 of them right after a move that video never confirmed |
| Bluetooth helper time per report | 16 ms (15 ms pace + 1 ms overhead) |
| Wheel | one notch per ~110 ms, with pauses of up to 1.6 s for pointer checks |
| Keyboard reports queued behind mouse reports | worst case 47 ms in this session (little typing while moving) |

What changed:
- **The movement model now fits itself to the phone.** One report of *s* units moves
  `a·s + b·s²` pixels; `a` and `b` are fitted (least squares, recent moves weighted most) to
  where video finds the dot after each hover. A move that misses badly is not believed at
  once: one odd observation can be moving content mistaken for the dot, and it also spoils
  the start of the next move. Two misses in a row that agree mean the tracking speed changed,
  and the curve is rescaled and refitted. While the model is unproven (new connection) or
  doubted, hover moves are kept to nine model pixels per axis, before the optional fixed scale,
  so an incorrect curve does not send the dot outside the video search area or against
  an edge, where it would teach nothing. The curve is saved to `pointer-curve` in the
  config directory and loaded at start; `pointer-scale` stays as an optional fixed multiplier.
  The app logs a line when the model shifts by more than 25%.
- **Hover no longer waits for video after every move.** It goes out in batches of four
  reports (60 ms); each transport acknowledgement starts the next batch toward wherever the
  mouse is *now*. Video confirms the position once the mouse rests, or after 1500 model
  pixels of unconfirmed travel. This only happens while the model's last confirmed move
  landed where it predicted; after a miss, every move is confirmed again until one does.
- **Bug: slow continuous movement gave up after eight moves.** The retry counter was only
  cleared when the target jumped by more than 4 px between two mouse events, which slow
  movement never does; the same bug would have dropped a slow drag. (Found in the timing
  simulation; one clear case in the log.) The counter now counts only confirmed moves that
  neither landed where predicted nor taught the model anything, so a phone that ignores
  reports is still abandoned after eight.
- **Leaving the window forgot the pointer position.** A cancelled hover (mouse left the
  window, focus lost, F1, Alt) is now kept as an unconfirmed estimate, so the next movement
  starts from there instead of from the corner. Cancelled drags and failures still re-anchor.
- **X finished before Y started.** Reports for the two axes now alternate, largest steps
  first, so a batch that is cut off still heads straight for the target. (The per-step fit
  above shows travel depends only on the report's own size, which is what makes this safe.)
- **One unit can be several pixels** at a high tracking speed. The last unit is now sent
  when it ends closer than stopping short, the reveal nudge shrinks from two units to one,
  and a position one unit cannot improve counts as arrived instead of retrying for ever.
- **Typing shared one queue with the mouse** and waited behind pointer batches and the
  150 ms anchor pause. Keyboard and consumer reports now have their own lane in the helper.
  Shutdown drains both lanes before closing Bluetooth, so final key releases are sent.
- **The window presented with vsync on the input thread**, so every video frame could stall
  mouse, keyboard and acknowledgement handling for up to one display refresh. Vsync is off
  (as in scrcpy), and the loop runs every 4 ms instead of 16 ms while input is in flight.

What did **not** change: no button is pressed on an estimate. A press still needs a fresh
video confirmation, movement with a button held is still confirmed after every move and never
teaches the model, and the 1.2 s freshness rule (with its reveal nudge) is untouched.

The first latency estimates below were collected before the live M5 check and the smaller
startup probes described afterward. They are **simulated**, not measured improvements on
the iPhone. Two kinds of evidence:

- *Real data, new model:* replaying the session's own confirmed moves through the fitting
  code brings the median prediction error from 26 px to 1.2 px for the middle period and
  from 38 px to 1.1 px for the current one (within 4 px: 62% and 75%; the rest includes the
  first moves after each change and observations that fit no curve at all).
- *Simulation of the real controller code* (16 ms per report, video 80 ms late):

| Simulated | Before | After |
|---|---|---|
| Phone at the default speed: time behind a cursor moving 200 / 800 / 2000 px/s | 702 / 189 / 146 ms | 59 / 73 / 55 ms |
| ... mouse stops → pointer on target (800 px/s) | 359 ms | 103 ms |
| ... mouse stops → position confirmed, a click or wheel can go | 502 ms | 198 ms |
| Phone at today's speed: time behind a cursor moving 200 / 800 / 2000 px/s | 654 / 138 / 78 ms | 46 / 59 / 53 ms |
| ... mouse stops → pointer on target (800 px/s) | 423 ms | 87 ms |
| ... mouse stops → position confirmed | 806 ms | 214 ms |
| Starting from the old model at today's speed: confirmed moves until the first target is reached | never settles (oscillates, re-anchors) | 6, then 1–2 per target |

Validation so far: 14 full-build checks pass in debug and release, 10 in the minimal build,
and the pointer test passes under address/undefined sanitizers. New regression tests cover
three phone speeds learned from the default model, a speed change in mid-session, a single
bad observation (the curve must stay right at every step), a speed where one unit exceeds
the alignment tolerance, chained hover batches, a turn-around mid-flight, a click arriving
mid-travel (waits for video), a cancelled hover followed by a click (video corrects a wrong
estimate first), the unconfirmed-travel limit, a 30-step slow drag, a phone that ignores
every report, key routing, and a key press sent while the mouse lane is inside a pause.

Live M5 check resumed on 2026-11-16. The release app connected to the iPhone and exposed a
startup failure the original simulation missed: with the earlier movement curve, a 60-pixel
probe could move the real dot past the video search area. The controller then waited for
feedback that could never arrive. The regression now limits simulated feedback to the same
search rectangle as the detector; it fails with the old probe size. Unproven moves now use
nine unscaled model pixels per axis, with an eight-pixel learning threshold. All three
measured phone curves converge with bounded feedback (six or seven confirmations for the
first target), without extra corner anchors. The optional fixed calibration also remains
supported. A second regression verifies that both helper queues drain before shutdown,
including a pending keyboard release. Debug/release/minimal suites and the pointer
address/undefined sanitizer check pass. The corrected release build was then checked against
the phone:

| Live check (release build, 23:22–23:31 IST) | Result |
|---|---|
| Start with saved curve `0.242 0.0499`, phone still at higher speed | Learns about `2.66 0.103` from video and resumes tracking |
| Five PC clicks: AC, 7, +, 8, = | Calculator shows `7 + 8 = 15`; all presses have acknowledged releases |
| One-second hover sweep (150,324) → (350,648) | Final video confirmation at (350,647), 1 px error; about 270 ms after the PC mouse stops |
| Visible hover lag at 200 px/s | Median 65 ms rightward / 64 ms leftward; 43 steady samples, all 52 captures found the dot, no external mouse input detected |
| Visible hover lag at 800 px/s | Median 83 ms rightward / 93 ms leftward; only three steady samples per direction in these short sweeps |
| Last movement batch sent → stable video confirmation | Median 145 ms across 13 moves in the Calculator/sweep check; includes transport and video confirmation, not continuous cursor lag |
| Click target movement begins → button down | About 190–650 ms across the five Calculator clicks; corrections and video checks still cost time |
| Drag (250,810) → (250,378), then reverse | Endpoints (251,380) and (251,812), about 2.2 px error; both queued button releases acknowledged |
| Wheel in Settings, 12 PC events each direction | Scrolls both ways and logs bottom/top boundary stops |
| Keyboard while pointer reports are active | Spotlight accepts `settings` and Enter opens Settings while mouse reports continue |
| Secondary click | Opens the native Home Screen app context menu; button release acknowledged |

The visible-lag check moved the PC cursor horizontally through Calculator's blank area and
located the native dot in XWD captures with the real C detector. Lag is the signed horizontal
separation divided by commanded speed; samples from the first 150 ms of each sweep were
excluded. Sampling is approximate and the faster sweeps are especially short. This is a
short release check, not a matched before/after benchmark. The negotiated Bluetooth interval,
a deliberate Tracking Sensitivity change, and longer/faster motion still need measurement.
Live logs are at `/tmp/porthole-m5-20261116-232158.log`; captures and sweep samples are under
`/tmp/porthole-m5-check-20261116-232206/`, `/tmp/porthole-m5-lag-20261116-233049/` and
`/tmp/porthole-m5-lag-20261116-233116/`.

Overscroll follow-up (2026-11-16): the short edge checks above did not cover sustained
wheel bursts with a moving cursor or a rebounding indicator. The edge guard could reopen
when the thumb bounced away from its learned endpoint, and moving the PC cursor over 20
model pixels discarded the bounds. Expanding headers or changing bottom toolbars could
also move the track origin and hide a compressed edge from the old fixed extrema check.
The stopped direction now stays latched through rebound, fading and pointer repositioning;
reversing the wheel, a navigation/button action, orientation change or reset releases it.
Near-edge wheel probes become smaller sooner, and a compressed thumb can stop input at a
shifted track origin. Regressions cover both edges, cursor repositioning, header/toolbar
changes, reverse input, horizontal scrolling while a vertical edge is blocked, and reset.
All 38 build checks and the scroll address/undefined sanitizer check pass. Live sustained
burst verification on the restored release build passed on 2026-11-17 (details below).
At 23:49 IST the USB Bluetooth
adapter disappeared from the VM; it was subsequently reattached. Porthole retries the helper
every five seconds when no adapter is available. The sustained burst check now lives at
`build/live/scroll_check.py`; the earlier `/tmp` copy was lost in the VM restart.

Bluetooth recovery follow-up (2026-11-17): after the adapter was reattached, the iPhone
connected but did not finish subscribing before the helper disconnected it. Code inspection
found lifecycle races in the previous helper too: advertising could start before asynchronous
GATT registration completed, the startup cleanup could drop a fresh connection, and a retry
timer from a disconnected link could refresh services or disconnect a newer link. Advertising
now starts only after successful registration, startup cleanup leaves fresh links alone, and
connection generations invalidate old recovery timers. Discovery has 15 seconds before
recovery starts, replacing the three-second cutoff. Seven callback-driven lifecycle checks
exercise the actual helper setup without a radio; the previous helper fails five and the
patched helper passes all seven. All 15 helper unit checks and debug/release Bluetooth app
checks pass. The previous committed release (`acea5fd`) was also built and tested separately
at `/tmp/porthole-baseline-acea5fd/`; the workspace was preserved. The VM restarted at 00:21, removing the earlier `/tmp` diagnostic artifacts. Live logs and
checks now persist under `build/live/` (`check.py` and `scroll_check.py`). The iPhone showed
"Pairing Unsuccessful" while Linux retained an old bond. Removing only that phone's Linux
pairing and pairing again restored all three HID subscriptions and app control at about
00:32; Home/Spotlight/typing opened Settings through real PC key events.

A further reconnect regression required fresh notification timestamps even when BlueZ
retained enabled CCC state for a bonded link (see BlueZ 5.72 `att_disconnected` in
https://github.com/bluez/bluez/blob/5.72/src/gatt-database.c). Within the same GATT service
instance the helper now restores readiness for a bonded peer only when all reports were
already enabled; explicit StopNotify and service replacement clear that cache. Unbonded or
partially subscribed links still wait for discovery. The helper now has 19 passing unit
checks, including three new cases that failed before this correction. Release Bluetooth
app checks pass. The helper restart restored all three fresh subscriptions at log time 908.739 (advertising
at 907.645), without resetting the pairing again. A controlled disconnect at 968.865 was
followed by automatic reconnection at 968.990 and restored readiness at 969.011, 21 ms
later, with no new StartNotify calls. This verifies the bonded notification-cache path on
the actual phone. Mirroring stayed connected throughout.

During manual wheel use the experimental automatic curve also became implausibly small
and caused repeated corner anchors. The pre-M5 controller and the current implementation
have been saved under `build/live/controller-m5-snapshot/`, and all current changes are in
`build/live/current-changes.patch`. The previous video-confirmed controller is now restored while Bluetooth and overscroll
fixes remain active. A fresh backup was made before restoration (its path is recorded in
`build/live/restore-backup-path`), and retained fix files were verified byte-for-byte.
Automatic curve fitting and streaming tests are archived with the experimental controller;
the restored tests retain the established click, drag, rotation and cancellation coverage
plus regressions for slow moving targets and ignored HID reports. All 38 debug, release
and no-AirPlay build checks pass with this restored combination, as does the pointer
address/undefined sanitizer check. The slow-drag regression fails against the original
retry implementation and passes with the retained fix. The release app was restarted at
00:56 IST: Bluetooth reconnected and all three reports subscribed at log time 7.321.
Screen Mirroring reconnected and updates normally. Live HID input then stopped; the kernel
logged `hci0: link tx timeout` / `killing stalled connection` at 00:59 and subsequent
advertising commands failed with `-56`. A normal Bluetooth power-off request also failed.
The Intel AX211 USB adapter was reattached to VMware at 01:05, and its firmware loaded
successfully. Connection/subscription status alone did not prove live input delivery during
the earlier adapter stall.

The iPhone then showed "Pairing Unsuccessful" again, while Linux retained a bond and
received no HID subscriptions. Removing only that iPhone's saved Linux pairing and pairing
afresh restored all three subscriptions at log time 117.709 in
`build/live/porthole-20261117-010731.log`. Keyboard navigation and native pointer movement
worked through the restored release app. The user confirmed that Tracking Sensitivity had
been changed since the earlier controller worked, and adjusted it again for this check.
At the resulting setting, three real PC mouse targets landed at (147,398), (347,651) and
(248,799), respectively 3.6, 3.2 and 2.2 pixels from their requested positions. Captures are
under `build/live/scroll-20261117-011333/`. Keep this sensitivity setting consistent with
the restored controller's calibration; the old slider position was not recorded.

The sustained Settings edge check then passed at 01:15: 188 PC wheel events downward sent
18 wheel reports before the bottom stop; 188 events upward sent 24 before the top stop.
Neither direction sent any wheel report after its stop, including cursor repositioning
while the wheel events continued. Both resting screenshots show normal page bounds, with
no large blank stretch. Logs, captures and `summary.json` are under
`build/live/scroll-20261117-011520/`. The earlier attempts that sent zero wheel reports did
not validate the guard and are retained separately in the diagnostic folders.

Early-stop and audio follow-up (2026-11-17, 01:17–01:40): the Bluetooth device list exposed
two more causes of stalled scrolling. A single unchanged video frame could latch an edge,
and old thumb lengths from another page or a changing list could survive into later
gestures. Edge decisions now require fresh batch feedback; a full-size pinned thumb needs
two confirmed batches. After 500 ms without wheel requests, a new gesture starts with a
one-unit probe and fresh bounds. Five regression assertions failed against the preceding
guard and pass with this correction. Wheel positioning now accepts a fresh video-confirmed
offset of 20 model pixels, while clicks and held drags continue to require four pixels.
Tests cover small wheel offsets, the following precise click, larger moves and stale video.

The final release was restarted at 01:36. A live Bluetooth-list check at 01:40 reached the
actual bottom (Other Devices / Apple Watch footer) and actual top (Bluetooth switch and
first saved device). With the cursor stationary, 212 downward PC wheel events sent three
reports before stopping; 217 upward events sent 23 before stopping. Both sent zero reports
after the boundary stop. Logs, captures and summary are under
`build/live/scroll-20261117-014034/`. An earlier cursor-repositioning stress run stopped at
the bottom but lost pointer recovery before reversal and sent no upward wheel reports;
that run is not a pass. Fast cursor changes during recovery remain a known validation gap.

The audio callbacks previously discarded received packets even though the receiver
advertised audio support. They now forward format, compressed packets, volume and flush
events to `src/audio/player.c`. FFmpeg decodes AAC-ELD, AAC-LC or ALAC into a bounded SDL
playback queue (200 ms maximum queued PCM). `--no-audio` mutes PC output. Decoder/device
access is serialized across network callbacks, and teardown stops the receiver first.
The new audio test decodes real AAC/ALAC encoder output and checks ELD configuration,
format changes, queue limits and flush. All 40 CTests pass across debug/release/minimal;
release also passes warnings-as-errors. Audio, pointer and scroll address/undefined
sanitizer checks pass. The actual iPhone negotiated AAC-ELD / 480 samples and began decoded
playback at log time 77.125 in `build/live/porthole-20261117-012849.log`; the user confirmed
hearing it through the PC speakers. Precise audio/video sync remains a polish task.

### Items 1–3 delivery checks (2026-11-17, 02:40–02:44)

Implementation commits are `c70352f` (control recovery, guided setup and diagnostics),
`c18f7ac` (Windows mirror/discovery/portable packaging) and `274469c` (small pointer
corrections). Final Linux builds use the isolated checkout `build/codex-items123` at
`274469c`, so Grok's concurrent, uncommitted work is excluded. All 60 CTest executions
passed; debug/minimal transcripts are `build/live/final-items123-debug.log` and
`build/live/final-items123-minimal.log`.

The first live guide attempts exposed a third-target oscillation at the current Tracking
Sensitivity. Small residual moves now use partial corrections, and progress must include
observed movement toward the target. Simulated phone curves cover three guide targets,
held drags, quantization, ignored tiny reports and several initial scale estimates. The
final live Calculator check confirmed **3/3 targets**, saved scales **2.103 / 3.105** and
completed setup. Its last target was (250,810), observed at (251,807), within the four-pixel
tolerance. Captures and logs are under `build/live/setup-final-20261117-024000/`.

The final rapid Settings scroll check sent **183 PC wheel events in each direction**.
The app emitted 23 downward wheel reports and 25 upward reports, reached the actual
bottom and top, then emitted **zero reports after each boundary stop**, including cursor
movement while events continued. Resting screenshots show normal bounds. Evidence is
under `build/live/scroll-20261117-024236/`. This closes the earlier tested reversal/recovery
gap; custom views and longer sessions remain follow-ups.

One local diagnostic sample during calibration showed 47 FPS, receive-to-decode 1 ms,
receive-to-present 13 ms and helper ACK 17 ms. These are local observations, not an
end-to-end latency claim or the negotiated Bluetooth interval.

The user confirmed Windows host mirroring with
`build/porthole-windows-c18f7ac-ucrt-preview.zip`. The preview contains the committed
`c18f7ac` source, 86 UCRT runtime DLLs, license/dependency/source metadata and checksums.
Its SHA-256 is `5eae6fdd6811e19d30c184847e3b6d99df420eeaa704da0d7914290995f4bb60`.
Windows Bluetooth remains item 4. No Grok work was included in these commits or the preview.

Integration note for the parallel audio/video-sync workstream: `ph_frame_timing` measures
local monotonic receipt/decode time. This delivery uses decoder packet PTS to carry local
receipt provenance; future media-timestamp scheduling must preserve that metadata
separately rather than treat it as the phone's media clock.

Targets for future M5 experiments: pointer within ~70 ms of the mouse while moving; a key press handed to
Bluetooth within one connection interval; no corner sweep in normal use; a changed tracking
speed absorbed within about six moves.

Still open in M5, in this order:
1. **Verify the restored controller:** alignment and sustained wheel-edge checks now pass
   with the current phone sensitivity. Repeat Calculator and drag checks before claiming
   new live results for those actions; their existing automated coverage passes.
   The earlier M5 live results above belong to the experimental build.
   Broader speed/sensitivity tests belong in an isolated experimental build before
   streaming or curve fitting is enabled again. Those experiments must measure long
   unconfirmed chains, detector search bounds and incorrect video observations.
2. **Where the bad observations come from.** Moving content during wheel scrolling is the
   likely source. They cost time (a doubted model moves in short steps) and they are the
   remaining way a position could be confirmed wrongly.
3. **Bluetooth connection interval.** `/etc/bluetooth/main.conf` sets no
   `MinConnectionInterval`/`MaxConnectionInterval`, so the kernel only asks for a new interval
   if the iPhone's choice is outside its 30–50 ms default, and iOS usually starts at 30 ms.
   That would deliver the 15 ms reports two at a time. Check the negotiated value with
   `sudo btmon` (LE Connection Complete / Connection Update Complete), then try
   `MinConnectionInterval=9` and `MaxConnectionInterval=12` (11.25–15 ms; Apple's accessory
   rules allow 15 ms, and 11.25 ms for HID devices). Recalibrate if the interval
   changes the travel per report.
4. **Fewer reports per move.** Measure (a) reports that move both axes at once and (b) step
   sizes between and above the six in use. At today's phone speed a 32-unit step is already
   189 px, so the curve fit could pick step sizes freely instead of powers of two.
5. **Streamed drags and flicks.** A drag still stops for video after every move, so a fast
   swipe has no inertia. Needs cursor tracking that survives moving content.
6. **Faster clicks after a pause** (the reveal nudge costs ~130 ms after 1.2 s of rest) and
   **faster wheel** (more than one notch per batch away from the edges).
7. **Latency on screen:** F3 local timing/FPS diagnostics are delivered. Phone capture,
   radio interval and visible end-to-end response still need independent measurements.
   Use the optimized release build for daily use.
8. Video path: check frame age end to end (AirPlay itself is ~80–100 ms here); USB capture
   in Phase 4 is the real fix.

### Phase 1: Wi-Fi mirror app (v0.1-alpha)
- Project skeleton, CMake, CI for Linux + Windows
- Vendor UxPlay core, replace GStreamer renderer with FFmpeg + SDL2
- Windows mDNS, fixed ports, firewall rules
- **Exit:** `porthole.exe` on Windows shows the iPhone screen over Wi-Fi

### Phase 2: Control (v0.1)
- BLE HID backend on Windows (or ESP32 fallback), requesting a 11.25–15 ms connection interval
- Coordinate mapper + calibration, keyboard forwarding, shortcut actions
- Responsiveness carried over from M5: separate key lane, fast input loop and no vsync on
  the input thread; streaming and automatic curve fitting remain experimental
- Setup wizard (AssistiveTouch, pairing, button mapping)
- **Exit:** you can open apps, scroll, swipe and type on the iPhone using only the PC, and the
  pointer keeps up with the mouse (M5 targets met on Windows too)

### Phase 3: Polish (v0.2)
- Basic PC audio is implemented; finish audio/video sync and audio settings UI
- Screenshot/recording, clipboard paste, PIN/allow-list, tray, installer
- FPS/latency overlay (moved up from "Later": M5 showed it is needed to keep control fast)

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
   F2 guided pairing/calibration is now implemented and its final live three-target check
   passes. F3 local diagnostics and the Windows Wi-Fi mirror are also implemented.
6. **Restored-controller verification:** alignment, guided Calculator calibration, both
   list edges and rapid wheel reversal with cursor movement now pass, as does PC audio.
   Repeat sustained live drags and longer sessions. The earlier 65 ms / 83–93 ms hover
   results and 190–650 ms click timings describe the withdrawn experimental controller.
7. **M5 bad observations:** find where the moves that fit no curve come from (about one in
   eight in the log), starting with wheel scrolling.
8. **M5 Bluetooth connection interval:** read the negotiated interval with `btmon`, try
   11.25–15 ms in `/etc/bluetooth/main.conf`, re-measure.
9. **M5 follow-ups:** fewer reports per move, streamed drags and faster click-after-pause
   and wheel. The F3 local latency overlay is implemented (details in the M5 section).
10. **S2:** the same Bluetooth device on the Windows host with the WinRT GATT server (fast
   advertising, LE-only, re-subscribe handling, 11.25–15 ms connection interval), as part of
   the Windows port.
