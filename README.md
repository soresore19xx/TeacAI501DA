# TeacAI501DA

A macOS DriverKit driver extension for the **TEAC AI-501DA** USB DAC
(VID `0x0644` / PID `0x8038`), which the generic system USB audio driver
cannot play reliably. It also drives the **TEAC UD-501** (units with
TEAC's DSD 11.2 MHz upgrade, `0x0644` / `0x8043`). The UD-501 plays fine
with macOS's own driver; this driver only adds the working volume control
macOS cannot give it — see [TEAC UD-501](#teac-ud-501-upgraded-units). The driver appears in macOS as
*Unofficial TEAC USB DAC Driver*.

Working and in daily use on macOS 26.5 (Tahoe): all six rates
44.1–192 kHz at 24-bit, live rate switching, an 8-hour continuous soak at
192 kHz with zero errors, and a teardown path crash-tested against removing
the device's AC power mid-stream. Current releases are built with Xcode 27.2
and checked on macOS 27.2.1, 26.7.3, 15.8.3 and 12.7.6 — see
[Tested macOS versions](#tested-macos-versions). Volume releases up to v43
had a mute bug — see [Mute state bug up to v43](#mute-state-bug-up-to-v43).
Releases up to v56 could play a fragment of earlier audio when a player
restarted its output — see
[Earlier audio on restart up to v56](#earlier-audio-on-restart-up-to-v56).
One problem is open: an Audio Hijack session with System-Wide Audio nearly
always fails to start when it plays to the AI-501DA and that is also the
default output — see
[Known issue: Audio Hijack with System-Wide Audio](#known-issue-audio-hijack-with-system-wide-audio).
An experimental release, v78, works around it; v68 stays the recommended
release.

**Based on driver source code officially obtained from TEAC**: the source of
TEAC USB HS Audio Driver 273.4.2, TEAC's own macOS driver for this device,
which TEAC provides through its open-source software request form under the
Apple Public Source License 2.0. See [Source provenance](#source-provenance).

Personal, non-commercial project. Unofficial: not affiliated with, endorsed
by, or supported by TEAC.

## Install

Download the notarized disk image from
[Releases](https://github.com/soresore19xx/TeacAI501DA/releases). v68 is the
recommended release. v78 is an experimental release, marked as a
pre-release, for those who need the Audio Hijack workaround — see
[Workaround: v78](#workaround-v78-experimental-release). Drag the app
(`Unofficial TEAC USB DAC Driver Installer`; up to v44 it was named
`TeacAI501DAInstaller`, and the new one updates a driver installed by the
old one) into `/Applications`, and launch it. It asks whether to install or uninstall
and reports the result; `--install` and `--uninstall` skip the prompt for
scripted runs. The first install needs approval, after which the DAC must be
power-cycled so it re-enumerates. Where to approve depends on the macOS
version (the app's message always names the macOS 15 location):

- macOS 15 or later: **System Settings > General > Login Items & Extensions >
  Driver Extensions**
- macOS 13 and 14: **System Settings > Privacy & Security**, then *Allow*
- macOS 12: **System Preferences > Security & Privacy > General**, unlock,
  then *Allow*

Universal for Intel and Apple Silicon, signed with Developer ID and notarized,
so no UDID registration or developer account is needed. Built for macOS 12 or
later.

### Tested macOS versions

| Mac | macOS | AI-501DA | UD-501 | Mute fix (v55, v56) |
|---|---|---|---|---|
| MacBook Neo (Apple A18 Pro) | 27.2.1 | yes | yes | yes |
| Mac Studio (2025, Apple M4 Max) | 27.2.1 | yes | yes | yes |
| Mac mini (2024, Apple M4) | 27.2.1 | yes | yes | yes |
| Mac mini (2024, Apple M4) | 26.5 | yes, up to v43, in daily use | — | — |
| MacBook Pro 13-inch (2020, Intel) | 26.7.3 | yes | yes | yes |
| MacBook Air 13-inch (2020, Intel) | 15.8.3 | yes | yes | yes |
| MacBook Air 11-inch (Early 2015, Intel) | 12.7.6 | yes | yes | yes |

- *yes* covers playback at every rate the DAC offers (AI-501DA 44.1–192
  kHz, UD-501 44.1–384 kHz), live rate switching, volume and mute, and the
  silence fill after a player is killed. Where both DACs are marked, they
  were also run at the same time.
- The Mac mini row for macOS 26.5 is from the releases up to v43
  (AI-501DA only): daily use and the 8-hour soak at 192 kHz.
- *Mute fix* is the change described in
  [Mute state bug up to v43](#mute-state-bug-up-to-v43).
- v66–v68 (the change in
  [Earlier audio on restart up to v56](#earlier-audio-on-restart-up-to-v56))
  were checked on the Mac Studio with macOS 27.2.1 only, with both DACs,
  by recording their analog output. The other Macs were not run again.

Other combinations should work but are untested — reports welcome.

Three flavors are published:

| Disk image | AI-501DA | UD-501 |
|---|---|---|
| `*-fixed-*.dmg` | fixed volume | not handled (system driver) |
| `*-volume-*.dmg` | volume and mute | not handled (system driver) |
| `*-volume-ud501-*.dmg` | volume and mute | volume and mute |

- **Volume**: volume and mute controls are exposed to macOS as a software
  gain; at 0 dB the gain path is bypassed for bit-perfect passthrough.
- **Fixed volume**: no controls are exposed and no gain path is compiled
  in, so output is always full-scale passthrough. The AI-501DA has no
  hardware volume reachable over USB, so use the amplifier's knob or
  remote.
- **UD-501**: only the `volume-ud501` flavor takes the UD-501 (upgraded
  units, see [TEAC UD-501](#teac-ud-501-upgraded-units)). With the other two
  the UD-501 stays with the system driver.

macOS replaces a driver extension only when `CFBundleVersion` increases, so
switching to a flavor with a lower version number needs an uninstall first.

Building from source with your own Apple Developer (Individual) team works
too — see [Build](#build).

## TEAC UD-501 (upgraded units)

**The UD-501 works with macOS's own USB audio driver.** It is a standard
USB Audio Class 2.0 device, and you do not need this driver to play it.

**Why a driver for it anyway: volume.** The UD-501's line outputs are
fixed (the front knob sets the headphone level only), and its USB volume
does not work. The unit declares a volume control (a USB Feature Unit), so
macOS shows a volume slider for it and the slider moves, but the level
never changes. Sending the values directly confirms it: the unit stores and
reports back any volume (−0.5 to −30 dB, on the master and on each channel,
even an out-of-range value), and the level stays the same. TEAC's own
Windows driver and HR Audio Player never set it either. With the system
driver, the level can therefore only be set in the player or after the
DAC. This driver gives the UD-501 a volume and mute that work from macOS,
as a software gain (details below). If you do not need that, leave the
unit to the system driver: install the `fixed` or `volume` flavor, or none
at all.

Handled units are those that went through TEAC's DSD 11.2 MHz upgrade,
which identify as `UD-501V2` over USB (`0x0644` / `0x8043`). Units without
the upgrade are left to the system driver (untested). Tested on one unit,
on macOS 27.2.1 (Apple M4 Max, Apple M4 and Apple A18 Pro) and on macOS
26.7.3, 15.8.3 and 12.7.6 (Intel). The UD-501 is handled by its own class
(`TeacUD501`), and the AI-501DA object files compile byte-identical with
and without it.

- **The volume is software, not the unit's hardware volume.** The driver
  scales the PCM samples digitally (−60 to 0 dB in 0.5 dB steps; mute
  sends zeros) before they reach the unit, the same kind of control as a
  player's DSP volume. At 0 dB the driver copies each 24-bit sample into
  the USB stream unchanged, so the output is bit-perfect for sources of up
  to 24 bits, provided the player sends them unaltered at their own rate.
  The stream is 24-bit, so 32-bit sources are reduced to 24 bits before
  they reach the driver. Below 0 dB the result is limited to the 24-bit
  resolution the unit declares for this stream (about 19 bits of signal
  remain at −30 dB). If you want an untouched signal at every setting, keep
  the slider at maximum and set the level after the DAC, or install the
  `fixed` or `volume` flavor, which leave the UD-501 to the system driver.
- PCM 44.1–384 kHz at 24-bit (the rate list is read from the device's
  clock), live rate switching, and the underrun silence fill.
- DoP passes through unchanged: any 1 ms chunk carrying DoP markers skips
  the gain, so DSD keeps playing at any volume setting (the slider has no
  effect on DSD). DoP at 176.4 and 352.8 kHz is recognised by the unit,
  whose display shows DSD 2.8 MHz and 5.6 MHz. DSD 11.2 MHz needs the
  unit's native DSD mode and is not supported.
- Matching is on the USB product name `UD-501V2`. The UD-503 and the
  NT-503 share the USB ID but are untested and left to the system driver.
- The output keeps the stock name `TEAC USB AUDIO DEVICE`, so scripts that
  select it by name keep working. Its device UID changes, so apps that
  remember the output by UID need it selected again.
- To check which driver is in charge, open **System Information**
  (System Settings > General > About > System Report):
  - *Software > Extensions* lists `Unofficial TEAC USB DAC Driver`
    (bundle ID `jp.hogehoge.TeacAI501DA`) with its version, i.e. what is
    installed.
  - *Hardware > Audio* shows the manufacturer of `TEAC USB AUDIO DEVICE`
    as `TEAC (TeacAI501DA driver)` while this driver runs the unit.
  Right after an install or update the unit stays with the previous
  driver until it is power-cycled, although Extensions already lists the
  new version.

## Mute state bug up to v43

Applies to the `volume` and `volume-ud501` flavors. The `fixed` flavor has
no mute control and was never affected.

Up to v43, the mute state that macOS read back from the driver could be
wrong. CoreAudio reads the state as a 4-byte value, but the AudioDriverKit
mute control filled only its lowest byte. The other three bytes were
leftover memory, so the value was almost never exactly 0 or 1.

- Garbage values were read on every Mac tried, with both the AI-501DA and
  the UD-501: Intel Macs with macOS 12.7.6, 15.8.3 and 26.7.3, and a
  MacBook Neo (Apple A18 Pro), a Mac mini (Apple M4) and a Mac Studio
  (Apple M4 Max) with macOS 27.2.1.
- The leftover bytes depend on what ran before. An earlier test on the
  Mac Studio happened to read clean values.
- The system driver returned a clean value for the UD-501 (checked on the
  MacBook Pro with macOS 26.7.3).

The symptoms, seen on the Intel Macs, were:

- The output went back to muted by itself after it had been unmuted. This
  happened after the DAC was power-cycled or reconnected (all three
  versions), and after every sample-rate change (macOS 15 and 26).
- macOS reported the mute state wrongly. AppleScript's `get volume
  settings` said `output muted:false` while the driver was muted, and the
  menu bar speaker icon could disagree with what you heard.

From v55 (`volume`) and v56 (`volume-ud501`), the driver hands the value
back as a clean 0 or 1. On all six Macs, mute and unmute now hold across
rate changes and power cycles, and AppleScript shows the right state (the
menu bar icon was checked on macOS 26.7.3). Raising the volume while muted
unmutes the output, as with other devices.

After updating, the output may come up muted once, because macOS saved
the wrong state earlier. Unmute it once and it stays unmuted.

## Earlier audio on restart up to v56

Applies to all three flavors and both DACs.

When a player stopped its output and started it again, the first part of
the new audio could be a fragment of what had played a second or two
earlier. Roon, for example, restarts its output between tracks of different
formats, so the end of the previous track seemed to be heard again just
before the next one.

The cause was in the driver. CoreAudio starts writing a little ahead of the
current play position (the safety offset), but the driver treated
everything below the newest write position as written. The stretch between
the play position and the first new write still held audio from one ring
length earlier, and the driver played it.

Measured on the Mac Studio with macOS 27.2.1 by recording the DAC's analog
output with a Focusrite Scarlett 2i2: a 1 kHz tone for 2 s, a 0.6 s stop,
then digital silence.

| | Before the fix | v66–v68 |
|---|---|---|
| UD-501 | 80 ms of the 1 kHz tone at its full level | nothing, in 1 try |
| AI-501DA | 50 ms and 220 ms (2 of 2 tries) | nothing, in 3 tries |

From v66 (`fixed`), v67 (`volume`) and v68 (`volume-ud501`), a write that
does not continue from the previous one first clears the ring between the
two. With the UD-501, v68 also halves the ring (and the zero-timestamp
period) to 49152 frames, so CoreAudio resumes sooner after a rate change:
the first audio arrives 1.0 s instead of 2.05 s after a switch to 96 kHz,
and 2.05 s instead of 4.1 s after a switch to 48 kHz.

## Known issue: Audio Hijack with System-Wide Audio

Seen with v68 and the later test builds; earlier releases were not tried.
Playing to the DAC directly and the volume control are not affected.

An Audio Hijack session whose source is *System-Wide Audio* nearly always
fails to start when it plays to the AI-501DA while the AI-501DA is also the
default output of macOS. The session looks as if it were running, but its
timer stays at 0:00. In the session that was checked, its second output did
not play either. The UD-501 was not tried as both the default output and the
session's output.

**What the log shows.** coreaudiod reports that the session's IO context is
already registered, and IO stops before the first frame:

```
HALS_MultiTap_Engine::RegisterIOContext: registering IOContext 55391
HALS_IOUAEngine.cpp:132  ... IOContext 55391 is already registered
HALS_IOUAEngine.cpp:157  Failed to register io thread!
IO Stopped Context 55391 after 0 frames.
```

`HALS_IOUAEngine` belongs to the AudioDriverKit part of coreaudiod. Between
08:04 and 09:06 on 2026-11-24 (v68, then v69) the log has 67 Audio Hijack IO
contexts whose first device is the AI-501DA, each started while the AI-501DA
was the default output: 42 have this error and ran no frame, 20 ran no frame
without it, and 5 ran (68096, 827392 and three times 512 frames). From 07:00
to 15:00 the error appears for 94 contexts, all of Audio Hijack; none of them
ran a frame.

**What was tried**, on 2026-11-24, and again with the test program on
2026-11-26, on one Mac (Mac Studio, Apple M4 Max, macOS 27.2.1) with Audio
Hijack 4.6.1:

| Default output | Played to | With | Build | Result |
|---|---|---|---|---|
| AI-501DA | AI-501DA | Audio Hijack | v68–v71 | fails (counts above for v68 and v69) |
| AI-501DA | UD-501 | Audio Hijack | v69 | plays |
| RME ADI-2 DAC | AI-501DA | Audio Hijack | v69 | plays |
| AI-501DA | minimal AudioDriverKit device | Audio Hijack | v72 | IO runs (one try, 7 s) |
| minimal AudioDriverKit device | the same minimal device | Audio Hijack | v72 | fails: 4 contexts ran no frame, 2 of them with the same error |
| RME Digiface USB | the same Digiface USB | Audio Hijack | v70 | IO runs (one try, 14 s; sound not checked) |
| AI-501DA | AI-501DA | test program, global tap, auto-start on | v68 | runs, 6 of 6 (2026-11-24) and 4 of 4 (2026-11-26) |
| AI-501DA | AI-501DA | test program, global tap, auto-start off | v68 | runs, 8 of 8 (2026-11-26) |
| AI-501DA | AI-501DA | test program, tap for the AI-501DA, auto-start off | v68 | fails, 24 of 24 (2026-11-26), same error |
| AI-501DA | AI-501DA | test program, tap for the AI-501DA, auto-start on | v68 | `AudioDeviceStart` returns 0, but no callback, 4 of 4 (2026-11-26), same error |

The minimal device had no USB, no IO operation handler and no controls, but it
was built into this driver's own extension (a test build of it), not into a
separate driver. The minimal device was not tried with the test program.

The test program puts a process tap (`CATapDescription`) and the AI-501DA into
one private aggregate device and plays silence to it. On 2026-11-24 the tap
was global and asked for auto-start (`kAudioAggregateDeviceTapAutoStartKey`);
the runs were between failed starts of Audio Hijack on the same DAC. The log
has `register_autostart_context` for each of them and for none of the 94
failed contexts. It also has "Client is not granted access to the tap" for
each of them; the captured level was checked in two of the six, and both had
audio.

The debug log of Audio Hijack (2026-11-25) shows a tap for the AI-501DA, by
device UID, and auto-start off. On 2026-11-26 the program, rewritten as a small
signed app, was run with a global tap and with a tap for the AI-501DA
(`excludingProcesses:deviceUID:stream:`), 4 starts per setting, Audio Hijack
not running. Auto-start off alone does not make it fail: the global tap ran
with auto-start off too, 4 starts with a test tone playing on the AI-501DA (the
tap captured it) and 4 without. The tap for the AI-501DA failed in all 24
starts with auto-start off (with and without the tone; plain, with drift
compensation and excluded processes, and with those and muting). coreaudiod
logged "already registered" for all 28 starts with the tap for the AI-501DA and
for none of the 12 with the global tap.

**What follows from it.**

- The failure does not need this driver's USB, IO handler or control code,
  since the minimal device fails too. Another AudioDriverKit driver was not
  tried, so it is not shown that this driver is uninvolved. Turning prewarming
  off (v70), removing the IO operation handler (v71) and a shorter ring (v69)
  did not stop the failure.
- It was seen only with AudioDriverKit devices. The Digiface is not one (see
  [the next section](#how-other-audio-drivers-are-built)); it is the only
  other product tried in both roles, once.
- Audio Hijack is not needed for it: with a tap made for the AI-501DA the test
  program fails the same way, and with a global tap it does not. The test
  program was not tried with another device or on another Mac. Whether the
  change is needed in macOS, in this driver or in Audio Hijack is open.

Reported to Apple (FB25705511) and to Rogue Amoeba on 2026-11-24. A search on
the same day found no public report of the same failure. It covered web
searches (English, one in Japanese), GitHub issue search, the thread titles on
the first two pages of the AudioDriverKit tag of the Apple Developer Forums,
the release notes of Audio Hijack and Loopback, and some of Rogue Amoeba's
support articles and blog posts. The closest is
[a 2021 forum thread](https://developer.apple.com/forums/thread/694795)
(FB9778734) about Apple's AudioDriverKit sample on macOS 12.0.1. It ends in
the same "Failed to register io thread!" and error 2003329396, but starts with
"Failed to register event link" and mentions no tap; there, setting an IO
operation handler helped, and this driver already has one. Feedback Assistant
lists 11 or more similar reports for FB25705511; whether they describe the
same failure is not known.

### Workaround: v78 (experimental release)

**Why it was released.** The evidence points to Core Audio rather than to
Audio Hijack (see above), it is reported to Apple, and when or whether macOS
will fix it is not known. Rogue Amoeba support, comparing Audio Hijack's
debug log, pointed to the same cause and could not promise a change on their
side. So the stopgap that has been in use on the Mac Studio since
2026-11-24 was published on 2026-11-28 as
[v78](https://github.com/soresore19xx/TeacAI501DA/releases/tag/v78), marked
as a pre-release. It is the v68 volume-ud501 flavor plus the "(Direct)"
devices and per-rate ring sizes. v68 stays the recommended release for
everyone who does not need the workaround.

The driver publishes a second device per DAC, named "(Direct)", that serves
as the default output. The captured and the played device then differ:

| | Default output of macOS | Output Device block in Audio Hijack |
|---|---|---|
| AI-501DA | `TEAC AI-501DA (Direct)` | `TEAC AI-501DA` |
| UD-501 | `TEAC USB AUDIO DEVICE (Direct)` | `TEAC USB AUDIO DEVICE` |

- "(Direct)" sends nothing to the DAC; only the session's output reaches it.
  With the session stopped there is no sound; to play without Audio Hijack,
  select the main device as the default output.
- Volume and mute of "(Direct)" mirror the main device, so the menu bar and
  the volume keys control the DAC. The sample rate of each follows the other.
- The session must have no block for a "(Direct)" device, not even a disabled
  one: one failed start (14:25) was led by the device of a disabled block.

- Scripts or apps that pick the output by name may now also match the
  "(Direct)" device.

**Per-rate ring sizes.** v78 also sizes the ring and the zero-timestamp
period by the sample rate (8192 frames up to 48 kHz, 16384 up to 96 kHz,
32768 up to 192 kHz, 49152 above). On the Mac Studio the wait before IO
starts went from 0.05–1.07 s to 0.08–0.22 s when this was made. On its own
it does not fix the Audio Hijack issue.

**Tested.** v78 was used daily on one Mac, the Mac Studio (Apple M4 Max,
macOS 27.2.1), with both DACs: playback through Audio Hijack with the
workaround, volume with the AI-501DA, and switching to and volume with the
UD-501. It was not run on other Macs or on Intel. The UD-501 was not tried
in the failing setup; it gets the same second device because it is the same
kind of device.

**Going back to v68.** v78 installs over v68, but v68 does not install over
v78, since macOS replaces a driver extension only when `CFBundleVersion`
increases. Choose "Uninstall" in the app first, then install v68 and
power-cycle the DAC.

This is a stopgap, to be dropped once it is no longer needed.

## How other audio drivers are built

A driver for a USB audio device has two jobs: moving the data over USB, and
presenting an audio device to coreaudiod. This driver does both in one
DriverKit extension, the second with AudioDriverKit. The other products on
the same Mac Studio (macOS 27.2.1) do not use AudioDriverKit. Read on
2026-11-24 from Core Audio's plug-in list, the I/O Registry, and the linked
frameworks and entitlements of the installed drivers:

| Product | USB side | Audio device for coreaudiod |
|---|---|---|
| **This driver** (AI-501DA, UD-501) | DriverKit extension (USBDriverKit) | **the same extension, through AudioDriverKit** |
| RME Digiface USB | DriverKit extension `de.rme-audio.dkusb` 1.0.59 (USBDriverKit) | Audio Server plug-in `RMEUsbAsp.driver` 1.0.22 |
| MOTU LP32 | DriverKit extension `com.motu.driver.pro.audio.dext` 2.0.2 (USBDriverKit, PCIDriverKit) | Audio Server plug-in `MOTUProAudioServerPlugin.driver` 2.0.2 |
| Run by macOS's own driver: Topping DX7s and D50, SMSL, RME ADI-2 DAC, Focusrite Scarlett 2i2 | `usbaudiod`, an ordinary process (IOUSBHost) | `usbaudiod` |

Other vendors were not looked at.

- **Both designs are Apple's.** [WWDC21](https://developer.apple.com/videos/play/wwdc2021/10190/)
  describes the plug-in plus DriverKit extension as the macOS 11 design and
  AudioDriverKit as its successor: "Starting in macOS Monterey, you only need
  a dext, no more plug-in." The plug-in interface is "not deprecated". Why
  RME and MOTU use a plug-in is not known here.
- **Path in coreaudiod.** The error above comes from `HALS_IOUAEngine`, in the
  AudioDriverKit part of coreaudiod. It did not appear with the Digiface; the
  MOTU and the devices run by `usbaudiod` were not tried in both roles.
- **Entitlements.** AudioDriverKit brings its own connection between
  coreaudiod and the extension, covered by
  `com.apple.developer.driverkit.family.audio`. A plug-in has to open the
  extension's user client, which takes a different entitlement: RME's extension has
  `com.apple.developer.driverkit.allow-any-userclient-access`, MOTU's plug-in
  has `com.apple.developer.driverkit.userclient-access` for its extension.
- **Moving this driver to that design** would mean splitting it in two, with
  a new plug-in for the audio half, and one of those entitlements.

## How it works

The generic system USB audio driver cannot pace this device: its
explicit-feedback values are unreliable, and following them raw makes the
clock loop collapse within seconds. The dext takes over the whole USB
device, drives the isochronous output from corrected feedback, and bridges
it to CoreAudio.

[**DESIGN.md**](DESIGN.md) has the full write-up: the failure signature,
the chip quirk and its two independent confirmations, the pacing and
teardown design, and the hardware details.

## Build

```sh
cd TeacAI501DA

# Compile check, no signing
xcodebuild -scheme TeacAI501DA -configuration Debug CODE_SIGNING_ALLOWED=NO build

# Development build. Needs a paid Apple Developer Program team; Individual is
# fine, since DriverKit development entitlements including family.audio are
# granted without review when the entitlements file uses the development
# wildcards. A Personal Team cannot provision DriverKit at all.
xcodebuild -scheme TeacAI501DA -configuration Debug build
```

A **fixed-volume** flavor builds from the same source with no controls exposed
to the OS and no gain path compiled in, leaving full-scale passthrough only:

```sh
xcodebuild -scheme TeacAI501DAInstaller -configuration Debug \
  GCC_PREPROCESSOR_DEFINITIONS='DEBUG=1 TEAC_FIXED_VOLUME=1' build
```

A plain build also takes the UD-501. To leave it out (as the `fixed` and
`volume` releases do), add `INFOPLIST_PREPROCESSOR_DEFINITIONS=TEAC_NO_UD501=1`;
the dext's `Info.plist` is preprocessed and drops the UD-501 personality.

macOS replaces a driver extension only when `CFBundleVersion` increases, so
bump `CURRENT_PROJECT_VERSION` before deploying, or uninstall first.

### Developer ID build

Distribution requires the DriverKit distribution entitlements on both App IDs
(separate portal entries from the `(development)` ones) and a Developer ID
certificate issued under the **G2 sub-CA** — the legacy sub-CA expires
2027-02-01 and caps the leaf certificate with it.

```sh
xcodebuild -project TeacAI501DA.xcodeproj \
  -scheme TeacAI501DAInstaller -configuration Release \
  DEVELOPMENT_TEAM=<TeamID> \
  CODE_SIGN_INJECT_BASE_ENTITLEMENTS=NO \
  OTHER_CODE_SIGN_FLAGS=--timestamp \
  GCC_GENERATE_DEBUGGING_SYMBOLS=NO \
  ONLY_ACTIVE_ARCH=NO ARCHS="x86_64 arm64" \
  clean build

ditto -c -k --keepParent TeacAI501DAInstaller.app app.zip
xcrun notarytool submit app.zip --key <AuthKey.p8> --key-id <KeyID> \
  --issuer <IssuerID> --wait
xcrun stapler staple TeacAI501DAInstaller.app
```

That builds the `volume-ud501` flavor. For the `volume` flavor add
`INFOPLIST_PREPROCESSOR_DEFINITIONS=TEAC_NO_UD501=1` to the `xcodebuild`
line, and for the `fixed` flavor add that and
`GCC_PREPROCESSOR_DEFINITIONS='TEAC_FIXED_VOLUME=1'`. The Release
configuration defines nothing else in either setting, so the overrides lose
nothing.

Every one of those settings matters. `Release` and
`CODE_SIGN_INJECT_BASE_ENTITLEMENTS=NO` keep `get-task-allow` out of the
signature, since a Developer ID profile does not carry it. `--timestamp`
overrides Xcode's default `--timestamp=none`, without which notarization
fails with *"The signature does not include a secure timestamp."* And
`GCC_GENERATE_DEBUGGING_SYMBOLS=NO` keeps absolute build paths out of the
shipped binaries — the debug map otherwise records the full source and
DerivedData paths, which leaks the builder's account name and directory
layout. `strings` does not surface them, so check with
`grep -c "$USER" <binary>` rather than by eye.

## Source provenance

This driver is based on source code obtained officially from TEAC. TEAC
distributes the source of its macOS driver for the AI-501DA, **TEAC USB HS
Audio Driver 273.4.2** (shared with the NP-H750), through its open-source
software request form at <https://teac-global.com/support/opensource/form/>
(model `AI-501DA_NP-H750`), under the Apple Public Source License 2.0. The
source used here was requested through that form and received directly from
TEAC in May 2026.

From that source this project takes:

- **The diagnosis.** TEAC's driver hard-codes a slow-PLL feedback correction
  for exactly this VID/PID (`TeacUSBAudioStream.cpp`). That pointed to the
  feedback quirk of the device's USB bridge chip, which the generic system
  driver does not compensate for. [DESIGN.md](DESIGN.md) has the details.
- **`ApplyAssp8802Correction()`**, ported from that correction (see
  [License](#license)).

The DriverKit implementation itself (USB device ownership, isochronous pacing,
the CoreAudio bridge, rate switching, teardown) is not taken from TEAC's
driver, which is an unsigned 2011 kernel extension that no longer runs on
current macOS.

## License

Original code is **MIT** (see `LICENSE`).

`ApplyAssp8802Correction()` in `TeacAI501DA/TeacAI501DA/TeacAI501DA.cpp` is
ported from the TEAC-provided source of TEAC USB HS Audio Driver 273.4.2
(`TeacUSBAudioStream.cpp`,
"Modified by GFEC (TENOR)", Copyright (c) 1998-2010 Apple Computer, Inc.),
which TEAC publishes under the **Apple Public Source License 2.0** (full text
in `LICENSES/APSL-2.0.txt`). The bundled `ReadMe(GFEC TENOR).txt` states:
*"You may obtain, copy, distribute and modify the source code for any portions
of the product under Apple Public Source License."* That fragment is compiled
for observation only; the active pacing path is original code.

USB descriptors, VID/PID values and protocol behaviour are facts and not
subject to copyright.

No warranty. Use at your own risk. **This is an unofficial community driver.
AI-501DA and TEAC are trademarks of TEAC Corporation; this project is not
affiliated with, endorsed by, or supported by TEAC.**
