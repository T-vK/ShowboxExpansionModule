import { Knob, FxSelect, LedButton, Fader, Pad } from "./Controls.jsx";
import { FX1, FX2, fx1Id, fx2Id, fxName, fxOptions } from "./fx.js";
import { EqGraph, CompGraph } from "./Graphs.jsx";

function val(entities, id, fallback) {
  const row = entities[id];
  if (!row) return fallback;
  return row.value;
}

function truthy(value) {
  return value === true || value === 1 || value === "1" || value === "true";
}

function SheetBar({ tag, on, onToggle, onClose }) {
  return (
    <div class="sheet-bar">
      {onToggle ? <LedButton on={on} label={on ? "On" : "Off"} title="Enable" onClick={onToggle} /> : null}
      <span class="sheet-tag">{tag}</span>
      <button type="button" class="ghost tight" onClick={onClose}>✕</button>
    </div>
  );
}

function EqBody({ prefix, entities, write, on }) {
  const low = val(entities, `${prefix}EQ_LOW_GAIN`, 0);
  const mid = val(entities, `${prefix}EQ_MID_GAIN`, 0);
  const high = val(entities, `${prefix}EQ_HIGH_GAIN`, 0);
  return (
    <div class="eq-sheet-body">
      <div class="eq-sheet-graph">
        <EqGraph prefix={prefix} low={low} mid={mid} high={high} on={on} onBand={write} />
      </div>
      <div class="eq">
        <Knob tone="low" label="Low" min={-15} max={15} unit="dB" entity={`${prefix}EQ_LOW_GAIN`} value={low} onChange={(v) => write(`${prefix}EQ_LOW_GAIN`, v)} />
        <Knob tone="mid" label="Mid" min={-15} max={15} unit="dB" entity={`${prefix}EQ_MID_GAIN`} value={mid} onChange={(v) => write(`${prefix}EQ_MID_GAIN`, v)} />
        <Knob tone="high" label="High" min={-15} max={15} unit="dB" entity={`${prefix}EQ_HIGH_GAIN`} value={high} onChange={(v) => write(`${prefix}EQ_HIGH_GAIN`, v)} />
      </div>
    </div>
  );
}

function FxBlock({ tone, tag, on, onToggle, select, amount, onAmount, hint, entity }) {
  return (
    <div class={"fx-block tone-" + tone}>
      <div class="fx-block-side">
        <div class="fx-block-head">
          <LedButton variant={tone} on={on} label={tag} onClick={onToggle} />
          {hint ? <span class="fx-hint">{hint}</span> : null}
        </div>
        {select}
      </div>
      <div class="fx-block-fader">
        <Fader entity={entity} min={0} max={1} unit="%" tone={tone} value={amount} onChange={onAmount} />
      </div>
    </div>
  );
}

export function ProcSheet({ ch, section, name, entities, write, onClose, autoGain, onAutogain }) {
  const title = name || ch.label;
  const tagBase = title;

  if (ch.kind === "aux" && section === "eq") {
    const on = truthy(val(entities, "STEREO_INPUT1_EQ_ENABLE", false));
    return (
      <div class="sheet proc-sheet eq-sheet" onClick={(event) => event.stopPropagation()}>
        <SheetBar tag={`${tagBase} EQ`} on={on} onToggle={() => write("STEREO_INPUT1_EQ_ENABLE", !on)} onClose={onClose} />
        <EqBody prefix="STEREO_INPUT1_" entities={entities} write={write} on={on} />
      </div>
    );
  }

  if (ch.kind !== "input") return null;
  const n = ch.n;
  const p = `INPUT${n}_`;

  if (section === "gain") {
    const listening = autoGain && autoGain.chan === n - 1 && autoGain.until > Date.now();
    return (
      <div class="sheet proc-sheet gain-sheet" onClick={(event) => event.stopPropagation()}>
        <SheetBar tag={`${tagBase} gain`} onClose={onClose} />
        <Pad
          label={listening ? "Listening…" : "Autogain"}
          on={listening}
          title="Listen for 10s and set preamp gain from the incoming signal"
          onClick={() => onAutogain && onAutogain(n - 1)}
        />
        <div class="proc-fader">
          <Fader entity={`${p}GAIN`} min={0} max={60} unit="dB" tone="gain" value={val(entities, `${p}GAIN`, 0)} onChange={(v) => write(`${p}GAIN`, v)} />
        </div>
      </div>
    );
  }

  if (section === "eq") {
    const on = truthy(val(entities, `${p}EQ_ENABLE`, false));
    return (
      <div class="sheet proc-sheet eq-sheet" onClick={(event) => event.stopPropagation()}>
        <SheetBar tag={`${tagBase} EQ`} on={on} onToggle={() => write(`${p}EQ_ENABLE`, !on)} onClose={onClose} />
        <EqBody prefix={p} entities={entities} write={write} on={on} />
      </div>
    );
  }

  if (section === "comp") {
    const on = truthy(val(entities, `${p}COMPRESSOR_ENABLE`, false));
    const amount = val(entities, `${p}COMPRESSOR_AMOUNT`, 0);
    return (
      <div class="sheet proc-sheet comp-sheet" onClick={(event) => event.stopPropagation()}>
        <SheetBar tag={`${tagBase} comp`} on={on} onToggle={() => write(`${p}COMPRESSOR_ENABLE`, !on)} onClose={onClose} />
        <div class="comp-sheet-body">
          <CompGraph amount={amount} on={on} />
          <div class="proc-fader">
            <Fader entity={`${p}COMPRESSOR_AMOUNT`} min={0} max={1} unit="%" tone="comp" value={amount} onChange={(v) => write(`${p}COMPRESSOR_AMOUNT`, v)} />
          </div>
        </div>
      </div>
    );
  }

  const fx1Mute = truthy(val(entities, `${p}EFFECT_1_MUTE`, false));
  const fx2Mute = truthy(val(entities, `${p}EFFECT_2_MUTE`, false));
  const extMute = truthy(val(entities, `${p}EXT_FX_MUTE`, false));
  const fx1Type = Number(val(entities, fx1Id(n), 0)) || 0;
  const fx2Type = Number(val(entities, fx2Id(n), 0)) || 0;
  return (
    <div class="sheet proc-sheet fx-sheet" onClick={(event) => event.stopPropagation()}>
      <SheetBar tag={`${tagBase} FX`} onClose={onClose} />
      <div class="fx-sheet-body">
        <FxBlock
          tone="fx1"
          tag="FX1"
          on={!fx1Mute}
          onToggle={() => write(`${p}EFFECT_1_MUTE`, !fx1Mute)}
          select={<FxSelect value={fx1Type} label={fxName(FX1, fx1Type)} options={fxOptions(FX1, n)} onChange={(id) => write(fx1Id(n), Number(id))} />}
          amount={val(entities, `${p}EFFECT_1_AMOUNT`, 0)}
          entity={`${p}EFFECT_1_AMOUNT`}
          onAmount={(v) => write(`${p}EFFECT_1_AMOUNT`, v)}
        />
        <FxBlock
          tone="fx2"
          tag="FX2"
          on={!fx2Mute}
          onToggle={() => write(`${p}EFFECT_2_MUTE`, !fx2Mute)}
          hint={n === 1 || n === 3 ? "shared CH1+CH3" : "shared CH2+CH4"}
          select={<FxSelect value={fx2Type} label={fxName(FX2, fx2Type)} options={fxOptions(FX2, n)} onChange={(id) => write(fx2Id(n), Number(id))} />}
          amount={val(entities, `${p}EFFECT_2_AMOUNT`, 0)}
          entity={`${p}EFFECT_2_AMOUNT`}
          onAmount={(v) => write(`${p}EFFECT_2_AMOUNT`, v)}
        />
        <FxBlock
          tone="ext"
          tag="Ext"
          on={!extMute}
          onToggle={() => write(`${p}EXT_FX_MUTE`, !extMute)}
          select={<span class="fx-spacer">FX loop</span>}
          amount={val(entities, `${p}EXT_FX_SENDS`, 0)}
          entity={`${p}EXT_FX_SENDS`}
          onAmount={(v) => write(`${p}EXT_FX_SENDS`, v)}
        />
      </div>
    </div>
  );
}
