import { useEffect, useRef, useState } from "preact/hooks";
import { Dropdown } from "../ui/Dropdown.jsx";
import { IconBattery, IconSignalStream } from "../mixer/Icons.jsx";
import { IconBluetooth } from "../mixer/Icons.jsx";
import { useLive } from "../live/LiveContext.jsx";

function battClass(pct) {
  const n = Number(pct);
  if (!Number.isFinite(n)) return "lvl-mid";
  if (n >= 60) return "lvl-ok";
  if (n >= 35) return "lvl-mid";
  if (n >= 15) return "lvl-low";
  return "lvl-crit";
}

function fmtDur(ms) {
  const s = Math.max(0, Math.floor(ms / 1000));
  if (s < 60) return `${s}s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m}m ${s % 60}s`;
  const h = Math.floor(m / 60);
  return `${h}h ${m % 60}m`;
}

function fmtTime(ts) {
  if (!ts) return "—";
  return new Date(ts).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

function deviceName(device) {
  return (device.name && device.name.trim()) || device.address || "unknown";
}

async function callApi(url) {
  const res = await fetch(url, { method: "POST" });
  if (!res.ok) {
    throw new Error(await res.text() || `HTTP ${res.status}`);
  }
}

function BtDeviceRow({ device, onAction }) {
  return (
    <li class="bt-device-row">
      <span class="bt-device-name" title={device.address}>{deviceName(device)}</span>
      {device.connected ? (
        <button
          type="button"
          class="ghost"
          onClick={() => onAction(`/api/v1/midi/ble/disconnect?address=${encodeURIComponent(device.address)}`)}
        >Disconnect</button>
      ) : (
        <button
          type="button"
          onClick={() => onAction(`/api/v1/midi/ble/connect?midi=${device.midi ? 1 : 0}&hid=${device.hid ? 1 : 0}&address=${encodeURIComponent(device.address)}&name=${encodeURIComponent(device.name || "")}`)}
        >Connect</button>
      )}
    </li>
  );
}

function BtPopup() {
  const [data, setData] = useState(null);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    let alive = true;
    const load = () => {
      fetch("/api/v1/midi", { cache: "no-store" })
        .then((res) => (res.ok ? res.json() : null))
        .then((json) => { if (alive && json) setData(json); })
        .catch(() => {});
    };
    load();
    const id = setInterval(load, 2500);
    return () => { alive = false; clearInterval(id); };
  }, []);

  async function act(url) {
    setBusy(true);
    try {
      await callApi(url);
      const res = await fetch("/api/v1/midi", { cache: "no-store" });
      if (res.ok) setData(await res.json());
    } catch (err) {
      console.warn("bluetooth action failed", err);
    }
    setBusy(false);
  }

  const slots = (data && data.bleSlots) || {};
  const linkCount = data ? data.bleLinkCount || 0 : 0;
  const linkLimit = data ? data.bleLinkLimit || 3 : 3;
  const hosts = data ? (data.hosts || []).filter((d) => d.connected) : [];
  const peripherals = data ? (data.peripherals || []).filter((d) => d.connected) : [];
  const connectable = data
    ? [
        ...(data.peripherals || []).filter((d) => !d.connected),
        ...(data.nearby || []),
      ]
    : [];
  const hostMax = slots.hostMax == null ? 1 : slots.hostMax;
  const periMax = slots.peripheralMax == null ? 2 : slots.peripheralMax;

  return (
    <div class="link-pop">
      <p class="link-pop-state ok">Bluetooth {linkCount}/{linkLimit} connected</p>
      <p class="meta">Hosts {slots.hostUsed || 0}/{hostMax} · Peripherals {slots.peripheralUsed || 0}/{periMax}</p>
      {hosts.length ? (
        <>
          <p>Hosts connected:</p>
          <ul>{hosts.map((d) => <BtDeviceRow key={d.address} device={d} onAction={act} />)}</ul>
        </>
      ) : <p class="meta">No hosts connected.</p>}
      {peripherals.length ? (
        <>
          <p>Peripherals connected:</p>
          <ul>{peripherals.map((d) => <BtDeviceRow key={d.address} device={d} onAction={act} />)}</ul>
        </>
      ) : <p class="meta">No peripherals connected.</p>}
      {connectable.length ? (
        <>
          <p>Available:</p>
          <ul>{connectable.map((d) => <BtDeviceRow key={d.address} device={{ ...d, connected: false }} onAction={act} />)}</ul>
        </>
      ) : <p class="meta">Nothing new nearby. Use Add device on the Bluetooth page to scan.</p>}
      <p class="actions">
        <a href="/bluetooth" class="bt-page-link">Bluetooth settings</a>
      </p>
      {busy ? <p class="meta">Working…</p> : null}
    </div>
  );
}

export function StatusCluster() {
  const { esp, link } = useLive();
  const [open, setOpen] = useState(false);
  const [pop, setPop] = useState("link");
  const [now, setNow] = useState(() => Date.now());
  const linkBtn = useRef(null);
  const btBtn = useRef(null);
  const battBtn = useRef(null);
  const popAnchor = useRef(null);
  const pct = link.battery == null || link.battery === ""
    ? null
    : (Number.isFinite(Number(link.battery)) ? Math.round(Number(link.battery)) : null);
  const live = !!esp.connected && !!link.ok;

  useEffect(() => {
    if (!open) return undefined;
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, [open]);

  const connectedFor = esp.connected && esp.since ? fmtDur(now - esp.since) : null;
  const lastSession = [...esp.sessions].reverse().find((row) => row.end);
  const lastLasted = lastSession ? fmtDur(lastSession.end - lastSession.start) : null;

  function toggle(which) {
    popAnchor.current = which === "batt" ? battBtn.current
      : which === "bt" ? btBtn.current
      : linkBtn.current;
    setPop(which);
    setOpen((v) => !(v && pop === which));
  }

  return (
    <>
      <button
        ref={linkBtn}
        type="button"
        class={"link-chip" + (esp.connected ? " on" : " off") + (live ? " live" : "")}
        title={live ? "Connected to Showbox" : (esp.connected ? "Module online, waiting for Showbox" : "Not connected")}
        onClick={() => toggle("link")}
      >
        <span class={"esp-pip " + (live ? "on live" : (esp.connected ? "wait" : "off"))}>
          <IconSignalStream live={live} on={esp.connected} />
        </span>
      </button>
      {link.bleOn ? (
        <button
          ref={btBtn}
          type="button"
          class="bt-chip"
          title={link.bleHosts > 0 ? `Bluetooth: ${link.bleHosts} device(s) connected` : "Bluetooth on, no devices"}
          onClick={() => toggle("bt")}
        >
          <IconBluetooth />
        </button>
      ) : null}
      <span
        ref={battBtn}
        class={"batt-chip " + (pct == null ? "unknown" : battClass(pct)) + (link.ok ? "" : " stale")}
        title={pct == null ? "Battery unknown" : `Showbox battery ${pct}%`}
      >
        <span class="batt-meter">
          <IconBattery pct={pct} />
        </span>
      </span>
      <Dropdown anchorRef={popAnchor} open={open} wide onClose={() => setOpen(false)}>
        {pop === "bt" ? <BtPopup /> : (
          <div class="link-pop">
            <p class={"link-pop-state " + (live ? "ok" : "bad")}>
              {live ? "Connected to Showbox" : (esp.connected ? "Waiting for Showbox" : "Web UI disconnected")}
            </p>
            {esp.connected ? (
              <p>This session {connectedFor} · since {fmtTime(esp.since)}</p>
            ) : (
              <p>Last drop {esp.lastDisconnect ? fmtTime(esp.lastDisconnect) : "—"}{lastLasted ? ` · that session ${lastLasted}` : ""}</p>
            )}
            <p>Drops this page: {esp.disconnects}</p>
            <p>Mixer ↔ base: {link.ok ? "live" : "waiting"}{pct != null ? ` · battery ${pct}%` : ""}</p>
            {esp.sessions.length ? (
              <ul>
                {esp.sessions.slice(-6).map((row, i) => (
                  <li key={`${row.start}-${i}`}>
                    {fmtTime(row.start)} – {row.end ? fmtTime(row.end) : "now"}
                    {" "}({fmtDur((row.end || now) - row.start)})
                  </li>
                ))}
              </ul>
            ) : <p class="meta">No sessions yet.</p>}
          </div>
        )}
      </Dropdown>
    </>
  );
}
