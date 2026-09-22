# RetroPlug for iOS and AUv3

This directory contains a SwiftUI player and AUv3 instrument backed by the
same native `Engine`, `QueuedInvoker`, RPC services, TypeScript project store,
and migration path as the desktop build. Apple code owns lifecycle and
presentation; it does not own `SameBoySystem` or define a second `.rplg`
schema.

The native iOS target is SameBoy-only. Mesen sources and GPL console backends
are excluded from the target. The QuickJS control-plane bundle is generated at
build time and embedded in `RetroPlugNative`; executable scripts are never
downloaded at runtime.

## Build

Prerequisites: Xcode, CMake, Ninja, pnpm, xcodegen, RGBDS, and initialized git
submodules. Install the root and nested `lv_binding_js` pnpm workspaces first.

```sh
pnpm install
(cd deps/dpf.js/deps/lv_binding_js && pnpm install)
./ios/build-native.sh
xcodegen generate --spec ios/project.yml --project ios
xcodebuild -project ios/RetroPlugIOS.xcodeproj -scheme RetroPlug \
  -destination 'generic/platform=iOS Simulator' -configuration Debug \
  CODE_SIGNING_ALLOWED=NO build
```

`build-native.sh` builds device and simulator slices and writes
`ios/.build/RetroPlugNative.xcframework`. It deliberately passes
`RETROPLUG_EMBED_MGB=OFF`. For a personal development build only, set
`RETROPLUG_EMBED_MGB=1`; do not distribute that artifact without compatible
ROM redistribution permission.

The generated project uses the valid placeholder prefix `org.example`. Tommy must choose the
product name, bundle identifiers, App Group identifier, signing team, and AU
manufacturer/subtype before release. To enable shared storage, add a matching
App Group entitlement to both app and extension and put its identifier in the
`RetroPlugAppGroupIdentifier` Info.plist key.

## Architecture

- `AppleEngineHost` adapts Apple audio/lifecycle calls to the shared Engine and
  canonical TypeScript control plane.
- `AppleQuickJsHost` is the small in-process QuickJS runtime used by app
  extensions; it avoids txiki process, socket, and FFI modules.
- `RetroPlugEngineAudioUnit.mm` exposes the mix plus Pulse 1, Pulse 2, Wave,
  and Noise stereo busses, AU MIDI input/output, and canonical `.rplg` data in
  `fullState`.
- `SharedProjectStore` optionally mirrors the canonical state into an App Group.
- The opt-in SameBoy APU register callback backs Note Out. It is disabled when
  the mode is inactive.

The player retains ROM import, battery saves, savestates, LSDj song management,
touch/controller input, MIDI modes, and video display. App Store builds require
users to import their own ROMs.

## Verification

The portable Apple-host smoke test is:

```sh
cmake --build build --target retroplug-apple-host-test -j8
./build/native/retroplug-apple-host-test
```

Before release, build both simulator and physical-device configurations, test
state restoration and multi-output/MIDI in GarageBand, Logic, AUM, and Cubasis,
then audit the archive:

```sh
./ios/audit-archive.sh /path/to/RetroPlug.xcarchive
```

See `docs/handoff-ios-auv3.md` for the remaining publisher and hardware checks.
