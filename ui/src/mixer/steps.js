const MAIN_MASTER_GAIN = [
  -120, -68.09374, -49.12886, -38.0351067, -30.1639671, -24.0586376, -19.0702229,
  -14.85257, -11.1990757, -7.97647, -5.093745, -2.48601365, -0.10532999, 2.084685, 4.112315, 6
];

const INPUT_GAIN = [0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60];

const INPUT_VOLUME = [
  -120, -66.44593, -46.878983, -35.433048, -27.312031, -21.01288, -15.866102,
  -11.514557, -7.745078, -4.420167, -1.445928, 1.244589, 3.70085, 5.960389, 8.052388, 10
];

const UNIT = [0, 0.066667, 0.133333, 0.2, 0.266667, 0.333333, 0.4, 0.466667, 0.533333, 0.6, 0.666667, 0.733333, 0.8, 0.866667, 0.933333, 1];

const EQ_GAIN = [
  -15, -12.857147, -10.71429, -8.571432, -6.428574, -4.285716, -2.142858,
  0, 2.142857, 4.285714, 6.428571, 8.571428, 10.714285, 12.857142, 15
];

const EXT_SENDS = [0, 0.066667, 0.133333, 0.2, 0.266667, 0.333333, 0.4, 0.466667, 0.533333, 0.6, 0.666667, 0.733333, 0.8, 0.866667, 0.933333, 1];

export const LOOP_HALF = 0.5;
export const LOOP_HALF_RADIUS = 0.015;
export const LOOP_FLOOR = -120;
export const LOOP_CEIL = 10;
export const LOOP_MIN = LOOP_FLOOR;
export const LOOP_MAX = LOOP_CEIL;
export const LOOP_UNITY = 0;
export const PHONES_SPAN = 130;
export const PHONES_FLOOR = -120;
export const PHONES_CEIL = 10;
export const PHONES_UNITY = 0;
export const PHONES_STEP = 0.5;

export function clamp01(n) {
  return Math.min(1, Math.max(0, n));
}

export const ANALOG_FLOOR_GRAB = -115;
export const ANALOG_CEIL_GRAB = 9.25;

export function loopPercent(value) {
  const n = Number(value);
  if (!Number.isFinite(n) || n <= ANALOG_FLOOR_GRAB) return 0;
  if (n >= ANALOG_CEIL_GRAB) return 200;
  if (n <= 0) return ((n - LOOP_FLOOR) / (0 - LOOP_FLOOR)) * 100;
  return 100 + (n / LOOP_CEIL) * 100;
}

export function loopRatio(value) {
  return loopPercent(value) / 200;
}

export function valueFromLoopRatio(ratio) {
  const t = clamp01(ratio);
  if (t <= 0.5) return LOOP_FLOOR + (t / 0.5) * (0 - LOOP_FLOOR);
  return ((t - 0.5) / 0.5) * LOOP_CEIL;
}

export function phonesRatio(value) {
  const n = Number(value);
  if (!Number.isFinite(n) || n <= ANALOG_FLOOR_GRAB) return 0;
  if (n >= ANALOG_CEIL_GRAB) return 1;
  if (n <= PHONES_UNITY) return 0.5 * ((n - PHONES_FLOOR) / (PHONES_UNITY - PHONES_FLOOR));
  return 0.5 + 0.5 * (n / PHONES_CEIL);
}

export function valueFromPhonesRatio(ratio) {
  const t = clamp01(ratio);
  if (t <= 0.004) return PHONES_FLOOR;
  const raw = t <= 0.5
    ? PHONES_FLOOR + (t / 0.5) * (PHONES_UNITY - PHONES_FLOOR)
    : ((t - 0.5) / 0.5) * PHONES_CEIL;
  return Math.round(raw / PHONES_STEP) * PHONES_STEP;
}

let snapEnabled = true;

export function setSnapEnabled(on) {
  snapEnabled = !!on;
}

export function isSnapEnabled() {
  return snapEnabled;
}

function nearest(value, steps) {
  return steps[nearestIndex(value, steps)];
}

export function nearestIndex(value, steps) {
  let best = 0;
  let bestD = Math.abs(value - steps[0]);
  for (let i = 1; i < steps.length; i++) {
    const d = Math.abs(value - steps[i]);
    if (d < bestD) {
      bestD = d;
      best = i;
    }
  }
  return best;
}

function inputOffset(id) {
  const match = /^INPUT([1-4])_(.+)$/.exec(id);
  if (!match) return null;
  return match[2];
}

export function stepsForEntity(id) {
  if (id === "MAIN_HEADPHONE_GAIN") return null;
  if (id === "MAIN_MASTER_GAIN") return MAIN_MASTER_GAIN;
  if (id === "LOOPER_LEVEL") return null;
  if (id === "STEREO_INPUT1_VOLUME") return INPUT_VOLUME;
  if (id === "STEREO_INPUT1_EQ_LOW_GAIN" || id === "STEREO_INPUT1_EQ_MID_GAIN" || id === "STEREO_INPUT1_EQ_HIGH_GAIN") {
    return EQ_GAIN;
  }
  const field = inputOffset(id);
  if (!field) return null;
  if (field === "GAIN") return INPUT_GAIN;
  if (field === "VOLUME") return INPUT_VOLUME;
  if (field === "EQ_LOW_GAIN" || field === "EQ_MID_GAIN" || field === "EQ_HIGH_GAIN") return EQ_GAIN;
  if (field === "COMPRESSOR_AMOUNT" || field === "EFFECT_1_AMOUNT" || field === "EFFECT_2_AMOUNT") return UNIT;
  if (field === "EXT_FX_SENDS") return EXT_SENDS;
  return null;
}

export function extraSnapForEntity() {
  return null;
}

export function centerValue(entity, min, max, unit) {
  if (unit === "loop" || entity === "LOOPER_LEVEL") return LOOP_UNITY;
  if (unit === "phones" || entity === "MAIN_HEADPHONE_GAIN") return PHONES_UNITY;
  const steps = entity ? stepsForEntity(entity) : null;
  const lo = min == null ? 0 : Number(min);
  const hi = max == null ? 1 : Number(max);
  if (steps && steps.length >= 2) {
    // Middle hardware step (1-based step 6 of 12 → index 5).
    return steps[Math.floor((steps.length - 1) / 2)];
  }
  return (lo + hi) / 2;
}

export function entityCanSnap(id) {
  if (!id || id === "LOOPER_LEVEL" || id === "MAIN_HEADPHONE_GAIN") return false;
  return !!stepsForEntity(id);
}

export function visualRatio(value, steps, extra) {
  if (!steps || steps.length < 2) return null;
  const n = Number(value);
  if (!Number.isFinite(n)) return 0;
  if (extra != null && Math.abs(n - extra) <= LOOP_HALF_RADIUS) return extra;
  return nearestIndex(n, steps) / (steps.length - 1);
}

export function valueFromVisualRatio(ratio, steps, extra) {
  if (!steps || steps.length < 2) return null;
  const last = steps.length - 1;
  const t = Math.min(1, Math.max(0, ratio));
  if (extra != null && Math.abs(t - extra) <= 0.02) return extra;
  return steps[Math.min(last, Math.max(0, Math.round(t * last)))];
}

export function linearRatio(value, min, max) {
  const n = Number(value);
  if (!Number.isFinite(n) || max === min) return 0;
  return Math.min(1, Math.max(0, (n - min) / (max - min)));
}

export function displayRatio(value, steps, extra) {
  if (!steps || steps.length < 2) return 0;
  const n = Number(value);
  if (!Number.isFinite(n)) return 0;
  const last = steps.length - 1;
  const ratioAt = (i) => i / last;
  if (extra != null && Math.abs(n - extra) <= LOOP_HALF_RADIUS) return extra;
  if (n <= steps[0]) return 0;
  if (n >= steps[last]) return 1;
  for (let i = 0; i < last; i++) {
    const a = steps[i];
    const b = steps[i + 1];
    const lo = Math.min(a, b);
    const hi = Math.max(a, b);
    if (n >= lo && n <= hi) {
      const t = a === b ? 0 : (n - a) / (b - a);
      return ratioAt(i) + (ratioAt(i + 1) - ratioAt(i)) * t;
    }
  }
  return ratioAt(nearestIndex(n, steps));
}

export function valueFromDisplayRatio(ratio, steps, extra, quantize) {
  if (!steps || steps.length < 2) return null;
  if (quantize) return valueFromVisualRatio(ratio, steps, extra);
  const t = Math.min(1, Math.max(0, ratio));
  if (extra != null && Math.abs(t - extra) <= 0.02) return extra;
  const last = steps.length - 1;
  const x = t * last;
  const i = Math.min(last - 1, Math.max(0, Math.floor(x)));
  const f = x - i;
  return steps[i] + (steps[i + 1] - steps[i]) * f;
}

export function snapEntity(id, value) {
  if (typeof value === "boolean") return value;
  const n = Number(value);
  if (!Number.isFinite(n)) return value;
  if (!isSnapEnabled() || !entityCanSnap(id)) return n;
  const steps = stepsForEntity(id);
  if (!steps) return n;
  return nearest(n, steps);
}
