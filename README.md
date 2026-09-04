# TeacAI501DA

A macOS DriverKit driver extension for the **TEAC AI-501DA** USB DAC
(VID `0x0644` / PID `0x8038`), which the generic system USB audio driver
cannot play reliably.

Working and in daily use on macOS 26.5 (Tahoe): all six rates
44.1–192 kHz at 24-bit, live rate switching, an 8-hour continuous soak at
192 kHz with zero errors, and a teardown path crash-tested against removing
the device's AC power mid-stream.

Personal, non-commercial reverse-engineering project. Unofficial, and not
affiliated with TEAC.

## Install

Download the notarized disk image from
[Releases](https://github.com/soresore19xx/TeacAI501DA/releases), drag the app
into `/Applications`, and launch it. It asks whether to install or uninstall
and reports the result; `--install` and `--uninstall` skip the prompt for
scripted runs. The first install needs approval under
**System Settings > General > Login Items & Extensions > Driver Extensions**,
after which the DAC must be power-cycled so it re-enumerates.

Requires macOS 26.5 or later. Universal (Intel and Apple Silicon), signed with
Developer ID and notarized, so no UDID registration or developer account is
needed. Note that a Developer ID signature names the signer, which
`codesign -dvv` shows on the shipped binaries.

Building from source with your own Apple Developer (Individual) team works
too — see [Build](#build).

## The problem

On macOS the generic USB audio service (`usbaudiod`, successor to
`AppleUSBAudio.kext`) plays the AI-501DA correctly for 2–12 seconds, then
degrades into glitches and finally silence. The more resampling sits in the
audio path, the longer it survives — a counter-intuitive signature that points
at sample pacing rather than data corruption. Other UAC2 devices work fine on
the same machine, and the symptom was reported on Intel macOS as far back as
2015, so this is a decade-old device incompatibility rather than a regression.

## The root cause

The AI-501DA's USB audio bridge is the **TENOR TE8802L (GFEC ASSP8802)**, and
its explicit-feedback values are unreliable as-is. Applying a raw UAC2-spec
Q16.16 feedback value directly to OUT-endpoint pacing makes the host/device
clock loop oscillate and collapse within seconds, which is exactly what
`usbaudiod` does.

Two independent vendors confirm the quirk. TEAC's own macOS driver (273.4.2,
APSL 2.0) hard-codes a slow-PLL correction for VID `0x0644`, and Linux ships
`tenor_fb_quirk` for the same chip family: *"sends wrong feedback frequency
values, thus causing the PC to send the samples at a wrong rate, which results
in clicks and crackles."*

This project found one more quirk: in the 24-bit alt setting the device
reports **(actual consumption − 1)**, so the true rate is `Ff + 1.0` (Q16.16).
All six rates calibrate to `(Ff + 1) = rate / 8000` within 0.03 %.

## How it works

- **Device-level match.** The dext matches `IOUSBHostDevice` on VID/PID with
  `IOProbeScore 100000` and no `IOMatchCategory`, so it outcompetes
  `AppleUSBHostCompositeDevice` and `usbaudiod` never sees the device. Owning
  the whole device is what makes UAC2 clock control on EP0 possible.
- **Forced reconfiguration.** `SetConfiguration(0)` then
  `SetConfiguration(1, matchInterfaces=false)` tears down composite-created
  interfaces before the dext opens AudioStreaming IF#3 directly.
- **24-bit for every rate.** alt2 (6 bytes/frame) is used throughout; the
  16-bit alt1 physically caps at 88.2 kHz, where `SET_CUR 96000` reports
  success but the clock does not move.
- **Feedback-paced output.** Feedback on EP `0x84 IN` is decoded with the
  `Ff + 1.0` quirk, sanity-banded to ±12.5 % of nominal, and drives a Q16.16
  accumulator emitting whole samples per microframe on EP `0x03 OUT`, four
  transfers in flight with resync on frame slip. Following the DAC's real
  consumption keeps its FIFO in the lock window and cancels long-term drift.
- **CoreAudio timing contract.** Ring length equals the zero-timestamp period
  (49152 frames), the timestamp is driven by completed on-the-wire samples
  with raw `mach_absolute_time()`, and the output safety offset scales with
  the rate. Getting any of these wrong is audible.
- **Crash-safe teardown.** `Stop()` quiesces before releasing anything: stop
  flag, interface close, an `OSAction::Cancel` chain whose handler fires only
  after every in-flight completion has finished, then release and a deferred
  superclass `Stop()`. Rate switching and completion delivery share one serial
  queue, so the reconfigure path never sleep-waits — the last drained
  completion re-arms the stream instead.
- **Ring guards.** If the USB reader is about to overtake the CoreAudio
  writer it re-anchors rather than replaying stale data, and every copy is
  clamped to what CoreAudio actually wrote, with the remainder zero-filled, so
  a source that dies mid-stream decays to silence.
- **Software volume and mute.** The device exposes no UAC2 Feature Unit, so
  level control lives in the driver, applied to 24-bit samples in the isoch
  copy path. Unity gain bypasses the multiply entirely, keeping the
  bit-perfect path intact.

The port of TEAC's slow-PLL correction is kept in the source for observation
but compiled out (`ENABLE_ASSP8802_PLL 0`): with the device clock actually
synced to USB SOF it converged about two samples per second low and starved
the FIFO. Direct feedback-following proved correct.

## Notes

- **No DSD/DoP.** Confirmed in practice — bit-perfect DoP markers reach the
  endpoint and the DAC never locks, exactly as with the stock driver — and by
  specification: TEAC's manual lists the AI-501DA among models without DSD
  support, the TE8802L is PCM-only, and the PCM5102 has no DSD input mode. In
  this product family DSD arrived with the AI-301DA and UD-501, which use a
  different USB front-end. 192 kHz / 24-bit is the hardware ceiling.
- **The device firmware can hang.** After about 10.5 hours of continuous
  operation the DAC once vanished from the USB bus with no dext involvement,
  matching long-standing reports from the Intel era. Only an AC power cycle
  revives it; the dext re-matches automatically once it re-enumerates.
- **Isoch data buffers are packed contiguously** (`txn[i].offset` accumulates
  `requestCount`; `wMaxPacketSize` is unused) — established by disassembling
  the DriverKit kernel shim after a strided layout produced garbage.
- **HID remote control looks unavailable.** TEAC's manual states the unit
  cannot be controlled from a PC and no stock driver touches IF#0.

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
  ONLY_ACTIVE_ARCH=NO ARCHS="x86_64 arm64" \
  clean build

ditto -c -k --keepParent TeacAI501DAInstaller.app app.zip
xcrun notarytool submit app.zip --key <AuthKey.p8> --key-id <KeyID> \
  --issuer <IssuerID> --wait
xcrun stapler staple TeacAI501DAInstaller.app
```

All three extra settings matter. `Release` and
`CODE_SIGN_INJECT_BASE_ENTITLEMENTS=NO` keep `get-task-allow` out of the
signature, since a Developer ID profile does not carry it, and `--timestamp`
overrides Xcode's default `--timestamp=none`, without which notarization
fails with *"The signature does not include a secure timestamp."*

## Layout

```
TeacAI501DA/                     Xcode project (dext + container app)
├── TeacAI501DA/
│   ├── TeacAI501DA.{iig,cpp}              driver
│   ├── TeacAI501DADevice.{iig,cpp}        IOUserAudioDevice subclass, rate changes
│   ├── TeacAI501DAVolumeControl.{iig,cpp} software volume
│   ├── TeacAI501DAMuteControl.{iig,cpp}   software mute
│   ├── TeacAI501DA.entitlements
│   └── Info.plist                         IOKitPersonalities, IOUSBHostDevice match
└── TeacAI501DAInstaller/        container app: install, update, uninstall

ai501da_lsusb.txt                full USB descriptor dump
```

## Hardware

- TEAC AI-501DA integrated amplifier / DAC (Reference 501 series, 2012)
- USB audio bridge: TENOR TE8802L (GFEC ASSP8802), High-Speed, UAC2
- DAC: Burr-Brown PCM5102, PCM-only
- Amplifier: ABLETEC ALC0180 Class-D, 68 W/ch @ 4 Ω
- alt1 16-bit / alt2 24-bit, async isochronous with an explicit feedback
  endpoint (4-byte Q16.16, bInterval 4), two clock sources synced to SOF

## References

- TEAC OSS distribution — <https://teac-global.com/support/opensource/form/>
  (request `AI-501DA_NP-H750` for the 273.4.2 KEXT source under APSL 2.0)
- Linux `tenor_fb_quirk` —
  <https://patchwork.kernel.org/project/alsa-devel/patch/1398939622-29451-1-git-send-email-zonque@gmail.com/>
- maudio, source of the "ZTS period equals ring length" contract —
  <https://github.com/2075/maudio>
- Apple [AudioDriverKit](https://developer.apple.com/documentation/audiodriverkit)
  and [USBDriverKit](https://developer.apple.com/documentation/usbdriverkit)
- Karabiner-DriverKit-VirtualHIDDevice, dext signing reference —
  <https://github.com/pqrs-org/Karabiner-DriverKit-VirtualHIDDevice>

## License

Original code is **MIT** (see `LICENSE`).

`ApplyAssp8802Correction()` in `TeacAI501DA/TeacAI501DA/TeacAI501DA.cpp` is
ported from TEAC USB HS Audio Driver 273.4.2 (`TeacUSBAudioStream.cpp`,
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
