import { useLayoutEffect, useRef, useState } from "preact/hooks";
import { Dropdown } from "../ui/Dropdown.jsx";
import {
  centerValue, displayRatio, entityCanSnap, extraSnapForEntity, isSnapEnabled,
  linearRatio, loopPercent, loopRatio, phonesRatio, stepsForEntity,
  valueFromDisplayRatio, valueFromLoopRatio, valueFromPhonesRatio
} from "./steps.js";
import { isDblTapEnabled } from "./views.js";

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function ticks(count) {
  const out = [];
  for (let i = 0; i < count; i++) out.push(-135 + (i * 270) / (count - 1));
  return out;
}

const KNOB_TICKS = ticks(11);

export function formatValue(value, min, max, unit) {
  const n = Number(value);
  if (!Number.isFinite(n)) return "—";
  if (unit === "loop") return `${Math.round(loopPercent(n))}%`;
  if (unit === "%") return `${Math.round(n * 100)}%`;
  if ((unit === "dB" || unit === "phones") && n <= -115) return "−∞";
  if (unit === "dB" || unit === "phones") {
    const shown = Math.abs(n) < 10 && n % 1 ? n.toFixed(1) : String(Math.round(n));
    const num = Number(shown);
    if (num === 0) return "0 dB";
    return num > 0 ? `+${shown} dB` : `−${String(shown).replace("-", "")} dB`;
  }
  if (max <= 1 && min >= 0) return `${Math.round(n * 100)}%`;
  if (Math.abs(max - min) <= 30) return n.toFixed(1);
  return n.toFixed(0);
}

function pickTier(tiers, heightPx, minPx = 28) {
  const usable = Math.max(48, (heightPx || 140) - 36);
  const maxLabels = Math.max(2, Math.floor(usable / minPx));
  let best = tiers[0];
  for (const tier of tiers) {
    if (tier.length <= maxLabels) best = tier;
  }
  return { labels: best, usable };
}

function formatMarkDb(n, signed) {
  if (!Number.isFinite(n) || n <= -115) return "−∞";
  const rounded = Math.round(n);
  if (rounded === 0) return "0";
  if (!signed) return String(rounded);
  return rounded > 0 ? `+${rounded}` : `−${Math.abs(rounded)}`;
}

function dbLabelTiers(steps) {
  const signed = steps.some((n) => n < -0.2);
  return stepNumberTiers(steps.length).map((tier) => tier.map((oneBased) => {
    const i = oneBased - 1;
    const value = steps[i];
    return { i, value, text: formatMarkDb(value, signed) };
  }));
}

function stepNumberTiers(count) {
  const last = count;
  const all = [];
  for (let i = 1; i <= last; i++) all.push(i);
  if (count === 16) {
    return [
      [1, 16],
      [1, 8, 16],
      [1, 4, 8, 12, 16],
      [1, 2, 4, 6, 8, 10, 12, 14, 16],
      all
    ];
  }
  if (count === 15) {
    return [
      [1, 15],
      [1, 8, 15],
      [1, 4, 8, 12, 15],
      [1, 2, 4, 6, 8, 10, 12, 14, 15],
      all
    ];
  }
  const mid = Math.round((last + 1) / 2);
  const every = (n) => {
    const out = [1];
    for (let i = n; i < last; i += n) out.push(i);
    if (out[out.length - 1] !== last) out.push(last);
    return out;
  };
  return [[1, last], [1, mid, last], every(Math.max(2, Math.round(last / 4))), every(2), all];
}

function loopLabelTiers() {
  return [
    [0, 200],
    [0, 100, 200],
    [0, 50, 100, 150, 200],
    [0, 20, 40, 60, 80, 100, 120, 140, 160, 180, 200]
  ];
}

function percentLabelTiers() {
  return [
    [0, 100],
    [0, 50, 100],
    [0, 25, 50, 75, 100],
    [0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100]
  ];
}

function phonesLabelTiers() {
  return [
    [-120, 0, 10],
    [-120, -60, 0, 10],
    [-120, -60, 0, 5, 10],
    [-120, -90, -60, -30, 0, 5, 10]
  ];
}

function subParts(usable, stepCount) {
  const gap = usable / Math.max(1, stepCount - 1);
  if (gap >= 22) return 4;
  if (gap >= 14) return 2;
  return 1;
}

function labelRatio(lab, unit, stepCount) {
  if (unit === "loop") return lab / 200;
  if (unit === "%") return lab / 100;
  if (unit === "phones") return phonesRatio(lab);
  return (lab - 1) / Math.max(1, stepCount - 1);
}

function labelText(lab, unit) {
  if (unit === "phones") {
    if (lab === 0) return "0";
    return lab > 0 ? `+${lab}` : `−${Math.abs(lab)}`;
  }
  return String(lab);
}

function attachLabel(marks, ratio, text, extras) {
  let best = 0;
  let bestD = 99;
  for (let i = 0; i < marks.length; i++) {
    const d = Math.abs(marks[i].ratio - ratio);
    if (d < bestD) {
      bestD = d;
      best = i;
    }
  }
  const row = bestD <= 0.03
    ? marks[best]
    : marks[marks.push({
      ratio,
      zero: false,
      half: false,
      tick: false,
      med: false,
      minor: false,
      label: ""
    }) - 1];
  row.tick = false;
  row.label = text;
  row.zero = !!(extras && extras.zero);
  row.half = !!(extras && extras.half);
}

function hardwareMarks({ steps, unit, heightPx }) {
  const phones = unit === "phones";
  const dbSteps = !phones && unit !== "loop" && unit !== "%" && steps && steps.length >= 2;
  const stepCount = (phones || unit === "loop") ? 16 : (steps && steps.length >= 2 ? steps.length : 16);
  const picked = unit === "loop"
    ? pickTier(loopLabelTiers(), heightPx)
    : unit === "%"
      ? pickTier(percentLabelTiers(), heightPx)
      : phones
        ? pickTier(phonesLabelTiers(), heightPx, 24)
        : dbSteps
          ? pickTier(dbLabelTiers(steps), heightPx, 32)
          : pickTier(stepNumberTiers(stepCount), heightPx);
  const { labels, usable } = picked;
  const parts = subParts(usable, stepCount);
  const marks = [];
  for (let i = 0; i < stepCount; i++) {
    const ratio = i / (stepCount - 1);
    marks.push({
      ratio,
      zero: false,
      half: false,
      tick: true,
      med: false,
      minor: false,
      label: ""
    });
    if (i === stepCount - 1 || parts <= 1) continue;
    const span = 1 / (stepCount - 1);
    for (let s = 1; s < parts; s++) {
      marks.push({
        ratio: ratio + (span * s) / parts,
        zero: false,
        half: false,
        tick: true,
        med: s * 2 === parts,
        minor: s * 2 !== parts,
        label: ""
      });
    }
  }
  if (dbSteps) {
    for (const lab of labels) {
      attachLabel(marks, lab.i / Math.max(1, stepCount - 1), lab.text, {
        zero: Math.abs(lab.value) < 0.2
      });
    }
  } else {
    for (const lab of labels) {
      const ratio = lab <= -119 ? 0 : (phones ? phonesRatio(lab) : labelRatio(lab, unit, stepCount));
      attachLabel(marks, ratio, lab <= -119 ? "−∞" : labelText(lab, unit), {
        zero: lab === 0,
        half: unit === "loop" ? lab === 100 : lab === 0 && phones
      });
    }
  }
  return marks.sort((a, b) => b.ratio - a.ratio);
}

export function Fader({ value, min, max, unit, entity, snap, onChange }) {
  const track = useRef(null);
  const cap = useRef(null);
  const lastTap = useRef(0);
  const [trackH, setTrackH] = useState(140);
  const lo = min == null ? -120 : min;
  const hi = max == null ? 10 : max;
  const n = Number(value);
  const snappable = snap !== false && entityCanSnap(entity);
  const stepList = entity ? stepsForEntity(entity) : null;
  const extra = snappable ? extraSnapForEntity(entity) : null;
  const quantize = snappable && isSnapEnabled();
  const shownUnit = entity === "MAIN_HEADPHONE_GAIN" ? "phones" : (unit || "dB");
  const useSteps = !!stepList && entityCanSnap(entity) && snap !== false;
  const kind = shownUnit === "loop" || entity === "LOOPER_LEVEL" ? "loop"
    : shownUnit === "phones" ? "phones"
    : useSteps ? "steps" : "linear";
  const ratio = kind === "loop" ? loopRatio(n)
    : kind === "phones" ? phonesRatio(n)
    : kind === "steps" ? (displayRatio(n, stepList, extra) || 0)
    : linearRatio(n, lo, hi);
  const marks = hardwareMarks({
    steps: kind === "steps" ? stepList : null,
    unit: shownUnit,
    heightPx: trackH
  });

  useLayoutEffect(() => {
    const node = track.current;
    if (!node || typeof ResizeObserver === "undefined") return;
    const apply = () => setTrackH(node.getBoundingClientRect().height);
    apply();
    const ro = new ResizeObserver(apply);
    ro.observe(node);
    return () => ro.disconnect();
  }, []);

  function fromPointer(event) {
    const box = track.current.getBoundingClientRect();
    const capH = cap.current ? cap.current.getBoundingClientRect().height : 36;
    const usable = Math.max(1, box.height - capH);
    const y = clamp((event.clientY - box.top - capH / 2) / usable, 0, 1);
    const t = 1 - y;
    if (kind === "loop") {
      onChange(valueFromLoopRatio(t));
      return;
    }
    if (kind === "phones") {
      onChange(valueFromPhonesRatio(t));
      return;
    }
    if (kind === "steps") {
      onChange(valueFromDisplayRatio(t, stepList, extra, quantize));
      return;
    }
    onChange(lo + t * (hi - lo));
  }

  function grab(event) {
    event.preventDefault();
    event.stopPropagation();
    const now = Date.now();
    if (isDblTapEnabled() && now - lastTap.current < 320) {
      lastTap.current = 0;
      onChange(centerValue(entity, lo, hi, shownUnit));
      return;
    }
    lastTap.current = now;
    event.currentTarget.setPointerCapture(event.pointerId);
    fromPointer(event);
  }

  function drag(event) {
    if (!event.currentTarget.hasPointerCapture(event.pointerId)) return;
    event.preventDefault();
    event.stopPropagation();
    fromPointer(event);
  }

  return (
    <div class="fader">
      <div class="fader-track" ref={track}>
        <div class="fader-groove" />
        <div class="fader-scale" aria-hidden="true">
          {marks.map((mark, i) => (
            <span
              key={`${mark.ratio}-${i}`}
              class={"fader-mark"
                + (mark.zero ? " zero" : "")
                + (mark.half ? " half" : "")
                + (mark.tick ? " tick" : " major")
                + (mark.med ? " med" : "")
                + (mark.minor ? " minor" : "")}
              style={{ bottom: `calc(${mark.ratio} * (100% - var(--cap)) + (var(--cap) / 2))` }}
            >
              {mark.tick ? null : <span class="fader-mark-lbl">{mark.label}</span>}
            </span>
          ))}
        </div>
        <div
          class="fader-cap"
          ref={cap}
          style={{ bottom: `calc(${ratio} * (100% - var(--cap)))` }}
          onPointerDown={grab}
          onPointerMove={drag}
        >
          <span class="fader-cap-face" />
        </div>
      </div>
      <small>{formatValue(n, lo, hi, shownUnit)}</small>
    </div>
  );
}

export function FillSlider({ value, min, max, unit, tone, entity, snap, onChange }) {
  const track = useRef(null);
  const lo = min == null ? 0 : min;
  const hi = max == null ? 1 : max;
  const n = Number(value);
  const snappable = snap !== false && entityCanSnap(entity);
  const steps = entity ? stepsForEntity(entity) : null;
  const extra = snappable ? extraSnapForEntity(entity) : null;
  const quantize = snappable && isSnapEnabled();
  const ratio = steps && snappable
    ? (displayRatio(n, steps, extra) || 0)
    : linearRatio(n, lo, hi);

  function fromPointer(event) {
    const box = track.current.getBoundingClientRect();
    const x = clamp((event.clientX - box.left) / box.width, 0, 1);
    if (steps && snappable) {
      onChange(valueFromDisplayRatio(x, steps, extra, quantize));
      return;
    }
    onChange(lo + x * (hi - lo));
  }

  return (
    <div
      class={"hslider" + (tone ? " tone-" + tone : "")}
      ref={track}
      onPointerDown={(event) => {
        event.preventDefault();
        event.currentTarget.setPointerCapture(event.pointerId);
        fromPointer(event);
      }}
      onPointerMove={(event) => {
        if (event.currentTarget.hasPointerCapture(event.pointerId)) {
          event.preventDefault();
          fromPointer(event);
        }
      }}
    >
      <span class="hslider-fill" style={{ width: `${ratio * 100}%` }} />
      <span class="hslider-txt">{formatValue(n, lo, hi, unit)}</span>
    </div>
  );
}

export function Knob({ value, min, max, label, unit, size, tone, entity, snap, onChange }) {
  const lo = min == null ? 0 : min;
  const hi = max == null ? 1 : max;
  const n = Number(value);
  const snappable = snap !== false && entityCanSnap(entity);
  const steps = entity ? stepsForEntity(entity) : null;
  const extra = snappable ? extraSnapForEntity(entity) : null;
  const quantize = snappable && isSnapEnabled();
  const ratio = steps && snappable
    ? (displayRatio(n, steps, extra) || 0)
    : (!Number.isFinite(n) ? 0.5 : linearRatio(n, lo, hi));
  const deg = -135 + ratio * 270;
  const start = useRef(null);
  const lastTap = useRef(0);
  const shown = formatValue(n, lo, hi, unit);
  const xs = size === "xs";

  function drag(event) {
    if (!start.current) return;
    const delta = (start.current.y - event.clientY) / 90;
    const next = clamp(start.current.ratio + delta, 0, 1);
    if (steps && snappable) {
      onChange(valueFromDisplayRatio(next, steps, extra, quantize));
      return;
    }
    onChange(lo + next * (hi - lo));
  }

  return (
    <div class={"knob" + (size ? " " + size : "") + (tone ? " tone-" + tone : "")} title={`${label} ${shown}`}>
      <div
        class="knob-face"
        onPointerDown={(event) => {
          event.preventDefault();
          const now = Date.now();
          if (isDblTapEnabled() && now - lastTap.current < 320) {
            lastTap.current = 0;
            onChange(centerValue(entity, lo, hi, unit));
            return;
          }
          lastTap.current = now;
          event.currentTarget.setPointerCapture(event.pointerId);
          start.current = { y: event.clientY, ratio };
        }}
        onPointerMove={(event) => {
          if (event.currentTarget.hasPointerCapture(event.pointerId)) {
            event.preventDefault();
            drag(event);
          }
        }}
      >
        <div class="knob-ticks" aria-hidden="true">
          {KNOB_TICKS.map((angle) => (
            <span key={angle} style={{ transform: `rotate(${angle}deg)` }} />
          ))}
        </div>
        <div class="knob-body" style={{ transform: `rotate(${deg}deg)` }}>
          <span class="knob-index" />
        </div>
      </div>
      {xs ? null : <strong>{label}</strong>}
      {xs ? null : <small>{shown}</small>}
    </div>
  );
}

export function Pad({ on, tone, label, title, disabled, onClick, icon, extra }) {
  return (
    <button
      type="button"
      class={"pad" + (on ? " on" : "") + (tone ? " " + tone : "") + (icon && !label ? " icon-only" : "") + (icon && label ? " has-ico" : "") + (extra ? " " + extra : "")}
      title={title || label}
      disabled={disabled}
      onClick={onClick}
    >
      {icon ? <span class="pad-ico">{icon}</span> : null}
      {label ? <span class="pad-txt">{label}</span> : null}
    </button>
  );
}

export function ModeToggle({ label, on, title, onClick, compact, icon }) {
  return (
    <button
      type="button"
      class={"mode-toggle" + (on ? " on" : "") + (compact ? " compact" : "") + (icon ? " has-ico" : "")}
      title={title || label}
      onClick={onClick}
    >
      {icon ? <span class="mode-ico">{icon}</span> : null}
      {label ? <span class="mode-txt">{label}</span> : null}
    </button>
  );
}

export function Seg({ value, options, onChange, variant }) {
  return (
    <div class={"seg" + (variant ? " " + variant : "")}>
      {options.map(([id, name]) => (
        <button
          type="button"
          key={String(id)}
          class={String(value) === String(id) ? "on" : ""}
          onClick={() => onChange(id)}
        >{name}</button>
      ))}
    </div>
  );
}

export function LedButton({ on, label, title, onClick, variant }) {
  return (
    <button type="button" class={"led-btn" + (on ? " on" : "") + (variant ? " " + variant : "")} title={title || label} onClick={onClick}>
      <span class="led-pip" />
      <span>{label}</span>
    </button>
  );
}

export function FxSelect({ value, options, label, onChange, wide }) {
  const pairs = options;
  const [open, setOpen] = useState(false);
  const btn = useRef(null);

  return (
    <>
      <button ref={btn} type="button" class="fx-name" title={label || "Type"} onClick={() => setOpen((v) => !v)}>
        {label || "Type"}
      </button>
      <Dropdown anchorRef={btn} open={open} wide={wide !== false} onClose={() => setOpen(false)}>
        {pairs.map(([id, name]) => (
          <button
            type="button"
            key={id}
            class={"picker-item" + (String(id) === String(value) ? " on" : "")}
            onClick={() => { onChange(id); setOpen(false); }}
          >{name}</button>
        ))}
      </Dropdown>
    </>
  );
}
