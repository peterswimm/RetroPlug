# iOS/AUv3 handoff for Tommy

Branches:

- `codex/ios-auv3-engine` is the Engine-backed iOS/AUv3 port, based on
  `upstream/main` at `2d602f38`.
- `codex/ios-midi-config-parity` is the focused follow-up that restores the
  useful MIDI-editor configuration and nonblocking autosave behavior from the
  historical port. It can be merged as a branch or cherry-picked after the
  Engine-backed port.

Both are published to Peter's fork for review/cherry-picking. Nothing has been
signed, submitted to an app store, or released.

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

The parity follow-up makes the historical editor settings useful without
reviving its duplicate native MIDI engine: all 17 mode/channel assignments,
mGB base-channel remapping, and the four-voice MI.OUT CC mode/scaling/number
matrix are AU parameters whose values feed the canonical `mgb` and `lsdj-sync`
TypeScript roles. The defaults match the Arduinoboy editor/firmware, so these
features work immediately but remain host-automatable and state-restorable.
Note Out separately honors each voice's note and CC output channels.

Autosave now reads the Engine's tear-free published SRAM snapshot directly.
It does not enter QuickJS, suspend rendering, or take ownership away from the
audio thread; an opportunistic snapshot may be up to the normal publication
interval stale. Explicit save operations retain the canonical control-plane
path.

## Build and tests

```sh
pnpm install
(cd deps/dpf.js/deps/lv_binding_js && pnpm install)
./ios/build-native.sh
xcodegen generate --spec ios/project.yml --project ios
xcodebuild -project ios/RetroPlugIOS.xcodeproj -scheme RetroPlug \
  -destination 'generic/platform=iOS Simulator' -configuration Debug \
  CODE_SIGNING_ALLOWED=NO build
cmake --build build --target retroplug-apple-host-test -j8
./build/native/retroplug-apple-host-test
pnpm test
```

Verified on 2026-09-20, with the parity checks repeated on 2026-09-23:

- `retroplug-apple-host-test`: exit 0, including control-plane startup, mGB
  project round-trip, lifecycle suspend/resume, real per-channel audio, and an
  SRAM snapshot read while audio-thread ownership is active.
- `ios/build-native.sh`: exit 0 for arm64 device and simulator slices.
- unsigned arm64 Release `xcodebuild`: exit 0 for both generic iOS device and
  simulator builds, including the container app and embedded AUv3.
- XCTest lifecycle/state/bus/parameter test: executed and passed on an iPhone
  17 / iOS 27.0 arm64 simulator (not merely compiled). It exercises all 57 AU
  parameters, including mGB and MI.OUT setters.
- bare QuickJS startup now supplies the guarded UTF-8 Web globals required by
  the canonical control plane and reports rejected module promises with their
  JavaScript stack instead of a generic readiness failure.
- TypeScript suite: 145 test files passed.
- TypeScript MIDI golden coverage includes mGB channel/base remapping,
  per-mode input filtering, configured Master Sync output, and the firmware's
  MI.OUT note/CC routing and scaling matrix.
- Native suite: 117 test files passed (ROM-dependent cases skipped when the
  external ROM corpus was unavailable).
- Plugin binaries passed except `retroplug-watcher-test`; its FSEvents checks
  received no notifications from the `/private/tmp` worktree and failed again
  in isolation. The iOS target does not enable or link the watcher.
- UI suite passed 28/29 in parallel; the lone `new-project-guard` failure
  passed immediately when rerun alone with `TEST_JOBS=1`.
- archive audit: exit 0 against the built simulator application.

Physical-device execution and third-party host checks remain a publisher
release gate, not something represented as completed here. Sanitizer and
Reaper-host suites were not rerun for this handoff.

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
