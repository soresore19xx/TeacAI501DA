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
