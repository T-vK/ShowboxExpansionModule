import { Fader, Pad } from "./Controls.jsx";
import { CHANNELS } from "./views.js";

function val(entities, id, fallback) {
  const row = entities[id];
  if (!row) return fallback;
  return row.value;
}

export function GainBoard({ entities, names, write, autoGain, onAutogain }) {
  const channels = CHANNELS.filter((ch) => ch.kind === "input");
  const listening = autoGain && autoGain.until > Date.now();
  return (
    <div class="fx-sends eq-board gain-board">
      {channels.map((ch) => {
        const title = names[ch.id] || ch.label;
        const entity = `INPUT${ch.n}_GAIN`;
        const on = listening && autoGain.chan === ch.n - 1;
        return (
          <div key={ch.id} class="fx-send-col eq-col">
            <header>
              <strong class="chan-id">{title}</strong>
              <Pad
                label={on ? "Listening" : "Autogain"}
                on={on}
                title="Listen for 10s and set preamp gain from the incoming signal"
                onClick={() => onAutogain && onAutogain(ch.n - 1)}
              />
            </header>
            <div class="eq-send-faders">
              <div class="eq-send tone-gain">
                <span>Gain</span>
                <Fader entity={entity} min={0} max={60} unit="dB" value={val(entities, entity, 0)} onChange={(v) => write(entity, v)} />
              </div>
            </div>
          </div>
        );
      })}
    </div>
  );
}
