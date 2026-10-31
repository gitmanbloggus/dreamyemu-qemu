"use client";

import type { InputMessage } from "@/lib/emulator-types";

/*
 * The Dream's slide-out QWERTY keyboard, laid out as on the phone: five
 * rows, MENU at the start of the fourth, DEL closing the third, and the
 * bottom row Shift, Alt, Search, @, Space, the ./ key, Alt, Shift.  Each
 * key sends the qcode -M dream maps to its trout-keypad-v3 scancode
 * (Search = compose, KEY 127; @ = mail, KEY 215; Menu = F1).
 */
type Key = { q: string; label: string; alt?: string; w?: number; tone?: "green" };

const ROWS: Key[][] = [
  [
    { q: "1", label: "1", alt: "!" }, { q: "2", label: "2", alt: "@" }, { q: "3", label: "3", alt: "#" },
    { q: "4", label: "4", alt: "$" }, { q: "5", label: "5", alt: "%" }, { q: "6", label: "6", alt: "^" },
    { q: "7", label: "7", alt: "&" }, { q: "8", label: "8", alt: "*" }, { q: "9", label: "9", alt: "(" },
    { q: "0", label: "0", alt: ")" },
  ],
  [
    { q: "q", label: "Q" }, { q: "w", label: "W" }, { q: "e", label: "E" }, { q: "r", label: "R" }, { q: "t", label: "T" },
    { q: "y", label: "Y" }, { q: "u", label: "U" }, { q: "i", label: "I" }, { q: "o", label: "O" }, { q: "p", label: "P" },
  ],
  [
    { q: "a", label: "A" }, { q: "s", label: "S" }, { q: "d", label: "D" }, { q: "f", label: "F" }, { q: "g", label: "G" },
    { q: "h", label: "H" }, { q: "j", label: "J" }, { q: "k", label: "K" }, { q: "l", label: "L" },
    { q: "backspace", label: "DEL" },
  ],
  [
    { q: "f1", label: "MENU" }, { q: "z", label: "Z" }, { q: "x", label: "X" }, { q: "c", label: "C" }, { q: "v", label: "V" },
    { q: "b", label: "B" }, { q: "n", label: "N" }, { q: "m", label: "M" }, { q: "comma", label: ",", alt: "?" },
    { q: "ret", label: "↵", w: 1.4 },
  ],
  [
    { q: "shift", label: "⇧" }, { q: "alt", label: "ALT", tone: "green" }, { q: "compose", label: "⌕" },
    { q: "mail", label: "@" }, { q: "spc", label: "SYM", w: 2.4, tone: "green" }, { q: "dot", label: ".", alt: "/" },
    { q: "alt_r", label: "ALT", tone: "green" }, { q: "shift_r", label: "⇧" },
  ],
];

type Props = {
  send: (m: InputMessage) => void;
  interactive: boolean;
  style: React.CSSProperties;
  /* key cap height in CSS pixels, so the labels scale with the phone */
  unit: number;
};

export const DreamKeyboard = ({ send, interactive, style, unit }: Props) => {
  const press = (q: string, down: boolean) => interactive && send({ t: "qkey", q, d: down });
  return (
    <div
      className="absolute flex flex-col justify-center gap-[2.2%] rounded-b-[18px] bg-gradient-to-b from-[#c9c9c6] to-[#a9a9a5] px-[3%] shadow-[inset_0_2px_6px_rgba(0,0,0,0.35),0_10px_30px_rgba(0,0,0,0.5)]"
      style={style}
      aria-label="Dream keyboard"
    >
      {ROWS.map((row, i) => (
        <div key={i} className="flex justify-center gap-[1.2%]" style={{ height: unit }}>
          {row.map((k) => (
            <button
              key={k.q}
              type="button"
              disabled={!interactive}
              title={k.alt ? `${k.label}  (Alt: ${k.alt})` : k.label}
              aria-label={k.q === "compose" ? "Search" : k.q === "spc" ? "Space" : k.label}
              className={`relative flex touch-none select-none items-center justify-center rounded-[28%] border border-black/15 bg-gradient-to-b from-white to-[#e4e9ec] font-medium shadow-[0_2px_0_rgba(0,0,0,0.25)] active:translate-y-px active:from-[#cfe8f5] active:to-[#b8dcef] disabled:opacity-60 ${
                k.tone === "green" ? "text-[#3c8a2e]" : "text-[#3a3f44]"
              }`}
              style={{ width: unit * 1.15 * (k.w ?? 1), fontSize: unit * (k.label.length > 2 ? 0.3 : 0.46) }}
              onPointerDown={(e) => {
                e.preventDefault();
                e.currentTarget.setPointerCapture(e.pointerId);
                press(k.q, true);
              }}
              onPointerUp={() => press(k.q, false)}
              onPointerCancel={() => press(k.q, false)}
            >
              {k.label}
              {k.alt && (
                <span className="absolute right-[12%] top-[4%] text-[#3c8a2e]" style={{ fontSize: unit * 0.26 }}>
                  {k.alt}
                </span>
              )}
            </button>
          ))}
        </div>
      ))}
    </div>
  );
};
