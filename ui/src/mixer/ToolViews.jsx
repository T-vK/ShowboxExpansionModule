import { useEffect, useState } from "preact/hooks";
import { Pad } from "./Controls.jsx";
import { CHANNELS } from "./views.js";
import { IconCircle, IconPen, IconPlay, IconLoad, IconStop, IconStore, IconTrash } from "./Icons.jsx";
import { useLive } from "../live/LiveContext.jsx";

export const SNAP_SLOTS = [
  [0, "1", false],
  [1, "2", false],
  [2, "3", false],
  [3, "4", false],
  [4, "5", false],
  [5, "Default", true]
];

export function looperLabel(state) {
  if (state === 1) return "Recording";
  if (state === 4) return "Overdubbing";
  if (state === 3) return "Playing";
  if (state === 2) return "Stopped";
  return "Cleared";
}

export function formatLoopMs(ms) {
  const n = Math.max(0, Number(ms) || 0);
  const total = n / 1000;
  const m = Math.floor(total / 60);
  const s = total - m * 60;
  if (m <= 0) return `${s.toFixed(1)}s`;
  return `${m}:${s.toFixed(1).padStart(4, "0")}`;
}

const TUNER_TICKS = [
  [0, "−50", "edge-l"],
  [38, "−12", ""],
  [50, "0", "center"],
  [62, "+12", ""],
  [100, "+50", "edge-r"]
];

export function TunerMeter({ cents, heard }) {
  const n = heard ? Math.max(-50, Math.min(50, Number(cents) || 0)) : 0;
  const pct = ((n + 50) / 100) * 100;
  const abs = Math.abs(n);
  const inTune = heard && abs <= 5;
  const dir = !heard ? "waiting" : inTune ? "in-tune" : n < 0 ? "flat" : "sharp";
  const text = !heard ? "Waiting for input" : inTune ? "In tune" : n < 0 ? `${Math.round(abs)} cents flat` : `${Math.round(abs)} cents sharp`;
  return (
    <div class={"tuner-meter " + dir}>
      <div class="tuner-scale" aria-hidden="true">
        {TUNER_TICKS.map(([left, label, edge]) => (
          <span key={label} class={"tuner-tick" + (edge ? " " + edge : "")} style={{ left: `${left}%` }}>
            <span class="tuner-tick-lbl">{label}</span>
            <span class="tuner-tick-mark" />
          </span>
        ))}
      </div>
      <div class="tuner-track">
        <span class="tuner-zone" />
        <span class={"tuner-needle" + (heard ? "" : " idle")} style={heard ? { left: `${pct}%` } : undefined} />
      </div>
      <p class={"tuner-dir" + (heard ? "" : " idle")}>{text}</p>
    </div>
  );
}

export function TunerView({ link, names, heard, note, cents, hz, onChan, onClose }) {
  const active = Number(link.tunerChan);
  const ch = CHANNELS[active] || CHANNELS[0];
  const activeName = (names && names[ch.id]) || ch.label;
  return (
    <div class="tool-view tuner-view">
      <div class="tuner-stage">
        <p class="tool-kicker">Listening on {activeName}</p>
        <div class={"tuner-readout" + (heard ? "" : " idle")}>
          {heard ? (
            <>
              <strong class="tuner-note">{note}</strong>
              <span class="tuner-cents">{`${cents > 0 ? "+" : ""}${Math.round(cents)} ¢`}</span>
              {hz ? <span class="tuner-hz">{hz}</span> : null}
            </>
          ) : (
            <strong class="tuner-note waiting" aria-hidden="true">♪</strong>
          )}
        </div>
        <TunerMeter cents={cents} heard={heard} />
      </div>
      <div class="tuner-chans">
        {[0, 1, 2, 3].map((chan) => {
          const row = CHANNELS[chan];
          const label = (names && names[row.id]) || row.label;
          const on = !!link.tunerOn && active === chan;
          return (
            <button
              key={chan}
              type="button"
              class={"tuner-chan" + (on ? " on" : "")}
              onClick={() => onChan(chan)}
            >
              <span class="tuner-chan-id">{label}</span>
            </button>
          );
        })}
      </div>
    </div>
  );
}

export function looperActions(state) {
  if (state === 0) {
    return [{ id: "rec", label: "Rec", action: "TAP", tone: "rec" }];
  }
  if (state === 1) {
    return [
      { id: "play", label: "Play", action: "TAP" },
      { id: "over", label: "Overdub", action: "TAP2", tone: "rec" },
      { id: "stop", label: "Stop", action: "DOUBLE_PRESS" },
      { id: "clear", label: "Clear", action: "LONG_PRESS" }
    ];
  }
  if (state === 3) {
    return [
      { id: "over", label: "Overdub", action: "TAP", tone: "rec" },
      { id: "stop", label: "Stop", action: "DOUBLE_PRESS" },
      { id: "clear", label: "Clear", action: "LONG_PRESS" }
    ];
  }
  if (state === 4) {
    return [
      { id: "play", label: "Play", action: "TAP" },
      { id: "stop", label: "Stop", action: "DOUBLE_PRESS" },
      { id: "clear", label: "Clear", action: "LONG_PRESS" }
    ];
  }
  return [
    { id: "play", label: "Play", action: "TAP" },
    { id: "over", label: "Overdub", action: "TAP2", tone: "rec" },
    { id: "clear", label: "Clear", action: "LONG_PRESS" }
  ];
}

export function loopButtons(state) {
  const icons = {
    rec: <IconCircle />,
    play: <IconPlay />,
    over: <IconCircle />,
    stop: <IconStop />,
    clear: <IconTrash />
  };
  return looperActions(state).map((btn) => ({ ...btn, icon: icons[btn.id] || null }));
}

export function LooperView({ state, recAt, lengthMs, onAction, onClose }) {
  const { link } = useLive();
  const [now, setNow] = useState(() => Date.now());
  const recording = state === 1 && recAt > 0;
  const live = !!link.ok;
  useEffect(() => {
    if (!recording) return undefined;
    setNow(Date.now());
    const id = setInterval(() => setNow(Date.now()), 100);
    return () => clearInterval(id);
  }, [recording, recAt]);
  const shownMs = recording ? Math.max(0, now - recAt) : lengthMs;
  const buttons = loopButtons(state);
  return (
    <div class="tool-view loop-view">
      <div class={"loop-stage s" + state + (live && state === 1 ? " loop-rec" : "") + (live && state === 3 ? " loop-play" : "") + (live && state === 4 ? " loop-over" : "")}>
        <strong class="loop-state">{looperLabel(state)}</strong>
        {state === 1 || shownMs > 0 ? <span class="loop-len">{formatLoopMs(shownMs)}</span> : null}
      </div>
      <div class={"loop-transport n" + buttons.length}>
        {buttons.map((btn) => (
          <Pad
            key={btn.id}
            on={btn.id === "over" && state === 4}
            tone={btn.tone}
            icon={btn.icon}
            label={btn.label}
            title={btn.label}
            onClick={() => onAction(btn.action)}
          />
        ))}
      </div>
    </div>
  );
}

export function SnapshotsView({ step, action, names, onPick, onBack, onSlot, onRename, onClose }) {
  const saving = action === "SAVE";
  const renaming = action === "RENAME";
  const [renameSlot, setRenameSlot] = useState(null);
  const [draft, setDraft] = useState("");
  const labelFor = (slot, fallback) => {
    const custom = names && names[slot];
    return custom && String(custom).trim() ? String(custom).trim() : fallback;
  };
  return (
    <div class="tool-view snap-view">
      {step !== "slots" ? (
        <div class="snap-pick">
          <button type="button" class="snap-pick-btn" onClick={() => onPick("RECALL")}>
            <span class="snap-ico"><IconLoad /></span>
            <strong>Load</strong>
            <span>Recall a stored mix</span>
          </button>
          <button type="button" class="snap-pick-btn save" onClick={() => onPick("SAVE")}>
            <span class="snap-ico"><IconStore /></span>
            <strong>Save</strong>
            <span>Overwrite a slot with the current mix</span>
          </button>
          <button type="button" class="snap-pick-btn" onClick={() => { setRenameSlot(null); onPick("RENAME"); }}>
            <span class="snap-ico"><IconPen /></span>
            <strong>Rename</strong>
            <span>Give a slot a custom name</span>
          </button>
        </div>
      ) : renameSlot != null ? (
        <div class="snap-rename">
          <p class="hint tight">Name for snapshot {renameSlot + 1}</p>
          <input
            type="text"
            maxlength="16"
            value={draft}
            autoFocus
            onInput={(event) => setDraft(event.currentTarget.value)}
            onKeyDown={(event) => {
              if (event.key === "Enter") {
                onRename && onRename(renameSlot, draft);
                setRenameSlot(null);
              }
              if (event.key === "Escape") setRenameSlot(null);
            }}
          />
          <p class="actions">
            <button
              type="button"
              onClick={() => {
                onRename && onRename(renameSlot, draft);
                setRenameSlot(null);
              }}
            >Save name</button>
            <button type="button" class="ghost" onClick={() => setRenameSlot(null)}>Cancel</button>
          </p>
        </div>
      ) : (
        <>
          <p class="hint tight">{saving ? "Choose a slot to overwrite." : renaming ? "Choose a slot to rename." : "Choose a slot to recall. Default restores factory settings."}</p>
          <div class="snap-bank">
            {SNAP_SLOTS.filter(([, , locked]) => (!saving && !renaming) || !locked).map(([slot, name, locked]) => (
              <button
                type="button"
                class={"snap-slot" + (locked ? " locked" : "")}
                key={slot}
                onClick={() => {
                  if (renaming) {
                    setDraft(labelFor(slot, name));
                    setRenameSlot(slot);
                    return;
                  }
                  onSlot(slot);
                }}
              >
                <span class="snap-slot-name">{locked ? "Default" : labelFor(slot, name)}</span>
                <span class="snap-slot-sub">{locked ? "Factory" : saving ? "Overwrite" : renaming ? "Rename" : "Recall"}</span>
              </button>
            ))}
          </div>
          <button type="button" class="ghost tool-off" onClick={() => { setRenameSlot(null); onBack(); }}>Back</button>
        </>
      )}
    </div>
  );
}
