// The two roles we're migrating off C++, driven through the DSP kernel end-to-end (pure TS, no
// backend). mGB forwards routed MIDI bytes to serial; lsdj-sync's MidiSync mode emits a 24-PPQN
// 0xF8 clock and Off emits nothing — the doc-06 "translators are scripts" shape as plain TS.
import { test, expect } from "../../testing/harness";
import { RoleRegistry } from "../../src/systemRoles";
import { registerDspRoles } from "../../src/dspRoles";
import { DspKernel, type BlockInput } from "../../src/dspKernel";

function kernel(): DspKernel {
  const reg = new RoleRegistry();
  registerDspRoles(reg);
  return new DspKernel(reg);
}

// 22050 frames @ 44100/120 = exactly 1 beat (24 ticks at 24 PPQN). The dynamic per-block input;
// structure is pushed separately via setSystems.
const baseDyn = (): BlockInput => ({
  frames: 22050,
  sampleRate: 44100,
  tempo: 120,
  ppqStart: 0,
  transport: false,
  midiIn: [],
  buttons: [],
  keys: [],
  serialOut: [],
});

test("mgb forwards each routed MIDI byte to its system's serial", () => {
  const k = kernel();
  k.setSystems({
    project: [{ kind: "midi-routing", config: { mode: "sendToAll" } }], // SendToAll
    systems: [{ id: 1, pipeline: [{ kind: "mgb", config: {} }] }],
  });
  const out = k.processBlock({ ...baseDyn(), midiIn: [{ frame: 0, data: [0x90, 60, 100] }] });
  expect(out.serialIn).toEqual([
    { system: 1, frame: 0, byte: 0x90 },
    { system: 1, frame: 0, byte: 60 },
    { system: 1, frame: 0, byte: 100 },
  ]);
});

test("mgb remaps configured channels to its five fixed voices and drops unassigned channels", () => {
  const k = kernel();
  k.setSystems({
    project: [{ kind: "midi-routing", config: { mode: "sendToAll" } }],
    systems: [{ id: 1, pipeline: [{ kind: "mgb", config: { channels: [6, 7, 8, 9, 10] } }] }],
  });
  const out = k.processBlock({ ...baseDyn(), midiIn: [
    { frame: 3, data: [0x96, 61, 99] }, // configured channel 7 -> mGB voice/channel 2
    { frame: 4, data: [0x90, 62, 99] }, // unassigned channel 1 -> dropped
    { frame: 5, data: [0xf8] },          // system realtime -> unchanged
  ] });
  expect(out.serialIn.map((event) => [event.frame, event.byte])).toEqual([
    [3, 0x91], [3, 61], [3, 99], [5, 0xf8],
  ]);
});

test("mgb baseChannel assigns a contiguous five-channel block", () => {
  const k = kernel();
  k.setSystems({
    project: [{ kind: "midi-routing", config: { mode: "sendToAll" } }],
    systems: [{ id: 1, pipeline: [{ kind: "mgb", config: { baseChannel: 9 } }] }],
  });
  const out = k.processBlock({ ...baseDyn(), midiIn: [
    { frame: 0, data: [0x9a, 64, 100] }, // input channel 11 -> voice 3 / status channel 2
  ] });
  expect(out.serialIn.map((event) => event.byte)).toEqual([0x92, 64, 100]);
});

test("nes-n8-midi forwards each routed MIDI message to its system's core (emitCoreMidi)", () => {
  const k = kernel();
  k.setSystems({
    project: [{ kind: "midi-routing", config: { mode: "sendToAll" } }], // SendToAll
    systems: [{ id: 1, pipeline: [{ kind: "nes-n8-midi", config: {} }] }],
  });
  const out = k.processBlock({ ...baseDyn(), midiIn: [{ frame: 0, data: [0x90, 60, 100] }] });
  // The whole message crosses as one coreMidi entry (native onMidi fans the bytes into the N8 FIFO).
  expect(out.coreMidi).toEqual([{ system: 1, frame: 0, data: [0x90, 60, 100] }]);
  expect(out.serialIn.length).toBe(0); // NES has no serial port — nothing on the mGB path
});

test("lsdj-sync MidiSync emits a 24-PPQN 0xF8 clock; Off emits nothing", () => {
  const run = (mode: string) => {
    const k = kernel();
    k.setSystems({ systems: [{ id: 1, pipeline: [{ kind: "lsdj-sync", config: { mode } }] }] });
    return k.processBlock({ ...baseDyn(), transport: true });
  };

  const on = run("midiSync"); // MidiSync
  expect(on.serialIn.length).toBe(24);
  expect(on.serialIn.every((s) => s.system === 1 && s.byte === 0xf8)).toBeTruthy();

  expect(run("off").serialIn.length).toBe(0); // Off
});
