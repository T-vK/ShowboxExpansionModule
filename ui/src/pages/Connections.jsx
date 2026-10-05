import { useEffect, useRef, useState } from "preact/hooks";
import { Nav } from "../Nav.jsx";
import { asList, getJson, postQuery, sleep } from "../api.js";

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
    "Requires reboot. Turns off USB serial/JTAG, USB MIDI Host, and USB MIDI Peripheral. OTA and the UART connector can still flash. If Timed USB test is on, serial/JTAG comes back after about 75 seconds.",
    "Requires reboot so USB serial/JTAG comes back."
  ],
  usbJtag: [
    "USB serial/JTAG is already on.",
    "Requires reboot. Turns off USB MIDI and USB HID on this port so serial/JTAG comes back. OTA still works."
  ]
};

function Switch({ checked, disabled, role, onChange }) {
  return (
    <label class={"switch" + (role ? " role" : "")}>
      <input type="checkbox" checked={!!checked} disabled={!!disabled} onChange={onChange} />
      <span />
    </label>
  );
}

function Badges({ labels }) {
  if (!labels.length) return null;
  return <div class="badges">{labels.map((label) => <span class="badge" key={label}>{label}</span>)}</div>;
}

function DeviceLine({ device, extra, children }) {
  const bits = [];
  if (device.address && device.name) bits.push(device.address);
  if (device.rssi !== undefined) bits.push(device.rssi + " dBm");
  if (typeof device.battery === "number" && device.battery >= 0) bits.push(device.battery + "%");
  if (device.connected === true) bits.push("Connected");
  if (device.connected === false) bits.push("Remembered, not connected");
  return (
    <li class="device">
      <div class="copy">
        <strong>{device.name || device.address || "(no name)"}</strong>
        <div class="meta">{bits.join(" · ")}</div>
        {extra}
      </div>
      <div class="actions">{children}</div>
    </li>
  );
}

function Row({ title, description, note, available = true, checked, disabled, role, danger, onChange, children }) {
  return (
    <div class={"row" + (available ? "" : " off") + (danger ? " danger" : "")}>
      <div class="copy">
        <strong>{title}</strong>
        {description ? <small>{description}</small> : null}
        {note ? <small>{note}</small> : null}
        {children}
      </div>
      {onChange ? <Switch checked={checked} disabled={disabled || !available} role={role} onChange={onChange} /> : null}
    </div>
  );
}

function Branch({ title, children }) {
  return (
    <div class="branch">
      <h3>{title}</h3>
      <div class="nest">{children}</div>
    </div>
  );
}

export function Connections() {
  const [data, setData] = useState(null);
  const [error, setError] = useState("");
  const [addOpen, setAddOpen] = useState(false);
  const [keytext, setKeytext] = useState("");
  const found = useRef(new Map());
  const usbSafe = !!(data && data.debug && data.debug.usbSafeTest);

  async function refresh() {
    const next = await getJson("/api/v1/midi");
    setData(next);
    setError("");
  }

  async function post(url) {
    setError("The module is busy. Retrying…");
    try {
      await postQuery(url);
      await sleep(250);
      await refresh();
    } catch (err) {
      setError(err.message);
      throw err;
    }
  }

  useEffect(() => {
    let stop = false;
    const tick = () => refresh().catch((err) => { if (!stop) setError(String(err.message || err)); });
    tick();
    const id = setInterval(tick, 3000);
    return () => { stop = true; clearInterval(id); };
  }, []);

  function peripheralBadges(device) {
    const labels = [];
    if (device.midi) labels.push("MIDI");
    if (device.hid || device.advertisedHid) labels.push("HID");
    if (device.advertisedBattery || (typeof device.battery === "number" && device.battery >= 0)) labels.push("Battery");
    if (device.pinned) labels.push("Reserved");
    return <Badges labels={labels} />;
  }

  function hostBadges(device) {
    const labels = [];
    if (device.midi) labels.push("MIDI");
    if (device.hid) {
      if (device.hidKeyboard) labels.push("Keyboard");
      if (device.hidMouse) labels.push("Mouse");
      if (device.hidAbs) labels.push("Abs pointer");
      if (device.hidDigitizer) labels.push("Touch");
      if (!device.hidKeyboard && !device.hidMouse && !device.hidAbs && !device.hidDigitizer) labels.push("HID");
    }
    if (device.pinned) labels.push("Reserved");
    return <Badges labels={labels} />;
  }

  function linkButtons(device, { disconnect = true } = {}) {
    return (
      <>
        {disconnect && device.connected ? (
          <button type="button" class="ghost" onClick={() => post("/api/v1/midi/ble/disconnect?address=" + encodeURIComponent(device.address)).catch(() => {})}>Disconnect</button>
        ) : null}
        <button type="button" class="warn" onClick={() => {
          if (!window.confirm(disconnect
            ? "Forget this device on ShowboxMod? Also forget ShowboxMod on the phone or computer."
            : "Forget this host here. Bluetooth cannot unpair the phone or computer for us, so also forget ShowboxMod there or it will keep retrying. New hosts stay blocked until you turn Accept new hosts back on.")) return;
          post("/api/v1/midi/ble/forget?address=" + encodeURIComponent(device.address)).catch(() => {});
        }}>Forget</button>
      </>
    );
  }

  const byId = {};
  for (const item of asList(data && data.transports)) byId[item.id] = item;
  const usbMix = (data && data.usbHidMix) || { keyboard: true, mouse: true, absPointer: true, digitizer: false };
  const usbHidOn = !!(byId.usbHid && byId.usbHid.enabled);
  const bleMix = (data && data.bleHidMix) || { keyboard: true, mouse: true, absPointer: false, digitizer: true };
  const debugInfo = (data && data.debug) || {};
  const peripherals = asList(data && data.peripherals);
  const hosts = asList(data && data.hosts);
  const link = (data && data.showboxLink) || {};
  const liveMix = (data && data.liveHidMix) || { keyboard: false, mouse: false, absPointer: false, digitizer: false };
  const nearby = [];
  if (addOpen) {
    const paired = new Set(peripherals.map((device) => (device.address || "").toLowerCase()));
    for (const device of asList(data && data.nearby)) {
      if (!device.address) continue;
      const key = device.address.toLowerCase();
      const prev = found.current.get(key);
      if (!prev) {
        found.current.set(key, { name: device.name || "", address: device.address, rssi: device.rssi, midi: !!device.midi, hid: !!device.hid, advertisedBattery: !!device.advertisedBattery, order: found.current.size });
      } else {
        if (device.name) prev.name = device.name;
        if (device.rssi !== undefined) prev.rssi = device.rssi;
        if (device.midi) prev.midi = true;
        if (device.hid) prev.hid = true;
        if (device.advertisedBattery) prev.advertisedBattery = true;
      }
    }
    nearby.push(...Array.from(found.current.values()).sort((a, b) => {
      const named = (a.name ? 0 : 1) - (b.name ? 0 : 1);
      return named !== 0 ? named : a.order - b.order;
    }).map((device) => ({ ...device, already: paired.has(device.address.toLowerCase()) })));
  }

  async function toggle(id, event, extraQs) {
    const on = event.target.checked;
    if (usbReboot[id] && !window.confirm(usbReboot[id][on ? 0 : 1])) {
      event.target.checked = !on;
      return;
    }
    if (id === "interceptor" && !on && !window.confirm("Mixer/base bytes still pass through, but this module stops reading faders, battery, and looper state. Turn off only to test whether parsing is stalling the amp link.")) {
      event.target.checked = true;
      return;
    }
    if (id === "wifi" && !on && !window.confirm("Turns off RTP-MIDI over Wi-Fi. The web page and OTA stay up.")) {
      event.target.checked = true;
      return;
    }
    try {
      let qs = id + "=" + (on ? "1" : "0");
      if (id === "usbHid" && on) qs = "usbSafeTest=" + (usbSafe ? "1" : "0") + "&" + qs;
      if (extraQs) qs = extraQs + "&" + qs;
      await post("/api/v1/midi/settings?" + qs);
    } catch (err) {
      event.target.checked = !on;
      setError(err.message);
    }
  }

  function transport(id) {
    return byId[id] || { available: false, enabled: false, reason: "", note: "" };
  }

  function serviceRow(id, title, description, opts = {}) {
    const item = opts.virtual || transport(id);
    const available = item.available !== false;
    return (
      <Row
        key={id + title}
        title={title}
        description={description || item.reason}
        note={item.note}
        available={available}
        checked={opts.checked !== undefined ? opts.checked : item.enabled}
        disabled={opts.disabled}
        role={opts.role}
        danger={opts.danger}
        onChange={opts.onChange || ((event) => toggle(id, event))}
      />
    );
  }

  const linkOk = !!link.ok;
  const agePart = (label, ms) => (ms != null && ms < 0xFFFFFF ? (" · " + label + " " + ms + " ms") : "");
  const linkLabel = !data ? "Mixer ↔ base: checking…"
    : linkOk ? ("Mixer ↔ base: live" + agePart("base", link.baseAgeMs) + agePart("mixer", link.mixerAgeMs) + (data.showboxBattery != null ? " · battery " + data.showboxBattery + "%" : ""))
    : ("Mixer ↔ base: not talking" + agePart("base", link.baseAgeMs) + agePart("mixer", link.mixerAgeMs));

  return (
    <main>
      <h1>Services</h1>
      <Nav current="/services" extra={data && data.ip ? (data.ip + " · showbox.local") : "showbox.local"} />
      <p id="error">{error}</p>
      <p class={"hint link-status" + (linkOk ? " ok" : " bad")}>{linkLabel}</p>
      {data && data.scanHeld ? <p class="hint">A host is using the live host slot ({(data.bleSlots && data.bleSlots.hostUsed) || 0}/{(data.bleSlots && data.bleSlots.hostMax) || 1}). Advertising stays on so another remembered host can take over when this one disconnects. Extra hosts are remembered, then dropped until a slot is free. Radio links {(data.bleLinkCount || 0)}/{(data.bleLinkLimit || 3)}.</p> : null}
      <p class="hint">Turn pieces off to see what is stressing the radio. Mixer/base forwarding stays on even if interceptor parsing is off. Bluetooth HID uses one GATT report map for every host; the per-host mix only chooses which reports we send on that link.</p>

      <section>
        <h2>Bluetooth</h2>
        <Branch title="Classic">
          <Branch title="Peripheral">
            {serviceRow("bleClassic", "MIDI over Serial", "The ESP32-S3 has no Bluetooth Classic radio.")}
          </Branch>
          <Branch title="Host">
            {serviceRow("bleClassic", "MIDI over Serial", "The ESP32-S3 has no Bluetooth Classic radio.")}
          </Branch>
        </Branch>
        <Branch title="Low Energy">
          <Branch title="Peripheral">
            {serviceRow("bleHid", "HID", "Keyboard, mouse, absolute pointer, and touch on the ShowboxMod name. Every host sees the same GATT HID map. The mix below is the default for a new host; each remembered host can use a different subset without re-pairing.")}
            <div class="nest">
              {[
                ["bleHidKeyboard", "keyboard", "Keyboard"],
                ["bleHidMouse", "mouse", "Mouse"],
                ["bleHidAbs", "absPointer", "Absolute mouse"],
                ["bleHidDigitizer", "digitizer", "Touch"]
              ].map(([id, field, title]) => (
                <Row key={id} title={title} checked={!!bleMix[field]} onChange={async (event) => {
                  try { await post("/api/v1/midi/settings?" + id + "=" + (event.target.checked ? "1" : "0")); }
                  catch (err) { event.target.checked = !event.target.checked; setError(err.message); }
                }} />
              ))}
            </div>
            {serviceRow("bleMidiPeripheral", "MIDI", "Advertise BLE MIDI on the same ShowboxMod name as HID.")}
            <Row
              title="Accept new hosts"
              description="Advertising stops once a computer or phone is connected so mixer↔base stays stable. Press the green button on the panel for a 30 second pairing window (or until something connects), or turn this on here for 90 seconds. A remembered host can reconnect on its own when no host is linked."
              checked={!!(data && data.acceptNewHosts)}
              role
              onChange={(event) => post("/api/v1/midi/settings?acceptNewHosts=" + (event.target.checked ? "1" : "0")).catch((err) => { event.target.checked = !event.target.checked; setError(err.message); })}
            />
            <div class="row">
              <div class="copy">
                <strong>Live slots</strong>
                <small>Three BLE links total. Default is one host (phone/laptop) and two peripherals (pedals), first come first served. Reserve slot pins a device: when that device connects, an unreserved occupant of the same kind is dropped. Host + peripheral counts cannot exceed 3.</small>
              </div>
              <label class="labeled">
                <span>Hosts {(data && data.bleSlots && data.bleSlots.hostUsed) || 0}/{(data && data.bleSlots && data.bleSlots.hostMax) || 1}</span>
                <select value={(data && data.bleSlots && data.bleSlots.hostMax) || 1} onChange={(event) => {
                  const hosts = Number(event.target.value);
                  const peri = Math.min((data && data.bleSlots && data.bleSlots.peripheralMax) || 2, 3 - hosts);
                  post("/api/v1/midi/ble/slots?hosts=" + hosts + "&peripherals=" + peri).catch((err) => setError(err.message));
                }}>
                  {[0, 1, 2, 3].map((n) => <option key={n} value={n}>{n}</option>)}
                </select>
              </label>
              <label class="labeled">
                <span>Peripherals {(data && data.bleSlots && data.bleSlots.peripheralUsed) || 0}/{(data && data.bleSlots && data.bleSlots.peripheralMax) || 2}</span>
                <select value={(data && data.bleSlots && data.bleSlots.peripheralMax) || 2} onChange={(event) => {
                  const peri = Number(event.target.value);
                  const hosts = Math.min((data && data.bleSlots && data.bleSlots.hostMax) || 1, 3 - peri);
                  post("/api/v1/midi/ble/slots?hosts=" + hosts + "&peripherals=" + peri).catch((err) => setError(err.message));
                }}>
                  {[0, 1, 2].map((n) => <option key={n} value={n}>{n}</option>)}
                </select>
              </label>
            </div>
          </Branch>
          <Branch title="Host">
            {serviceRow("bleMidiHost", "MIDI", data && data.scanHeld
              ? "Discovery scans wait while a host is live so mixer UART and this page stay up. Remembered pedals still connect by address into free peripheral slots, first come first served. Advertising stays on."
              : "Connect out to BLE MIDI devices such as the AIRSTEP. Up to two peripherals share the radio with one host by default. A third pedal waits until a slot is free.")}
          </Branch>
        </Branch>
      </section>

      <section>
        <h2>Network</h2>
        <Branch title="Interfaces">
          <Row title="Wi-Fi" description="Station mode for this web page, OTA, and mDNS. Do not turn this off from here or you lose the UI until serial recovery." checked available={false} />
          {serviceRow("ethernet", "Ethernet", "W5500 link. MIDI sessions use UDP 5004/5005.")}
        </Branch>
        <Branch title="Services">
          {serviceRow("wifi", "RTP-MIDI", "AppleMIDI / RTP-MIDI on UDP 5004. Name AppleMidi.")}
          <Row title="mDNS" description="Advertises showbox.local. Always on after Wi-Fi is up so you can find the module." checked available={false} />
          <Row title="DHCP client" description="Gets the station address from the access point. Always on with Wi-Fi." checked available={false} />
          <Row title="DHCP server" description="Only while the Wi-Fi setup portal is running (port 8080)." checked={false} available={false} />
          <Row title="ICMP" description="Ping replies. Always on with the IP stack." checked available={false} />
          <Row title="Captive portal" description="WiFiManager setup UI on port 8080 when there is no saved network." checked={false} available={false} />
          <Row title="OTA" description="Arduino OTA on port 3232. Keep this on unless you are isolating radio load. Flash still works over USB serial/JTAG and the UART connector." checked available={false} danger />
        </Branch>
      </section>

      <section>
        <h2>USB</h2>
        <Branch title="Peripheral">
          <Row
            title="JTAG/Serial"
            description="Default USB port. Turning on MIDI or HID peripheral/host takes this away until you reboot back."
            checked={!!(data && data.usbJtag)}
            danger
            onChange={async (event) => {
              if (event.target.checked) {
                if (!window.confirm(usbReboot.usbJtag[1])) {
                  event.target.checked = false;
                  return;
                }
                try { await post("/api/v1/midi/settings?usbHid=0&usbMidiHost=0&usbMidiPeripheral=0"); }
                catch (err) { event.target.checked = false; setError(err.message); }
                return;
              }
              event.target.checked = true;
              setError("Pick USB MIDI or USB HID below. That reboot is what turns serial/JTAG off.");
            }}
          />
          {serviceRow("usbMidiPeripheral", "MIDI")}
          {serviceRow("usbHid", "HID")}
          <div class="nest">
            <p class="hint">USB HID mix is one profile for whoever is on the cable. Applied on reboot or unplug/replug.</p>
            {[
              ["usbHidKeyboard", "keyboard", "Keyboard"],
              ["usbHidMouse", "mouse", "Mouse"],
              ["usbHidAbs", "absPointer", "Absolute mouse"],
              ["usbHidDigitizer", "digitizer", "Touch"]
            ].map(([id, field, title]) => (
              <Row key={id} title={title} checked={!!usbMix[field]} onChange={async (event) => {
                const turningOn = event.target.checked;
                if (usbHidOn && !window.confirm(turningOn
                  ? "Requires reboot so USB picks up the new HID layout."
                  : "Requires reboot so USB drops this HID input.")) {
                  event.target.checked = !turningOn;
                  return;
                }
                try { await post("/api/v1/midi/settings?" + id + "=" + (turningOn ? "1" : "0")); }
                catch (err) { event.target.checked = !turningOn; setError(err.message); }
              }} />
            ))}
          </div>
        </Branch>
        <Branch title="Host">
          {serviceRow("usbMidiHost", "MIDI")}
        </Branch>
      </section>

      <section>
        <h2>Misc</h2>
        <Row
          title="Mackie Showbox interceptor"
          description="Always forwards mixer ↔ base. When parsing is on, this module also reads entities, battery, and looper. Turn parsing off only to test UART stability."
          checked={link.parsing !== false}
          danger
          onChange={(event) => toggle("interceptor", event)}
        />
        {serviceRow("din", "5-pin DIN MIDI", "In/Out/Through on the DIN jacks.")}
        <Row
          title="Keep logs until telnet connects"
          description="Last 64 KB of debug lines for telnet showbox.local 23."
          checked={!!debugInfo.wifiLogHistory}
          onChange={(event) => toggle("wifiLogHistory", event)}
        />
        <Row
          title="Log mixer/base packets"
          description="Writes UART packets into the Wi-Fi serial history. Heartbeats fill that buffer quickly."
          checked={!!debugInfo.uartPackets}
          onChange={(event) => toggle("uartPackets", event)}
        />
        <Row
          title="Timed USB test"
          description="Next USB MIDI/HID boot restores serial/JTAG after about 75 seconds."
          checked={!!debugInfo.usbSafeTest}
          onChange={(event) => toggle("usbSafeTest", event)}
        />
      </section>

      <section>
        <h2>Remembered peripherals</h2>
        <p class="hint">Devices this module connects out to, such as a BLE MIDI pedal. Forget removes our bond. Connect opens a BLE MIDI link. Reserve slot means this pedal can take an unreserved peripheral slot when it connects.</p>
        <p class="actions"><button type="button" onClick={() => { found.current.clear(); setAddOpen(true); post("/api/v1/midi/ble/scan").catch(() => {}); }}>Add device…</button></p>
        <ul>
          {peripherals.length ? peripherals.map((device) => (
            <DeviceLine key={device.address} device={device} extra={peripheralBadges(device)}>
              <div class="labeled">
                <span>Auto Connect</span>
                <Switch role checked={device.autoConnect} onChange={(event) => post("/api/v1/midi/ble/auto?address=" + encodeURIComponent(device.address) + "&enabled=" + (event.target.checked ? "1" : "0")).catch((err) => { event.target.checked = !event.target.checked; setError(err.message); })} />
              </div>
              <div class="labeled">
                <span>Reserve slot</span>
                <Switch role checked={!!device.pinned} onChange={(event) => post("/api/v1/midi/ble/role?address=" + encodeURIComponent(device.address) + "&pinned=" + (event.target.checked ? "1" : "0")).catch((err) => { event.target.checked = !event.target.checked; setError(err.message); })} />
              </div>
              {!device.connected ? (
                <button type="button" onClick={() => post("/api/v1/midi/ble/connect?midi=1&address=" + encodeURIComponent(device.address) + "&name=" + encodeURIComponent(device.name || "")).catch(() => {})}>Connect</button>
              ) : null}
              {linkButtons(device)}
            </DeviceLine>
          )) : <li>No remembered peripherals.</li>}
        </ul>
      </section>

      <section>
        <h2>Remembered hosts</h2>
        <p class="hint">Phones and computers that connected in to ShowboxMod. One host uses the live slot at a time unless you raise host slots. If the phone holding the slot turns off, a paired tablet can take over; the phone is denied until the tablet disconnects. Reserve slot lets that host bump an unreserved occupant. Forget only drops our side.</p>
        {data && data.acceptNewHosts === false ? <p class="hint">Advertising is off while a host is connected. Press the green button or turn Accept new hosts on to pair another computer or phone for 30–90 seconds.</p> : null}
        <ul>
          {hosts.length ? hosts.map((device) => (
            <DeviceLine key={device.address} device={device} extra={hostBadges(device)}>
              {linkButtons(device)}
              <div class="labeled">
                <span>Reserve slot</span>
                <Switch role checked={!!device.pinned} onChange={(event) => post("/api/v1/midi/ble/role?address=" + encodeURIComponent(device.address) + "&pinned=" + (event.target.checked ? "1" : "0")).catch((err) => { event.target.checked = !event.target.checked; setError(err.message); })} />
              </div>
              {device.hid ? (
                <div class="hid-mix">
                  {[
                    ["hidKeyboard", "Keyboard"],
                    ["hidMouse", "Mouse"],
                    ["hidAbs", "Abs mouse"],
                    ["hidDigitizer", "Touch"]
                  ].map(([field, title]) => (
                    <label key={field} class="labeled">
                      <span>{title}</span>
                      <Switch role checked={!!device[field]} onChange={(event) => {
                        if (!window.confirm("This host keeps the same GATT map. The next HID reports follow the mix you just chose; no re-pair.")) {
                          event.target.checked = !event.target.checked;
                          return;
                        }
                        post("/api/v1/midi/ble/role?address=" + encodeURIComponent(device.address) + "&" + field + "=" + (event.target.checked ? "1" : "0")).catch((err) => {
                          event.target.checked = !event.target.checked;
                          setError(err.message);
                        });
                      }} />
                    </label>
                  ))}
                </div>
              ) : null}
            </DeviceLine>
          )) : <li>No remembered hosts.</li>}
        </ul>
      </section>

      <section>
        <h2>HID tests</h2>
        <p class="hint">
          Live mix across connected hosts:
          {" "}{[liveMix.keyboard && "keyboard", liveMix.mouse && "mouse", liveMix.absPointer && "absolute mouse", liveMix.digitizer && "touch"].filter(Boolean).join(", ") || "none"}.
          {" "}{data && data.hidReady ? "HID notify is ready." : "No HID host is connected."}
          Each host only receives the reports enabled for it. Touch is for Android (no cursor). Absolute mouse is for Linux. A host that paired before unique report IDs still has the old combined map until it forgets ShowboxMod and pairs again.
        </p>
        <h3>Keyboard</h3>
        <form onSubmit={(event) => { event.preventDefault(); post("/api/v1/hid/keyboard?text=" + encodeURIComponent(keytext)).catch(() => {}); }}>
          <input type="text" maxlength="24" placeholder="Text to type" value={keytext} onInput={(event) => setKeytext(event.target.value)} />
          <button type="submit" disabled={!liveMix.keyboard}>Send keys</button>
        </form>
        <p class="actions">
          <button type="button" disabled={!liveMix.keyboard} onClick={() => post("/api/v1/hid/keyboard?key=pageup").catch(() => {})}>Page up</button>
          <button type="button" disabled={!liveMix.keyboard} onClick={() => post("/api/v1/hid/keyboard?key=pagedown").catch(() => {})}>Page down</button>
          <button type="button" class="ghost" disabled={!liveMix.keyboard} onClick={() => post("/api/v1/hid/keyboard?key=enter").catch(() => {})}>Enter</button>
        </p>
        <h3>Mouse</h3>
        <p class="actions">
          <button type="button" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?x=-20&y=0").catch(() => {})}>Left</button>
          <button type="button" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?x=20&y=0").catch(() => {})}>Right</button>
          <button type="button" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?x=0&y=-20").catch(() => {})}>Up</button>
          <button type="button" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?x=0&y=20").catch(() => {})}>Down</button>
          <button type="button" class="ghost" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?x=0&y=0&buttons=1").catch(() => {})}>Click</button>
        </p>
        <p class="actions">
          <button type="button" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?wheel=5").catch(() => {})}>Wheel up</button>
          <button type="button" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?wheel=-5").catch(() => {})}>Wheel down</button>
          <button type="button" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?pan=-5").catch(() => {})}>Wheel left</button>
          <button type="button" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?pan=5").catch(() => {})}>Wheel right</button>
        </p>
        <h3>Absolute mouse</h3>
        <p class="actions">
          <button type="button" disabled={!liveMix.absPointer} onClick={() => post("/api/v1/hid/touch?action=swipe&x=5000&y=5000&x2=5000&y2=2000&via=abs").catch(() => {})}>Swipe up</button>
          <button type="button" disabled={!liveMix.absPointer} onClick={() => post("/api/v1/hid/touch?action=swipe&x=5000&y=5000&x2=5000&y2=8000&via=abs").catch(() => {})}>Swipe down</button>
          <button type="button" disabled={!liveMix.absPointer} onClick={() => post("/api/v1/hid/touch?action=swipe&x=5000&y=5000&x2=2000&y2=5000&via=abs").catch(() => {})}>Swipe left</button>
          <button type="button" disabled={!liveMix.absPointer} onClick={() => post("/api/v1/hid/touch?action=swipe&x=5000&y=5000&x2=8000&y2=5000&via=abs").catch(() => {})}>Swipe right</button>
        </p>
        <h3>Touch</h3>
        <p class="actions">
          <button type="button" disabled={!liveMix.digitizer} onClick={() => post("/api/v1/hid/touch?action=tap&x=5000&y=5000&via=touch").catch(() => {})}>Tap</button>
          <button type="button" disabled={!liveMix.digitizer} onClick={() => post("/api/v1/hid/touch?action=swipe&x=5000&y=5000&x2=5000&y2=2000&via=touch").catch(() => {})}>Swipe up</button>
          <button type="button" disabled={!liveMix.digitizer} onClick={() => post("/api/v1/hid/touch?action=swipe&x=5000&y=5000&x2=5000&y2=8000&via=touch").catch(() => {})}>Swipe down</button>
          <button type="button" disabled={!liveMix.digitizer} onClick={() => post("/api/v1/hid/touch?action=swipe&x=5000&y=5000&x2=2000&y2=5000&via=touch").catch(() => {})}>Swipe left</button>
          <button type="button" disabled={!liveMix.digitizer} onClick={() => post("/api/v1/hid/touch?action=swipe&x=5000&y=5000&x2=8000&y2=5000&via=touch").catch(() => {})}>Swipe right</button>
          <button type="button" class="ghost" disabled={!liveMix.digitizer} onClick={() => post("/api/v1/hid/touch?action=pinch-in&x=5000&y=5000&via=touch").catch(() => {})}>Pinch in</button>
          <button type="button" class="ghost" disabled={!liveMix.digitizer} onClick={() => post("/api/v1/hid/touch?action=pinch-out&x=5000&y=5000&via=touch").catch(() => {})}>Pinch out</button>
        </p>
      </section>

      <section>
        <h2>MIDI test</h2>
        <p class="hint">Sends middle C on channel 1 through every MIDI transport that is on, then note-off 250 ms later. Watch the activity log and any connected BLE MIDI pedal or USB MIDI device.</p>
        <p class="actions">
          <button type="button" onClick={() => post("/api/v1/midi/test").catch(() => {})}>Send C4</button>
        </p>
      </section>

      <section>
        <h2>Activity</h2>
        <pre class="logbox">{asList(data && data.log).length ? asList(data && data.log).join("\n") : "No test events yet."}</pre>
      </section>

      <div id="add" class={"overlay" + (addOpen ? " open" : "")} onClick={(event) => { if (event.target.id === "add") setAddOpen(false); }}>
        <div class="sheet">
          <h2>New devices</h2>
          <p class="hint">Scan finds BLE MIDI pedals. Connect saves the pedal and opens a link. A live computer/phone on HID can make that take a few seconds; mixer forwarding stays on.</p>
          <p class="actions">
            <button type="button" onClick={() => post("/api/v1/midi/ble/scan").catch(() => {})}>{data && data.scanning ? "Scanning…" : "Scan"}</button>
            <button type="button" class="ghost" onClick={() => { setAddOpen(false); found.current.clear(); }}>Close</button>
          </p>
          <ul>
            {nearby.length ? nearby.map((device) => (
              <DeviceLine key={device.address} device={device} extra={peripheralBadges(device)}>
                <button type="button" disabled={device.already} onClick={() => post("/api/v1/midi/ble/connect?pair=1&address=" + encodeURIComponent(device.address) + "&name=" + encodeURIComponent(device.name || "")).catch(() => {})}>{device.already ? "Saved" : "Connect"}</button>
              </DeviceLine>
            )) : <li>{data && data.scanning ? "Scanning…" : "No devices yet."}</li>}
          </ul>
        </div>
      </div>
    </main>
  );
}
