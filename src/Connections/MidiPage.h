#pragma once

// Served at /midi. No external assets: the setup access point has no internet.
static const char MIDI_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Connections</title>
<style>
  :root { color-scheme: dark; }
  body { margin: 0; font: 16px/1.45 system-ui, sans-serif; background: #121212; color: #eee; }
  main { max-width: 46rem; margin: 0 auto; padding: 1.25rem; }
  h1 { font-size: 1.4rem; margin: 0 0 0.25rem; }
  h2 { font-size: 1rem; margin: 0.75rem 0; }
  a { color: #9cf; }
  p.lead, .hint { color: #aaa; }
  section { background: #1c1c1c; border-radius: 12px; padding: 0.4rem 1rem 1rem; margin: 1rem 0; }
  .row, li { display: flex; align-items: flex-start; justify-content: space-between; gap: 0.8rem; padding: 0.75rem 0; border-top: 1px solid #2a2a2a; }
  .row:first-of-type, li:first-child { border-top: 0; }
  .copy { min-width: 0; }
  .copy strong { display: block; }
  .copy small, .meta { display: block; color: #9a9a9a; }
  .off { opacity: 0.6; }
  button, .switch { font: inherit; }
  button { background: #2d6cdf; color: white; border: 0; border-radius: 8px; padding: 0.4rem 0.7rem; }
  button.ghost { background: #333; }
  button.warn { background: #633; }
  button:disabled { opacity: 0.45; }
  .actions { display: flex; flex-wrap: wrap; gap: 0.45rem; align-items: flex-end; }
  .switch { position: relative; width: 2.8rem; height: 1.6rem; flex: none; }
  .switch input { opacity: 0; width: 0; height: 0; }
  .switch span { position: absolute; inset: 0; background: #444; border-radius: 999px; }
  .switch span:before { content: ""; position: absolute; height: 1.2rem; width: 1.2rem; left: 0.2rem; top: 0.2rem; background: white; border-radius: 50%; }
  .switch input:checked + span { background: #2d6cdf; }
  .switch input:checked + span:before { transform: translateX(1.2rem); }
  .switch input:disabled + span { opacity: 0.5; }
  .role span { background: #9a3030; }
  .role input:checked + span { background: #1f8a45; }
  .role input:disabled + span { background: #3a3a3a; }
  .labeled { display: flex; flex-direction: column; align-items: center; gap: 0.15rem; font-size: 0.72rem; color: #bbb; }
  .labeled.disabled { color: #666; }
  ul { list-style: none; padding: 0; margin: 0; }
  .overlay { display: none; position: fixed; inset: 0; background: rgba(0,0,0,.62); z-index: 20; padding: 1.25rem; }
  .overlay.open { display: block; }
  .sheet { background: #1c1c1c; border-radius: 12px; max-width: 46rem; margin: 0 auto; max-height: calc(100vh - 2.5rem); overflow: auto; padding: 0.4rem 1rem 1rem; }
  #error { color: #f88; min-height: 1.2em; }
  form { display: flex; flex-wrap: wrap; gap: 0.4rem; align-items: center; }
  input[type=text] { font: inherit; background: #111; color: #eee; border: 1px solid #444; border-radius: 8px; padding: 0.4rem 0.6rem; min-width: 12rem; }
</style>
</head>
<body>
<main>
  <h1>Connections</h1>
  <p class="lead"><a href="/remote-ui">Showbox controls</a></p>
  <p id="error"></p>
  <section>
    <h2>Transports</h2>
    <div id="interfaces"></div>
  </section>
  <section>
    <h2>Paired peripherals</h2>
    <p class="hint">Green is on, red is off. Disconnect drops the link. Turn Auto Connect off and on to connect again. Forget is shown only while a device is not connected. Serial MIDI stays off because the ESP32-S3 has no Bluetooth Classic radio.</p>
    <p class="actions"><button id="add-open" type="button">Add new device...</button></p>
    <ul id="peripherals"></ul>
  </section>
  <section>
    <h2>Paired hosts</h2>
    <p class="hint">Turn on Discoverable so a computer or phone can find ShowboxMod and pair. MIDI and HID apply to each computer. Disconnect is shown only while it is connected. Forget is shown only while it is not.</p>
    <div id="discover"></div>
    <ul id="hosts"></ul>
  </section>
  <section>
    <h2>Send keys and mouse</h2>
    <p class="hint">Sends keyboard and mouse input to a connected tablet, phone, or computer. Use Bluetooth HID Peripheral with a paired host, or USB HID Peripheral.</p>
    <form id="keys">
      <input id="keytext" type="text" maxlength="24" placeholder="Text to type">
      <button type="submit">Send keys</button>
    </form>
    <p class="actions">
      <button type="button" data-x="-20" data-y="0">Left</button>
      <button type="button" data-x="20" data-y="0">Right</button>
      <button type="button" data-x="0" data-y="-20">Up</button>
      <button type="button" data-x="0" data-y="20">Down</button>
      <button type="button" id="click" class="ghost">Click</button>
    </p>
  </section>
</main>
<div id="add" class="overlay">
  <div class="sheet">
    <h2>New devices</h2>
    <p class="hint">Results stay here until you close this window.</p>
    <p class="actions"><button id="scan" type="button">Scan</button><button id="add-close" type="button" class="ghost">Close</button></p>
    <ul id="nearby"></ul>
  </div>
</div>
<script>
const transports = [
  ["din", "DIN MIDI", "Send and receive MIDI through the three 5-pin DIN connectors (In/Out/Through)."],
  ["bleMidiHost", "Bluetooth MIDI Host", "Find and connect to Bluetooth LE MIDI devices such as pedals and controllers."],
  ["bleMidiPeripheral", "Bluetooth MIDI Peripheral", "Connect to laptops, phones, or tablets as a Bluetooth MIDI peripheral. Advertises as ShowboxMod."],
  ["bleClassic", "Bluetooth Serial MIDI", "The ESP32-S3 has no Bluetooth Classic radio, so Serial MIDI over Bluetooth stays off."],
  ["usbMidiHost", "USB MIDI Host", "Plug a USB MIDI device into the USB port. Requires reboot. Turns off USB MIDI Peripheral, USB HID Peripheral, and USB serial/JTAG. OTA and the UART connector can still flash."],
  ["usbMidiPeripheral", "USB MIDI Peripheral", "Appear as a USB MIDI device. Requires reboot. Turns off USB MIDI Host, USB HID Peripheral, and USB serial/JTAG. OTA and the UART connector can still flash."],
  ["wifi", "WiFi MIDI", "Send and receive MIDI over Wi-Fi. RTP-MIDI / AppleMIDI on UDP port 5004. The service name is AppleMidi."],
  ["ethernet", "Ethernet MIDI", "Send and receive MIDI over the W5500 on UDP ports 5004 and 5005. Add a manual session to the address shown here."],
  ["bleHid", "Bluetooth HID Peripheral", "Allow sending keyboard and mouse input to a connected tablet, phone, or computer via Bluetooth."],
  ["usbHid", "USB HID Peripheral", "Allow sending keyboard and mouse input to a connected tablet, phone, or computer via USB. Requires reboot. Turns off USB MIDI Host, USB MIDI Peripheral, and USB serial/JTAG. OTA and the UART connector can still flash."]
];
let addOpen = false;
const foundDevices = new Map();

function showError(text) { document.getElementById("error").textContent = text || ""; }
function asList(value) { return Array.isArray(value) ? value : []; }

function toggle(checked, available, onChange) {
  const label = document.createElement("label");
  label.className = "switch";
  const input = document.createElement("input");
  input.type = "checkbox";
  input.checked = !!checked;
  input.disabled = !available;
  input.addEventListener("change", onChange);
  label.appendChild(input);
  label.appendChild(document.createElement("span"));
  return label;
}

function roleToggle(checked, enabled, onChange) {
  const label = document.createElement("label");
  label.className = "switch role";
  const input = document.createElement("input");
  input.type = "checkbox";
  input.checked = !!checked && enabled;
  input.disabled = !enabled;
  if (onChange) input.addEventListener("change", onChange);
  label.appendChild(input);
  label.appendChild(document.createElement("span"));
  return label;
}

function labeled(text, control, enabled, title) {
  const wrap = document.createElement("div");
  wrap.className = "labeled" + (enabled ? "" : " disabled");
  if (title) wrap.title = title;
  const name = document.createElement("span");
  name.textContent = text;
  wrap.appendChild(name);
  wrap.appendChild(control);
  return wrap;
}

async function post(url) {
  const response = await fetch(url, { method: "POST" });
  if (!response.ok) throw new Error(await response.text());
  showError("");
  refresh();
}

function transportRow(spec, item) {
  const [id, title, description] = spec;
  const available = !item || item.available !== false;
  const wrap = document.createElement("div");
  wrap.className = "row" + (available ? "" : " off");
  const text = document.createElement("div");
  text.className = "copy";
  const heading = document.createElement("strong");
  heading.textContent = title;
  const small = document.createElement("small");
  small.textContent = description;
  text.appendChild(heading);
  text.appendChild(small);
  if (item && item.note) {
    const note = document.createElement("small");
    note.textContent = item.note;
    text.appendChild(note);
  }
  const input = toggle(item && item.enabled, available && item, async (event) => {
    const usbReboot = {
      usbMidiPeripheral: [
        "Requires reboot. Turns off USB serial/JTAG, USB MIDI Host, and USB HID Peripheral. OTA and the UART connector can still flash.",
        "Requires reboot so USB serial/JTAG comes back."
      ],
      usbMidiHost: [
        "Requires reboot. Turns off USB serial/JTAG, USB MIDI Peripheral, and USB HID Peripheral. Plug a MIDI device into the USB port. OTA and the UART connector can still flash.",
        "Requires reboot so USB serial/JTAG comes back."
      ],
      usbHid: [
        "Requires reboot. Turns off USB serial/JTAG, USB MIDI Host, and USB MIDI Peripheral. OTA and the UART connector can still flash.",
        "Requires reboot so USB serial/JTAG comes back."
      ]
    };
    if (usbReboot[id]) {
      const turningOn = event.target.checked;
      if (!window.confirm(usbReboot[id][turningOn ? 0 : 1])) {
        event.target.checked = !turningOn;
        return;
      }
    }
    try {
      await post("/api/v1/midi/settings?" + id + "=" + (event.target.checked ? "1" : "0"));
    } catch (err) {
      event.target.checked = !event.target.checked;
      showError(err.message);
    }
  });
  wrap.appendChild(text);
  wrap.appendChild(input);
  return wrap;
}

function setRole(address, key, on, event) {
  post("/api/v1/midi/ble/role?address=" + encodeURIComponent(address) + "&" + key + "=" + (on ? "1" : "0"))
    .catch(err => { event.target.checked = !on; showError(err.message); });
}

function deviceLine(device, actions) {
  const li = document.createElement("li");
  const info = document.createElement("div");
  info.className = "copy";
  const title = document.createElement("strong");
  title.textContent = device.name || device.address || "(no name)";
  const meta = document.createElement("div");
  meta.className = "meta";
  const bits = [];
  if (device.address && device.name) bits.push(device.address);
  if (device.rssi !== undefined) bits.push(device.rssi + " dBm");
  if (device.connected === true) bits.push("Connected");
  if (device.connected === false) bits.push("Not connected");
  meta.textContent = bits.join(" · ");
  info.appendChild(title);
  info.appendChild(meta);
  li.appendChild(info);
  li.appendChild(actions);
  return li;
}

async function refresh() {
  const data = await fetch("/api/v1/midi").then(r => r.json());
  const byId = {};
  for (const item of asList(data.transports)) byId[item.id] = item;
  const box = document.getElementById("interfaces");
  box.replaceChildren();
  for (const spec of transports) box.appendChild(transportRow(spec, byId[spec[0]] || { available: false, enabled: false }));

  document.getElementById("scan").textContent = data.scanning ? "Scanning…" : "Scan";
  if (addOpen) {
    const paired = new Set(asList(data.peripherals).map(device => (device.address || "").toLowerCase()));
    for (const device of asList(data.nearby)) {
      if (!device.address) continue;
      const key = device.address.toLowerCase();
      const prev = foundDevices.get(key);
      if (!prev) {
        foundDevices.set(key, { name: device.name || "", address: device.address, rssi: device.rssi, midi: !!device.midi, order: foundDevices.size });
      } else {
        if (device.name) prev.name = device.name;
        if (device.rssi !== undefined) prev.rssi = device.rssi;
        if (device.midi) prev.midi = true;
      }
    }
    const nearby = document.getElementById("nearby");
    nearby.replaceChildren();
    const rows = Array.from(foundDevices.values()).sort((a, b) => {
      const named = (a.name ? 0 : 1) - (b.name ? 0 : 1);
      return named !== 0 ? named : a.order - b.order;
    });
    for (const device of rows) {
      const actions = document.createElement("div");
      actions.className = "actions";
      const pair = document.createElement("button");
      pair.type = "button";
      const already = paired.has(device.address.toLowerCase());
      pair.textContent = already ? "Paired" : "Pair";
      pair.disabled = already;
      pair.addEventListener("click", () => post("/api/v1/midi/ble/connect?pair=1&address=" + encodeURIComponent(device.address) + "&name=" + encodeURIComponent(device.name || "")).catch(err => showError(err.message)));
      actions.appendChild(pair);
      nearby.appendChild(deviceLine(device, actions));
    }
    if (nearby.children.length === 0) {
      const li = document.createElement("li");
      li.textContent = data.scanning ? "Scanning…" : "No devices yet.";
      nearby.appendChild(li);
    }
  }

  const peripherals = document.getElementById("peripherals");
  peripherals.replaceChildren();
  for (const device of asList(data.peripherals)) {
    const actions = document.createElement("div");
    actions.className = "actions";
    const serialHint = "The ESP32-S3 has no Bluetooth Classic radio, so Serial MIDI stays off.";
    actions.appendChild(labeled("MIDI", roleToggle(device.midi, true, (event) => setRole(device.address, "midi", event.target.checked, event)), true));
    actions.appendChild(labeled("Serial MIDI", roleToggle(false, false, null), false, serialHint));
    actions.appendChild(labeled("Auto Connect", roleToggle(device.autoConnect, true, (event) => post("/api/v1/midi/ble/auto?address=" + encodeURIComponent(device.address) + "&enabled=" + (event.target.checked ? "1" : "0")).catch(err => { event.target.checked = !event.target.checked; showError(err.message); })), true));
    if (device.connected) {
      const disconnect = document.createElement("button");
      disconnect.type = "button";
      disconnect.className = "ghost";
      disconnect.textContent = "Disconnect";
      disconnect.addEventListener("click", () => post("/api/v1/midi/ble/disconnect?address=" + encodeURIComponent(device.address)).catch(err => showError(err.message)));
      actions.appendChild(disconnect);
    } else {
      const forget = document.createElement("button");
      forget.type = "button";
      forget.className = "warn";
      forget.textContent = "Forget";
      forget.addEventListener("click", () => post("/api/v1/midi/ble/forget?address=" + encodeURIComponent(device.address)).catch(err => showError(err.message)));
      actions.appendChild(forget);
    }
    peripherals.appendChild(deviceLine(device, actions));
  }
  if (peripherals.children.length === 0) {
    const li = document.createElement("li");
    li.textContent = "No paired peripherals.";
    peripherals.appendChild(li);
  }

  const discover = document.getElementById("discover");
  discover.replaceChildren();
  const discoverRow = document.createElement("div");
  discoverRow.className = "row";
  const discoverCopy = document.createElement("div");
  discoverCopy.className = "copy";
  const discoverTitle = document.createElement("strong");
  discoverTitle.textContent = "Discoverable";
  const discoverHint = document.createElement("small");
  discoverHint.textContent = "A computer or phone can find ShowboxMod and pair with it.";
  discoverCopy.appendChild(discoverTitle);
  discoverCopy.appendChild(discoverHint);
  discoverRow.appendChild(discoverCopy);
  discoverRow.appendChild(roleToggle(!!data.discoverable, true, (event) => post("/api/v1/midi/settings?bleMidiPeripheral=" + (event.target.checked ? "1" : "0")).catch(err => { event.target.checked = !event.target.checked; showError(err.message); })));
  discover.appendChild(discoverRow);

  const hosts = document.getElementById("hosts");
  hosts.replaceChildren();
  for (const device of asList(data.hosts)) {
    const actions = document.createElement("div");
    actions.className = "actions";
    actions.appendChild(labeled("MIDI", roleToggle(device.midi, true, (event) => setRole(device.address, "midi", event.target.checked, event)), true));
    actions.appendChild(labeled("HID", roleToggle(device.hid, true, (event) => setRole(device.address, "hid", event.target.checked, event)), true));
    if (device.connected) {
      const disconnect = document.createElement("button");
      disconnect.type = "button";
      disconnect.className = "ghost";
      disconnect.textContent = "Disconnect";
      disconnect.addEventListener("click", () => post("/api/v1/midi/ble/disconnect?address=" + encodeURIComponent(device.address)).catch(err => showError(err.message)));
      actions.appendChild(disconnect);
    } else {
      const forget = document.createElement("button");
      forget.type = "button";
      forget.className = "warn";
      forget.textContent = "Forget";
      forget.addEventListener("click", () => post("/api/v1/midi/ble/forget?address=" + encodeURIComponent(device.address)).catch(err => showError(err.message)));
      actions.appendChild(forget);
    }
    hosts.appendChild(deviceLine(device, actions));
  }
  if (hosts.children.length === 0) {
    const li = document.createElement("li");
    li.textContent = "No paired hosts.";
    hosts.appendChild(li);
  }
}

function openAdd() {
  foundDevices.clear();
  addOpen = true;
  document.getElementById("add").classList.add("open");
  document.getElementById("nearby").replaceChildren();
  post("/api/v1/midi/ble/scan").catch(err => showError(err.message));
}
function closeAdd() {
  addOpen = false;
  foundDevices.clear();
  document.getElementById("add").classList.remove("open");
}
document.getElementById("add-open").addEventListener("click", openAdd);
document.getElementById("add-close").addEventListener("click", closeAdd);
document.getElementById("add").addEventListener("click", (event) => { if (event.target.id === "add") closeAdd(); });
document.getElementById("scan").addEventListener("click", () => post("/api/v1/midi/ble/scan").catch(err => showError(err.message)));
document.getElementById("keys").addEventListener("submit", (event) => {
  event.preventDefault();
  const text = document.getElementById("keytext").value;
  post("/api/v1/hid/keyboard?text=" + encodeURIComponent(text)).catch(err => showError(err.message));
});
for (const button of document.querySelectorAll("[data-x]")) {
  button.addEventListener("click", () => {
    post("/api/v1/hid/mouse?x=" + button.dataset.x + "&y=" + button.dataset.y).catch(err => showError(err.message));
  });
}
document.getElementById("click").addEventListener("click", () => post("/api/v1/hid/mouse?x=0&y=0&buttons=1").catch(err => showError(err.message)));
refresh().catch(err => showError(String(err)));
setInterval(() => refresh().catch(err => showError(String(err))), 2000);
</script>
</body>
</html>
)rawliteral";
