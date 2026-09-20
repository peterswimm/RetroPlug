// The `linksync` CLI tool — RetroPlug as the host-side LSDj sync bridge for Chromatic hardware.
//
//   retroplug-cli linksync [--bpm n] [--divisor n] [--mode name] [--duration t]
//                          [--block-ms n] [--auto-start] [--out file]
//
// It runs the SAME lsdj-sync clock the plugin uses (LinkSyncBridge → walkTicks) and emits the resulting
// LSDj serial bytes as `rpsync <mode> <byte...>` command lines — the exact commands the Chromatic MCU
// firmware consumes to inject onto the Game Boy link (see the FPGA repo docs/retroplug-port + the MCU
// repo docs/retroplug-sync.md). Pipe the output to the console device to drive real hardware:
//
//   retroplug-cli linksync --bpm 120 --duration 4s --out sync.txt   # then: cat sync.txt > /dev/ttyACM0
//
// Tempo source: for this PoC the tempo is a fixed --bpm (deterministic, testable). A LIVE Ableton Link
// source that streams to /dev/ttyACM0 in real time needs native serial + Link-SDK adapters that the
// txiki CLI runtime doesn't have — documented as future work (docs/retroplug-port/ableton-link.md).

import type { CliTool } from "../tools";
import type { Session } from "../session";
import { LinkSyncBridge, LsdjSyncModeNum } from "./linksyncBridge.ts";

const MODE_BY_NAME: Record<string, number> = {
  off: LsdjSyncModeNum.Off,
  midisync: LsdjSyncModeNum.MidiSync,
  arduinoboy: LsdjSyncModeNum.MidiSyncArduinoboy,
  midisyncarduinoboy: LsdjSyncModeNum.MidiSyncArduinoboy,
};

export interface LinkSyncOpts {
  bpm: number;
  divisor: number;
  mode: number;
  durationMs: number;
  blockMs: number;
  autoStart: boolean;
  sampleRate: number;
  out?: string;
  adapter?: "chromatic" | "gblink";
  serial?: string;
  live?: boolean;
  dryRun?: boolean;
  lookaheadMs?: number;
}

const DEFAULTS: LinkSyncOpts = {
  bpm: 120,
  divisor: 1,
  mode: LsdjSyncModeNum.MidiSync,
  durationMs: 4000,
  blockMs: 20,
  autoStart: false,
  sampleRate: 44100,
  adapter: "chromatic",
  live: false,
  dryRun: false,
  lookaheadMs: 0,
};

/** "1500ms" | "2s" | "500" → milliseconds. */
function parseTime(v: string): number {
  const m = /^(\d+(?:\.\d+)?)(ms|s)?$/.exec(v.trim());
  if (!m) throw new Error(`linksync: bad time '${v}'`);
  const n = parseFloat(m[1]);
  return m[2] === "s" ? Math.round(n * 1000) : Math.round(n);
}

export function parseLinkSyncArgs(args: string[]): LinkSyncOpts {
  const o: LinkSyncOpts = { ...DEFAULTS };
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    const next = () => {
      if (i + 1 >= args.length) throw new Error(`linksync: ${a} needs a value`);
      return args[++i];
    };
    switch (a) {
      case "--bpm": o.bpm = parseFloat(next()); break;
      case "--divisor": o.divisor = parseInt(next(), 10); break;
      case "--mode": {
        const name = next().toLowerCase();
        if (!(name in MODE_BY_NAME)) throw new Error(`linksync: unknown --mode '${name}'`);
        o.mode = MODE_BY_NAME[name];
        break;
      }
      case "--duration": o.durationMs = parseTime(next()); break;
      case "--block-ms": o.blockMs = parseInt(next(), 10); break;
      case "--auto-start": o.autoStart = true; break;
      case "--sample-rate": o.sampleRate = parseInt(next(), 10); break;
      case "--out": o.out = next(); break;
      case "--adapter": {
        const adapter = next().toLowerCase();
        if (adapter !== "chromatic" && adapter !== "gblink") throw new Error(`linksync: unknown adapter '${adapter}'`);
        o.adapter = adapter;
        break;
      }
      case "--serial": o.serial = next(); break;
      case "--live": o.live = true; break;
      case "--dry-run": o.dryRun = true; break;
      case "--lookahead-ms": o.lookaheadMs = Math.max(0, parseInt(next(), 10)); break;
      default: throw new Error(`linksync: unknown arg '${a}'`);
    }
  }
  if (o.bpm <= 0) throw new Error("linksync: --bpm must be > 0");
  return o;
}

/** Format one FPGA sync command line: `rpsync <mode_hex> <byte_hex ...>` (matches the MCU console command). */
export function formatRpsync(mode: number, bytes: number[]): string {
  const hex = (n: number) => n.toString(16);
  return `rpsync ${hex(mode)} ${bytes.map(hex).join(" ")}`;
}

/**
 * Generate the full `rpsync` command script for a fixed-tempo run — pure and deterministic, so it is the
 * golden vector for the hardware sync stream. Walks the timeline in `blockMs` blocks (as the DSP kernel
 * would), advancing PPQ by tempo, and emits one command line per block that produced bytes.
 */
export function generateSyncScript(o: LinkSyncOpts): string[] {
  const bridge = new LinkSyncBridge();
  const framesPerBlock = Math.max(1, Math.round((o.sampleRate * o.blockMs) / 1000));
  const beatsPerBlock = framesPerBlock / ((o.sampleRate * 60) / o.bpm);
  const totalBlocks = Math.ceil((o.durationMs / 1000) * (o.sampleRate / framesPerBlock));

  const lines: string[] = [];
  let ppq = 0;
  for (let i = 0; i < totalBlocks; i++) {
    const block = {
      frames: framesPerBlock,
      sampleRate: o.sampleRate,
      tempo: o.bpm,
      ppqStart: ppq,
      transport: true,
    };
    const { events, pressStart } = bridge.processBlock(block, {
      mode: o.mode,
      tempoDivisor: o.divisor,
      autoStart: o.autoStart,
    });
    if (pressStart) lines.push("poke 8"); // GB Start bit (kButton_Start) — the MCU 'poke' command
    if (events.length > 0) lines.push(formatRpsync(o.mode, events.map((e) => e.byte)));
    ppq += beatsPerBlock;
  }
  return lines;
}

export function adapterBytes(adapter: "chromatic" | "gblink", mode: number, bytes: number[]): Uint8Array {
  if (adapter === "gblink") return Uint8Array.from(bytes);
  return new TextEncoder().encode(`${formatRpsync(mode, bytes)}\n`);
}

function runLiveLinkSync(o: LinkSyncOpts): void {
  if (!o.serial) throw new Error("linksync: --live requires --serial <port>");
  const adapter = o.adapter ?? "chromatic";
  if (adapter === "gblink" && o.autoStart)
    console.log("linksync: GBLink cannot press Start; --auto-start is ignored for this adapter");
  const call = makeRpcCall();
  const handle = call("serialOpenConfigured", o.serial, adapter === "gblink" ? 19200 : 115200, 8, "none", 1) as number;
  if (handle < 0) throw new Error(`cannot open serial port: ${o.serial}`);
  const port = {
    write: (bytes: Uint8Array) => call("serialWrite", handle, bytes) as number,
    read: (size: number, timeout: number) => (call("serialRead", handle, size, timeout) as Uint8Array | undefined) ?? new Uint8Array(),
    close: () => void call("serialClose", handle),
  };
  const bridge = new LinkSyncBridge();
  const frames = Math.max(1, Math.round((o.sampleRate * o.blockMs) / 1000));
  const beats = frames / ((o.sampleRate * 60) / o.bpm);
  const started = Date.now();
  let nextBlock = started;
  let ppq = 0;
  let sent = 0;
  let received = 0;
  const queue: { due: number; bytes: number[] }[] = [];
  (globalThis as { __rp_keepAlive?: () => void }).__rp_keepAlive?.();
  declareTimer(() => {
    const now = Date.now();
    while (now >= nextBlock && nextBlock - started < o.durationMs) {
      const result = bridge.processBlock({ frames, sampleRate: o.sampleRate, tempo: o.bpm, ppqStart: ppq, transport: true },
        { mode: o.mode, tempoDivisor: o.divisor, autoStart: !!o.autoStart && adapter === "chromatic" });
      if (result.pressStart) queue.push({ due: now + (o.lookaheadMs ?? 0), bytes: [] });
      if (result.events.length) queue.push({ due: now + (o.lookaheadMs ?? 0), bytes: result.events.map((e) => e.byte) });
      ppq += beats;
      nextBlock += o.blockMs;
    }
    while (queue.length && queue[0].due <= now) {
      const item = queue.shift()!;
      if (item.bytes.length === 0) {
        const poke = new TextEncoder().encode("poke 8\n");
        port.write(poke);
        continue;
      }
      if (adapter === "gblink") {
        for (const byte of item.bytes) {
          port.write(Uint8Array.of(byte));
          const reply = port.read(1, 100);
          if (reply.length !== 1) throw new Error("GBLink response timeout");
          received++;
          sent++;
        }
      } else {
        port.write(adapterBytes(adapter, o.mode, item.bytes));
        sent += item.bytes.length;
      }
    }
    if (now - started >= o.durationMs && queue.length === 0) {
      port.close();
      console.log(`linksync: sent ${sent} bytes${adapter === "gblink" ? `, received ${received}` : ""}`);
      exitCli(0);
    }
  }, 1);
  console.log(`linksync: ${adapter} live output on ${o.serial} (${o.bpm} BPM, ${o.lookaheadMs ?? 0} ms lookahead)`);
}

// txiki supplies setInterval; wrapping it keeps Node's type environment out of the CLI bundle.
function declareTimer(fn: () => void, ms: number): void {
  (globalThis as unknown as { setInterval(cb: () => void, delay: number): unknown }).setInterval(fn, ms);
}

function makeRpcCall(): (method: string, ...params: unknown[]) => unknown {
  type Send = (request: unknown) => { result?: unknown; error?: { code: number; message: string } } | undefined;
  const ns = (globalThis as Record<symbol, unknown>)[Symbol.for("plugin")] as { __rpcSend?: Send } | undefined;
  if (!ns?.__rpcSend) throw new Error("linksync: native serial RPC is unavailable");
  let id = 1;
  return (method, ...params) => {
    const reply = ns.__rpcSend!({ jsonrpc: "2.0", id: id++, method, params });
    if (reply?.error) throw new Error(`rpc ${method}: ${reply.error.message}`);
    return reply?.result;
  };
}

function exitCli(code: number): void {
  (globalThis as unknown as { tjs: { exit(code: number): void } }).tjs.exit(code);
}

function runLinkSync(s: Session, args: string[]): void {
  const o = parseLinkSyncArgs(args);
  if (o.live && !o.dryRun) { runLiveLinkSync(o); return; }
  const lines = generateSyncScript(o);
  const text = lines.join("\n") + (lines.length ? "\n" : "");

  if (o.out) {
    s.backend.writeFile(o.out, new TextEncoder().encode(text));
    console.log(`linksync: wrote ${lines.length} command lines → ${o.out}`);
    console.log(`  send to hardware: cat ${o.out} > /dev/ttyACM0`);
  } else {
    process.stdout ? process.stdout.write(text) : console.log(text);
  }
  exitCli(0);
}

const LINKSYNC_HELP = `retroplug-cli linksync — generate an LSDj sync command stream for Chromatic hardware

usage: retroplug-cli linksync [options]

  --bpm <n>          tempo (default 120)
  --divisor <n>      LSDj clock divisor 1/2/4/8 (default 1)
  --mode <name>      midiSync | arduinoboy (default midiSync)
  --duration <t>     length, e.g. 4s / 2000ms (default 4s)
  --block-ms <n>     block size in ms (default 20)
  --auto-start       tap Start on the transport rise (emits a 'poke' line)
  --sample-rate <hz> timeline sample rate (default 44100)
  --out <file>       write command lines to a file (else stdout)
  --adapter <name>   chromatic | gblink (default chromatic)
  --serial <port>    serial device for --live
  --lookahead-ms <n> constant live-output delay (default 0)
  --live              transmit in real time (never implied)
  --dry-run           force deterministic text output even with --live

Emits 'rpsync <mode> <byte...>' lines (and 'poke' for Start), the exact commands the
Chromatic MCU firmware consumes. Send them to the device console, e.g.:

  retroplug-cli linksync --bpm 120 --duration 4s --out sync.txt
  cat sync.txt > /dev/ttyACM0

The clock is the same walkTicks the plugin's lsdj-sync role uses, so the hardware stream
matches an in-plugin render by construction. A live Ableton Link → serial daemon needs
native adapters (see docs/retroplug-port/ableton-link.md).`;

export const linksyncTool: CliTool = {
  name: "linksync",
  summary: "generate an LSDj sync command stream for Chromatic hardware",
  help: LINKSYNC_HELP,
  longRunning: true,
  run: runLinkSync,
};
