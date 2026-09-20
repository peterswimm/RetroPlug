export type GameBoyLinkAdapter = "chromatic" | "gblink";

export interface GameBoyLinkConfig {
  ports: { port: string }[];
  selectedPort: string;
  adapter: GameBoyLinkAdapter;
  connected: boolean;
  enabled: boolean;
  lookaheadMs: number;
  linkMode: number;
  baudRate: number;
  dataBits: number;
  parity: string;
  stopBits: number;
  bytesSent: number;
  bytesReceived: number;
  dropped: number;
  errors: number;
  error: string;
}

type Globals = {
  __rp_getGameBoyLinkConfig?: () => Partial<GameBoyLinkConfig>;
  __rp_setGameBoyLinkPort?: (port: string) => void;
  __rp_setGameBoyLinkAdapter?: (adapter: GameBoyLinkAdapter) => void;
  __rp_connectGameBoyLink?: (enabled: boolean) => void;
  __rp_setGameBoyLinkLookahead?: (ms: number) => void;
  __rp_setGameBoyLinkMode?: (mode: number) => void;
  __rp_drainGameBoyLinkRx?: () => { system: number; byte: number }[];
};

const g = (): Globals => globalThis as Globals;

export function getGameBoyLinkConfig(): GameBoyLinkConfig | null {
  const fn = g().__rp_getGameBoyLinkConfig;
  if (!fn) return null;
  const c = fn() ?? {};
  return {
    ports: Array.isArray(c.ports) ? c.ports.filter((p) => typeof p?.port === "string") : [],
    selectedPort: typeof c.selectedPort === "string" ? c.selectedPort : "",
    adapter: c.adapter === "gblink" ? "gblink" : "chromatic",
    connected: !!c.connected,
    enabled: !!c.enabled,
    lookaheadMs: typeof c.lookaheadMs === "number" ? c.lookaheadMs : 10,
    linkMode: typeof c.linkMode === "number" ? c.linkMode : 1,
    baudRate: typeof c.baudRate === "number" ? c.baudRate : 115200,
    dataBits: typeof c.dataBits === "number" ? c.dataBits : 8,
    parity: typeof c.parity === "string" ? c.parity : "none",
    stopBits: typeof c.stopBits === "number" ? c.stopBits : 1,
    bytesSent: typeof c.bytesSent === "number" ? c.bytesSent : 0,
    bytesReceived: typeof c.bytesReceived === "number" ? c.bytesReceived : 0,
    dropped: typeof c.dropped === "number" ? c.dropped : 0,
    errors: typeof c.errors === "number" ? c.errors : 0,
    error: typeof c.error === "string" ? c.error : "",
  };
}

export const setGameBoyLinkPort = (v: string): void => g().__rp_setGameBoyLinkPort?.(v);
export const setGameBoyLinkAdapter = (v: GameBoyLinkAdapter): void => g().__rp_setGameBoyLinkAdapter?.(v);
export const connectGameBoyLink = (v: boolean): void => g().__rp_connectGameBoyLink?.(v);
export const setGameBoyLinkLookahead = (v: number): void => g().__rp_setGameBoyLinkLookahead?.(v);
export const setGameBoyLinkMode = (v: number): void => g().__rp_setGameBoyLinkMode?.(v);
export const drainGameBoyLinkRx = (): { system: number; byte: number }[] => g().__rp_drainGameBoyLinkRx?.() ?? [];
