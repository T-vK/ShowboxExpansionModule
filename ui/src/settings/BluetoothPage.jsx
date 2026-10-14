import { DeviceLine, useServices } from "./services.jsx";
import { Switch } from "../ui/Widgets.jsx";
import { ServiceNode } from "./ServiceUi.jsx";

export function BluetoothPage() {
  const {
    data, error, setError, post, toggle, transport,
    addOpen, setAddOpen, found, peripheralBadges, hostBadges, linkButtons
  } = useServices();
  const bluetoothOn = data ? data.bluetoothRadio !== false : true;
  const periOn = !!(transport("bleHid").enabled || transport("bleMidiPeripheral").enabled);
  const hostOn = !!transport("bleMidiHost").enabled;
  const bleMix = (data && data.bleHidMix) || { keyboard: true, mouse: true, absPointer: false, digitizer: true };
  const peripherals = [];
  const hosts = [];
  if (data) {
    peripherals.push(...(Array.isArray(data.peripherals) ? data.peripherals : []));
    hosts.push(...(Array.isArray(data.hosts) ? data.hosts : []));
  }
  const nearby = [];
  if (addOpen) {
    const paired = new Set(peripherals.map((device) => (device.address || "").toLowerCase()));
    for (const device of (Array.isArray(data && data.nearby) ? data.nearby : [])) {
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

  return (
    <div class="mgmt-page">
      <p id="error">{error}</p>
      {data && data.scanHeld ? <p class="hint">A host is using the live host slot ({(data.bleSlots && data.bleSlots.hostUsed) || 0}/{(data.bleSlots && data.bleSlots.hostMax) || 1}). Extra hosts are remembered, then dropped until a slot is free.</p> : null}
      <div class="svc-tree">
        <ServiceNode
          title="Bluetooth radio"
          info="ESP32-S3 Low Energy radio. Off reboots. Remembered hosts and pedals stay saved."
          checked={bluetoothOn}
          onChange={(event) => toggle("bluetooth", event)}
        >
          <ServiceNode
            title="Peripheral"
            info="Phones and computers that connect in to ShowboxMod."
            checked={periOn}
            onChange={(event) => {
              const on = event.target.checked ? "1" : "0";
              post("/api/v1/midi/settings?bleHid=" + on + "&bleMidiPeripheral=" + on).catch((err) => {
                event.target.checked = !event.target.checked;
                setError(err.message);
              });
            }}
          >
            <ServiceNode title="HID" info="GATT HID map advertised to hosts." checked={!!transport("bleHid").enabled} onChange={(event) => toggle("bleHid", event)} />
            <ServiceNode title="MIDI" info="BLE MIDI on the same ShowboxMod name." href="/midi" checked={!!transport("bleMidiPeripheral").enabled} onChange={(event) => toggle("bleMidiPeripheral", event)} />
          </ServiceNode>
          <ServiceNode
            title="Host"
            info="Connect out to BLE MIDI pedals such as the AIRSTEP."
            checked={hostOn}
            onChange={(event) => toggle("bleMidiHost", event)}
          />
          <ServiceNode
            title="Accept new hosts"
            info="Advertising stops once a computer or phone is connected. Press the blue button for a 30 second pairing window, or turn this on for 90 seconds."
            checked={!!(data && data.acceptNewHosts)}
            onChange={(event) => post("/api/v1/midi/settings?acceptNewHosts=" + (event.target.checked ? "1" : "0")).catch((err) => { event.target.checked = !event.target.checked; setError(err.message); })}
          />
        </ServiceNode>
      </div>

      <section>
        <h2>Default HID mix</h2>
        <p class="hint">New hosts get this mix. Each remembered host can use a different subset without re-pairing.</p>
        <div class="hid-mix">
          {[
            ["bleHidKeyboard", "keyboard", "Keyboard"],
            ["bleHidMouse", "mouse", "Mouse"],
            ["bleHidAbs", "absPointer", "Absolute mouse"],
            ["bleHidDigitizer", "digitizer", "Touch"]
          ].map(([id, field, title]) => (
            <label key={id} class="labeled">
              <span>{title}</span>
              <Switch role checked={!!bleMix[field]} onChange={async (event) => {
                try { await post("/api/v1/midi/settings?" + id + "=" + (event.target.checked ? "1" : "0")); }
                catch (err) { event.target.checked = !event.target.checked; setError(err.message); }
              }} />
            </label>
          ))}
        </div>
      </section>

      <section>
        <h2>Live slots</h2>
        <p class="hint">Three BLE links total. Reserve slot on a device lets it take an unreserved occupant of the same kind.</p>
        <div class="slot-row">
          <label class="labeled">
            <span>Hosts {(data && data.bleSlots && data.bleSlots.hostUsed) || 0}/{(data && data.bleSlots && data.bleSlots.hostMax) || 1}</span>
            <select value={(data && data.bleSlots && data.bleSlots.hostMax) || 1} onChange={(event) => {
              const hostsN = Number(event.target.value);
              const peri = Math.min((data && data.bleSlots && data.bleSlots.peripheralMax) || 2, 3 - hostsN);
              post("/api/v1/midi/ble/slots?hosts=" + hostsN + "&peripherals=" + peri).catch((err) => setError(err.message));
            }}>
              {[0, 1, 2, 3].map((n) => <option key={n} value={n}>{n}</option>)}
            </select>
          </label>
          <label class="labeled">
            <span>Peripherals {(data && data.bleSlots && data.bleSlots.peripheralUsed) || 0}/{(data && data.bleSlots && data.bleSlots.peripheralMax) || 2}</span>
            <select value={(data && data.bleSlots && data.bleSlots.peripheralMax) || 2} onChange={(event) => {
              const peri = Number(event.target.value);
              const hostsN = Math.min((data && data.bleSlots && data.bleSlots.hostMax) || 1, 3 - peri);
              post("/api/v1/midi/ble/slots?hosts=" + hostsN + "&peripherals=" + peri).catch((err) => setError(err.message));
            }}>
              {[0, 1, 2].map((n) => <option key={n} value={n}>{n}</option>)}
            </select>
          </label>
        </div>
      </section>

      <section>
        <h2>Remembered peripherals</h2>
        <p class="hint">Devices this module connects out to, such as a BLE MIDI pedal.</p>
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
        <p class="hint">Phones and computers that connected in. Reserve slot lets that host bump an unreserved occupant. Forget only drops our side.</p>
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

      <div id="add" class={"overlay" + (addOpen ? " open" : "")} onClick={(event) => { if (event.target.id === "add") setAddOpen(false); }}>
        <div class="sheet">
          <h2>New devices</h2>
          <p class="hint">Scan finds BLE MIDI pedals. Connect saves the pedal and opens a link.</p>
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
    </div>
  );
}
