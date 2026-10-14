import { useLayoutEffect, useRef, useState } from "preact/hooks";
import { snapEntity } from "./steps.js";

// Showbox 3-band EQ as drawn in the Mackie manual (Cal Perkins neo-classic):
// 1st-order low shelf @ 80 Hz, wide peak @ 2.5 kHz, 1st-order high shelf @ 12 kHz.
// Shelves keep moving past the named corner (low toward 20 Hz, high toward 20 kHz).
// Boost/cut are mirrored the way the published plots are.
const EQ_LOW_FC = 80;
const EQ_MID_FC = 2500;
const EQ_HIGH_FC = 12000;
const EQ_MID_Q = 0.6;

function dbRatio(gainDb) {
  return Math.pow(10, Math.abs(gainDb) / 20);
}

function signedShape(gainDb, positiveDb) {
  const mag = Math.abs(Number(gainDb) || 0);
  if (mag < 1e-9) return 0;
  const n = Number(positiveDb);
  if (!Number.isFinite(n)) return 0;
  return (gainDb < 0 ? -1 : 1) * n;
}

function dbLowShelf(f, fc, gainDb) {
  const mag = Math.abs(Number(gainDb) || 0);
  if (mag < 1e-9) return 0;
  const A = dbRatio(mag);
  const at = (hz) => 20 * Math.log10(Math.hypot(hz, A * fc) / Math.hypot(hz, fc));
  const edge = at(20);
  return signedShape(gainDb, edge === 0 ? 0 : at(f) * (mag / edge));
}

function dbHighShelf(f, fc, gainDb) {
  const mag = Math.abs(Number(gainDb) || 0);
  if (mag < 1e-9) return 0;
  const A = dbRatio(mag);
  const at = (hz) => 20 * Math.log10(Math.hypot(A * hz, fc) / Math.hypot(hz, fc));
  const edge = at(20000);
  return signedShape(gainDb, edge === 0 ? 0 : at(f) * (mag / edge));
}

function dbPeak(f, fc, gainDb, q = EQ_MID_Q) {
  const mag = Math.abs(Number(gainDb) || 0);
  if (mag < 1e-9) return 0;
  const A = dbRatio(mag);
  const w = f / fc;
  const oneMinus = 1 - w * w;
  const aq = (A * w) / q;
  const iq = w / q;
  const pos = 10 * Math.log10((oneMinus * oneMinus + aq * aq) / (oneMinus * oneMinus + iq * iq));
  return signedShape(gainDb, pos);
}

function freqs(count = 72) {
  const out = [];
  for (let i = 0; i < count; i++) out.push(20 * Math.pow(1000, i / (count - 1)));
  return out;
}

function xOf(f, width, x0 = 0, span) {
  const s = span == null ? width : span;
  return x0 + (Math.log(f / 20) / Math.log(1000)) * s;
}

function yOf(db, height, min = -15, max = 15) {
  const t = Math.min(1, Math.max(0, (db - min) / (max - min)));
  return height - t * height;
}

function bandDb(id, f, gains) {
  if (id === "low") return dbLowShelf(f, EQ_LOW_FC, gains.low);
  if (id === "mid") return dbPeak(f, EQ_MID_FC, gains.mid);
  return dbHighShelf(f, EQ_HIGH_FC, gains.high);
}

function curveDb(f, gains) {
  return bandDb("low", f, gains) + bandDb("mid", f, gains) + bandDb("high", f, gains);
}

function bandArea(id, gains, hz, w, x0, plotW, y0, plotH) {
  const zeroY = y0 + yOf(0, plotH);
  const pts = hz.map((f) => {
    const x = xOf(f, w, x0, plotW);
    const y = y0 + yOf(bandDb(id, f, gains), plotH);
    return [x, y];
  });
  if (!pts.length) return "";
  const first = pts[0];
  const last = pts[pts.length - 1];
  const line = pts.map(([x, y]) => `${x.toFixed(1)},${y.toFixed(1)}`).join(" ");
  return `M${first[0].toFixed(1)},${zeroY.toFixed(1)} L${line} L${last[0].toFixed(1)},${zeroY.toFixed(1)} Z`;
}

function miniEqPercent(v) {
  const n = Number(v);
  if (!Number.isFinite(n)) return "50%";
  return `${Math.round(((Math.max(-15, Math.min(15, n)) + 15) / 30) * 100)}%`;
}

const BAND_FREQS = [EQ_LOW_FC, EQ_MID_FC, EQ_HIGH_FC];
const FREQ_MARKS = [
  [20, "20", 3],
  [50, "50", 1],
  [80, "80", 4],
  [100, "100", 2],
  [200, "200", 1],
  [500, "500", 2],
  [1000, "1k", 2],
  [2000, "2k", 1],
  [2500, "2.5k", 4],
  [5000, "5k", 2],
  [10000, "10k", 2],
  [12000, "12k", 4],
  [20000, "20k", 3]
];

function pickFreqLabels(candidates, minGap, x0, pw) {
  const ranked = candidates.slice().sort((a, b) => b.prio - a.prio || a.f - b.f);
  const chosen = [];
  for (const item of ranked) {
    if (item.x < x0 - 2 || item.x > x0 + pw + 2) continue;
    const hit = chosen.some((row) => Math.abs(row.x - item.x) < minGap);
    if (hit && item.prio < 4) continue;
    if (hit) {
      const other = chosen.find((row) => Math.abs(row.x - item.x) < minGap);
      if (other && other.prio <= item.prio) continue;
      if (other) chosen.splice(chosen.indexOf(other), 1);
    }
    chosen.push(item);
  }
  return chosen.sort((a, b) => a.x - b.x);
}

function EqGrid({ w, h, y0 = 0, plotH, x0 = 0, plotW, dbMin = -15, dbMax = 15, dbStep = 5 }) {
  const ph = plotH == null ? h : plotH;
  const pw = plotW == null ? w - x0 : plotW;
  const lines = [];
  const labels = [];
  const minFreqGap = Math.max(16, Math.min(30, pw / 16));
  const candidates = FREQ_MARKS.map(([f, name, prio]) => ({
    f,
    name,
    prio: BAND_FREQS.includes(f) ? 4 : prio,
    x: xOf(f, w, x0, pw)
  }));
  for (const item of candidates) {
    const named = item.prio >= 2 || BAND_FREQS.includes(item.f);
    lines.push(<line key={"f" + item.f} x1={item.x} y1={y0} x2={item.x} y2={y0 + ph} class={named ? "eq-grid" : "eq-grid dim"} />);
  }
  for (const item of pickFreqLabels(candidates, minFreqGap, x0, pw)) {
    const edge = item.x > x0 + pw - 20;
    const start = item.x < x0 + 16;
    labels.push(
      <text
        key={"ft" + item.f}
        x={item.x}
        y={y0 + ph + 11}
        text-anchor={edge ? "end" : start ? "start" : "middle"}
        class={"eq-axis-x" + (BAND_FREQS.includes(item.f) ? " band" : "")}
      >{item.name}</text>
    );
  }
  const minDbGap = Math.max(10, Math.min(14, ph / 12));
  const step = ph >= 160 && (dbMax - dbMin) >= 20 ? Math.min(dbStep, 5) : dbStep;
  let lastDbY = -999;
  for (let db = dbMin; db <= dbMax + 0.01; db += step) {
    const y = y0 + yOf(db, ph, dbMin, dbMax);
    lines.push(<line key={"d" + db} x1={x0} y1={y} x2={x0 + pw} y2={y} class={db === 0 ? "eq-zero" : "eq-grid dim"} />);
    if (db !== 0 && Math.abs(y - lastDbY) < minDbGap) continue;
    lastDbY = y;
    labels.push(
      <text key={"dt" + db} x={x0 - 5} y={y} text-anchor="end" dominant-baseline="middle" class="eq-axis-y">
        {db > 0 ? "+" + db : String(db)}
      </text>
    );
  }
  return (
    <g>
      {lines}
      {labels}
    </g>
  );
}

const EQ_BANDS = [
  { id: "low", f: EQ_LOW_FC, tone: "low" },
  { id: "mid", f: EQ_MID_FC, tone: "mid" },
  { id: "high", f: EQ_HIGH_FC, tone: "high" }
];

export function EqGraph({ low, mid, high, on, mini, prefix, onBand, overlay }) {
  const svgRef = useRef(null);
  const drag = useRef(null);
  const [box, setBox] = useState({ w: mini ? 240 : 360, h: mini ? 52 : 200 });

  useLayoutEffect(() => {
    const node = svgRef.current;
    if (!node || mini) return undefined;
    const apply = () => {
      const w = Math.max(2, Math.round(node.clientWidth));
      const h = Math.max(2, Math.round(node.clientHeight));
      setBox((prev) => (prev.w === w && prev.h === h ? prev : { w, h }));
    };
    apply();
    if (typeof ResizeObserver === "undefined") return undefined;
    const ro = new ResizeObserver(apply);
    ro.observe(node);
    return () => ro.disconnect();
  }, [mini]);

  const w = mini ? 240 : box.w;
  const h = mini ? 64 : box.h;
  const enabled = on !== false;
  const gains = { low: Number(low) || 0, mid: Number(mid) || 0, high: Number(high) || 0 };
  const vis = mini ? 4.4 : Math.max(5.5, Math.min(7, h * 0.045));
  const x0 = mini ? 0 : 30;
  const y0 = mini ? vis + 1 : vis + 2;
  const yPadBottom = mini ? vis + 12 : vis + 14;
  const plotW = Math.max(8, w - x0 - (mini ? 0 : 6));
  const plotH = Math.max(8, h - y0 - yPadBottom);
  const hz = freqs(mini ? 64 : 128);
  const pts = hz.map((f) => {
    return `${xOf(f, w, x0, plotW).toFixed(1)},${(y0 + yOf(curveDb(f, gains), plotH)).toFixed(1)}`;
  });

  function bandFromPoint(clientY, svg) {
    const rect = svg.getBoundingClientRect();
    const y = clientY - rect.top;
    const scale = rect.height / Math.max(1, h);
    const top = y0 * scale;
    const db = 15 - ((y - top) / Math.max(1, plotH * scale)) * 30;
    return Math.max(-15, Math.min(15, db));
  }

  function pointer(event, band) {
    if (!onBand || !prefix) return;
    event.preventDefault();
    event.stopPropagation();
    const svg = event.currentTarget.ownerSVGElement || event.currentTarget;
    if (event.type === "pointerdown") {
      event.currentTarget.setPointerCapture(event.pointerId);
      drag.current = band;
    }
    const id = drag.current || band;
    if (!id) return;
    const entity = `${prefix}EQ_${id.toUpperCase()}_GAIN`;
    onBand(entity, snapEntity(entity, bandFromPoint(event.clientY, svg)));
  }

  return (
    <svg
      ref={svgRef}
      class={"eq-graph" + (mini ? " mini" : " full") + (enabled ? "" : " disabled") + (onBand ? " interactive" : "")}
      viewBox={`0 0 ${w} ${h}`}
      preserveAspectRatio="xMidYMid meet"
      aria-hidden={onBand ? "false" : "true"}
    >
      {mini ? <line x1="0" y1={y0 + plotH / 2} x2={w} y2={y0 + plotH / 2} class="eq-zero" /> : <EqGrid w={w} h={h} y0={y0} plotH={plotH} x0={x0} plotW={plotW} />}
      <g class="eq-fills">
        {EQ_BANDS.map((band) => (
          <path
            key={"fill-" + band.id}
            class={"eq-fill tone-" + band.tone}
            d={bandArea(band.id, gains, hz, w, x0, plotW, y0, plotH)}
          />
        ))}
      </g>
      {EQ_BANDS.map((band) => (
        <polyline
          key={"curve-" + band.id}
          fill="none"
          class={"eq-curve tone-" + band.tone + (enabled ? "" : " flat")}
          points={hz.map((f) => `${xOf(f, w, x0, plotW).toFixed(1)},${(y0 + yOf(bandDb(band.id, f, gains), plotH)).toFixed(1)}`).join(" ")}
        />
      ))}
      <polyline fill="none" points={pts.join(" ")} class={"eq-curve sum" + (enabled ? "" : " flat")} />
      {EQ_BANDS.map((band) => {
        const cx = xOf(band.f, w, x0, plotW);
        const cy = y0 + yOf(bandDb(band.id, band.f, gains), plotH);
        const hit = mini ? vis : Math.max(18, vis * 2.8);
        return (
          <g key={band.id} class={"eq-dot-hit tone-" + band.tone}>
            {onBand ? (
              <circle
                class="eq-dot-pad"
                cx={cx}
                cy={cy}
                r={hit}
                onPointerDown={(event) => pointer(event, band.id)}
                onPointerMove={(event) => {
                  if (event.currentTarget.hasPointerCapture(event.pointerId)) pointer(event, band.id);
                }}
                onPointerUp={() => { drag.current = null; }}
              />
            ) : null}
            <circle class={"eq-dot tone-" + band.tone} cx={cx} cy={cy} r={vis} />
          </g>
        );
      })}
      {mini ? (
        <g class="eq-mini-db">
          <text x="4" y={h - 3} text-anchor="start" font-size="16">{miniEqPercent(gains.low)}</text>
          <text x={w / 2} y={h - 3} text-anchor="middle" font-size="16">{miniEqPercent(gains.mid)}</text>
          <text x={w - 4} y={h - 3} text-anchor="end" font-size="16">{miniEqPercent(gains.high)}</text>
        </g>
      ) : null}
      {mini && overlay ? (
        <text class="eq-mini-title" x={w / 2} y="3" text-anchor="middle" dominant-baseline="hanging">{overlay}</text>
      ) : null}
    </svg>
  );
}

export function CompGraph({ amount, on, mini, overlay }) {
  const amt = Math.max(0, Math.min(1, Number(amount) || 0));
  const thresh = amt * -30;
  const makeup = amt * 10;
  if (mini) return <CompGraphMini amount={amt} on={on} overlay={overlay} />;
  // Full overlay graph: 54 dB per axis, 6 dB grid, 3 dB minor lines.
  const w = 360;
  const h = 216;
  const padL = 34;
  const padT = 6;
  const padB = 22;
  const padR = 8;
  const dbMin = -36;
  const dbMax = 18;
  const span = dbMax - dbMin;
  const plotW = w - padL - padR;
  const plotH = h - padT - padB;
  const map = (db) => (db - dbMin) / span;
  const ratio = 4;
  const enabled = on !== false;
  const points = [];
  for (let i = 0; i <= 60; i++) {
    const input = dbMin + (i / 60) * 42;
    let output = input;
    if (input > thresh) output = thresh + (input - thresh) / ratio;
    output += makeup;
    points.push(`${(padL + map(input) * plotW).toFixed(1)},${(padT + plotH - map(output) * plotH).toFixed(1)}`);
  }
  const tx = padL + map(thresh) * plotW;
  const xLines = [];
  const xLabels = [];
  const yLabels = [];
  for (let db = dbMin; db <= dbMax + 0.01; db += 3) {
    const major = db % 6 === 0;
    const x = padL + map(db) * plotW;
    const y = padT + plotH - map(db) * plotH;
    if (x <= padL + plotW + 0.5) {
      xLines.push(<line key={"vx" + db} x1={x} y1={padT} x2={x} y2={padT + plotH} class={"eq-grid" + (major ? "" : " dim")} />);
      if (major && db <= 6.01) {
        xLabels.push(<text key={"xt" + db} x={x} y={h - 7} text-anchor="middle" class="eq-axis-x">{db > 0 ? "+" + db : String(db)}</text>);
      }
    }
    if (y >= padT - 0.5) {
      xLines.push(<line key={"hy" + db} x1={padL} y1={y} x2={padL + plotW} y2={y} class={"eq-grid" + (major ? "" : " dim")} />);
      if (major && db % 12 === 0) {
        yLabels.push(<text key={"yt" + db} x={padL - 5} y={y} text-anchor="end" dominant-baseline="middle" class="eq-axis-y">{db > 0 ? "+" + db : String(db)}</text>);
      }
    }
  }
  return (
    <div class={"comp-viz" + (enabled ? "" : " disabled")}>
      <svg class="comp-graph" viewBox={`0 0 ${w} ${h}`} aria-hidden="true">
        {xLines}
        <line x1={padL} y1={padT + plotH - map(dbMin) * plotH} x2={padL + plotW} y2={padT + plotH - map(dbMax) * plotH} class="eq-zero" />
        {xLabels}
        {yLabels}
        <polyline fill="none" points={points.join(" ")} class={"eq-curve" + (enabled ? "" : " flat")} />
        <line x1={tx} y1={padT} x2={tx} y2={padT + plotH} class="comp-thresh" />
        <text x={tx + 3} y={padT + 8} class="comp-thresh-lbl">T</text>
      </svg>
      <p class="comp-readout">
        {Math.round(amt * 100)}%
        <span>thresh {thresh.toFixed(1)} dB</span>
        <span>makeup +{makeup.toFixed(1)} dB</span>
      </p>
    </div>
  );
}

function CompGraphMini({ amount, on, overlay }) {
  const amt = Math.max(0, Math.min(1, Number(amount) || 0));
  const thresh = amt * -30;
  const makeup = amt * 10;
  const w = 240;
  const h = 64;
  const map = (db) => {
    const t = (db + 30) / 36;
    return Math.max(0, Math.min(1, t));
  };
  const ratio = 4;
  const padL = 0;
  const padT = 0;
  const padB = 12;
  const plotW = w - padL;
  const plotH = h - padB - padT;
  const points = [];
  for (let i = 0; i <= 24; i++) {
    const input = -30 + (i / 24) * 36;
    let output = input;
    if (input > thresh) output = thresh + (input - thresh) / ratio;
    output += makeup;
    points.push(`${(padL + map(input) * plotW).toFixed(1)},${(padT + plotH - map(output) * plotH).toFixed(1)}`);
  }
  const tx = padL + map(thresh) * plotW;
  const enabled = on !== false;
  return (
    <div class={"comp-viz mini" + (enabled ? "" : " disabled")}>
      <svg class="comp-graph mini" viewBox={`0 0 ${w} ${h}`} aria-hidden="true">
        <line x1={padL} y1={padT + plotH} x2={padL + plotW} y2={padT} class="eq-zero" />
        <polyline fill="none" points={points.join(" ")} class={"eq-curve" + (enabled ? "" : " flat")} />
        <line x1={tx} y1={padT} x2={tx} y2={padT + plotH} class="comp-thresh" />
        <text x={w / 2} y={h - 2} text-anchor="middle" class="eq-mini-db" font-size="16">{Math.round(amt * 100)}%</text>
        {overlay ? (
          <text class="eq-mini-title" x={w / 2} y="3" text-anchor="middle" dominant-baseline="hanging">{overlay}</text>
        ) : null}
      </svg>
    </div>
  );
}

function voicingAt(f, amp, outdoor) {
  const x = Math.log(f / 20) / Math.log(1000);
  const mid = Math.exp(-Math.pow((x - 0.46) * 3.1, 2));
  const air = Math.exp(-Math.pow((x - 0.88) * 4.2, 2));
  const low = Math.exp(-Math.pow((x - 0.12) * 5, 2));
  const paAmp = amp ? 2.1 * mid : -2.3 * mid + 1.3 * (low + air);
  const loc = outdoor ? 1.6 * air + 0.8 * low : 0.2 * mid;
  return paAmp + loc;
}

export function VoicingGraph({ amp, outdoor, mini }) {
  const w = 240;
  const h = mini ? 52 : 70;
  const x0 = mini ? 0 : 26;
  const y0 = mini ? 0 : 4;
  const plotW = w - x0 - (mini ? 0 : 4);
  const plotH = h - (mini ? 0 : 14);
  const pts = freqs(mini ? 48 : 72).map((f) => {
    const db = voicingAt(f, amp, outdoor);
    return `${xOf(f, w, x0, plotW).toFixed(1)},${(y0 + yOf(db, plotH, -4, 4)).toFixed(1)}`;
  });
  return (
    <svg class={"eq-graph voice-graph" + (mini ? " mini" : "")} viewBox={`0 0 ${w} ${h}`} title="Master voicing (PA/Amp + Indoor/Outdoor)">
      {mini ? <line x1="0" y1={h / 2} x2={w} y2={h / 2} class="eq-zero" /> : <EqGrid w={w} h={h} y0={y0} plotH={plotH} x0={x0} plotW={plotW} dbMin={-4} dbMax={4} dbStep={2} />}
      <polyline fill="none" points={pts.join(" ")} class="eq-curve" />
    </svg>
  );
}
