# TeacAI501DA

A macOS DriverKit DEXT (Driver Extension) that takes over the **TEAC AI-501DA**
USB DAC (VID `0x0644` / PID `0x8038`) and restores stable audio playback on
**Apple Silicon macOS 26.5 (Tahoe)**.

**Status: working.** The dext is signed (Apple Developer Program, development
profile), activated on the target machine (M4 Mac mini), and plays clean audio
at all supported rates (44.1–192 kHz / 24-bit). It has survived a 3 h 51 m
continuous hi-res soak with zero errors, and its teardown path has been
crash-tested against live-streaming device removal.

This is a personal, non-commercial reverse-engineering project.

## The Problem

On macOS 26.5 Apple Silicon, the generic system USB audio service
(`usbaudiod`, the user-space replacement of `AppleUSBAudio.kext` 850.5)
fails to play continuous audio on the AI-501DA:

- The first **2–12 seconds** play correctly
- After that, playback becomes **glitchy and chopped**
- Eventually the stream goes silent entirely
- The amount of "correct time" depends on how much resampling/conversion is in
  the audio path: more conversion → longer survival (a counter-intuitive
  signature pointing at a sample-pacing problem)

This is **specific to the AI-501DA** — other UAC2 devices (e.g. SMSL USB Audio)
work fine on the same Mac, same OS, same USB controller. The same symptom was
reported on Intel macOS as far back as El Capitan (2015), so this is a
decade-old incompatibility, not a Tahoe regression.

## The Root Cause

The AI-501DA's USB audio bridge is the **TENOR TE8802L (GFEC ASSP8802)** chip.
Its **explicit-feedback values are unreliable as-is**: applying a raw
UAC2-spec Q16.16 feedback value directly to OUT-endpoint pacing makes the
host/device clock loop oscillate and collapse within seconds — which is
exactly what Apple's generic `usbaudiod` does.

Two independent vendors confirm the chip quirk:

- **TEAC's own macOS driver** (273.4.2 source, APSL 2.0) hard-codes a slow-PLL
  correction for `VID 0x0644` (`TeacUSBAudioStream.cpp:179–186,
  m_bGFECASSP8802 = true`).
- **Linux** ships `tenor_fb_quirk` (formerly `udh01_fb_quirk`) for the same
  chip family: *"sends wrong feedback frequency values, thus causing the PC to
  send the samples at a wrong rate, which results in clicks and crackles."*

An additional device quirk discovered during this project: in the 24-bit alt
setting the AI-501DA reports **(actual consumption − 1)** in its feedback
value, so the true rate is `Ff + 1.0` (Q16.16). All six rates calibrate to
`(Ff + 1) = rate / 8000` within 0.03 %.

## Architecture (v29)

The dext owns the **whole USB device**, not just the streaming interface:

1. **Device-level match** — `IOProviderClass IOUSBHostDevice` (VID/PID,
   `IOProbeScore 100000`) with no `IOMatchCategory`, so the dext outcompetes
   `AppleUSBHostCompositeDevice` and `usbaudiod` never sees the device.
2. **Forced reconfiguration** — `SetConfiguration(0)` → `SetConfiguration(1,
   matchInterfaces=false)` tears down any composite-created interfaces, then
   the dext opens AudioStreaming IF#3 directly and issues UAC2 clock
   `SET_CUR`/`GET_CUR` on EP0 (possible only with device ownership).
3. **24-bit alt setting for everything** — alt2 (6 bytes/frame) for all rates
   44.1/48/88.2/96/176.4/192 kHz. (alt1 16-bit physically caps at 88.2 kHz:
   `SET_CUR 96000` claims success but the clock stays at 88.2 kHz.)
4. **Feedback-paced OUT stream** — EP `0x84 IN` feedback is decoded with the
   `Ff + 1.0` quirk, sanity-banded to ±12.5 % of nominal, and drives a Q16.16
   accumulator that emits whole samples per microframe on EP `0x03 OUT`
   (pipelined, 4 transfers in flight, consecutive frame numbers with resync on
   slip). Following the DAC's actual consumption keeps its FIFO in the lock
   window and cancels long-term host/DAC drift.
5. **CoreAudio bridge (AudioDriverKit)** — `IOUserAudioDevice` named
   "TEAC AI-501DA", 24-bit LPCM at the six rates. Critical timing contract
   (learned from the maudio project and bitter experience):
   - ring buffer length **==** zero-timestamp period (`49152` frames),
   - ZTS driven by **completed** (on-the-wire) samples with raw
     `mach_absolute_time()`,
   - output safety offset scaled to the rate (40 ms).
   Rate changes arrive via an `IOUserAudioDevice` subclass
   (`PerformDeviceConfigurationChange` → `ReconfigureForRate`: abort pipes →
   alt0 → clock `SET_CUR` with read-back verification → alt2 → **re-CopyPipe**
   → counter/ring/ZTS reset → re-arm).
6. **Crash-safe teardown (v28/v29)** — `Stop()` quiesces before anything is
   released: stop flag → interface close (implicit abort) →
   `OSAction::Cancel` chain (its handler fires only after all in-flight
   completion callbacks have finished) → release + deferred
   `Stop(provider, SUPERDISPATCH)`; `ivars` lives until `free()`.
   Rate switching and completion delivery share one serial dispatch queue
   (measured, not documented), so the reconfigure path never sleep-waits on
   completions — instead the last drained completion re-arms the stream
   (*rearm-after-drain*). This fixed a real SIGSEGV (use-after-free between a
   queued isoch completion and teardown) captured on 2026-07-04, and the fix
   is verified by killing the DAC's AC power mid-playback: clean quiesce, no
   crash report, automatic re-match on power-up.
7. **Overrun guard** — if the USB reader is about to overtake the CoreAudio
   writer (divergence accumulated from short frames), the reader re-anchors
   behind the write position instead of looping stale ring data.

The TEAC slow-PLL port (`ApplyAssp8802Correction`) is retained in the source
for observation but **disabled** (`ENABLE_ASSP8802_PLL 0`): with the device's
clock actually synced to USB SOF, the ported PLL converged ~2 samples/s low
and starved the FIFO. Direct feedback-following (item 4) proved correct.

## Findings Worth Recording

- **The AI-501DA does not support DSD/DoP on USB.** Verified in practice
  (bit-perfect DoP markers delivered and confirmed at the endpoint — DAC never
  locks; the stock driver behaves the same) and by specification: TEAC's HR
  Audio Player manual explicitly lists the AI-501DA among models where "DSD
  playback is not supported"; the TE8802L is PCM-only; the PCM5102 DAC has no
  DSD input mode. In this product family DSD arrived with the AI-301DA
  (2014) / UD-501 (different USB front-end: TMS320C6748 + PCM1795). PCM
  192 kHz / 24-bit is the ceiling — by design, not by driver limitation.
- **The device firmware itself can hang.** After ~10.5 h of continuous
  operation the AI-501DA once vanished from the USB bus entirely (no dext
  involvement — the same long-standing firmware issue reported back in the
  Intel era). Only an AC power
  cycle revives it. A host-side watchdog (detect + smart-plug power cycle) is
  a planned companion, since the dext auto re-matches after re-enumeration.
- **isoch data buffers are packed contiguously** (`txn[i].offset` accumulates
  `requestCount`, `wMaxPacketSize` unused) — verified by disassembling the
  DriverKit kernel shim after a strided-layout experiment produced garbage.

## Status

| Milestone | Status |
|---|---|
| USB device takeover, EP0 clock control | ✅ |
| Feedback-paced isoch OUT (pipelined ×4) | ✅ |
| CoreAudio output device, clean playback | ✅ (user-verified "no noise") |
| All rates 44.1–192 kHz / 24-bit, live rate switching | ✅ (all six rates physically calibrated) |
| Long soak | ✅ 3 h 51 m hi-res, zero bad/short/overrun |
| Teardown UAF fix + live-unplug crash test | ✅ v28/v29 |
| DSD/DoP | ❌ Not supported by the hardware (closed) |
| Hi-res robustness hardening ("cause B": memcpy ring copy, deeper pipeline) | 📋 planned |
| Device-hang watchdog (AC power cycle automation) | 📋 planned |
| Volume control (currently full-scale fixed), HID remote (IF#0) | 📋 future |

## Layout

```
TeacAI501DA/                    Xcode project (dext + installer app)
├── TeacAI501DA.xcodeproj
├── TeacAI501DA/
│   ├── TeacAI501DA.iig         driver class declaration
│   ├── TeacAI501DA.cpp         driver implementation (~1350 lines)
│   ├── TeacAI501DADevice.iig   IOUserAudioDevice subclass (rate changes)
│   ├── TeacAI501DADevice.cpp
│   ├── TeacAI501DA.entitlements  (development variant: idVendor "*")
│   └── Info.plist              IOKitPersonalities (IOUSBHostDevice match)
└── TeacAI501DAInstaller/       minimal container app
    ├── main.swift              OSSystemExtensionRequest.activationRequest
    └── TeacAI501DAInstaller.entitlements

ai501da_lsusb.txt               Full USB descriptor dump of the device
memory-backup/                  Project notes (USB quirks, design decisions)
```

## Build & Deploy

```sh
cd TeacAI501DA
# Compile check (unsigned)
xcodebuild -scheme TeacAI501DA -configuration Debug CODE_SIGNING_ALLOWED=NO build

# Signed build requires a paid Apple Developer Program team (Individual is
# fine — DriverKit development entitlements incl. family.audio are granted
# without per-entitlement review when the entitlements file uses the
# "development variant" wildcards). A Personal Team cannot provision DriverKit.
xcodebuild -scheme TeacAI501DA -configuration Debug build
```

Deployment loop (the dext is replaced only when `CFBundleVersion` increases):

1. bump `CURRENT_PROJECT_VERSION`, build signed,
2. copy `TeacAI501DAInstaller.app` to `/Applications` and launch it
   (sends the activation request; first install needs the System Settings
   "Driver Extensions" toggle),
3. kill the old dext process if it lingers ("terminating for upgrade" state),
4. power-cycle / replug the device to re-enumerate.

## Hardware

- TEAC AI-501DA integrated amplifier / DAC (Reference 501 series, 2012)
- USB audio bridge: TENOR TE8802L (GFEC ASSP8802), USB High-Speed, UAC2
- DAC: Burr-Brown PCM5102 (PCM-only part — no DSD input mode)
- Amplifier: ABLETEC ALC0180 Class-D, 68 W/ch @ 4 Ω
- Per descriptor: alt1 16-bit / alt2 24-bit, async isochronous with explicit
  feedback EP (4-byte Q16.16, bInterval 4), two programmable clock sources
  synced to SOF, SPDIF output terminal layout

## References

- TEAC OSS distribution: <https://teac-global.com/support/opensource/form/>
  (request `AI-501DA_NP-H750` to get the 273.4.2 KEXT source under APSL 2.0)
- Linux `tenor_fb_quirk`:
  <https://patchwork.kernel.org/project/alsa-devel/patch/1398939622-29451-1-git-send-email-zonque@gmail.com/>
- maudio (M-Audio Fast Track Ultra dext — source of the "ZTS period == ring
  length" contract): <https://github.com/2075/maudio>
- Apple AudioDriverKit: <https://developer.apple.com/documentation/audiodriverkit>
- Apple USBDriverKit: <https://developer.apple.com/documentation/usbdriverkit>
- Karabiner-DriverKit-VirtualHIDDevice (dext signing flow reference):
  <https://github.com/pqrs-org/Karabiner-DriverKit-VirtualHIDDevice>

## Status of Distribution

No binary releases are available yet. The DEXT currently runs as a
development-signed build on the author's own machines (development profiles
expire yearly; rebuild + re-activate required).

A **Developer ID distribution path is in progress**: a DriverKit distribution
entitlement request has been prepared for Apple. If granted, a Developer ID +
notarized build keeps working long-term (Developer ID provisioning profiles
are valid 18 years from creation; notarization tickets do not expire)
independent of Apple Developer Program membership status. Until then, other
AI-501DA owners can build and development-sign the project with their own
Apple Developer (Individual) team — see Build & Deploy above. Licensing in
this repository (see below) is already distribution-ready.

## License

- **Original code** — everything in the `TeacAI501DA/` Xcode project except
  the fragment noted below — is released under the **MIT License** (see
  `LICENSE`).
- **APSL-derived fragment**: `ApplyAssp8802Correction()` in
  `TeacAI501DA/TeacAI501DA/TeacAI501DA.cpp` is ported from TEAC USB HS Audio
  Driver 273.4.2 (`TeacUSBAudioStream.cpp`, "Modified by GFEC (TENOR)",
  Copyright (c) 1998-2010 Apple Computer, Inc.), which TEAC publishes under
  the **Apple Public Source License 2.0** (full text: `LICENSES/APSL-2.0.txt`).
  The bundled `ReadMe(GFEC TENOR).txt` in TEAC's source distribution states:
  *"You may obtain, copy, distribute and modify the source code for any
  portions of the product under Apple Public Source License."* The fragment is
  currently compiled for observation only (`ENABLE_ASSP8802_PLL == 0`); the
  active pacing path is original code.
- USB descriptors, VID/PID values and protocol behaviour are facts and not
  subject to copyright.

No warranty. Use at your own risk. **This is an unofficial community driver.
AI-501DA and TEAC are trademarks of TEAC Corporation; this project is not
affiliated with, endorsed by, or supported by TEAC.**
