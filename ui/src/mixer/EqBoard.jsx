import { Fader, Pad } from "./Controls.jsx";
import { EqGraph } from "./Graphs.jsx";
import { CHANNELS } from "./views.js";

function val(entities, id, fallback) {
  const row = entities[id];
  if (!row) return fallback;
  return row.value;
}

function truthy(value) {
  return value === true || value === 1 || value === "1" || value === "true";
}

function BandFaders({ prefix, entities, write }) {
  return (
    <div class="eq-send-faders">
      <div class="eq-send tone-low">
        <span>Low</span>
        <Fader entity={`${prefix}EQ_LOW_GAIN`} min={-15} max={15} unit="dB" value={val(entities, `${prefix}EQ_LOW_GAIN`, 0)} onChange={(v) => write(`${prefix}EQ_LOW_GAIN`, v)} />
      </div>
      <div class="eq-send tone-mid">
        <span>Mid</span>
        <Fader entity={`${prefix}EQ_MID_GAIN`} min={-15} max={15} unit="dB" value={val(entities, `${prefix}EQ_MID_GAIN`, 0)} onChange={(v) => write(`${prefix}EQ_MID_GAIN`, v)} />
      </div>
      <div class="eq-send tone-high">
        <span>High</span>
        <Fader entity={`${prefix}EQ_HIGH_GAIN`} min={-15} max={15} unit="dB" value={val(entities, `${prefix}EQ_HIGH_GAIN`, 0)} onChange={(v) => write(`${prefix}EQ_HIGH_GAIN`, v)} />
      </div>
    </div>
  );
}

export function EqBoard({ entities, names, write }) {
  const channels = CHANNELS.filter((ch) => ch.kind === "input" || ch.kind === "aux");

  return (
    <div class="fx-sends eq-board">
      {channels.map((ch) => {
        const title = names[ch.id] || ch.label;
        const prefix = ch.kind === "aux" ? "STEREO_INPUT1_" : `INPUT${ch.n}_`;
        const enableId = `${prefix}EQ_ENABLE`;
        const on = truthy(val(entities, enableId, false));
        return (
          <div key={ch.id} class="fx-send-col eq-col">
            <header>
              <strong class="chan-id">{title}</strong>
              <Pad label={on ? "On" : "Off"} on={on} onClick={() => write(enableId, !on)} />
            </header>
            <div class={"eq-board-curve" + (on ? "" : " off")}>
              <EqGraph
                prefix={prefix}
                low={val(entities, `${prefix}EQ_LOW_GAIN`, 0)}
                mid={val(entities, `${prefix}EQ_MID_GAIN`, 0)}
                high={val(entities, `${prefix}EQ_HIGH_GAIN`, 0)}
                on={on}
                onBand={write}
              />
            </div>
            <BandFaders prefix={prefix} entities={entities} write={write} />
          </div>
        );
      })}
    </div>
  );
}
