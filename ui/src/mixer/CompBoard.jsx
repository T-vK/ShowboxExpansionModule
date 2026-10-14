import { Fader, Pad } from "./Controls.jsx";
import { CHANNELS } from "./views.js";

function val(entities, id, fallback) {
  const row = entities[id];
  if (!row) return fallback;
  return row.value;
}

function truthy(value) {
  return value === true || value === 1 || value === "1" || value === "true";
}

export function CompBoard({ entities, names, write }) {
  const channels = CHANNELS.filter((ch) => ch.kind === "input");
  return (
    <div class="fx-sends eq-board gain-board comp-board">
      {channels.map((ch) => {
        const title = names[ch.id] || ch.label;
        const amountId = `INPUT${ch.n}_COMPRESSOR_AMOUNT`;
        const enableId = `INPUT${ch.n}_COMPRESSOR_ENABLE`;
        const on = truthy(val(entities, enableId, false));
        return (
          <div key={ch.id} class="fx-send-col eq-col">
            <header>
              <strong class="chan-id">{title}</strong>
              <Pad label={on ? "On" : "Off"} on={on} onClick={() => write(enableId, !on)} />
            </header>
            <div class={"eq-send-faders" + (on ? "" : " off")}>
              <div class="eq-send tone-comp">
                <span>Comp</span>
                <Fader entity={amountId} min={0} max={1} unit="%" value={val(entities, amountId, 0)} onChange={(v) => write(amountId, v)} />
              </div>
            </div>
          </div>
        );
      })}
    </div>
  );
}
