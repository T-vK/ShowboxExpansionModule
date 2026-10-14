import { createContext } from "preact";
import { useContext, useEffect, useRef, useState } from "preact/hooks";
import { asList, getJson, postQuery, sleep } from "../api.js";
import { Switch } from "../ui/Widgets.jsx";

export const usbReboot = {
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

const Ctx = createContext(null);

export function Badges({ labels }) {
  if (!labels.length) return null;
  return <div class="badges">{labels.map((label) => <span class="badge" key={label}>{label}</span>)}</div>;
}

export function DeviceLine({ device, extra, children }) {
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

export function HidMix({ mix, prefix, usbHidOn, post, setError }) {
  return (
    <div class="hid-mix">
      {[
        [prefix + "Keyboard", "keyboard", "Keyboard"],
        [prefix + "Mouse", "mouse", "Mouse"],
        [prefix + "Abs", "absPointer", "Absolute mouse"],
        [prefix + "Digitizer", "digitizer", "Touch"]
      ].map(([id, field, title]) => (
        <label key={id} class="labeled">
          <span>{title}</span>
          <Switch role checked={!!mix[field]} onChange={async (event) => {
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
        </label>
      ))}
    </div>
  );
}

export function ServicesProvider({ children }) {
  const [data, setData] = useState(null);
  const [error, setError] = useState("");
  const [addOpen, setAddOpen] = useState(false);
  const [hideUnsupported, setHideUnsupported] = useState(() => {
    try { return localStorage.getItem("sxm-hide-unsupported") !== "0"; }
    catch { return true; }
  });
  const found = useRef(new Map());
  const usbSafe = !!(data && data.debug && data.debug.usbSafeTest);

  function setHide(next) {
    setHideUnsupported(next);
    try { localStorage.setItem("sxm-hide-unsupported", next ? "1" : "0"); }
    catch { /* ignore */ }
  }

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

  const byId = {};
  for (const item of asList(data && data.transports)) byId[item.id] = item;

  function transport(id) {
    return byId[id] || { available: false, enabled: false, reason: "", note: "" };
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
    if (id === "bluetooth") {
      if (!window.confirm(on
        ? "Requires reboot to start the Bluetooth radio."
        : "Requires reboot to stop the Bluetooth radio. Remembered devices stay saved.")) {
        event.target.checked = !on;
        return;
      }
    }
    if (id === "wifiRadio" && !on && !window.confirm("Turns off Wi-Fi. This page goes away unless you are on Ethernet. Long-press the green button to turn Wi-Fi back on.")) {
      event.target.checked = true;
      return;
    }
    if (id === "wifi" && !on && !window.confirm("Turns off RTP-MIDI over Wi-Fi. The web page and OTA stay up.")) {
      event.target.checked = true;
      return;
    }
    if (id === "https") {
      if (!window.confirm(on
        ? "Warning: HTTPS can stall Wi-Fi while Bluetooth and the mixer are running. HTTP on port 80 stays up — there is no redirect. Only turn this on if you need a trusted PWA install, then install the device CA and open https://showbox.local/."
        : "Stops TLS. HTTP on port 80 stays up. The device CA stays saved so you can turn HTTPS back on without reinstalling.")) {
        event.target.checked = !on;
        return;
      }
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

  function peripheralBadges(device) {
    const labels = [];
    if (device.midi) labels.push("MIDI");
    if (device.hid || device.advertisedHid) labels.push("HID");
    if (device.advertisedBattery || (typeof device.battery === "number" && device.battery >= 0)) labels.push("Battery");
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

  const value = {
    data, error, setError, post, toggle, refresh, transport, byId,
    hideUnsupported, setHide, addOpen, setAddOpen, found, usbSafe,
    peripheralBadges, hostBadges, linkButtons
  };

  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}

export function useServices() {
  const value = useContext(Ctx);
  if (!value) throw new Error("useServices needs ServicesProvider");
  return value;
}

export function usbModeOf(data, byId) {
  if (byId.usbHid && byId.usbHid.enabled) return "hid";
  if (byId.usbMidiHost && byId.usbMidiHost.enabled) return "midiHost";
  if (byId.usbMidiPeripheral && byId.usbMidiPeripheral.enabled) return "midiPeri";
  return "jtag";
}
