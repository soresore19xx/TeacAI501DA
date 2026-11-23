# Design notes

Why this driver exists, what the device does wrong, and how the driver
works around it. For installing or building, see the
[README](README.md).

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
APSL 2.0; its source was obtained officially from TEAC, see the README's
*Source provenance*) hard-codes a slow-PLL correction for VID `0x0644`, and
Linux ships
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
  the rate. Getting any of these wrong is audible. A write that does not
  continue from the previous write end (the player stopped and restarted)
  clears the ring between the two before the new end is published;
  otherwise the stretch below the first new write, which CoreAudio skips
  because of the safety offset, still holds audio from one ring length
  earlier and is played (fixed in v66–v68).
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
- **Mute value returned as a clean 0/1.** CoreAudio reads a boolean control
  as a `UInt32`. AudioDriverKit's `IOUserAudioBooleanControl` fills only the
  low byte and leaves the other three as leftover memory, so the value is
  almost never exactly 0 or 1. Seen on every Mac tried (Intel with macOS
  12.7.6, 15.8.3 and 26.7.3; Apple A18 Pro, M4 and M4 Max with 27.2.1); one
  earlier test on the M4 Max happened to read clean values. coreaudiod then saves the wrong state and
  re-applies mute after a rate change or a reconnect, and `get volume
  settings` reports the state wrongly. Both mute controls
  (`TeacAI501DAMuteControl`, `TeacUD501MuteControl`) override
  `_GetPropertyData`, call the base, and for `kAudioBooleanControlPropertyValue`
  (`'bcvl'`) replace the result with the low byte as 0 or 1. The method is
  declared in a private class extension that iig does not see, so it is
  declared `LOCALONLY` without `override` in the `.iig`; the generated header
  still emits it as an override. The first corrected read per control is
  logged once. Where the base already returns a clean value, nothing is
  replaced.

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

## Layout

```
TeacAI501DA/                     Xcode project (dext + container app)
├── TeacAI501DA/
│   ├── TeacAI501DA.{iig,cpp}              driver
│   ├── TeacAI501DADevice.{iig,cpp}        IOUserAudioDevice subclass, rate changes
│   ├── TeacAI501DAVolumeControl.{iig,cpp} software volume
│   ├── TeacAI501DAMuteControl.{iig,cpp}   software mute
│   ├── TeacUD501*.{iig,cpp}               UD-501 driver, device, volume, mute
│   ├── TeacAI501DA.entitlements
│   └── Info.plist                         IOKitPersonalities, IOUSBHostDevice match
└── TeacAI501DAInstaller/        container app: install, update, uninstall

ai501da_lsusb.txt                full USB descriptor dump
ud501v2_lsusb.txt                UD-501 (upgraded) USB descriptor dump (serial removed)
```

## UD-501 (upgraded units)

A second personality (`0x0644` / `0x8043`, `IOPropertyMatch` on the USB
product name `UD-501V2`) starts the `TeacUD501` class from the same dext.
It follows the AI-501DA design — device-level ownership, pipelined OUT paced
by explicit feedback, zero timestamps from completed samples, silence fill,
and the quiesced `Stop` — re-done for a different device:

- The USB side is a TI C67x DSP running TEAC firmware, not the TE8802L. Its
  feedback is spec-conformant Q16.16 per microframe (exactly nominal at every
  rate measured), so it is used as-is; a ±1.5% band still guards against
  malformed values, and a "+1.0" encoding is accepted only if it is the one
  that lands in band.
- Topology: AudioControl IF 0 with clock source 41, streaming IF 1 alt 2
  (24-bit samples MSB-aligned in 4-byte subslots), OUT EP 0x01, feedback EP
  0x81. The CoreAudio ring stays packed 24-bit; the copy expands each sample
  into its 4-byte slot.
- Following Linux's quirks for this USB ID, the start and every rate switch
  go alt 0 → vendor PCM-mode request (`0x41`, request 0, value 0, index 1) →
  clock `SET_CUR` → alt 2, with 20 ms after control requests and 50 ms after
  interface changes for the DSP to settle.
- Volume is a software gain: the unit's Feature Unit accepts volume
  requests but the level does not change, so there is no hardware volume
  to drive. The gain writes 32-bit results into the 4-byte subslot, but
  alt 2 declares 24-bit resolution, so the effective precision is 24 bits
  (the low byte is most likely ignored by the unit). Alt 3 declares 32-bit
  and is untested for PCM. Chunks with a run of 16 DoP marker frames skip the
  gain; the decision is per chunk with no hold-over, so PCM following DoP at
  the same rate is attenuated from the next millisecond.
- Ring and zero-timestamp period are 49152 frames, 128 ms at 384 kHz (98304
  up to v56; with 98304, CoreAudio waited 2.05 s at 96 kHz and 4.1 s at
  48 kHz after a rate change before it ran IO again, half that with 49152).
- Adding the UD-501 did not touch the AI-501DA sources (the later mute fix
  changed `TeacAI501DAMuteControl` for both devices, and the restart fix of
  v66–v68 changed the IO handler of both), and every helper of `TeacUD501`
  lives in an anonymous namespace so that no same-named symbol can be
  coalesced with the AI-501DA translation unit at link time.

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
