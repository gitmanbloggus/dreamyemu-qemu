/* hd2: -M htcleo (cLK/MAGLDR/WinMo NAND); dream: -M dream (HTC Dream, SPL + NAND) */
export type Device = "hd2" | "dream";
export type Mode = "hle" | "lle";
export type BootTarget = "nand" | "recovery" | "menu";
export type AudioRoute = "browser" | "host" | "both" | "off";
export type GpuBackend = "gl" | "soft";
export type VideoEncoder = "x264" | "nvenc";
export type Cable = "usb" | "ac" | "none";

export type EmulatorConfig = {
  device: Device;
  baseband: Mode;
  dsp: Mode;
  usb: Mode;
  nand: string;
  sdcard: string;
  boot: BootTarget;
  gpu: GpuBackend;
  audio: AudioRoute;
  wifi: boolean;
  mobileData: boolean;
  adbPort: number;
  amssLog: boolean;
  dspShift: number;
  extraArgs: string;
  encoder: VideoEncoder;
  quality: number;
  maxFps: number;
};

export const DEFAULT_CONFIG: EmulatorConfig = {
  device: "hd2",
  baseband: "hle",
  dsp: "hle",
  usb: "hle",
  nand: "nand.img",
  sdcard: "sdcard.img",
  boot: "nand",
  gpu: "gl",
  audio: "browser",
  wifi: true,
  mobileData: true,
  adbPort: 5555,
  amssLog: false,
  dspShift: -1,
  extraArgs: "",
  encoder: "x264",
  quality: 20,
  maxFps: 60,
};

export type PowerState = "stopped" | "starting" | "running" | "stopping" | "crashed";

/* os: what a NAND image boots (a stock Windows Mobile ROM, or Android etc. through cLK/MAGLDR) */
export type NandOs = "winmo" | "other";
export type ImageFile = { name: string; size: number; kind: "nand" | "sdcard" | "firmware"; os?: NandOs; device?: Device };

/*
 * What the machine can actually do.  `vhci` is not firmware: it says the
 * kernel's virtual host controller is there *and* that we may claim a port,
 * which is what usb=lle needs.  vhci_hcd's attach attribute is root-only and
 * the kernel resolves the socket fd against the process that writes it, so
 * this is false whenever QEMU is not running as root.
 */
export type Firmware = { amss: boolean; dsp1: boolean; arm: boolean; hexagon: boolean; vhci: boolean; spl: boolean };

export type EmulatorStatus = {
  state: PowerState;
  since: number;
  /* the server's clock when it sent this (since is on that clock) */
  now: number;
  message: string;
  external: boolean;
  running: EmulatorConfig | null;
  pids: { arm?: number; hexagon?: number };
  stream: { fps: number; kbps: number; clients: number; width: number; height: number };
  sensors: { battery: number; cable: Cable };
};

export type ServerState = {
  config: EmulatorConfig;
  images: ImageFile[];
  firmware: Firmware;
  status: EmulatorStatus;
};

export type HardKey = "volup" | "voldown" | "menu" | "call" | "end" | "back" | "home" | "power" | "trackball";

export const HARD_KEY_QCODE: Record<HardKey, string> = {
  volup: "pgup",
  voldown: "pgdn",
  menu: "f1",
  call: "f2",
  end: "f3",
  back: "esc",
  home: "home",
  power: "f12",
  trackball: "f10",
};

/* -M dream (docs/dream.md): the red End key is the power key, F12 or End */
export const DREAM_KEY_QCODE: Partial<Record<HardKey, string>> = { end: "end" };

export const hardKeyQcode = (device: Device, k: HardKey) =>
  (device === "dream" ? DREAM_KEY_QCODE[k] : undefined) ?? HARD_KEY_QCODE[k];

/*
 * The Dream has a QWERTY keypad, so the browser's keys go to it as QEMU
 * qcodes (KeyboardEvent.code -> qcode); the HD2 types through adb instead.
 */
export const codeToQcode = (code: string): string | null => {
  let m = /^Key([A-Z])$/.exec(code);
  if (m) return m[1].toLowerCase();
  m = /^Digit([0-9])$/.exec(code);
  if (m) return m[1];
  const t: Record<string, string> = {
    Space: "spc", Enter: "ret", Backspace: "backspace", Tab: "tab",
    ShiftLeft: "shift", ShiftRight: "shift_r", AltLeft: "alt", AltRight: "alt_r",
    Comma: "comma", Period: "dot", Slash: "slash", Semicolon: "semicolon", Quote: "apostrophe",
    Minus: "minus", Equal: "equal", BracketLeft: "bracket_left", BracketRight: "bracket_right",
    ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right",
    F8: "f8", F9: "f9", F10: "f10", F11: "f11",
  };
  return t[code] ?? null;
};

export type InputMessage =
  | { t: "touch"; a: "down" | "move" | "up"; x: number; y: number }
  | { t: "key"; k: HardKey; d: boolean }
  | { t: "qkey"; q: string; d: boolean };

export type LogName = "uart1" | "ramcon" | "qemu" | "q6";
