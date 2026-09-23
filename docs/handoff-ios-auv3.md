# iOS/AUv3 handoff for Tommy

Branch: `codex/ios-auv3-engine`, based on `upstream/main` at `2d602f38`.
Nothing from this branch has been pushed, signed, submitted, or published.

## Purpose and architecture

The historical iOS player/AUv3 work is forward-ported onto the current shared
`Engine`, `QueuedInvoker`, native RPC services, and TypeScript control plane.
SwiftUI owns Apple lifecycle and presentation. `AppleEngineHost` owns the
engine; Apple code never owns a `SameBoySystem` directly. Canonical `.rplg`
bytes are stored in AU `fullState` and may be mirrored to an App Group selected
by the publisher.

The AU exposes five stereo output busses (mix plus four Game Boy voices),
timestamped MIDI input/output, project restoration, ROM/SRAM/state operations,
and the player workflows. Note Out uses an opt-in SameBoy APU register-write
callback. The iOS native target registers SameBoy only and does not link Mesen.
The control-plane bytecode is embedded in the binary.

## Build and tests

```sh
pnpm install
(cd deps/dpf.js/deps/lv_binding_js && pnpm install)
./ios/build-native.sh
xcodegen generate --spec ios/project.yml --project ios
xcodebuild -project ios/RetroPlugIOS.xcodeproj -scheme RetroPlug \
  -sdk iphonesimulator -configuration Debug CODE_SIGNING_ALLOWED=NO build
cmake --build build --target retroplug-apple-host-test -j8
./build/native/retroplug-apple-host-test
pnpm test
```

Automated checks on this branch cover control-plane startup, project state
round-tripping, lifecycle suspend/resume, multi-bus rendering, and the existing
TypeScript/native regressions. Hardware and third-party host checks remain a
publisher release gate, not something represented as completed here.

## Distribution and licensing

- `RETROPLUG_EMBED_MGB` defaults on for existing desktop/development builds but
  is forced off by the iOS distribution build. Users import their own ROMs.
- `ios/audit-archive.sh` rejects ROM files, Mesen symbols/code, and known GPL
  console-backend markers in an `.xcarchive`.
- `ios/THIRD_PARTY_NOTICES.md` lists the native dependencies that need review.
- Do not ship mGB or another third-party ROM unless compatible redistribution
  permission is obtained and documented.
- The project intentionally contains placeholder bundle identifiers and no
  signing identity. Product naming, RetroPlug branding, App Group ID, AU
  manufacturer/subtype, signing, store metadata, and final licensing approval
  belong to Tommy.

## Known limitations and release checklist

- Validate on physical iPhone/iPad hardware and test state restoration after
  closing/reopening GarageBand, Logic, AUM, and Cubasis.
- Exercise every MIDI mode, Note Out, each stem bus, SRAM/savestate handling,
  and LSDj song management with user-supplied ROMs.
- Configure matching App Group entitlements for app and extension if shared
  library storage is desired.
- Build a Release archive, run `./ios/audit-archive.sh <archive>`, inspect linked
  libraries manually, and review all third-party notices.
- Choose publisher-controlled identifiers and signing, then run App Store
  validation. No release approval is implied by this engineering handoff.
