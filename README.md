# TeacAI501DA

A macOS DriverKit driver extension for the **TEAC AI-501DA** USB DAC
(VID `0x0644` / PID `0x8038`), which the generic system USB audio driver
cannot play reliably.

Working and in daily use on macOS 26.5 (Tahoe): all six rates
44.1–192 kHz at 24-bit, live rate switching, an 8-hour continuous soak at
192 kHz with zero errors, and a teardown path crash-tested against removing
the device's AC power mid-stream. Current releases are built with Xcode 27.2
and checked on macOS 27.2.1 (Apple Silicon) and macOS 12.7.6 (Intel): playback
at 44.1, 96 and 192 kHz, live rate switching, volume control, and the underrun
silence fill.

**Based on driver source code officially obtained from TEAC**: the source of
TEAC USB HS Audio Driver 273.4.2, TEAC's own macOS driver for this device,
which TEAC provides through its open-source software request form under the
Apple Public Source License 2.0. See [Source provenance](#source-provenance).

Personal, non-commercial project. Unofficial: not affiliated with, endorsed
by, or supported by TEAC.

## Install

Download the notarized disk image from
[Releases](https://github.com/soresore19xx/TeacAI501DA/releases), drag the app
into `/Applications`, and launch it. It asks whether to install or uninstall
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
later; verified on macOS 27.2 and 26.5 on Apple Silicon and on macOS 12.7.6 on
an Intel MacBook Air (Early 2015). Other combinations should work but are
untested — reports welcome.

Two flavors are published:

- **Volume** (`*-volume-*.dmg`): volume and mute controls are exposed to
  macOS as a software gain; at 0 dB the gain path is bypassed for
  bit-perfect passthrough.
- **Fixed-volume** (`*-fixed-*.dmg`): no controls are exposed and no gain
  path is compiled in, so output is always full-scale passthrough. The
  device has no hardware volume reachable over USB, so use the amplifier's
  knob or remote.

macOS replaces a driver extension only when `CFBundleVersion` increases, so
switching to a flavor with a lower version number needs an uninstall first.

Building from source with your own Apple Developer (Individual) team works
too — see [Build](#build).

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

For the fixed-volume flavor, add
`GCC_PREPROCESSOR_DEFINITIONS='TEAC_FIXED_VOLUME=1'` to the `xcodebuild` line.
The Release configuration defines nothing else there, so the override loses
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
