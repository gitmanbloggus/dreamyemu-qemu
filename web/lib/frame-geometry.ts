import type { Device, HardKey } from "./emulator-types";

/* Positions in public/hd2-frame.webp (1165 x 2045), measured on the photo. */
export const FRAME = { w: 1165, h: 2045 };
export const SCREEN = { x: 110, y: 161.75, w: 941, h: 1568 };

export type Hotspot = { key: HardKey; x: number; y: number; w: number; h: number; label: string; side?: boolean };

export const HOTSPOTS: Hotspot[] = [
  { key: "call", x: 140, y: 1866, w: 160, h: 86, label: "Call" },
  { key: "home", x: 306, y: 1866, w: 164, h: 86, label: "Home" },
  { key: "menu", x: 478, y: 1866, w: 172, h: 86, label: "Windows (Menu)" },
  { key: "back", x: 650, y: 1866, w: 190, h: 86, label: "Back" },
  { key: "end", x: 856, y: 1866, w: 154, h: 86, label: "End call" },
  { key: "volup", x: 0, y: 573, w: 52, h: 196, label: "Volume up", side: true },
  { key: "voldown", x: 0, y: 769, w: 52, h: 196, label: "Volume down", side: true },
];

/* The power key sits on the top edge, at the right, so the front photo cannot show it. */
export const POWER = { x: 820, y: 2, w: 150, h: 30 };

export const pct = (v: number, of: number) => `${(v / of) * 100}%`;

/*
 * The Dream has no photo: DreamFrame draws the body.  Units are screen
 * pixels (320 x 480).  Below the screen, as on the phone: Call, Home, the
 * trackball, Back, End, with Menu above the trackball; volume on the left.
 */
export const DREAM_FRAME = { w: 400, h: 720 };
export const DREAM_SCREEN = { x: 40, y: 56, w: 320, h: 480 };
export const DREAM_HOTSPOTS: Hotspot[] = [
  { key: "call", x: 30, y: 600, w: 62, h: 50, label: "Call (F2)" },
  { key: "home", x: 100, y: 600, w: 62, h: 50, label: "Home" },
  { key: "menu", x: 170, y: 552, w: 60, h: 30, label: "Menu (F1)" },
  { key: "trackball", x: 176, y: 594, w: 48, h: 48, label: "Trackball: press F10, roll with the arrow keys" },
  { key: "back", x: 238, y: 600, w: 62, h: 50, label: "Back (Esc)" },
  { key: "end", x: 308, y: 600, w: 62, h: 50, label: "End / power (F12)" },
  { key: "volup", x: 0, y: 150, w: 14, h: 56, label: "Volume up", side: true },
  { key: "voldown", x: 0, y: 210, w: 14, h: 56, label: "Volume down", side: true },
];

/*
 * Landscape: the portrait frame turned 90 degrees counter-clockwise (the
 * button chin on the right, as the phone is held with the keyboard out),
 * and the keyboard drawn under it.  Units as above.
 */
export const DREAM_KEYBOARD = { x: 14, w: 560, h: 250 };
/*
 * With the keyboard out the screen slab slides away from the chin: the
 * slab is the portrait frame down to SLAB_H (no chin), and the chin stays
 * with the keyboard half, at its right, rising past the slab's lower edge
 * as on the phone.  Landscape units; keys are chin-local.
 */
export const DREAM_SLAB_H = 548;
export const DREAM_KBD_BODY = { x: 14, y: 380, w: 546, h: 270 };
export const DREAM_CHIN = { x: 560, y: 222, w: 160, h: 428 };
export const DREAM_CHIN_KEYS: Hotspot[] = [
  { key: "end", x: 68, y: 22, w: 56, h: 64, label: "End / power (F12)" },
  { key: "back", x: 68, y: 96, w: 56, h: 64, label: "Back (Esc)" },
  { key: "trackball", x: 72, y: 178, w: 48, h: 48, label: "Trackball: press F10, roll with the arrow keys" },
  { key: "menu", x: 18, y: 166, w: 32, h: 72, label: "Menu (F1)" },
  { key: "home", x: 68, y: 244, w: 56, h: 64, label: "Home" },
  { key: "call", x: 68, y: 318, w: 56, h: 64, label: "Call (F2)" },
];

/** The whole drawn phone's box, in frame units */
export const boxFor = (device: Device, landscape: boolean, keyboard: boolean) => {
  const f = geometryFor(device).frame;
  if (device !== "dream" || !landscape) return { w: f.w, h: f.h };
  return { w: f.h, h: f.w + (keyboard ? DREAM_KEYBOARD.h : 0) };
};

export const geometryFor = (device: Device) =>
  device === "dream"
    ? { frame: DREAM_FRAME, screen: DREAM_SCREEN, native: { w: 320, h: 480 } }
    : { frame: FRAME, screen: SCREEN, native: { w: 480, h: 800 } };
