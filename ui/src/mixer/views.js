export const CHANNELS = [
  { id: "ch1", kind: "input", n: 1, label: "1" },
  { id: "ch2", kind: "input", n: 2, label: "2" },
  { id: "ch3", kind: "input", n: 3, label: "3" },
  { id: "ch4", kind: "input", n: 4, label: "4" },
  { id: "aux", kind: "aux", label: "AUX" },
  { id: "loop", kind: "loop", label: "LOOP" },
  { id: "phones", kind: "phones", label: "PHONES" },
  { id: "main", kind: "main", label: "MAIN" }
];

export const CONTROL_KEYS = [
  { id: "gain", label: "Gain", kinds: ["input"] },
  { id: "eq", label: "EQ", kinds: ["input", "aux"] },
  { id: "comp", label: "Compressor", kinds: ["input"] },
  { id: "fx1", label: "FX1", kinds: ["input"] },
  { id: "fx2", label: "FX2", kinds: ["input"] },
  { id: "ext", label: "Ext FX", kinds: ["input"] },
  { id: "mute", label: "Mute", kinds: ["input", "aux", "main"] },
  { id: "fader", label: "Fader", kinds: ["input", "aux", "main", "loop", "phones"] }
];

const ALL_IDS = CHANNELS.map((ch) => ch.id);

function controls(flags) {
  const fxOn = flags && flags.fx === false ? false : true;
  return {
    gain: true,
    eq: true,
    comp: true,
    fx1: fxOn,
    fx2: fxOn,
    ext: false,
    mute: true,
    fader: true,
    ...flags
  };
}

export const BUILTIN_VIEWS = [
  { id: "full", name: "Full mixer", builtin: true, channels: ALL_IDS, controls: controls() },
  {
    id: "live",
    name: "Live",
    builtin: true,
    channels: ALL_IDS,
    controls: controls({ comp: false, fx1: false, fx2: false, ext: false })
  },
  {
    id: "faders",
    name: "Faders",
    builtin: true,
    channels: ALL_IDS,
    controls: controls({ gain: false, eq: false, comp: false, fx1: false, fx2: false, ext: false })
  },
  {
    id: "inputs",
    name: "Inputs",
    builtin: true,
    channels: ["ch1", "ch2", "ch3", "ch4", "main"],
    controls: controls()
  }
];

const VIEWS_KEY = "sxm-mixer-views";
const ACTIVE_KEY = "sxm-mixer-view";
const NAMES_KEY = "sxm-chan-names";
const PINS_KEY = "sxm-view-pins";
const SNAP_KEY = "sxm-snap";
const DBLTAP_KEY = "sxm-dbltap";

export const BOARD_VIEWS = [
  { id: "gain", name: "Gain" },
  { id: "fx", name: "FX sends" },
  { id: "eq", name: "EQ" },
  { id: "comp", name: "Compressor" },
  { id: "snap", name: "Snapshots" },
  { id: "tuner", name: "Tuner" },
  { id: "loop", name: "Looper" }
];

const DEFAULT_PINS = ["full", "snap", "tuner", "loop"];

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function normalizeView(raw) {
  if (!raw || !raw.id) return null;
  const channels = Array.isArray(raw.channels) ? raw.channels.filter((id) => CHANNELS.some((ch) => ch.id === id)) : ALL_IDS.slice();
  const flags = { ...(raw.controls || {}) };
  if (flags.fx === false && flags.fx1 == null && flags.fx2 == null) {
    flags.fx1 = false;
    flags.fx2 = false;
    flags.ext = false;
  }
  return {
    id: String(raw.id),
    name: String(raw.name || "View").slice(0, 32),
    builtin: !!raw.builtin,
    channels: channels.length ? channels : ALL_IDS.slice(),
    controls: controls(flags)
  };
}

export function loadViews() {
  let custom = [];
  try {
    custom = JSON.parse(localStorage.getItem(VIEWS_KEY) || "[]");
  } catch {
    custom = [];
  }
  if (!Array.isArray(custom)) custom = [];
  const byId = new Map();
  for (const view of BUILTIN_VIEWS) byId.set(view.id, clone(view));
  for (const raw of custom) {
    const view = normalizeView(raw);
    if (!view) continue;
    if (byId.has(view.id) && byId.get(view.id).builtin) {
      byId.set(view.id, { ...byId.get(view.id), ...view, builtin: true, id: view.id });
    } else {
      byId.set(view.id, { ...view, builtin: false });
    }
  }
  return Array.from(byId.values());
}

export function saveViews(views) {
  const payload = (views || []).map((view) => normalizeView(view)).filter(Boolean);
  try {
    localStorage.setItem(VIEWS_KEY, JSON.stringify(payload));
  } catch {
    /* ignore */
  }
  return payload;
}

export function defaultViewId() {
  try {
    const saved = localStorage.getItem(ACTIVE_KEY);
    if (saved) return saved;
  } catch {
    /* ignore */
  }
  return typeof window !== "undefined" && window.innerHeight < 560 ? "live" : "full";
}

export function saveActiveView(id) {
  try {
    localStorage.setItem(ACTIVE_KEY, id);
  } catch {
    /* ignore */
  }
}

export function loadNames() {
  try {
    const raw = JSON.parse(localStorage.getItem(NAMES_KEY) || "{}");
    return raw && typeof raw === "object" ? raw : {};
  } catch {
    return {};
  }
}

export function saveNames(names) {
  try {
    localStorage.setItem(NAMES_KEY, JSON.stringify(names || {}));
  } catch {
    /* ignore */
  }
}

export function visibleChannels(view) {
  const ids = new Set((view && view.channels) || ALL_IDS);
  return CHANNELS.filter((ch) => ids.has(ch.id));
}

export function controlOn(view, key, kind) {
  const spec = CONTROL_KEYS.find((item) => item.id === key);
  if (spec && kind && !spec.kinds.includes(kind)) return false;
  const flags = (view && view.controls) || {};
  if (key === "fader") return flags.fader !== false;
  if (key === "fx1" || key === "fx2") {
    if (flags[key] != null) return !!flags[key];
    return flags.fx !== false;
  }
  if (key === "ext") return !!flags.ext;
  return !!flags[key];
}

export function newView(from) {
  const base = from ? clone(from) : clone(BUILTIN_VIEWS[0]);
  return {
    ...base,
    id: "view-" + Date.now().toString(36),
    name: from ? `${from.name} copy` : "Custom view",
    builtin: false
  };
}

export function gapBefore(prev, ch) {
  if (!prev) return false;
  return (prev.id === "ch4" && ch.id === "aux")
    || (prev.id === "aux" && ch.id === "loop")
    || (prev.id === "loop" && ch.id === "phones");
}

export function loadPins() {
  try {
    const raw = JSON.parse(localStorage.getItem(PINS_KEY) || "null");
    if (Array.isArray(raw)) {
      const pins = raw.map(String).filter(Boolean);
      if (!pins.includes("full")) pins.unshift("full");
      return pins;
    }
  } catch {
    /* ignore */
  }
  return DEFAULT_PINS.slice();
}

export function savePins(ids) {
  const pins = [];
  const seen = new Set();
  for (const id of ["full", ...((ids || []).map(String))]) {
    if (!id || seen.has(id)) continue;
    seen.add(id);
    pins.push(id);
  }
  try {
    localStorage.setItem(PINS_KEY, JSON.stringify(pins));
  } catch {
    /* ignore */
  }
  return pins;
}

export function loadSnap() {
  try {
    const raw = localStorage.getItem(SNAP_KEY);
    if (raw === "0") return false;
    if (raw === "1") return true;
  } catch {
    /* ignore */
  }
  return true;
}

export function saveSnap(on) {
  try {
    localStorage.setItem(SNAP_KEY, on ? "1" : "0");
  } catch {
    /* ignore */
  }
}

let dblTapEnabled = false;

export function loadDblTap() {
  try {
    dblTapEnabled = localStorage.getItem(DBLTAP_KEY) === "1";
  } catch {
    dblTapEnabled = false;
  }
  return dblTapEnabled;
}

export function saveDblTap(on) {
  dblTapEnabled = !!on;
  try {
    localStorage.setItem(DBLTAP_KEY, dblTapEnabled ? "1" : "0");
  } catch {
    /* ignore */
  }
}

export function isDblTapEnabled() {
  return dblTapEnabled;
}
