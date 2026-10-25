import { useState, useEffect } from "preact/hooks";
import { useLive } from "../live/LiveContext.jsx";
import { Fader, ModeToggle, formatValue } from "./Controls.jsx";
import { EqGraph, CompGraph, VoicingGraph } from "./Graphs.jsx";
import { FX1, FX2, fx1Id, fx2Id, fxName, fxShort } from "./fx.js";
import { loopButtons, looperLabel, formatLoopMs } from "./ToolViews.jsx";
import { controlOn } from "./views.js";

function val(entities, id, fallback) {
  const row = entities[id];
  if (!row) return fallback;
  return row.value;
}

function truthy(value) {
  return value === true || value === 1 || value === "1" || value === "true";
}

function clamp01(value) {
  const n = Number(value);
  if (!Number.isFinite(n)) return 0;
  return Math.max(0, Math.min(1, n));
}

function Header({ title, clipOn, onSelect, onRename, selected }) {
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState(title || "");

  function startEdit() {
    setDraft(title || "");
    setEditing(true);
  }

  function commit() {
    if (onRename) onRename(draft);
    setEditing(false);
  }

  function onBadgeClick(event) {
    event.stopPropagation();
    if (onSelect && !selected) {
      onSelect();
      return;
    }
    if (onRename) startEdit();
  }

  return (
    <header>
      {editing ? (
        <input
          class="chan-id-input"
          autoFocus
          maxlength="16"
          value={draft}
          onInput={(event) => setDraft(event.currentTarget.value)}
          onPointerDown={(event) => event.stopPropagation()}
          onClick={(event) => event.stopPropagation()}
          onBlur={commit}
          onKeyDown={(event) => {
            if (event.key === "Enter") event.currentTarget.blur();
            if (event.key === "Escape") setEditing(false);
          }}
        />
      ) : (
        <button
          type="button"
          class="chan-id"
          title={onSelect ? "Select · tap again to rename" : "Rename channel"}
          onClick={onBadgeClick}
        >{title || "Name"}</button>
      )}
      {clipOn != null ? <span class={"clip" + (clipOn ? " on" : "")} title="Clip" /> : null}
    </header>
  );
}

function GainRect({ value, min, max, onClick }) {
  const lo = min == null ? 0 : min;
  const hi = max == null ? 60 : max;
  const n = Number(value);
  const ratio = !Number.isFinite(n) ? 0 : Math.max(0, Math.min(1, (n - lo) / (hi - lo)));
  return (
    <button type="button" class="gain-bar" title="Gain" onClick={onClick}>
      <span class="gain-fill" style={{ width: `${ratio * 100}%` }} />
      <span class="gain-txt">Gain</span>
      <span class="gain-val">{formatValue(n, lo, hi, "dB")}</span>
    </button>
  );
}

function FxLine({ label, amount, on, tone, onClick, title, compact }) {
  const pct = clamp01(amount) * 100;
  return (
    <button type="button" class={"fx-line" + (tone ? " tone-" + tone : "") + (on ? " on" : " off") + (compact ? " compact" : "")} title={title || label} onClick={onClick}>
      <span class="fx-line-fill" style={{ width: `${pct}%` }} />
      <span class="fx-line-txt">{label}</span>
      {compact ? null : <span class="fx-line-val">{Math.round(pct)}%</span>}
    </button>
  );
}

function MuteBtn({ muted, onClick, disabled }) {
  return (
    <button
      type="button"
      class={"mute-btn" + (muted ? " on" : "")}
      title="Mute"
      disabled={disabled}
      onClick={onClick}
    >MUTE</button>
  );
}

// Record/play clock for the looper strip's state field, same as LooperView.
function LoopStripTime({ state, recAt, lengthMs }) {
  const recording = state === 1 && recAt > 0;
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    if (!recording) return undefined;
    setNow(Date.now());
    const id = setInterval(() => setNow(Date.now()), 100);
    return () => clearInterval(id);
  }, [recording, recAt]);
  const shown = recording ? Math.max(0, now - recAt) : lengthMs;
  if (state !== 1 && !(shown > 0)) return null;
  return <span class="loop-status-time">{formatLoopMs(shown)}</span>;
}

export function ChannelStrip({ ch, entities, selected, name, view, onSelect, onRename, onProc, onLooper, write, loopRecAt, loopLengthMs }) {
  const { link } = useLive();
  const shown = (key) => controlOn(view, key, ch.kind);
  const title = (name && name.trim()) ? name : ch.label;
  const fader = shown("fader");
  const amp = truthy(val(entities, "AMP_PA_MODE", false));
  // LOCATION_MODE wire truth: 0 = outdoor, 1 = indoor.
  const indoor = truthy(val(entities, "LOCATION_MODE", false));
  const bypass = truthy(val(entities, "FX_BYPASS", false));
  const linkOk = !!link.ok;

  const parts = [];
  let kindClass = ch.kind;
  let muted = false;

  if (ch.kind === "input") {
    const n = ch.n;
    const p = `INPUT${n}_`;
    muted = truthy(val(entities, `${p}MUTE`, false));
    kindClass = "input";
    const eqOn = truthy(val(entities, `${p}EQ_ENABLE`, false));
    const compOn = truthy(val(entities, `${p}COMPRESSOR_ENABLE`, false));
    const fx1Type = Number(val(entities, fx1Id(n), 0)) || 0;
    const fx2Type = Number(val(entities, fx2Id(n), 0)) || 0;
    const fx1Mute = truthy(val(entities, `${p}EFFECT_1_MUTE`, false));
    const fx2Mute = truthy(val(entities, `${p}EFFECT_2_MUTE`, false));
    const extMute = truthy(val(entities, `${p}EXT_FX_MUTE`, false));
    const fxPair = [];
    const fxRest = [];
    if (shown("fx1")) {
      fxPair.push(
        <FxLine
          key="fx1"
          tone="fx1"
          label={fx1Type ? fxShort(FX1, fx1Type) : "FX1"}
          amount={val(entities, `${p}EFFECT_1_AMOUNT`, 0)}
          on={!!fx1Type && !fx1Mute}
          compact
          title={`${fxName(FX1, fx1Type) || "FX1"} ${Math.round(clamp01(val(entities, `${p}EFFECT_1_AMOUNT`, 0)) * 100)}%`}
          onClick={() => onProc && onProc("fx")}
        />
      );
    }
    if (shown("fx2")) {
      fxPair.push(
        <FxLine
          key="fx2"
          tone="fx2"
          label={fx2Type ? fxShort(FX2, fx2Type) : "FX2"}
          amount={val(entities, `${p}EFFECT_2_AMOUNT`, 0)}
          on={!!fx2Type && !fx2Mute}
          compact
          title={`${fxName(FX2, fx2Type) || "FX2"} ${Math.round(clamp01(val(entities, `${p}EFFECT_2_AMOUNT`, 0)) * 100)}%`}
          onClick={() => onProc && onProc("fx")}
        />
      );
    }
    if (shown("ext")) {
      fxRest.push(
        <FxLine
          key="ext"
          tone="ext"
          label="Ext FX"
          amount={val(entities, `${p}EXT_FX_SENDS`, 0)}
          on={!extMute}
          title="External FX loop"
          onClick={() => onProc && onProc("fx")}
        />
      );
    }
    const fxRows = fxPair.length || fxRest.length
      ? [
          fxPair.length ? <div key="pair" class="strip-fx-pair">{fxPair}</div> : null,
          ...fxRest
        ].filter(Boolean)
      : [];
    parts.push(
      <Header
        key="h"
        title={title}
        clipOn={truthy(val(entities, `${p}CLIP_OL_PRE`, false)) || truthy(val(entities, `${p}CLIP_OL_POST`, false))}
        onSelect={onSelect}
        onRename={onRename}
        selected={selected}
      />
    );
    if (shown("gain")) {
      parts.push(<GainRect key="g" value={val(entities, `${p}GAIN`, 0)} onClick={() => onProc && onProc("gain")} />);
    }
    if (shown("eq")) {
      parts.push(
        <button key="eq" type="button" class={"curve-btn" + (eqOn ? "" : " off")} title="EQ" onClick={() => onProc && onProc("eq")}>
          <EqGraph
            low={val(entities, `${p}EQ_LOW_GAIN`, 0)}
            mid={val(entities, `${p}EQ_MID_GAIN`, 0)}
            high={val(entities, `${p}EQ_HIGH_GAIN`, 0)}
            on={eqOn}
            mini
            overlay="EQ"
          />
        </button>
      );
    }
    if (shown("comp")) {
      parts.push(
        <button key="c" type="button" class={"curve-btn tone-comp" + (compOn ? "" : " off")} title="Compressor" onClick={() => onProc && onProc("comp")}>
          <CompGraph amount={val(entities, `${p}COMPRESSOR_AMOUNT`, 0)} on={compOn} mini overlay="Compressor" />
        </button>
      );
    }
    if (fxRows.length) {
      parts.push(<div key="fx" class="strip-fx">{fxRows}</div>);
    }
    if (fader) {
      parts.push(
        <div key="f" class="strip-fader">
          <Fader entity={`${p}VOLUME`} min={-120} max={10} unit="dB" value={val(entities, `${p}VOLUME`, -120)} onChange={(v) => write(`${p}VOLUME`, v)} />
        </div>
      );
    }
    if (shown("mute")) {
      parts.push(<div key="m" class="strip-mute"><MuteBtn muted={muted} onClick={() => write(`${p}MUTE`, !muted)} /></div>);
    }
  } else if (ch.kind === "aux") {
    muted = truthy(val(entities, "STEREO_INPUT1_MUTE", false));
    kindClass = "stereo";
    const eqOn = truthy(val(entities, "STEREO_INPUT1_EQ_ENABLE", false));
    parts.push(
      <Header
        key="h"
        title={title}
        clipOn={truthy(val(entities, "STEREO_INPUT1_CLIP_OL_PRE", false)) || truthy(val(entities, "STEREO_INPUT1_CLIP_OL_POST", false))}
        onRename={onRename}
      />
    );
    if (shown("eq")) {
      parts.push(
        <button key="eq" type="button" class={"curve-btn" + (eqOn ? "" : " off")} title="EQ" onClick={() => onProc && onProc("eq")}>
          <EqGraph
            low={val(entities, "STEREO_INPUT1_EQ_LOW_GAIN", 0)}
            mid={val(entities, "STEREO_INPUT1_EQ_MID_GAIN", 0)}
            high={val(entities, "STEREO_INPUT1_EQ_HIGH_GAIN", 0)}
            on={eqOn}
            mini
            overlay="EQ"
          />
        </button>
      );
    }
    if (fader) {
      parts.push(
        <div key="f" class="strip-fader">
          <Fader entity="STEREO_INPUT1_VOLUME" min={-120} max={10} unit="dB" value={val(entities, "STEREO_INPUT1_VOLUME", -120)} onChange={(v) => write("STEREO_INPUT1_VOLUME", v)} />
        </div>
      );
    }
    if (shown("mute")) {
      parts.push(<div key="m" class="strip-mute"><MuteBtn muted={muted} onClick={() => write("STEREO_INPUT1_MUTE", !muted)} /></div>);
    }
  } else if (ch.kind === "main") {
    muted = truthy(val(entities, "MAIN_MUTE", false));
    kindClass = "master";
    parts.push(<Header key="h" title={title} clipOn={truthy(val(entities, "MAIN_CLIP_OL", false))} onRename={onRename} />);
    parts.push(
      <div key="eq" class="curve-btn readonly" title="Main voicing">
        <VoicingGraph amp={amp} outdoor={!indoor} mini />
      </div>
    );
    const feedback = Number(val(entities, "FEEDBACK_ELIM", 0)) === 1;
    parts.push(
      <div key="modes" class="strip-modes">
        <ModeToggle
          label={amp ? "Amp Mode" : "PA Mode"}
          title="Toggle PA / Amp voicing"
          on
          onClick={() => write("AMP_PA_MODE", !amp)}
        />
        <ModeToggle
          label={indoor ? "Indoor Mode" : "Outdoor Mode"}
          title="Toggle Indoor / Outdoor voicing"
          on
          onClick={() => write("LOCATION_MODE", !indoor)}
        />
        <ModeToggle
          label="Bypass FX"
          title="Showbox hardware FX bypass"
          on={bypass}
          onClick={() => write("FX_BYPASS", !bypass)}
        />
        <ModeToggle
          label="FB Elim"
          title="Feedback elimination"
          on={feedback}
          onClick={() => write("FEEDBACK_ELIM", feedback ? 0 : 1)}
        />
      </div>
    );
    if (fader) {
      parts.push(
        <div key="f" class="strip-fader">
          <Fader entity="MAIN_MASTER_GAIN" min={-120} max={6} unit="dB" value={val(entities, "MAIN_MASTER_GAIN", -120)} onChange={(v) => write("MAIN_MASTER_GAIN", v)} />
        </div>
      );
    }
    if (shown("mute")) {
      parts.push(<div key="m" class="strip-mute"><MuteBtn muted={muted} onClick={() => write("MAIN_MUTE", !muted)} /></div>);
    }
  } else if (ch.kind === "loop") {
    const loopState = Number(val(entities, "LOOPER_STATE", 0));
    kindClass = "loop";
    if (linkOk && loopState === 1) kindClass += " loop-rec";
    if (linkOk && loopState === 3) kindClass += " loop-play";
    if (linkOk && loopState === 4) kindClass += " loop-over";
    parts.push(<Header key="h" title={title} onRename={onRename} />);
    parts.push(
      <div key="st" class="loop-status">
        <span class="loop-status-label">{looperLabel(loopState)}</span>
        <LoopStripTime state={loopState} recAt={loopRecAt} lengthMs={loopLengthMs} />
      </div>
    );
    const loopBtns = loopButtons(loopState);
    const loopRows = [];
    for (let i = 0; i < loopBtns.length; i += 2) loopRows.push(loopBtns.slice(i, i + 2));
    parts.push(
      <div key="modes" class={"strip-modes loop-modes n" + loopBtns.length}>
        {loopRows.map((row, ri) => (
          <div key={ri} class={"loop-mode-row" + (row.length === 1 ? " one" : "")}>
            {row.map((btn) => (
              <ModeToggle
                key={btn.id}
                icon={btn.icon}
                label={btn.label}
                title={btn.label}
                on={btn.id === "over" && loopState === 4}
                onClick={() => onLooper && onLooper(btn.action)}
              />
            ))}
          </div>
        ))}
      </div>
    );
    if (fader) {
      parts.push(
        <div key="f" class="strip-fader">
          <Fader entity="LOOPER_LEVEL" snap={false} min={-120} max={10} unit="loop" value={val(entities, "LOOPER_LEVEL", 0)} onChange={(v) => write("LOOPER_LEVEL", v)} />
        </div>
      );
    }
    parts.push(<div key="m" class="strip-mute"><MuteBtn muted={false} disabled /></div>);
  } else {
    kindClass = "phones master";
    parts.push(<Header key="h" title={title} onRename={onRename} />);
    if (fader) {
      parts.push(
        <div key="f" class="strip-fader">
          <Fader entity="MAIN_HEADPHONE_GAIN" snap={false} min={-120} max={10} unit="phones" value={val(entities, "MAIN_HEADPHONE_GAIN", -120)} onChange={(v) => write("MAIN_HEADPHONE_GAIN", v)} />
        </div>
      );
    }
    parts.push(<div key="m" class="strip-mute"><MuteBtn muted={false} disabled /></div>);
  }

  return (
    <div class={"strip " + kindClass + (muted ? " muted" : "")}>
      {parts}
    </div>
  );
}
