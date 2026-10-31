"use client";

import { useCallback, useEffect, useRef, useState, type ReactNode } from "react";
import { DREAM_CHIN, DREAM_CHIN_KEYS, DREAM_HOTSPOTS, DREAM_KBD_BODY, DREAM_KEYBOARD, DREAM_SLAB_H, HOTSPOTS, POWER, geometryFor, pct } from "@/lib/frame-geometry";
import { DreamKeyboard } from "@/components/device/DreamKeyboard";
import type { Device, HardKey, InputMessage } from "@/lib/emulator-types";

/* Dream key caps: green Call, red End, a pearl trackball, the rest graphite */
const DREAM_KEY_STYLE: Partial<Record<HardKey, string>> = {
  call: "rounded-[10px] bg-gradient-to-b from-[#2f6b35] to-[#1b3f1f]",
  end: "rounded-[10px] bg-gradient-to-b from-[#7a2626] to-[#461414]",
  home: "rounded-[10px] bg-gradient-to-b from-[#3a3a3a] to-[#1c1c1c]",
  back: "rounded-[10px] bg-gradient-to-b from-[#3a3a3a] to-[#1c1c1c]",
  menu: "rounded-[8px] bg-gradient-to-b from-[#3a3a3a] to-[#1c1c1c]",
  trackball: "rounded-full bg-[radial-gradient(circle_at_35%_30%,#f4f4f4,#9a9a9a_60%,#5c5c5c)]",
  volup: "rounded-l-[4px] bg-[#2a2a2a]",
  voldown: "rounded-l-[4px] bg-[#2a2a2a]",
};

type Props = {
  device: Device;
  /* the portrait frame's width in CSS pixels */
  width: number;
  /* -M dream: turned 90 degrees counter-clockwise, and with the keyboard out */
  landscape?: boolean;
  keyboard?: boolean;
  canvasRef: React.RefObject<HTMLCanvasElement | null>;
  send: (m: InputMessage) => void;
  interactive: boolean;
  overlay?: ReactNode;
  pressed: Set<HardKey>;
  onKey: (k: HardKey, down: boolean) => void;
};

export const DeviceFrame = ({ device, width, landscape = false, keyboard = false, canvasRef, send, interactive, overlay, pressed, onKey }: Props) => {
  const { frame: FRAME, screen: SCREEN, native } = geometryFor(device);
  const dream = device === "dream";
  const turned = dream && landscape;
  const down = useRef(false);
  const pending = useRef<{ x: number; y: number } | null>(null);
  const raf = useRef(0);
  const [touchAt, setTouchAt] = useState<{ x: number; y: number } | null>(null);

  /* panel coordinates (0..1); turned counter-clockwise, the panel's x runs up the page and its y to the right */
  const norm = (e: React.PointerEvent<HTMLCanvasElement>) => {
    const r = e.currentTarget.getBoundingClientRect();
    if (turned) return { x: (r.bottom - e.clientY) / r.height, y: (e.clientX - r.left) / r.width };
    return { x: (e.clientX - r.left) / r.width, y: (e.clientY - r.top) / r.height };
  };

  const flush = useCallback(() => {
    raf.current = 0;
    const p = pending.current;
    pending.current = null;
    if (p && down.current) send({ t: "touch", a: "move", ...p });
  }, [send]);

  useEffect(() => () => cancelAnimationFrame(raf.current), []);

  const onDown = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (!interactive || e.button > 0) return;
    e.currentTarget.setPointerCapture(e.pointerId);
    down.current = true;
    const p = norm(e);
    setTouchAt(p);
    send({ t: "touch", a: "down", ...p });
  };
  const onMove = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (!down.current) return;
    const p = norm(e);
    pending.current = p;
    setTouchAt(p);
    if (!raf.current) raf.current = requestAnimationFrame(flush);
  };
  const onUp = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (!down.current) return;
    down.current = false;
    cancelAnimationFrame(raf.current);
    raf.current = 0;
    pending.current = null;
    setTouchAt(null);
    send({ t: "touch", a: "up", ...norm(e) });
  };

  const keyHandlers = (k: HardKey) => ({
    onPointerDown: (e: React.PointerEvent) => {
      e.preventDefault();
      (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
      onKey(k, true);
    },
    onPointerUp: () => onKey(k, false),
    onPointerCancel: () => onKey(k, false),
    onLostPointerCapture: () => pressed.has(k) && onKey(k, false),
  });

  const height = (width * FRAME.h) / FRAME.w;
  const phone = (
    <div className="relative select-none" style={{ width, height }}>
      {dream ? (
        <div
          aria-hidden
          className="pointer-events-none absolute z-0 rounded-[9%/5%] bg-gradient-to-b from-[#2b2b2d] via-[#1a1a1b] to-[#101011] shadow-[inset_0_1px_0_rgba(255,255,255,0.12),0_10px_40px_rgba(0,0,0,0.6)]"
          style={{ left: pct(10, FRAME.w), top: 0, right: 0, bottom: 0 }}
        >
          <span className="absolute left-1/2 top-[3.2%] h-[0.6%] w-[16%] -translate-x-1/2 rounded-full bg-[#050505]" />
        </div>
      ) : (
        <button
          type="button"
          aria-label="Power"
          title="Power (F12)"
          data-down={pressed.has("power")}
          className="absolute z-0 rounded-t-[6px] bg-gradient-to-b from-[#3a3a3a] to-[#111] shadow-[inset_0_1px_0_rgba(255,255,255,0.18)] transition-transform data-[down=true]:translate-y-[2px] data-[down=true]:shadow-[0_0_14px_rgba(51,181,229,0.6)]"
          style={{ left: pct(POWER.x, FRAME.w), top: pct(POWER.y, FRAME.h), width: pct(POWER.w, FRAME.w), height: pct(POWER.h, FRAME.h) }}
          {...keyHandlers("power")}
        />
      )}
      <canvas
        ref={canvasRef}
        width={native.w}
        height={native.h}
        className={`absolute z-[1] touch-none bg-black ${interactive ? "cursor-crosshair" : ""}`}
        style={{
          left: pct(SCREEN.x - 2, FRAME.w),
          top: pct(SCREEN.y - 2, FRAME.h),
          width: pct(SCREEN.w + 4, FRAME.w),
          height: pct(SCREEN.h + 4, FRAME.h),
        }}
        onPointerDown={onDown}
        onPointerMove={onMove}
        onPointerUp={onUp}
        onPointerCancel={onUp}
        onContextMenu={(e) => e.preventDefault()}
      />
      {touchAt && (
        <span
          className="pointer-events-none absolute z-[3] size-9 -translate-x-1/2 -translate-y-1/2 rounded-full border-2 border-holo-blue/80 bg-holo-blue/20"
          style={{
            left: `calc(${pct(SCREEN.x, FRAME.w)} + ${touchAt.x} * ${pct(SCREEN.w, FRAME.w)})`,
            top: `calc(${pct(SCREEN.y, FRAME.h)} + ${touchAt.y} * ${pct(SCREEN.h, FRAME.h)})`,
          }}
        />
      )}
      {overlay && (
        <div
          className="absolute z-[2] flex items-center justify-center"
          style={{ left: pct(SCREEN.x, FRAME.w), top: pct(SCREEN.y, FRAME.h), width: pct(SCREEN.w, FRAME.w), height: pct(SCREEN.h, FRAME.h) }}
        >
          {overlay}
        </div>
      )}
      {!dream && (
        /* eslint-disable-next-line @next/next/no-img-element */
        <img
          src="/hd2-frame.webp"
          alt="HTC HD2"
          draggable={false}
          className="pointer-events-none absolute inset-0 z-[4] h-full w-full"
        />
      )}
      {(dream ? DREAM_HOTSPOTS : HOTSPOTS).map((h) => (
        <button
          key={h.key}
          type="button"
          aria-label={h.label}
          title={h.label}
          data-down={pressed.has(h.key)}
          className={
            dream
              ? `absolute z-[5] touch-none shadow-[inset_0_1px_0_rgba(255,255,255,0.15)] transition data-[down=true]:brightness-150 data-[down=true]:shadow-[0_0_14px_rgba(51,181,229,0.6)] ${DREAM_KEY_STYLE[h.key] ?? ""}`
              : `hd2-key absolute z-[5] touch-none ${h.side ? "rounded-[6px]" : ""}`
          }
          style={{ left: pct(h.x, FRAME.w), top: pct(h.y, FRAME.h), width: pct(h.w, FRAME.w), height: pct(h.h, FRAME.h) }}
          {...keyHandlers(h.key)}
        />
      ))}
    </div>
  );
  if (!turned) return phone;
  /* the frame unit in CSS pixels */
  const u = width / FRAME.w;
  const turn = { transformOrigin: "0 0", transform: `translateY(${width}px) rotate(-90deg)` } as const;
  if (!keyboard) {
    /* turned on its side by hand: the whole phone, chin and all */
    return (
      <div className="relative select-none" style={{ width: height, height: width }}>
        <div className="absolute left-0 top-0" style={{ width, height, ...turn }}>
          {phone}
        </div>
      </div>
    );
  }
  /* keyboard out: the slab (screen, no chin) slid up, the chin with the keyboard */
  return (
    <div className="relative select-none" style={{ width: height, height: width + DREAM_KEYBOARD.h * u }}>
      <DreamKeyboard
        send={send}
        interactive={interactive}
        unit={DREAM_KBD_BODY.h * u * 0.14}
        style={{ left: DREAM_KBD_BODY.x * u, top: DREAM_KBD_BODY.y * u, width: DREAM_KBD_BODY.w * u, height: DREAM_KBD_BODY.h * u, zIndex: 1, paddingTop: 18 * u }}
      />
      <div
        className="absolute z-[1] rounded-[22px] bg-gradient-to-l from-[#2b2b2d] via-[#1a1a1b] to-[#101011] shadow-[inset_0_1px_0_rgba(255,255,255,0.12),0_10px_40px_rgba(0,0,0,0.6)]"
        style={{ left: DREAM_CHIN.x * u, top: DREAM_CHIN.y * u, width: DREAM_CHIN.w * u, height: DREAM_CHIN.h * u }}
      >
        {DREAM_CHIN_KEYS.map((h) => (
          <button
            key={h.key}
            type="button"
            aria-label={h.label}
            title={h.label}
            data-down={pressed.has(h.key)}
            className={`absolute touch-none shadow-[inset_0_1px_0_rgba(255,255,255,0.15)] transition data-[down=true]:brightness-150 data-[down=true]:shadow-[0_0_14px_rgba(51,181,229,0.6)] ${DREAM_KEY_STYLE[h.key] ?? ""}`}
            style={{ left: h.x * u, top: h.y * u, width: h.w * u, height: h.h * u }}
            {...keyHandlers(h.key)}
          />
        ))}
      </div>
      <div className="absolute left-0 top-0 z-[2] overflow-hidden rounded-b-[26px]" style={{ width, height: DREAM_SLAB_H * u, ...turn }}>
        {phone}
      </div>
    </div>
  );
};
