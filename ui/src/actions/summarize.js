import {
  actionTypes, conditionTypes, edges, entityGroups, gestures, held, hidDevices, hidOps,
  jacks, links, looperOps, mediaKeys, midiMessages, midiPorts, ops, panelButtons, states,
  systemOps, triggerTypes, varOps
} from "./schema.js";

const MIDI_SHORT = {
  noteOn: "Note on",
  noteOff: "Note off",
  cc: "CC",
  pc: "PC",
  pitchBend: "Pitch bend",
  aftertouch: "Aftertouch",
  polyAftertouch: "Poly AT",
  clock: "Clock",
  start: "Start",
  continue: "Continue",
  stop: "Stop",
  reset: "Reset",
  raw: "SysEx",
  sysex: "SysEx"
};

const LOOPER_SHORT = {
  cycle: "rec/play/overdub",
  stop: "stop",
  delete: "delete",
  press: "press",
  long: "long press",
  double: "double press",
  up: "release"
};

const LOOPER_STATE = {
  0: "cleared",
  1: "recording",
  2: "stopped",
  3: "play",
  4: "overdub"
};

function pick(options, id, fallback) {
  if (id == null || id === "") return fallback || "";
  const hit = options.find(([key]) => key === String(id));
  return hit ? hit[1] : String(id);
}

function joinList(items, word) {
  const bits = items.filter(Boolean);
  if (!bits.length) return "";
  if (bits.length === 1) return bits[0];
  if (bits.length === 2) return `${bits[0]} ${word} ${bits[1]}`;
  return `${bits.slice(0, -1).join(", ")} ${word} ${bits[bits.length - 1]}`;
}

function entityLabel(id) {
  if (!id) return "control";
  for (const group of entityGroups) {
    for (const field of group.fields) {
      if (field.id === id) {
        if (id === "LOOPER_STATE") return "Looper state";
        if (id === "LOOPER_LEVEL") return "Looper level";
        if (group.title === "Main") return `Main ${field.label.toLowerCase()}`;
        return `${group.title} ${field.label.toLowerCase()}`;
      }
    }
  }
  return String(id);
}

function prettyValue(entity, value) {
  if (value == null || value === "") return "";
  const s = String(value);
  if (entity === "LOOPER_STATE" && LOOPER_STATE[s] != null) return LOOPER_STATE[s];
  if (s === "true") return "on";
  if (s === "false") return "off";
  return s;
}

function midiBits(item) {
  const kind = MIDI_SHORT[item.message] || pick(midiMessages, item.message, "MIDI");
  let head = kind;
  if (item.number != null && item.number !== "") head = `${kind} ${item.number}`;
  const extra = [];
  if (item.channel != null && item.channel !== "") extra.push(`ch ${item.channel}`);
  if (item.value != null && item.value !== "") extra.push(`value ${item.value}`);
  const ports = Array.isArray(item.ports) ? item.ports : (item.port ? [item.port] : []);
  if (ports.length) extra.push(ports.map((port) => pick(midiPorts, port, port)).join("/"));
  return extra.length ? `${head}, ${extra.join(", ")}` : head;
}

function describeMidi(item) {
  return `MIDI (${midiBits(item)})`;
}

function describeTrigger(item) {
  const type = item && item.type;
  if (type === "midi") return describeMidi(item);
  if (type === "trs") {
    const jack = pick(jacks, item.jack, item.jack || "TRS");
    const gesture = pick(gestures, item.gesture, item.gesture || "press").toLowerCase();
    return `${jack} (${gesture})`;
  }
  if (type === "panel") {
    const button = pick(panelButtons, item.button, item.button || "button").split(" (")[0];
    const gesture = item.gesture === "long" ? "long press" : "click";
    return `Front panel (${button}, ${gesture})`;
  }
  if (type === "connect") {
    const link = pick(links, item.link, item.link || "link");
    const change = pick(edges, item.change, item.change || "change").toLowerCase();
    return `Connection (${link} ${change})`;
  }
  if (type === "showbox") {
    const name = entityLabel(item.entity);
    const value = prettyValue(item.entity, item.value);
    return value ? `Showbox (${name} = ${value})` : `Showbox (${name})`;
  }
  if (type === "showboxPacket") {
    const bits = [item.dir && item.dir !== "any" ? item.dir : "any"];
    if (item.packetType != null && item.packetType !== "") bits.push(`type ${item.packetType}`);
    if (item.hex) bits.push(item.hex);
    return `Showbox packet (${bits.join(", ")})`;
  }
  if (type === "variable") {
    const value = item.value != null && item.value !== "" ? ` = ${item.value}` : "";
    return `Variable (${item.name || "var"}${value})`;
  }
  return pick(triggerTypes, type, type || "trigger");
}

function opWord(op) {
  return pick(ops, op || "eq", op || "equals").toLowerCase();
}

function describeLeafCondition(node) {
  const type = node.type;
  if (type === "showbox") {
    const name = entityLabel(node.entity);
    const value = prettyValue(node.entity, node.value) || "anything";
    return `${name} ${opWord(node.op)} ${value}`;
  }
  if (type === "connection") {
    return `${pick(links, node.link, node.link)} ${pick(states, node.state, node.state).toLowerCase()}`;
  }
  if (type === "midiPort") {
    return `MIDI ${pick(midiPorts, node.port || node.link, node.port || node.link)} ${pick(states, node.state, node.state).toLowerCase()}`;
  }
  if (type === "trs") {
    return `${pick(jacks, node.jack, node.jack)} ${pick(held, node.state, node.state).toLowerCase()}`;
  }
  if (type === "variable") {
    return `${node.name || "variable"} ${opWord(node.op)} ${node.value ?? ""}`.trim();
  }
  return pick(conditionTypes, type, type || "condition");
}

function describeCondition(node) {
  if (!node) return "";
  if (node.op === "and" || node.op === "or" || node.op === "not") {
    const items = (node.items || []).map(describeCondition).filter(Boolean);
    if (node.op === "not") return items[0] ? `not ${items[0]}` : "";
    return joinList(items, node.op);
  }
  return describeLeafCondition(node);
}

function describeAction(item) {
  const type = item && item.type;
  if (type === "midi") return describeMidi(item);
  if (type === "looper") {
    const op = item.button || item.op || "cycle";
    return `Looper (${LOOPER_SHORT[op] || pick(looperOps, op, op).toLowerCase()})`;
  }
  if (type === "gainBoost") {
    const name = entityLabel(item.entity || "INPUT2_GAIN");
    const bits = [name, item.on || "toggle"];
    if (item.steps) bits.push(`${item.steps} steps`);
    if (item.low != null && item.low !== "" && item.high != null && item.high !== "") {
      bits.push(`${item.low}–${item.high}`);
    }
    return `Gain boost (${bits.join(", ")})`;
  }
  if (type === "hid") {
    const deviceId = item.device || "mouse";
    const device = pick(hidDevices, deviceId, deviceId);
    const opsFor = hidOps[deviceId] || [];
    const op = pick(opsFor, item.op, item.op || "").toLowerCase();
    const extra = [];
    if (item.key) extra.push(item.key);
    if (item.text) extra.push(`“${item.text}”`);
    if (item.media || (item.op === "media" && item.key)) extra.push(pick(mediaKeys, item.media || item.key, item.media || item.key));
    return `HID (${device} ${op}${extra.length ? " " + extra.join(" ") : ""})`.replace(/\s+/g, " ").trim();
  }
  if (type === "sleep") return `Wait (${item.ms || 0} ms)`;
  if (type === "showbox") {
    const name = entityLabel(item.entity);
    const value = prettyValue(item.entity, item.value);
    return value ? `Showbox (${name} = ${value})` : `Showbox (${name})`;
  }
  if (type === "showboxPacket") return "Showbox packet";
  if (type === "variable") {
    const op = pick(varOps, item.op || "set", item.op).toLowerCase();
    const value = item.op === "toggle" ? "" : ` = ${item.value ?? ""}`;
    return `Variable (${op} ${item.name || "var"}${value})`;
  }
  if (type === "system") return pick(systemOps, item.command, item.command || "system");
  if (type === "snapshot") return `Recall snapshot ${item.slot || 1}`;
  if (type === "sdcard") return "Record to SD";
  if (type === "tuner") {
    const bits = [item.action || "on"];
    if (item.chan != null && item.chan !== "") bits.push(`ch ${item.chan}`);
    return `Tuner (${bits.join(", ")})`;
  }
  return pick(actionTypes, type, type || "action");
}

export function summarize(rule) {
  const triggers = (rule.triggers || []).map(describeTrigger);
  const actions = (rule.actions || []).map(describeAction);
  let cond = describeCondition(rule.condition);
  if (!cond && rule.conditions && rule.conditions.length) {
    cond = joinList(rule.conditions.map(describeCondition), "and");
  }
  let text = `When ${joinList(triggers, "or") || "trigger"}`;
  if (cond) text += ` and ${cond}`;
  text += ` then ${joinList(actions, "and") || "action"}`;
  return text;
}
