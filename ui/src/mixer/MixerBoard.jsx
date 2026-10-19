import { useEffect, useMemo, useRef, useState } from "preact/hooks";
import { getJson, postJson, wsSendMsg } from "../api.js";
import { useLive } from "../live/LiveContext.jsx";
import { StatusCluster } from "../status/StatusCluster.jsx";
import { Pad } from "./Controls.jsx";
import { ChannelStrip } from "./ChannelStrip.jsx";
import { ProcSheet } from "./ProcSheet.jsx";
import { FxSendsBoard } from "./FxSends.jsx";
import { EqBoard } from "./EqBoard.jsx";
import { GainBoard } from "./GainBoard.jsx";
import { CompBoard } from "./CompBoard.jsx";
import { LooperView, SnapshotsView, TunerView } from "./ToolViews.jsx";
import { Dropdown } from "../ui/Dropdown.jsx";
import {
  IconBars, IconPen, IconExpand, IconCompress, IconBulb, IconCircle
} from "./Icons.jsx";
import { PwaInstallPad, isStandalone } from "../pwa/Install.jsx";
import {
  BOARD_VIEWS, CHANNELS, CONTROL_KEYS, defaultViewId, gapBefore, loadNames, loadPins,
  loadDblTap, loadSnap, loadViews, newView, saveActiveView, saveDblTap, saveNames, savePins, saveSnap, saveViews,
  visibleChannels
} from "./views.js";
import { mockEntities } from "./mock.js";
import { entityCanSnap, isSnapEnabled, nearestIndex, setSnapEnabled, snapEntity, stepsForEntity } from "./steps.js";

const TUNER_PC = ["", "A", "A#", "B", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#"];
const TUNER_DEFAULT_CHAN = 1;
const TUNER_CHAN_KEY = "sxm-tuner-chan";
const FS_SKIP_KEY = "sxm-fs-skip";
const AUTOGAIN_MS = 10000;

// Echoes of our own writes are confirmations, never commands. While a local
// write is still pending (debounced POST in flight, mixer quantizing), every
// echo for that entity is ignored unless it is the authoritative value of the
// latest write. A fast drag emits many volume events; their stale intermediate
// echoes used to land after newer ones and yank the fader back.
const ECHO_PENDING_MS = 1500;
const ECHO_EPS = 0.01;
const pendingWrites = new Map();

function authoritativeOf(id, value) {
  const steps = stepsForEntity(id);
  if (!steps || steps.length < 2) return Number(value);
  return steps[nearestIndex(Number(value), steps)];
}

function expirePending(id) {
  const pending = pendingWrites.get(id);
  if (pending && Date.now() - pending.at > ECHO_PENDING_MS) pendingWrites.delete(id);
  return pendingWrites.get(id);
}

// Returns the value to apply, or undefined when the echo must be dropped.
function reconcileEcho(id, value) {
  const pending = expirePending(id);
  if (!pending) return value;
  const want = authoritativeOf(id, pending.value);
  const got = Number(value);
  if (Number.isFinite(want) && Number.isFinite(got) && Math.abs(want - got) <= ECHO_EPS) {
    // Confirmation of the latest write: adopt the mixer's authoritative value.
    pendingWrites.delete(id);
    return value;
  }
  // Stale echo of an older step: never re-apply past actions.
  return undefined;
}

function hasPendingWrite(id) {
  return !!expirePending(id);
}

function loadFsSkip() {
  try {
    return localStorage.getItem(FS_SKIP_KEY) === "1";
  } catch {
    return false;
  }
}

function loadTunerChan() {
  try {
    const n = Number(localStorage.getItem(TUNER_CHAN_KEY));
    if (Number.isInteger(n) && n >= 0 && n <= 3) return n;
  } catch { /* ignore */ }
  return TUNER_DEFAULT_CHAN;
}

function saveTunerChan(chan) {
  if (chan < 0 || chan > 3) return;
  try { localStorage.setItem(TUNER_CHAN_KEY, String(chan)); } catch { /* ignore */ }
}

function val(entities, id, fallback) {
  const row = entities[id];
  if (!row) return fallback;
  return row.value;
}

function truthy(value) {
  return value === true || value === 1 || value === "1" || value === "true";
}

function pitchClassLabel(noteId) {
  const n = Number(noteId);
  if (!Number.isInteger(n) || n < 1 || n > 12) return "—";
  return TUNER_PC[n];
}

export function MixerBoard({ onMenu, stage, onToggleStage }) {
  const { subscribe, link: liveLink, esp } = useLive();
  const [entities, setEntities] = useState(() => import.meta.env.DEV ? mockEntities() : {});
  const [error, setError] = useState("");
  const [selected, setSelected] = useState(0);
  const [link, setLink] = useState({
    ok: false, battery: null, tunerOn: false, tunerChan: loadTunerChan(), sdCard: 0,
    tunerMidi: 0, tunerNoteId: 0, tunerCents: 0, tunerHz: 0, tunerHeard: false,
    analogHoldPhones: true, analogHoldLoop: true
  });
  const [views, setViews] = useState(() => loadViews());
  const [activeId, setActiveId] = useState(() => defaultViewId());
  const [names, setNames] = useState(() => loadNames());
  const [viewMenu, setViewMenu] = useState(false);
  const [editorOpen, setEditorOpen] = useState(false);
  const [draft, setDraft] = useState(null);
  const [snapAction, setSnapAction] = useState("RECALL");
  const [snapStep, setSnapStep] = useState("pick");
  const [loopRecAt, setLoopRecAt] = useState(0);
  const [loopLengthMs, setLoopLengthMs] = useState(0);
  const [fsHint, setFsHint] = useState(() => {
    if (import.meta.env.DEV) {
      try {
        const q = new URLSearchParams(location.search);
        if (q.get("proc") || q.has("nofs")) return false;
      } catch { /* ignore */ }
    }
    return !loadFsSkip() && !isStandalone();
  });
  const [proc, setProc] = useState(() => {
    if (!import.meta.env.DEV) return null;
    try {
      const q = new URLSearchParams(location.search);
      const section = q.get("proc");
      return section ? { id: q.get("ch") || "ch1", section } : null;
    } catch {
      return null;
    }
  });
  const [board, setBoard] = useState(() => {
    if (!import.meta.env.DEV) return "mixer";
    try {
      return new URLSearchParams(location.search).get("board") || "mixer";
    } catch {
      return "mixer";
    }
  });
  const [warn, setWarn] = useState("");
  const [pins, setPins] = useState(() => loadPins());
  const [snapOn, setSnapOn] = useState(() => {
    const on = loadSnap();
    setSnapEnabled(on);
    return on;
  });
  const [dblTapOn, setDblTapOn] = useState(() => loadDblTap());
  const [snapNames, setSnapNames] = useState(["1", "2", "3", "4", "5"]);
  const [autoGain, setAutoGain] = useState({ chan: -1, until: 0 });
  const [advancedOpen, setAdvancedOpen] = useState(false);
  const pending = useRef({});
  const latestWrite = useRef({});
  const writing = useRef({});
  const viewBtn = useRef(null);
  const boardRef = useRef(board);
  const linkRef = useRef(link);
  const dumpSeqRef = useRef(0);
  const linkOkRef = useRef(false);
  const loopClock = useRef({ recAt: 0, lengthMs: 0 });
  const sdRecAt = useRef(0);
  const [recNow, setRecNow] = useState(0);
  const loopHold = useRef({ expect: null, until: 0 });
  const snapReturn = useRef("mixer");
  const snapWait = useRef(0);
  const tunerHold = useRef({ chan: null, until: 0 });
  boardRef.current = board;
  linkRef.current = link;

  const view = views.find((item) => item.id === activeId) || views[0];
  const strips = useMemo(() => visibleChannels(view), [view]);
  const catalog = useMemo(() => {
    const mixer = views.map((item) => ({ id: item.id, name: item.name, kind: "mixer" }));
    const boards = BOARD_VIEWS.filter((item) => !mixer.some((row) => row.id === item.id));
    return mixer.concat(boards.map((item) => ({ ...item, kind: "board" })));
  }, [views]);
  const pinButtons = useMemo(() => {
    const byId = new Map(catalog.map((item) => [item.id, item]));
    const ordered = [];
    for (const id of ["full", ...pins]) {
      const item = byId.get(id);
      if (!item || ordered.some((row) => row.id === id)) continue;
      ordered.push(item);
    }
    return ordered;
  }, [catalog, pins]);

  function liveLooperOk(value) {
    const hold = loopHold.current;
    if (hold.expect == null || Date.now() >= hold.until) {
      hold.expect = null;
      return true;
    }
    return Number(value) === hold.expect;
  }

  function merge(list) {
    setEntities((prev) => {
      const next = { ...prev };
      for (const row of list || []) {
        if (row.id === "LOOPER_STATE" && !liveLooperOk(row.value)) continue;
        if (hasPendingWrite(row.id)) continue;
        next[row.id] = { ...row, value: row.value };
      }
      return next;
    });
  }

  async function refresh() {
    const data = await getJson("/api/v1/showbox/entities");
    setEntities((prev) => {
      const rows = data.entities || [];
      if (!rows.length && Object.keys(prev).length) return prev;
      const next = {};
      for (const row of rows) {
        if (row.id === "LOOPER_STATE" && !liveLooperOk(row.value)) {
          next[row.id] = prev[row.id] || row;
          continue;
        }
        if (hasPendingWrite(row.id) && prev[row.id]) {
          next[row.id] = prev[row.id];
          continue;
        }
        next[row.id] = row;
      }
      return next;
    });
    setError("");
  }

  useEffect(() => {
    refresh().catch((err) => setError(String(err.message || err)));
    const stop = subscribe((msg) => {
      if (msg.type === "mixer") merge(msg.entities);
      if (msg.type === "entity") {
        if (msg.id === "LOOPER_STATE" && !liveLooperOk(msg.value)) return;
        const shown = reconcileEcho(msg.id, msg.value);
        if (shown === undefined) return;
        setEntities((prev) => ({
          ...prev,
          [msg.id]: { ...(prev[msg.id] || { id: msg.id }), value: shown }
        }));
      }
      if (msg.type === "link") {
        const hold = tunerHold.current;
        const holding = hold.chan != null && Date.now() < hold.until;
        const nowOk = !!msg.ok;
        const rose = nowOk && !linkOkRef.current;
        linkOkRef.current = nowOk;
        setLink((prev) => ({
          ...prev,
          ...msg,
          battery: msg.battery == null || msg.battery === "" ? null : Number(msg.battery),
          tunerChan: holding ? hold.chan : msg.tunerChan,
          tunerOn: holding ? true : msg.tunerOn,
          tunerHeard: holding && Number(msg.tunerChan) !== hold.chan ? false : msg.tunerHeard
        }));
        const seq = Number(msg.dumpSeq);
        const dumped = Number.isFinite(seq) && seq > dumpSeqRef.current;
        if (dumped) dumpSeqRef.current = seq;
        if (dumped || rose) refresh().catch(() => {});
      }
      if (msg.type === "names") {
        if (msg.channels && typeof msg.channels === "object") {
          const next = {};
          for (const [id, name] of Object.entries(msg.channels)) {
            if (name) next[id] = String(name);
          }
          setNames(next);
          saveNames(next);
        }
        if (Array.isArray(msg.snapshots)) {
          setSnapNames(msg.snapshots.map((name, i) => String(name || i + 1)));
        }
      }
      if (msg.type === "snapshot" && msg.ok) {
        // The recall restored the mixer's state; the dump that follows sends
        // the recalled values. Clear pending writes so the echo reconciler
        // doesn't drop them as stale echoes of the user's last fader moves.
        pendingWrites.clear();
        if (boardRef.current === "snap") {
          clearTimeout(snapWait.current);
          const back = snapReturn.current && snapReturn.current !== "snap" ? snapReturn.current : "mixer";
          setBoard(back === "tuner" ? "mixer" : back);
        }
      }
    });
    getJson("/api/v1/showbox/names").then((data) => {
      if (data.channels && typeof data.channels === "object") {
        const next = {};
        for (const [id, name] of Object.entries(data.channels)) {
          if (name) next[id] = String(name);
        }
        setNames(next);
        saveNames(next);
      }
      if (Array.isArray(data.snapshots)) setSnapNames(data.snapshots.map((name, i) => String(name || i + 1)));
    }).catch(() => {});
    const retries = [600, 1800, 4000, 8000].map((ms) => setTimeout(() => refresh().catch(() => {}), ms));
    const id = setInterval(() => {
      if (dumpSeqRef.current === 0) refresh().catch(() => {});
    }, 2000);
    const slow = setInterval(() => refresh().catch(() => {}), 30000);
    if (import.meta.env.DEV) {
      const q = new URLSearchParams(location.search);
      const kind = q.get("overlay") || q.get("board");
      if (kind === "tuner" || kind === "loop" || kind === "snap" || kind === "fx" || kind === "eq") setBoard(kind);
      if (kind === "warn") setWarn("No SD card inserted.");
      if (q.get("fs") === "1") setFsHint(true);
      if (q.get("fs") === "0") setFsHint(false);
      if (q.get("adv") === "1") setAdvancedOpen(true);
      const section = q.get("proc");
      if (section) setProc({ id: q.get("ch") || "ch1", section, slot: q.get("slot") || undefined });
      if (q.get("fx") === "1") setBoard("fx");
      if (q.get("eq") === "1") setBoard("eq");
      if (q.get("rec") === "1") setLink((prev) => ({ ...prev, sdCard: 2 }));
      if (q.get("phone") === "1") document.documentElement.classList.add("phone-preview");
    }
    return () => {
      stop();
      clearInterval(id);
      clearInterval(slow);
      retries.forEach(clearTimeout);
      clearTimeout(snapWait.current);
    };
  }, []);

  useEffect(() => {
    if (!esp.connected) return;
    refresh().catch(() => {});
  }, [esp.connected, esp.since]);

  function write(id, value) {
    const snapped = isSnapEnabled() ? snapEntity(id, value) : value;
    setEntities((prev) => ({ ...prev, [id]: { ...(prev[id] || { id }), value: snapped } }));
    pendingWrites.set(id, { value: snapped, at: Date.now() });
    latestWrite.current[id] = snapped;
    clearTimeout(pending.current[id]);
    pending.current[id] = setTimeout(() => {
      const flush = (entity) => {
        if (writing.current[entity]) return;
        const next = latestWrite.current[entity];
        if (next === undefined) return;
        delete latestWrite.current[entity];
        writing.current[entity] = true;
        postJson(`/api/v1/showbox/entities/${entity}`, { value: next })
          .catch(() => (
            latestWrite.current[entity] === undefined
              ? postJson(`/api/v1/showbox/entities/${entity}`, { value: next })
              : Promise.resolve()
          ))
          .then(() => setError(""))
          .catch((err) => setError(err.message))
          .finally(() => {
            writing.current[entity] = false;
            if (latestWrite.current[entity] !== undefined) flush(entity);
          });
      };
      flush(id);
    }, 90);
  }

  function setAnalogHold(phones, loop) {
    setLink((prev) => ({ ...prev, analogHoldPhones: phones, analogHoldLoop: loop }));
    postJson("/api/v1/showbox/analog-hold", { phones, loop, on: phones && loop }).then(() => setError("")).catch((err) => setError(err.message));
  }

  function setIgnoreShake(on) {
    setAnalogHold(on, on);
  }

  function toggleSnap() {
    const next = !snapOn;
    setSnapEnabled(next);
    setSnapOn(next);
    saveSnap(next);
    if (!next) return;
    setEntities((prev) => {
      const out = { ...prev };
      for (const id of Object.keys(out)) {
        if (!entityCanSnap(id)) continue;
        const v = snapEntity(id, out[id].value);
        if (Number(v) === Number(out[id].value)) continue;
        out[id] = { ...out[id], value: v };
        postJson(`/api/v1/showbox/entities/${id}`, { value: v }).catch((err) => setError(err.message));
      }
      return out;
    });
  }

  function togglePin(id, on) {
    if (id === "full") return;
    const next = on ? pins.concat([id]) : pins.filter((item) => item !== id);
    setPins(savePins(next));
  }

  function startAutogain(chan) {
    const n = Number(chan);
    if (!Number.isInteger(n) || n < 0 || n > 3) return;
    setSelected(n);
    write("SELECTED_CHAN", n);
    setAutoGain({ chan: n, until: Date.now() + AUTOGAIN_MS });
    send("/api/v1/showbox/action/autogain", { chan: n });
  }

  async function send(url, body, extra) {
    try {
      await postJson(url, body);
      if (extra) extra();
      setError("");
    } catch (err) {
      setError(String(err.message || err));
    }
  }

  function chooseView(id) {
    setActiveId(id);
    saveActiveView(id);
    setViewMenu(false);
    setToolBoard("mixer");
  }

  function persistViews(next, id) {
    saveViews(next);
    setViews(loadViews());
    if (id) {
      setActiveId(id);
      saveActiveView(id);
    }
  }

  function openEditor(target) {
    const src = target || view;
    setDraft({
      id: src.id,
      name: src.name,
      builtin: !!src.builtin,
      channels: src.channels.slice(),
      controls: { ...src.controls }
    });
    setEditorOpen(true);
  }

  function saveDraft() {
    if (!draft) return;
    const next = views.map((item) => item.id === draft.id ? { ...item, ...draft } : item);
    if (!next.some((item) => item.id === draft.id)) next.push({ ...draft, builtin: false });
    persistViews(next, draft.id);
    setEditorOpen(false);
  }

  function duplicateView() {
    const copy = newView(draft || view);
    persistViews([...views, copy], copy.id);
    setDraft(copy);
    setEditorOpen(true);
  }

  function deleteView() {
    if (!draft || draft.builtin) return;
    const next = views.filter((item) => item.id !== draft.id);
    persistViews(next, next[0] && next[0].id);
    setEditorOpen(false);
  }

  function rename(id, value) {
    const trimmed = String(value || "").trim().slice(0, 16);
    const next = { ...names };
    const ch = CHANNELS.find((item) => item.id === id);
    if (!trimmed || (ch && trimmed === ch.label)) delete next[id];
    else next[id] = trimmed;
    setNames(next);
    saveNames(next);
    postJson("/api/v1/showbox/names", { id, name: trimmed }).catch((err) => setError(err.message));
  }

  function predictLooper(state, action) {
    if (action === "TAP") {
      if (state === 0) return 1;
      if (state === 1 || state === 2 || state === 4) return 3;
      if (state === 3) return 4;
    }
    if (action === "TAP2") {
      if (state === 0) return 1;
      if (state === 1 || state === 2 || state === 3) return 4;
    }
    if (action === "DOUBLE_PRESS" && (state === 1 || state === 3 || state === 4)) return 2;
    if (action === "LONG_PRESS") return 0;
    return state;
  }

  function applyLooperState(next) {
    const cur = Number(val(entities, "LOOPER_STATE", 0));
    loopHold.current = { expect: next, until: Date.now() + 4000 };
    setEntities((prev) => ({
      ...prev,
      LOOPER_STATE: { ...(prev.LOOPER_STATE || { id: "LOOPER_STATE" }), value: next }
    }));
    if (next === 1 && cur !== 1 && !loopClock.current.recAt) {
      loopClock.current.recAt = Date.now();
      loopClock.current.lengthMs = 0;
      setLoopRecAt(loopClock.current.recAt);
      setLoopLengthMs(0);
    }
    if (cur === 1 && next !== 1 && loopClock.current.recAt) {
      loopClock.current.lengthMs = Date.now() - loopClock.current.recAt;
      loopClock.current.recAt = 0;
      setLoopRecAt(0);
      setLoopLengthMs(loopClock.current.lengthMs);
    }
    if (next === 0) {
      loopClock.current.recAt = 0;
      loopClock.current.lengthMs = 0;
      setLoopRecAt(0);
      setLoopLengthMs(0);
    }
  }

  function looper(action) {
    const cur = Number(val(entities, "LOOPER_STATE", 0));
    const next = predictLooper(cur, action);
    if (next !== cur) applyLooperState(next);
    if (action === "TAP2") {
      send("/api/v1/showbox/action/looper_button", { action: "TAP" });
      setTimeout(() => send("/api/v1/showbox/action/looper_button", { action: "TAP" }), 180);
      return;
    }
    send("/api/v1/showbox/action/looper_button", { action });
  }

  function snapshotPick(kind) {
    setSnapAction(kind);
    setSnapStep("slots");
  }

  function snapshotSlot(slot) {
    if (snapAction === "SAVE" && slot === 5) return;
    // Snapshot actions go over WS (persistent) — HTTP POSTs fail during
    // radio windows and the action is silently lost.
    wsSendMsg("s" + JSON.stringify({ action: snapAction === "SAVE" ? "SAVE" : "RECALL", slot }));
    // On recall, drop pending writes: the dump that follows carries the
    // recalled values, not the echoes of the user's last fader moves.
    if (snapAction !== "SAVE") pendingWrites.clear();
    clearTimeout(snapWait.current);
    // The module now reports snapshot done when the mixer really finished
    // (saves take seconds); this timeout is only a fallback.
    snapWait.current = setTimeout(() => {
      if (boardRef.current !== "snap") return;
      const back = snapReturn.current && snapReturn.current !== "snap" ? snapReturn.current : "mixer";
      setBoard(back === "tuner" ? "mixer" : back);
    }, 7000);
  }

  function snapshotRename(slot, name) {
    const trimmed = String(name || "").trim().slice(0, 16);
    setSnapNames((prev) => {
      const next = prev.slice();
      next[slot] = trimmed || String(slot + 1);
      return next;
    });
    postJson("/api/v1/showbox/names", { snapshot: slot, name: trimmed }).catch((err) => setError(err.message));
  }

  function tuner(chan) {
    saveTunerChan(chan);
    tunerHold.current = { chan, until: Date.now() + 900 };
    setLink((prev) => ({
      ...prev,
      tunerOn: true,
      tunerChan: chan,
      tunerHeard: prev.tunerChan === chan ? prev.tunerHeard : false,
      tunerMidi: prev.tunerChan === chan ? prev.tunerMidi : 0,
      tunerNoteId: prev.tunerChan === chan ? prev.tunerNoteId : 0,
      tunerCents: prev.tunerChan === chan ? prev.tunerCents : 0,
      tunerHz: prev.tunerChan === chan ? prev.tunerHz : 0
    }));
    send("/api/v1/showbox/action/tuner", { action: "TURN_ON", chan });
  }

  function enableTuner(chan) {
    const next = chan >= 0 && chan <= 3 ? chan : loadTunerChan();
    saveTunerChan(next);
    tunerHold.current = { chan: next, until: Date.now() + 900 };
    setLink((prev) => ({ ...prev, tunerOn: true, tunerChan: next, tunerHeard: false, tunerMidi: 0, tunerNoteId: 0, tunerCents: 0, tunerHz: 0 }));
    send("/api/v1/showbox/action/tuner", { action: "TURN_ON", chan: next });
  }

  function disableTuner() {
    const chan = Number(linkRef.current.tunerChan);
    const saved = Number.isInteger(chan) && chan >= 0 && chan <= 3 ? chan : loadTunerChan();
    saveTunerChan(saved);
    tunerHold.current = { chan: null, until: 0 };
    setLink((prev) => ({ ...prev, tunerOn: false, tunerHeard: false, tunerMidi: 0, tunerNoteId: 0, tunerCents: 0, tunerHz: 0 }));
    send("/api/v1/showbox/action/tuner", { action: "TURN_OFF", chan: saved });
  }

  function setToolBoard(next, toggle) {
    const prev = boardRef.current;
    if (toggle && next === prev) next = "mixer";
    if (prev === "tuner" && next !== "tuner") disableTuner();
    if (next === "tuner" && prev !== "tuner") enableTuner(loadTunerChan());
    if (next === "snap" && prev !== "snap") {
      snapReturn.current = prev || "mixer";
      setSnapStep("pick");
    }
    setBoard(next);
    setViewMenu(false);
  }

  function dismissFs() {
    setFsHint(false);
  }

  function skipFsForever() {
    try { localStorage.setItem(FS_SKIP_KEY, "1"); } catch { /* ignore */ }
    setFsHint(false);
  }

  function enterFs() {
    dismissFs();
    onToggleStage && onToggleStage();
  }

  function recordSd() {
    const sdCard = Number(link.sdCard);
    const rec = sdCard === 2;
    if (!rec && link.sdHeard && sdCard === 0) {
      setWarn("The mixer reports no SD card. Record from the mixer itself to confirm.");
      return;
    }
    if (!rec) sdRecAt.current = Date.now();
    else sdRecAt.current = 0;
    setLink((prev) => ({ ...prev, sdCard: rec ? 1 : 2, sdHeard: true }));
    send("/api/v1/showbox/action/sdcard", {});
  }

  useEffect(() => {
    if (!autoGain.until) return undefined;
    const wait = autoGain.until - Date.now();
    if (wait <= 0) {
      setAutoGain({ chan: -1, until: 0 });
      return undefined;
    }
    const id = setTimeout(() => setAutoGain({ chan: -1, until: 0 }), wait);
    return () => clearTimeout(id);
  }, [autoGain]);

  const sel = Number(val(entities, "SELECTED_CHAN", selected)) || 0;
  const looperState = Number(val(entities, "LOOPER_STATE", 0));

  useEffect(() => {
    if (looperState === 1) {
      if (!loopClock.current.recAt) {
        loopClock.current.recAt = Date.now();
        loopClock.current.lengthMs = 0;
        setLoopRecAt(loopClock.current.recAt);
        setLoopLengthMs(0);
      }
      return;
    }
    if (looperState === 0) {
      loopClock.current.recAt = 0;
      loopClock.current.lengthMs = 0;
      setLoopRecAt(0);
      setLoopLengthMs(0);
      return;
    }
    if (loopClock.current.recAt) {
      loopClock.current.lengthMs = Date.now() - loopClock.current.recAt;
      loopClock.current.recAt = 0;
      setLoopRecAt(0);
      setLoopLengthMs(loopClock.current.lengthMs);
    }
  }, [looperState]);
  const frontLed = truthy(val(entities, "FRONT_LED", false));
  const sdCard = Number(link.sdCard);
  const sdRec = sdCard === 2;
  const recSecs = sdRec && sdRecAt.current
    ? Math.max(0, Math.floor(((recNow || Date.now()) - sdRecAt.current) / 1000))
    : 0;
  const recLabel = sdRec ? `${Math.floor(recSecs / 60)}:${String(recSecs % 60).padStart(2, "0")}` : undefined;

  useEffect(() => {
    if (!sdRec) {
      sdRecAt.current = 0;
      return undefined;
    }
    if (!sdRecAt.current) sdRecAt.current = Date.now();
    setRecNow(Date.now());
    const id = setInterval(() => setRecNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, [sdRec]);
  const procCh = proc && CHANNELS.find((item) => item.id === proc.id);
  const noteId = Number(link.tunerNoteId);
  const heard = !!link.tunerOn && !!link.tunerHeard && noteId >= 1 && noteId <= 12;
  const tunerNote = heard ? pitchClassLabel(noteId) : "—";
  const tunerCents = heard ? Number(link.tunerCents) || 0 : 0;
  const boardLabel = board === "fx" ? "FX sends"
    : board === "eq" ? "EQ"
    : board === "gain" ? "Gain"
    : board === "comp" ? "Compressor"
    : board === "snap" ? "Snapshots"
    : board === "tuner" ? "Tuner"
    : board === "loop" ? "Looper"
    : (view ? view.name : "View");
  const ignoreShake = link.analogHoldPhones !== false && link.analogHoldLoop !== false;
  const pinOn = (item) => item.kind === "board" ? board === item.id : board === "mixer" && view && view.id === item.id;

  return (
    <div class="console">
      {error ? <p class="banner">{error}</p> : null}
      {sdRec ? <p class="banner rec-banner">Recording to SD. Stop &amp; Save or this take is lost.</p> : null}

      <div class="mixer-hud">
        <div class="hud-left">
          <Pad icon={<IconBars />} title="Pages" onClick={onMenu} />
          <button ref={viewBtn} type="button" class="hud-view" onClick={() => setViewMenu((v) => !v)}>
            {boardLabel}
          </button>
          <Dropdown anchorRef={viewBtn} open={viewMenu} wide onClose={() => setViewMenu(false)}>
            {catalog.map((item) => (
              <div key={item.id} class={"picker-row" + (pinOn(item) ? " on" : "")}>
                <label class="pin-check" title="Show a dedicated button" onPointerDown={(event) => event.stopPropagation()}>
                  <input
                    type="checkbox"
                    checked={item.id === "full" || pins.includes(item.id)}
                    disabled={item.id === "full"}
                    onChange={(event) => togglePin(item.id, event.currentTarget.checked)}
                  />
                </label>
                <button
                  type="button"
                  class={"picker-item" + (pinOn(item) ? " on" : "")}
                  onClick={() => {
                    if (item.kind === "board") setToolBoard(item.id);
                    else chooseView(item.id);
                  }}
                >{item.name}</button>
              </div>
            ))}
          </Dropdown>
          {board === "mixer" ? <Pad icon={<IconPen />} title="Edit this view" onClick={() => openEditor(view)} /> : null}
          <div class="hud-views" role="tablist" aria-label="Views">
            {pinButtons.map((item) => {
              const loopLive = item.id === "loop" && !!(link.ok || liveLink.ok);
              const loopClass = loopLive && looperState === 1 ? " loop-rec"
                : loopLive && looperState === 3 ? " loop-play"
                : loopLive && looperState === 4 ? " loop-over"
                : "";
              return (
                <button
                  type="button"
                  key={item.id}
                  class={(pinOn(item) ? "on" : "") + loopClass}
                  onClick={() => {
                    if (item.kind === "board") setToolBoard(item.id);
                    else chooseView(item.id);
                  }}
                >{item.id === "full" ? "Mixer" : item.name}</button>
              );
            })}
          </div>
        </div>
        <div class="hud-right">
          <Pad label="Advanced" title="Front LEDs, snap, and analog jitter" on={advancedOpen} onClick={() => setAdvancedOpen(true)} />
          <Pad
            extra={"hud-rec" + (sdRec ? " rec-live" : "")}
            icon={<IconCircle />}
            label={recLabel}
            tone="rec"
            title={sdRec ? "Stop and save. Leaving it running loses the take." : "Record to SD"}
            on={sdRec}
            onClick={recordSd}
          />
          <StatusCluster />
          <Pad
            extra="hud-fs"
            icon={stage ? <IconCompress /> : <IconExpand />}
            title={stage ? "Exit fullscreen" : "Fullscreen mixer"}
            on={!!stage}
            onClick={onToggleStage}
          />
        </div>
      </div>

      <div class="mixer-body">
        {board === "fx" ? (
          <FxSendsBoard
            entities={entities}
            names={names}
            view={view}
            write={write}
            onProc={(id, section, slot) => setProc({ id, section, slot })}
            onToggle={(key, on) => {
              const next = views.map((item) => item.id === view.id
                ? { ...item, controls: { ...item.controls, [key]: on } }
                : item);
              persistViews(next, view.id);
            }}
          />
        ) : board === "eq" ? (
          <EqBoard entities={entities} names={names} write={write} />
        ) : board === "gain" ? (
          <GainBoard entities={entities} names={names} write={write} autoGain={autoGain} onAutogain={startAutogain} />
        ) : board === "comp" ? (
          <CompBoard entities={entities} names={names} write={write} />
        ) : board === "snap" ? (
          <SnapshotsView
            step={snapStep}
            action={snapAction}
            names={snapNames}
            onPick={snapshotPick}
            onBack={() => setSnapStep("pick")}
            onSlot={snapshotSlot}
            onRename={snapshotRename}
            onClose={() => setToolBoard("mixer")}
          />
        ) : board === "tuner" ? (
          <TunerView
            link={link}
            names={names}
            heard={heard}
            note={tunerNote}
            cents={tunerCents}
            onChan={tuner}
            onClose={() => setToolBoard("mixer")}
          />
        ) : board === "loop" ? (
          <LooperView
            state={looperState}
            recAt={loopRecAt}
            lengthMs={loopLengthMs}
            onAction={looper}
            onClose={() => setToolBoard("mixer")}
          />
        ) : (
          <div class="surface">
            {strips.flatMap((ch, index) => {
              const nodes = [];
              if (gapBefore(strips[index - 1], ch)) {
                nodes.push(<div key={"gap-" + ch.id} class="strip-gap" aria-hidden="true" />);
              }
              nodes.push(
                <ChannelStrip
                  key={ch.id}
                  ch={ch}
                  view={view}
                  entities={entities}
                  selected={ch.kind === "input" && sel === ch.n - 1}
                  name={names[ch.id]}
                  write={write}
                  onProc={(section, slot) => setProc({ id: ch.id, section, slot })}
                  onLooper={looper}
                  onSelect={ch.kind === "input" ? () => { setSelected(ch.n - 1); write("SELECTED_CHAN", ch.n - 1); } : undefined}
                  onRename={(value) => rename(ch.id, value)}
                  loopRecAt={loopRecAt}
                  loopLengthMs={loopLengthMs}
                />
              );
              return nodes;
            })}
          </div>
        )}
      </div>

      <div class={"overlay" + (fsHint && !stage ? " open" : "")} onClick={(event) => { if (event.currentTarget === event.target) dismissFs(); }}>
        {fsHint && !stage ? (
          <div class="sheet fs-sheet" onClick={(event) => event.stopPropagation()}>
            <div class="sheet-bar">
              <span class="sheet-tag">Fullscreen</span>
              <button type="button" class="ghost tight" onClick={dismissFs}>✕</button>
            </div>
            <p class="hint tight">Open the mixer full screen so the strips have the whole display.</p>
            <div class="fs-choices">
              <button type="button" onClick={enterFs}>Fullscreen</button>
              <button type="button" class="ghost" onClick={dismissFs}>Maybe later</button>
              <button type="button" class="ghost" onClick={skipFsForever}>Don’t ask again</button>
            </div>
          </div>
        ) : null}
      </div>

      <div class={"overlay" + (procCh ? " open proc-overlay" : "")} onClick={(event) => { if (event.currentTarget === event.target) setProc(null); }}>
        {procCh ? (
          <ProcSheet
            ch={procCh}
            section={proc.section}
            slot={proc.slot}
            name={names[procCh.id]}
            entities={entities}
            write={write}
            autoGain={autoGain}
            onAutogain={startAutogain}
            onClose={() => setProc(null)}
          />
        ) : null}
      </div>

      <div class={"overlay" + (advancedOpen ? " open" : "")} onClick={(event) => { if (event.currentTarget === event.target) setAdvancedOpen(false); }}>
        {advancedOpen ? (
          <div class="sheet adv-sheet" onClick={(event) => event.stopPropagation()}>
            <div class="sheet-bar">
              <span class="sheet-tag">Advanced</span>
              <button type="button" class="ghost tight" onClick={() => setAdvancedOpen(false)}>✕</button>
            </div>
            <div class="adv-list">
              <Pad
                icon={<IconBulb />}
                label="Front LEDs"
                title="Front LEDs"
                on={frontLed}
                onClick={() => write("FRONT_LED", !frontLed)}
              />
              <Pad
                label="Snap faders & knobs"
                title="Snap faders and knobs to mixer steps"
                on={snapOn}
                onClick={toggleSnap}
              />
              <Pad
                label="Ignore unwanted knob jitter"
                title="Ignore tiny analog-knob jitter from vibration on loop and phones"
                on={ignoreShake}
                onClick={() => setIgnoreShake(!ignoreShake)}
              />
              <Pad
                label="Double-tap fader to center"
                title="Double-tap a fader cap to jump to 0 dB / 100% / midpoint"
                on={dblTapOn}
                onClick={() => {
                  const next = !dblTapOn;
                  setDblTapOn(next);
                  saveDblTap(next);
                }}
              />
              <PwaInstallPad />
            </div>
          </div>
        ) : null}
      </div>

      <div class={"overlay" + (warn ? " open" : "")} onClick={(event) => { if (event.currentTarget === event.target) setWarn(""); }}>
        {warn ? (
          <div class="sheet warn-sheet" onClick={(event) => event.stopPropagation()}>
            <p>{warn}</p>
            <p class="actions">
              <button type="button" onClick={() => setWarn("")}>OK</button>
            </p>
          </div>
        ) : null}
      </div>

      <div class={"overlay" + (editorOpen ? " open" : "")} onClick={(event) => { if (event.currentTarget === event.target) setEditorOpen(false); }}>
        {editorOpen && draft ? (
          <div class="sheet view-sheet" onClick={(event) => event.stopPropagation()}>
            <div class="sheet-bar">
              <span class="sheet-tag">View</span>
              <button type="button" class="ghost tight" onClick={() => setEditorOpen(false)}>✕</button>
            </div>
            <label class="field">Name
              <input type="text" maxlength="32" value={draft.name} onInput={(event) => setDraft({ ...draft, name: event.target.value })} />
            </label>
            <h3>Channels</h3>
            <div class="view-grid">
              {CHANNELS.map((ch) => (
                <label key={ch.id} class="check-chip">
                  <input
                    type="checkbox"
                    checked={draft.channels.includes(ch.id)}
                    onChange={(event) => {
                      const on = event.target.checked;
                      const channels = on ? draft.channels.concat([ch.id]) : draft.channels.filter((id) => id !== ch.id);
                      setDraft({ ...draft, channels: channels.length ? channels : [ch.id] });
                    }}
                  />
                  {ch.label}
                </label>
              ))}
            </div>
            <h3>Controls</h3>
            <div class="view-grid">
              {CONTROL_KEYS.map((item) => (
                <label key={item.id} class="check-chip">
                  <input
                    type="checkbox"
                    checked={draft.controls[item.id] !== false}
                    onChange={(event) => setDraft({ ...draft, controls: { ...draft.controls, [item.id]: event.target.checked } })}
                  />
                  {item.label}
                </label>
              ))}
            </div>
            <p class="actions">
              <button type="button" onClick={saveDraft}>Save view</button>
              <button type="button" class="ghost" onClick={duplicateView}>Duplicate</button>
              {!draft.builtin ? <button type="button" class="warn" onClick={deleteView}>Delete</button> : null}
            </p>
          </div>
        ) : null}
      </div>
    </div>
  );
}
