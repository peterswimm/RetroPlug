# linksync — RetroPlug as a hardware LSDj sync bridge

`retroplug-cli linksync` turns RetroPlug into the host-side tempo brain for a ModRetro Chromatic or
[vaguilar/GBLink](https://github.com/vaguilar/gblink). It runs the **same** `lsdj-sync` clock the plugin uses
and streams the resulting LSDj serial bytes to the console as commands the Chromatic firmware
injects onto the Game Boy link — so a DAW / Ableton Link session drives LSDj tempo on real
hardware, the way RetroPlug drives an emulated Game Boy in a DAW.

This is the host half of the RetroPlug → Chromatic port. See the FPGA repo's
`docs/retroplug-port/` and the MCU repo's `docs/retroplug-sync.md` for the device halves.

## How it reuses the plugin's clock

The drift-exact 24-PPQN tick walker `walkTicks` was extracted to
[`src/ppqClock.ts`](../packages/retroplug/src/ppqClock.ts) (dependency-free) and is imported by
both the DSP kernel's `lsdj-sync` role ([`dspKernel.ts`](../packages/retroplug/src/dspKernel.ts) /
[`dspRoles.ts`](../packages/retroplug/src/dspRoles.ts)) and the bridge
([`cli/sessions/linksyncBridge.ts`](../packages/retroplug/cli/sessions/linksyncBridge.ts)). Because
the clock is one source of truth, the hardware byte stream matches an in-plugin render **by
construction** — the plugin's sync tests are the bridge's golden vector.

## Usage

```sh
retroplug-cli linksync --bpm 120 --duration 4s --out sync.txt
retroplug-cli linksync --bpm 120 --duration 4s --adapter chromatic --serial /dev/ttyACM0 --live
retroplug-cli linksync --bpm 120 --duration 4s --adapter gblink --serial /dev/tty.usbserial-123 --live
```

Flags: `--bpm`, `--divisor` (1/2/4/8), `--mode` (midiSync | arduinoboy), `--duration`,
`--block-ms`, `--auto-start`, `--sample-rate`, `--out`, `--adapter`, `--serial`, `--lookahead-ms`,
`--live`, and `--dry-run`. Without explicit `--live` it emits `rpsync <mode> <byte…>` lines
(and a `poke` for Start when `--auto-start` arms a SYNC=MIDI cart) — the exact console commands
the Chromatic MCU firmware consumes.

Chromatic uses its textual command protocol. GBLink opens at 19200/8N1 and exchanges exactly one raw
byte in each direction. Returned Game Boy bytes are counted and exposed for diagnostics, but rev 1 does
not decode them. Because GBLink is always serial master, rev 1 supports host-driven modes only and cannot
drive LSDj MI.OUT/MasterSync or synthesize the Start button. `--auto-start` is ignored for GBLink.

## Desktop app and plugin

The Game Boy Link menu selects the adapter, serial port, link mode, and lookahead. Its configuration is
stored in `gblink.cfg`; transient system IDs are not persisted. The audio thread only enqueues
system-addressed bytes. A dedicated worker owns the UART, schedules their intra-block offsets, and records
sent/received/drop/error counters. No serial I/O runs on the audio thread.

## Ableton Link tempo source (future)

Live UART output currently follows the command's fixed `--bpm`. Following an Ableton Link session remains
future work; it requires a Link-SDK tempo-source adapter, not changes to the shared clock or UART framing.

## Tests

The bridge core is pure and Node-runnable (no build required):

```sh
node --test packages/retroplug/cli/sessions/linksyncBridge.test.ts \
            packages/retroplug/cli/sessions/linksync.test.ts
node scripts/cmake-build.js retroplug-gblink-test
build/bin/retroplug-gblink-test
```
