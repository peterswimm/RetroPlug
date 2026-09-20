# LinkSync/UART handoff

Branch: `codex/linksync-uart`, based on `upstream/main` at `2d602f38`.

## What changed

- The historical deterministic LinkSync command generator is forward-ported and shares `walkTicks` with
  the DSP kernel.
- `linksync --live` supports Chromatic command framing and GBLink raw 19200/8N1 exchange. Live device I/O
  is never implicit.
- The desktop/plugin Engine exposes a system-addressed serial-link observer. `GameBoyLink` moves all UART
  operations to a bounded worker queue and retains sample offsets, counters, and returned GBLink bytes.
- Desktop controls persist adapter, port, lookahead, enable state, and Chromatic link mode in `gblink.cfg`.

## Rev 1 limits

- GBLink is a serial-master device. Host-driven modes work; LSDj MI.OUT/MasterSync does not.
- GBLink cannot press Start, so auto-start is Chromatic-only.
- Returned GBLink bytes are diagnostic and are not routed into an emulator or MIDI decoder.
- The tempo source is fixed BPM or host/DAW transport. An Ableton Link SDK adapter is not included.

## Verification performed

- `./build.sh --tests` — exit 0; CLI, SDL app, plugins, macOS AU, and shared libraries compiled.
- LinkSync Node tests — 11/11 passed.
- `retroplug-gblink-test` — 16 assertions in 3 cases passed.
- `pnpm test` — all 145 TypeScript test files passed.

Hardware verification remains: sustained timing on real Chromatic and GBLink adapters, unplug/reconnect,
and multi-system port selection. No remote was pushed and no release artifact was published.

## Review notes

The working tree's modified `deps/sameboy` is the expected configure-time per-channel patch. No submodule
pointer is part of this branch. Review the commits in order: shared clock, configured CLI UART, native
desktop bridge, then this handoff.
