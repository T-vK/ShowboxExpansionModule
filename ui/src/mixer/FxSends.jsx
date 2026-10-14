import { useState } from "preact/hooks";
import { Fader } from "./Controls.jsx";
import { FX1, FX2, fx1Id, fx2Id, fxShort } from "./fx.js";
import { CHANNELS, controlOn } from "./views.js";

const NONE_KEY = "sxm-fx-show-none";

function loadShowNone() {
  try { return localStorage.getItem(NONE_KEY) === "1"; }
  catch { return false; }
}

function saveShowNone(on) {
  try { localStorage.setItem(NONE_KEY, on ? "1" : "0"); }
  catch { /* ignore */ }
}

function val(entities, id, fallback) {
  const row = entities[id];
  if (!row) return fallback;
  return row.value;
}

function truthy(value) {
  return value === true || value === 1 || value === "1" || value === "true";
}

function SendFader({ label, amount, mute, tone, hidden, unused, onAmount, onOpen, onMute, entity }) {
  if (hidden) return null;
  return (
    <div class={"fx-send tone-" + tone + (mute ? " muted" : "") + (unused ? " unused" : "")}>
      <button type="button" class="fx-send-name" onClick={onOpen}>{label}</button>
      <Fader entity={entity} min={0} max={1} unit="%" value={amount} onChange={onAmount} />
      <button type="button" class={"pad" + (mute ? " on mute" : "")} onClick={onMute}>{mute ? "Off" : "On"}</button>
    </div>
  );
}

const FX_TOGGLES = [
  ["fx1", "FX1"],
  ["fx2", "FX2"],
  ["ext", "Ext FX"]
];

export function FxSendsBoard({ entities, names, view, write, onProc, onToggle }) {
  const channels = CHANNELS.filter((ch) => ch.kind === "input");
  const showFx1 = controlOn(view, "fx1");
  const showFx2 = controlOn(view, "fx2");
  const showExt = controlOn(view, "ext");
  const [showNone, setShowNone] = useState(() => loadShowNone());

  function toggleNone() {
    const next = !showNone;
    setShowNone(next);
    saveShowNone(next);
  }

  return (
    <div class="fx-sends-wrap">
      <div class="fx-sends-bar">
        <span class="hint">Show</span>
        {FX_TOGGLES.map(([id, name]) => (
          <button
            key={id}
            type="button"
            class={"pad" + (controlOn(view, id) ? " on" : "")}
            onClick={() => onToggle && onToggle(id, !controlOn(view, id))}
          >{name}</button>
        ))}
        <button
          type="button"
          class={"pad" + (showNone ? " on" : "")}
          title="Show FX1/FX2 even when the type is None"
          onClick={toggleNone}
        >Unused</button>
      </div>
      <div class="fx-sends">
        {channels.map((ch) => {
          const n = ch.n;
          const p = `INPUT${n}_`;
          const title = names[ch.id] || ch.label;
          const fx1Type = Number(val(entities, fx1Id(n), 0)) || 0;
          const fx2Type = Number(val(entities, fx2Id(n), 0)) || 0;
          const fx1Mute = truthy(val(entities, `${p}EFFECT_1_MUTE`, false));
          const fx2Mute = truthy(val(entities, `${p}EFFECT_2_MUTE`, false));
          const extMute = truthy(val(entities, `${p}EXT_FX_MUTE`, false));
          return (
            <div key={ch.id} class="fx-send-col">
              <header>
                <strong class="chan-id">{title}</strong>
              </header>
              <div class="fx-send-faders">
                <SendFader
                  tone="fx1"
                  hidden={!showFx1 || (!fx1Type && !showNone)}
                  unused={!fx1Type}
                  label={fxShort(FX1, fx1Type)}
                  amount={val(entities, `${p}EFFECT_1_AMOUNT`, 0)}
                  mute={fx1Mute}
                  onAmount={(v) => write(`${p}EFFECT_1_AMOUNT`, v)}
                  entity={`${p}EFFECT_1_AMOUNT`}
                  onOpen={() => onProc && onProc(ch.id, "fx")}
                  onMute={() => write(`${p}EFFECT_1_MUTE`, !fx1Mute)}
                />
                <SendFader
                  tone="fx2"
                  hidden={!showFx2 || (!fx2Type && !showNone)}
                  unused={!fx2Type}
                  label={fxShort(FX2, fx2Type)}
                  amount={val(entities, `${p}EFFECT_2_AMOUNT`, 0)}
                  mute={fx2Mute}
                  onAmount={(v) => write(`${p}EFFECT_2_AMOUNT`, v)}
                  entity={`${p}EFFECT_2_AMOUNT`}
                  onOpen={() => onProc && onProc(ch.id, "fx")}
                  onMute={() => write(`${p}EFFECT_2_MUTE`, !fx2Mute)}
                />
                <SendFader
                  tone="ext"
                  hidden={!showExt}
                  label="Ext FX"
                  amount={val(entities, `${p}EXT_FX_SENDS`, 0)}
                  mute={extMute}
                  onAmount={(v) => write(`${p}EXT_FX_SENDS`, v)}
                  entity={`${p}EXT_FX_SENDS`}
                  onOpen={() => onProc && onProc(ch.id, "fx")}
                  onMute={() => write(`${p}EXT_FX_MUTE`, !extMute)}
                />
              </div>
            </div>
          );
        })}
      </div>
    </div>
  );
}
