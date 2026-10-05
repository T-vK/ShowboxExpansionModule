import { useEffect, useState } from "preact/hooks";
import { Nav } from "../Nav.jsx";
import { getJson, postJson, postQuery } from "../api.js";

const jacks = [["1t","TRS 1 tip"],["1r","TRS 1 ring"],["2t","TRS 2 tip"],["2r","TRS 2 ring"],["3t","TRS 3 tip"],["3r","TRS 3 ring"],["4t","TRS 4 tip"],["4r","TRS 4 ring"]];
const gestures = [["press","Press"],["long","Long press"],["shortRelease","Short release"],["longRelease","Long release"],["double","Double press"]];
const messages = [["noteOn","Note on"],["noteOff","Note off"],["cc","Control change"]];
const links = [["bleHost","Bluetooth MIDI host"],["blePeripheral","Bluetooth MIDI peripheral"],["usbMidi","USB MIDI"]];
const edges = [["up","Comes up"],["down","Goes down"]];
const states = [["up","Is up"],["down","Is down"]];
const held = [["down","Is held"],["up","Is released"]];
const ops = [["eq","Equals"],["ne","Does not equal"],["gt","Greater than"],["lt","Less than"]];
const keys = [["pageup","Page Up"],["pagedown","Page Down"],["up","Up"],["down","Down"],["left","Left"],["right","Right"],["space","Space"],["enter","Enter"],["escape","Escape"],["tab","Tab"]];
const mouseButtons = [["0","None"],["1","Left"],["2","Right"],["4","Middle"]];
const touchGestures = [["tap","Tap"],["swipe","Swipe"],["pinch-in","Pinch in"],["pinch-out","Pinch out"]];
const looper = [["down","Press"],["up","Release"],["double","Double press"],["long","Long press"]];
const triggerTypes = [["trs","TRS footswitch"],["midi","MIDI message"],["connect","Connection change"],["showbox","Showbox control"]];
const conditionTypes = [["showbox","Showbox control"],["connection","Connection"],["trs","TRS footswitch"]];
const actionTypes = [["midi","MIDI message"],["hidKey","Keyboard key"],["hidMouse","Mouse"],["hidScroll","Mouse scroll"],["hidTouch","Touch"],["sleep","Wait"],["showbox","Showbox control"],["looper","Looper button"],["snapshot","Recall snapshot"]];
const entities = ["MAIN_MUTE","LOOPER_STATE","LOOPER_LEVEL","FX_BYPASS","FRONT_LED","SELECTED_CHAN","MAIN_MASTER_GAIN","MAIN_HEADPHONE_GAIN","INPUT1_GAIN","INPUT1_MUTE","INPUT2_GAIN","INPUT2_MUTE","INPUT3_GAIN","INPUT3_MUTE","INPUT4_GAIN","INPUT4_MUTE"];

function typesFor(group) {
  if (group === "triggers") return triggerTypes;
  if (group === "conditions") return conditionTypes;
  return actionTypes;
}

function Select({ label, value, options, onChange }) {
  return (
    <label class="field">{label}
      <select value={value || options[0][0]} onChange={(event) => onChange(event.target.value)}>
        {options.map(([id, name]) => <option value={id} key={id}>{name}</option>)}
      </select>
    </label>
  );
}

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
      <input type="text" list="entities" value={value == null ? "" : value} placeholder={placeholder || ""} onInput={(event) => onChange(event.target.value)} />
    </label>
  );
}

function setKey(item, key, value) {
  return { ...item, [key]: value };
}

function Fields({ group, item, onChange }) {
  const type = item.type;
  if (group === "triggers" && type === "trs") {
    return <>
      <Select label="Jack" options={jacks} value={item.jack} onChange={(v) => onChange(setKey(item, "jack", v))} />
      <Select label="Gesture" options={gestures} value={item.gesture} onChange={(v) => onChange(setKey(item, "gesture", v))} />
    </>;
  }
  if ((group === "triggers" || group === "actions") && type === "midi") {
    const any = group === "triggers";
    return <>
      <Select label="Message" options={messages} value={item.message} onChange={(v) => onChange(setKey(item, "message", v))} />
      <NumberField label="Channel" value={item.channel} placeholder={any ? "Any" : "1"} onChange={(v) => onChange(setKey(item, "channel", v))} />
      <NumberField label="Number" value={item.number} placeholder={any ? "Any" : "0"} onChange={(v) => onChange(setKey(item, "number", v))} />
      <NumberField label="Value" value={item.value} placeholder={any ? "Any" : "0"} onChange={(v) => onChange(setKey(item, "value", v))} />
    </>;
  }
  if (group === "triggers" && type === "connect") {
    return <>
      <Select label="Link" options={links} value={item.link} onChange={(v) => onChange(setKey(item, "link", v))} />
      <Select label="Change" options={edges} value={item.change} onChange={(v) => onChange(setKey(item, "change", v))} />
    </>;
  }
  if ((group === "triggers" || group === "actions" || group === "conditions") && type === "showbox") {
    return <>
      <TextField label="Entity" value={item.entity} placeholder="MAIN_MUTE" onChange={(v) => onChange(setKey(item, "entity", v))} />
      {group === "conditions" ? <Select label="Compare" options={ops} value={item.op || "eq"} onChange={(v) => onChange(setKey(item, "op", v))} /> : null}
      <TextField label="Value" value={item.value} placeholder={group === "triggers" ? "Any" : ""} onChange={(v) => onChange(setKey(item, "value", v))} />
    </>;
  }
  if (group === "conditions" && type === "connection") {
    return <>
      <Select label="Link" options={links} value={item.link} onChange={(v) => onChange(setKey(item, "link", v))} />
      <Select label="State" options={states} value={item.state} onChange={(v) => onChange(setKey(item, "state", v))} />
    </>;
  }
  if (group === "conditions" && type === "trs") {
    return <>
      <Select label="Jack" options={jacks} value={item.jack} onChange={(v) => onChange(setKey(item, "jack", v))} />
      <Select label="State" options={held} value={item.state} onChange={(v) => onChange(setKey(item, "state", v))} />
    </>;
  }
  if (type === "hidScroll") return <NumberField label="Amount" value={item.amount} placeholder="Negative is down" onChange={(v) => onChange(setKey(item, "amount", v))} />;
  if (type === "hidKey") return <Select label="Key" options={keys} value={item.key} onChange={(v) => onChange(setKey(item, "key", v))} />;
  if (type === "hidMouse") {
    return <>
      <NumberField label="X" value={item.x} placeholder="Relative -127 to 127" onChange={(v) => onChange(setKey(item, "x", v))} />
      <NumberField label="Y" value={item.y} placeholder="Relative -127 to 127" onChange={(v) => onChange(setKey(item, "y", v))} />
      <Select label="Buttons" options={mouseButtons} value={item.buttons || "0"} onChange={(v) => onChange(setKey(item, "buttons", v))} />
      <NumberField label="Wheel" value={item.wheel} placeholder="Negative is down" onChange={(v) => onChange(setKey(item, "wheel", v))} />
    </>;
  }
  if (type === "hidTouch") {
    const swipe = (item.gesture || item.action || "tap") === "swipe";
    return <>
      <div class="span"><Select label="Gesture" options={touchGestures} value={item.gesture || item.action || "tap"} onChange={(v) => onChange(setKey(item, "gesture", v))} /></div>
      <NumberField label="X" value={item.x} placeholder="0-10000, 5000 is center" onChange={(v) => onChange(setKey(item, "x", v))} />
      <NumberField label="Y" value={item.y} placeholder="0-10000, 5000 is center" onChange={(v) => onChange(setKey(item, "y", v))} />
      {swipe ? <>
        <NumberField label="X2" value={item.x2} placeholder="Swipe end X" onChange={(v) => onChange(setKey(item, "x2", v))} />
        <NumberField label="Y2" value={item.y2} placeholder="Swipe end Y" onChange={(v) => onChange(setKey(item, "y2", v))} />
      </> : null}
    </>;
  }
  if (type === "sleep") return <NumberField label="Milliseconds" value={item.ms} placeholder="0" onChange={(v) => onChange(setKey(item, "ms", v))} />;
  if (type === "looper") return <Select label="Button" options={looper} value={item.button} onChange={(v) => onChange(setKey(item, "button", v))} />;
  if (type === "snapshot") return <NumberField label="Slot" value={item.slot} placeholder="1" onChange={(v) => onChange(setKey(item, "slot", v))} />;
  return null;
}

function EditorRows({ group, items, onChange }) {
  const types = typesFor(group);
  return (
    <div>
      {items.map((item, index) => (
        <div class="editor-row" key={index}>
          <div class="top">
            <select class="kind" value={item.type} onChange={(event) => {
              const next = items.slice();
              next[index] = { type: event.target.value };
              onChange(next);
            }}>
              {types.map(([id, name]) => <option value={id} key={id}>{name}</option>)}
            </select>
            <button type="button" class="ghost" onClick={() => onChange(items.filter((_, i) => i !== index))}>Remove</button>
          </div>
          <div class="grid">
            <Fields group={group} item={item} onChange={(next) => {
              const copy = items.slice();
              copy[index] = next;
              onChange(copy);
            }} />
          </div>
        </div>
      ))}
    </div>
  );
}

function labelOf(list, value) {
  const found = list.find((entry) => entry[0] === value);
  return found ? found[1] : (value || "");
}

function describe(group, item) {
  if (!item) return "";
  if (item.type === "trs" && group === "triggers") return `TRS ${labelOf(jacks, item.jack)} ${labelOf(gestures, item.gesture)}`;
  if (item.type === "trs") return `TRS ${labelOf(jacks, item.jack)} ${labelOf(held, item.state)}`;
  if (item.type === "midi") {
    const where = [item.channel ? `ch ${item.channel}` : "any channel", item.number != null && item.number !== "" ? `#${item.number}` : "any number", item.value != null && item.value !== "" ? item.value : "any value"].join(", ");
    return `${labelOf(messages, item.message)} ${where}`;
  }
  if (item.type === "connect") return `${labelOf(links, item.link)} ${labelOf(edges, item.change)}`;
  if (item.type === "connection") return `${labelOf(links, item.link)} ${labelOf(states, item.state)}`;
  if (item.type === "showbox") {
    const op = item.op ? ` ${labelOf(ops, item.op)}` : "";
    return `${item.entity || "Showbox"}${op} ${item.value != null && item.value !== "" ? item.value : "any"}`;
  }
  if (item.type === "hidScroll") return `Scroll ${item.amount}`;
  if (item.type === "hidKey") return labelOf(keys, item.key);
  if (item.type === "hidMouse") {
    const bits = [];
    if (item.x != null && item.x !== "" && Number(item.x) !== 0) bits.push("x " + item.x);
    if (item.y != null && item.y !== "" && Number(item.y) !== 0) bits.push("y " + item.y);
    if (item.buttons != null && Number(item.buttons) !== 0) bits.push(labelOf(mouseButtons, String(item.buttons)));
    if (item.wheel != null && item.wheel !== "" && Number(item.wheel) !== 0) bits.push("wheel " + item.wheel);
    return bits.length ? "Mouse " + bits.join(", ") : "Mouse";
  }
  if (item.type === "hidTouch") {
    const gesture = item.gesture || item.action || "tap";
    const start = `${item.x == null || item.x === "" ? 5000 : item.x},${item.y == null || item.y === "" ? 5000 : item.y}`;
    if (gesture === "swipe") {
      return `Touch swipe ${start} → ${item.x2 == null || item.x2 === "" ? 7000 : item.x2},${item.y2 == null || item.y2 === "" ? 5000 : item.y2}`;
    }
    return `Touch ${labelOf(touchGestures, gesture)} ${start}`;
  }
  if (item.type === "sleep") return `Wait ${item.ms} ms`;
  if (item.type === "looper") return `Looper ${labelOf(looper, item.button)}`;
  if (item.type === "snapshot") return `Snapshot ${item.slot}`;
  return item.type;
}

export function Reactions() {
  const [data, setData] = useState({ automations: [] });
  const [error, setError] = useState("");
  const [editing, setEditing] = useState(null);

  async function refresh() {
    const next = await getJson("/api/v1/actions");
    setData(next);
    setError("");
  }

  useEffect(() => {
    refresh().catch((err) => setError(String(err.message || err)));
  }, []);

  function openEditor(rule) {
    setEditing({
      id: rule ? rule.id : 0,
      name: rule && rule.name ? rule.name : "",
      on: rule ? !!rule.on : true,
      triggers: rule && rule.triggers && rule.triggers.length ? rule.triggers : [{ type: "midi", message: "noteOn", channel: "16", number: "127" }],
      conditions: rule && rule.conditions ? rule.conditions : [],
      actions: rule && rule.actions && rule.actions.length ? rule.actions : [{ type: "sleep", ms: "0" }]
    });
    setError("");
  }

  const rules = data.automations || [];

  return (
    <main>
      <h1>Reactions</h1>
      <Nav current="/actions" />
      <p class="hint">When any trigger happens, and every condition is true, the actions run in order. A footswitch trigger replaces the built-in looper, boost, mute, or snapshot for that same gesture. Mouse moves are relative (-127 to 127). Touch coordinates are 0–10000 (5000 is center); X2/Y2 are the swipe end. Reactions are stored as JSON with triggers, conditions, and actions.</p>
      <p id="error">{error}</p>
      <p class="actions"><button type="button" onClick={() => openEditor(null)}>Add reaction</button></p>
      <div>
        {rules.length ? rules.map((rule) => {
          const lines = []
            .concat((rule.triggers || []).map((item) => "When " + describe("triggers", item)))
            .concat((rule.conditions || []).map((item) => "And if " + describe("conditions", item)))
            .concat((rule.actions || []).map((item) => "Then " + describe("actions", item)));
          return (
            <article class="card" key={rule.id}>
              <label class="switch">
                <input type="checkbox" checked={!!rule.on} onChange={(event) => {
                  const next = { ...rule, on: event.target.checked };
                  postJson("/api/v1/actions", next).then(setData).catch((err) => { event.target.checked = !event.target.checked; setError(err.message); });
                }} />
                <span />
              </label>
              <div class="body">
                <strong>{rule.name || ("Reaction " + rule.id)}</strong>
                <ul class="lines">{lines.map((line, i) => <li key={i}>{line}</li>)}</ul>
                <div class="actions">
                  <button type="button" class="ghost" onClick={() => postQuery("/api/v1/actions/run?id=" + rule.id).catch((err) => setError(err.message))}>Run</button>
                  <button type="button" class="ghost" onClick={() => openEditor(rule)}>Edit</button>
                  <button type="button" class="warn" onClick={() => postQuery("/api/v1/actions/delete?id=" + rule.id).then(setData).catch((err) => setError(err.message))}>Delete</button>
                </div>
              </div>
            </article>
          );
        }) : <p class="empty">No reactions yet.</p>}
      </div>
      <datalist id="entities">{entities.map((name) => <option value={name} key={name} />)}</datalist>
      <div class={"overlay" + (editing ? " open" : "")}>
        {editing ? (
          <div class="sheet">
            <h2>{editing.id ? "Edit reaction" : "Add reaction"}</h2>
            <label class="field">Name
              <input type="text" maxlength="80" placeholder="Optional" value={editing.name} onInput={(event) => setEditing({ ...editing, name: event.target.value })} />
            </label>
            <h2>When</h2>
            <p class="hint">Any one of these.</p>
            <EditorRows group="triggers" items={editing.triggers} onChange={(triggers) => setEditing({ ...editing, triggers })} />
            <p class="actions"><button type="button" class="ghost" onClick={() => setEditing({ ...editing, triggers: editing.triggers.concat([{ type: "trs", jack: "1t", gesture: "press" }]) })}>Add trigger</button></p>
            <h2>And if</h2>
            <p class="hint">Every one of these is true. Leave this empty to always run.</p>
            <EditorRows group="conditions" items={editing.conditions} onChange={(conditions) => setEditing({ ...editing, conditions })} />
            <p class="actions"><button type="button" class="ghost" onClick={() => setEditing({ ...editing, conditions: editing.conditions.concat([{ type: "connection", link: "blePeripheral", state: "up" }]) })}>Add condition</button></p>
            <h2>Then</h2>
            <EditorRows group="actions" items={editing.actions} onChange={(actions) => setEditing({ ...editing, actions })} />
            <p class="actions">
              <button type="button" class="ghost" onClick={() => setEditing({ ...editing, actions: editing.actions.concat([{ type: "midi", message: "noteOn", channel: "1", number: "60", value: "100" }]) })}>Add action</button>
              <button type="button" onClick={() => {
                postJson("/api/v1/actions", editing).then((next) => { setEditing(null); setData(next); }).catch((err) => setError(err.message));
              }}>Save</button>
              <button type="button" class="ghost" onClick={() => setEditing(null)}>Cancel</button>
            </p>
          </div>
        ) : null}
      </div>
    </main>
  );
}
