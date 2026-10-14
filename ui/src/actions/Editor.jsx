import {
  actionTypes, blankAction, blankCondition, blankGroup, blankTrigger, conditionTypes,
  edges, entityGroups, gestures, held, hidDevices, hidOps, jacks, keys, links, looperOps,
  mediaKeys, midiMessages, midiPorts, modifiers, mouseButtons, ops, panelButtons, states,
  systemOps, triggerTypes, varOps, varScopes
} from "./schema.js";
import { EntityPicker, MenuSelect } from "../ui/Widgets.jsx";

function NumberField({ label, value, placeholder, onChange }) {
  return (
    <label class="field">{label}
      <input type="number" value={value == null ? "" : value} placeholder={placeholder || ""} onInput={(event) => onChange(event.target.value)} />
    </label>
  );
}

function TextField({ label, value, placeholder, onChange }) {
  return (
    <label class="field">{label}
      <input type="text" value={value == null ? "" : value} placeholder={placeholder || ""} onInput={(event) => onChange(event.target.value)} />
    </label>
  );
}

function Ports({ value, onChange }) {
  const selected = Array.isArray(value) ? value : (value ? [value] : []);
  return (
    <div class="ports">
      {midiPorts.map(([id, name]) => (
        <label key={id}>
          <input type="checkbox" checked={selected.includes(id)} onChange={(event) => {
            const next = event.target.checked ? selected.concat([id]) : selected.filter((item) => item !== id);
            onChange(next);
          }} />{name}
        </label>
      ))}
    </div>
  );
}

function setKey(item, key, value) {
  return { ...item, [key]: value };
}

export function StepFields({ group, item, onChange }) {
  const type = item.type;
  if ((group === "triggers" || group === "actions") && type === "midi") {
    return <>
      <MenuSelect label="Message" options={midiMessages} value={item.message} onChange={(v) => onChange(setKey(item, "message", v))} />
      <NumberField label="Channel" value={item.channel} placeholder="Any" onChange={(v) => onChange(setKey(item, "channel", v))} />
      <NumberField label="Number" value={item.number} placeholder="Any" onChange={(v) => onChange(setKey(item, "number", v))} />
      <NumberField label="Value" value={item.value} placeholder="Any" onChange={(v) => onChange(setKey(item, "value", v))} />
      <div class="span"><span class="hint">Interfaces</span><Ports value={item.ports || item.port} onChange={(v) => onChange(setKey(item, "ports", v))} /></div>
      {item.message === "raw" || item.message === "sysex" ? <div class="span"><TextField label="Hex" value={item.hex} placeholder="F0 7F ..." onChange={(v) => onChange(setKey(item, "hex", v))} /></div> : null}
    </>;
  }
  if (group === "triggers" && type === "trs") {
    return <>
      <MenuSelect label="Jack" options={jacks} value={item.jack} onChange={(v) => onChange(setKey(item, "jack", v))} />
      <MenuSelect label="Gesture" options={gestures} value={item.gesture} onChange={(v) => onChange(setKey(item, "gesture", v))} />
    </>;
  }
  if (group === "triggers" && type === "panel") {
    return <>
      <MenuSelect label="Button" options={panelButtons} value={item.button} onChange={(v) => onChange(setKey(item, "button", v))} />
      <MenuSelect label="Gesture" options={[["press","Click"],["long","Long press"]]} value={item.gesture || "press"} onChange={(v) => onChange(setKey(item, "gesture", v))} />
    </>;
  }
  if (group === "triggers" && type === "connect") {
    return <>
      <MenuSelect label="Link" options={links} value={item.link} onChange={(v) => onChange(setKey(item, "link", v))} />
      <MenuSelect label="Change" options={edges} value={item.change} onChange={(v) => onChange(setKey(item, "change", v))} />
    </>;
  }
  if (type === "showbox") {
    return <>
      <div class="span"><EntityPicker label="Control" value={item.entity} groups={entityGroups} onChange={(v) => onChange(setKey(item, "entity", v))} /></div>
      {group === "conditions" ? <MenuSelect label="Compare" options={ops} value={item.op || "eq"} onChange={(v) => onChange(setKey(item, "op", v))} /> : null}
      <TextField label="Value" value={item.value} placeholder={group === "triggers" ? "Any" : ""} onChange={(v) => onChange(setKey(item, "value", v))} />
    </>;
  }
  if (type === "showboxPacket") {
    return <>
      <MenuSelect label="Direction" options={[["any","Any"],["mixer","From mixer"],["base","From base"],["both","Both (send)"]]} value={item.dir || "any"} onChange={(v) => onChange(setKey(item, "dir", v))} />
      <NumberField label="Type byte" value={item.packetType} placeholder="Any" onChange={(v) => onChange(setKey(item, "packetType", v))} />
      <div class="span"><TextField label="Hex" value={item.hex} placeholder="BEEF011700EFBE" onChange={(v) => onChange(setKey(item, "hex", v))} /></div>
    </>;
  }
  if (type === "connection" || type === "midiPort") {
    return <>
      <MenuSelect label={type === "midiPort" ? "Port" : "Link"} options={type === "midiPort" ? midiPorts : links} value={item.link || item.port} onChange={(v) => onChange(setKey(item, type === "midiPort" ? "port" : "link", v))} />
      <MenuSelect label="State" options={states} value={item.state} onChange={(v) => onChange(setKey(item, "state", v))} />
    </>;
  }
  if (type === "trs" && group === "conditions") {
    return <>
      <MenuSelect label="Jack" options={jacks} value={item.jack} onChange={(v) => onChange(setKey(item, "jack", v))} />
      <MenuSelect label="State" options={held} value={item.state} onChange={(v) => onChange(setKey(item, "state", v))} />
    </>;
  }
  if (type === "variable") {
    return <>
      <TextField label="Name" value={item.name} placeholder="boostOn" onChange={(v) => onChange(setKey(item, "name", v))} />
      {group === "actions" ? <MenuSelect label="Operation" options={varOps} value={item.op || "set"} onChange={(v) => onChange(setKey(item, "op", v))} /> : <MenuSelect label="Compare" options={ops} value={item.op || "eq"} onChange={(v) => onChange(setKey(item, "op", v))} />}
      <TextField label="Value" value={item.value} placeholder={group === "actions" && item.op === "toggle" ? "ignored" : "1"} onChange={(v) => onChange(setKey(item, "value", v))} />
      {group === "actions" ? <>
        <MenuSelect label="Scope" options={varScopes} value={item.scope || "global"} onChange={(v) => onChange(setKey(item, "scope", v))} />
        <label class="field check"><span>Remember after reboot</span>
          <input type="checkbox" disabled={item.scope === "local"} checked={item.persist !== false && item.scope !== "local"} onChange={(event) => onChange(setKey(item, "persist", event.target.checked))} />
        </label>
      </> : null}
    </>;
  }
  if (type === "hid") {
    const device = item.device || "mouse";
    const opsFor = hidOps[device] || hidOps.mouse;
    return <>
      <MenuSelect label="Device" options={hidDevices} value={device} onChange={(v) => onChange({ ...item, device: v, op: (hidOps[v] || hidOps.mouse)[0][0] })} />
      <MenuSelect label="Operation" options={opsFor} value={item.op || opsFor[0][0]} onChange={(v) => onChange(setKey(item, "op", v))} />
      {device === "keyboard" && item.op === "text" ? <div class="span"><TextField label="Text" value={item.text} onChange={(v) => onChange(setKey(item, "text", v))} /></div> : null}
      {device === "keyboard" && item.op === "media" ? <MenuSelect label="Media" options={mediaKeys} value={item.media || item.key} onChange={(v) => onChange(setKey(item, "media", v))} /> : null}
      {device === "keyboard" && item.op !== "text" && item.op !== "media" ? <>
        <MenuSelect label="Key" options={keys} value={item.key} onChange={(v) => onChange(setKey(item, "key", v))} />
        <div class="span ports">{modifiers.map(([id, name]) => (
          <label key={id}>
            <input type="checkbox" checked={(item.modifiers || []).includes(id)} onChange={(event) => {
              const selected = item.modifiers || [];
              onChange(setKey(item, "modifiers", event.target.checked ? selected.concat([id]) : selected.filter((m) => m !== id)));
            }} />{name}
          </label>
        ))}</div>
      </> : null}
      {device === "mouse" || device === "abs" ? <MenuSelect label="Button" options={mouseButtons} value={item.button || "left"} onChange={(v) => onChange(setKey(item, "button", v))} /> : null}
      {device !== "keyboard" ? <>
        <NumberField label="X" value={item.x} placeholder={device === "mouse" && item.op !== "move" ? "rel" : "0-10000"} onChange={(v) => onChange(setKey(item, "x", v))} />
        <NumberField label="Y" value={item.y} onChange={(v) => onChange(setKey(item, "y", v))} />
      </> : null}
      {item.op === "move" || item.op === "swipe" ? <>
        <NumberField label="X2" value={item.x2} onChange={(v) => onChange(setKey(item, "x2", v))} />
        <NumberField label="Y2" value={item.y2} onChange={(v) => onChange(setKey(item, "y2", v))} />
        <NumberField label="Duration ms" value={item.speedMs} placeholder="300" onChange={(v) => onChange(setKey(item, "speedMs", v))} />
      </> : null}
      {item.op === "scroll" ? <NumberField label="Wheel" value={item.amount || item.wheel} onChange={(v) => onChange(setKey(item, "amount", v))} /> : null}
    </>;
  }
  if (type === "sleep") return <NumberField label="Milliseconds" value={item.ms} placeholder="0" onChange={(v) => onChange(setKey(item, "ms", v))} />;
  if (type === "looper") return <MenuSelect label="Operation" options={looperOps} value={item.button || item.op || "cycle"} onChange={(v) => onChange(setKey(item, "button", v))} />;
  if (type === "gainBoost") return <>
    <div class="span"><EntityPicker label="Gain control" value={item.entity || "INPUT2_GAIN"} groups={entityGroups} onChange={(v) => onChange(setKey(item, "entity", v))} /></div>
    <MenuSelect label="Set" options={[["on","On"],["off","Off"],["toggle","Toggle"]]} value={item.on || "toggle"} onChange={(v) => onChange(setKey(item, "on", v))} />
    <NumberField label="Steps" value={item.steps} placeholder="2" onChange={(v) => onChange(setKey(item, "steps", v))} />
    <NumberField label="Low" value={item.low} placeholder="optional" onChange={(v) => onChange(setKey(item, "low", v))} />
    <NumberField label="High" value={item.high} placeholder="optional" onChange={(v) => onChange(setKey(item, "high", v))} />
    <TextField label="Variable" value={item.var} placeholder="boostOn" onChange={(v) => onChange(setKey(item, "var", v))} />
    <label class="field check"><span>Remember after reboot</span>
      <input type="checkbox" checked={item.persist !== false} onChange={(event) => onChange(setKey(item, "persist", event.target.checked))} />
    </label>
  </>;
  if (type === "system") return <MenuSelect label="Command" options={systemOps} value={item.command} onChange={(v) => onChange(setKey(item, "command", v))} />;
  if (type === "snapshot") return <NumberField label="Slot" value={item.slot} placeholder="1" onChange={(v) => onChange(setKey(item, "slot", v))} />;
  if (type === "tuner") return <>
    <MenuSelect label="Action" options={[["on","On"],["off","Off"]]} value={item.action || "on"} onChange={(v) => onChange(setKey(item, "action", v))} />
    <NumberField label="Channel" value={item.chan} placeholder="0" onChange={(v) => onChange(setKey(item, "chan", v))} />
  </>;
  return null;
}

export function ConditionTree({ node, onChange }) {
  if (!node) {
    return <p class="actions"><button type="button" class="ghost" onClick={() => onChange(blankGroup("and"))}>Add condition</button></p>;
  }
  const grouped = node.op === "and" || node.op === "or" || node.op === "not";
  if (grouped) {
    const items = node.items || [];
    return (
      <div class={"group " + node.op}>
        <div class="top">
          <MenuSelect options={[["and","All of (AND)"],["or","Any of (OR)"],["not","Not"]]} value={node.op} onChange={(v) => onChange({ ...node, op: v })} />
          <button type="button" class="ghost" onClick={() => onChange(null)}>Remove group</button>
        </div>
        {items.map((item, index) => (
          <ConditionTree
            key={index}
            node={item}
            onChange={(next) => {
              const copy = items.slice();
              if (next == null) copy.splice(index, 1);
              else copy[index] = next;
              onChange({ ...node, items: copy });
            }}
          />
        ))}
        {node.op !== "not" || items.length === 0 ? (
          <p class="actions">
            <button type="button" class="ghost" onClick={() => onChange({ ...node, items: items.concat([blankCondition()]) })}>Add condition</button>
            <button type="button" class="ghost" onClick={() => onChange({ ...node, items: items.concat([blankGroup("or")]) })}>Add group</button>
          </p>
        ) : null}
      </div>
    );
  }
  return (
    <div class="editor-row">
      <div class="top">
        <MenuSelect options={conditionTypes} value={node.type} onChange={(type) => {
          if (type === "and" || type === "or" || type === "not") onChange(blankGroup(type));
          else onChange({ type });
        }} />
        <button type="button" class="ghost" onClick={() => onChange(null)}>Remove</button>
      </div>
      <div class="grid">
        <StepFields group="conditions" item={node} onChange={onChange} />
      </div>
    </div>
  );
}

export function EditorRows({ group, items, onChange, blank }) {
  const types = group === "triggers" ? triggerTypes : actionTypes;
  return (
    <div>
      {items.map((item, index) => (
        <div class="editor-row" key={index}>
          <div class="top">
            <MenuSelect options={types} value={item.type} onChange={(type) => {
              const next = items.slice();
              next[index] = { type };
              onChange(next);
            }} />
            <button type="button" class="ghost" onClick={() => onChange(items.filter((_, i) => i !== index))}>Remove</button>
          </div>
          <div class="grid">
            <StepFields group={group} item={item} onChange={(next) => {
              const copy = items.slice();
              copy[index] = next;
              onChange(copy);
            }} />
          </div>
        </div>
      ))}
      <p class="actions"><button type="button" class="ghost" onClick={() => onChange(items.concat([blank()]))}>Add</button></p>
    </div>
  );
}

export { blankTrigger, blankAction };
