import { useEffect, useState } from "preact/hooks";
import { getJson, postJson, postQuery } from "../api.js";
import { ServiceNode } from "./ServiceUi.jsx";

export function WifiPanel() {
  const [status, setStatus] = useState({});
  const [networks, setNetworks] = useState([]);
  const [scanning, setScanning] = useState(false);
  const [ssid, setSsid] = useState("");
  const [password, setPassword] = useState("");
  const [error, setError] = useState("");

  async function refresh() {
    const next = await getJson("/api/v1/wifi");
    setStatus(next);
    setError("");
  }

  async function scan() {
    setScanning(true);
    try {
      await postQuery("/api/v1/wifi/scan");
      for (let i = 0; i < 10; i++) {
        await new Promise((resolve) => setTimeout(resolve, 700));
        const data = await getJson("/api/v1/wifi/networks");
        setNetworks(data.networks || []);
        if (!data.scanning) break;
      }
    } catch (err) {
      setError(String(err.message || err));
    } finally {
      setScanning(false);
    }
  }

  function patch(body) {
    return postJson("/api/v1/wifi", body).then(setStatus).catch((err) => {
      setError(err.message);
      throw err;
    });
  }

  useEffect(() => {
    refresh().catch((err) => setError(String(err.message || err)));
    const id = setInterval(() => refresh().catch(() => {}), 4000);
    return () => clearInterval(id);
  }, []);

  const remembered = Array.isArray(status.remembered) ? status.remembered : [];

  return (
    <div class="wifi-panel">
      <p id="error">{error}</p>
      <section>
        <h2>Join a network</h2>
        <p class="meta">{status.sta ? `Joined ${status.ssid} · ${status.ip} · ${status.rssi} dBm` : "Not joined to a network"}</p>
        <label class="field">Network
          <input type="text" value={ssid} placeholder={status.ssid || "SSID"} onInput={(event) => setSsid(event.target.value)} />
        </label>
        <label class="field">Password
          <input type="password" value={password} onInput={(event) => setPassword(event.target.value)} />
        </label>
        <p class="actions">
          <button type="button" onClick={() => patch({ ssid, password })}>Join</button>
          <button type="button" class="ghost" onClick={scan} disabled={scanning}>{scanning ? "Scanning…" : "Scan"}</button>
        </p>
        <ul>
          {networks.map((net) => (
            <li class="device" key={net.ssid + net.channel}>
              <div class="copy">
                <strong>{net.ssid || "(hidden)"}</strong>
                <div class="meta">{net.rssi} dBm · ch {net.channel}{net.secure ? " · secured" : " · open"}</div>
              </div>
              <button type="button" class="ghost" onClick={() => setSsid(net.ssid)}>Use</button>
            </li>
          ))}
        </ul>
      </section>

      <section>
        <h2>Remembered networks</h2>
        <p class="hint">Saved SSIDs this module can join again. Auto-connect retries the last network if the link drops.</p>
        <ul>
          {remembered.length ? remembered.map((net) => (
            <li class="device" key={net.ssid}>
              <div class="copy">
                <strong>{net.ssid}</strong>
                <div class="meta">{net.current ? "Joined now" : (net.saved ? "Password saved" : "Open / no password stored")}</div>
              </div>
              <div class="actions">
                <button type="button" class="ghost" onClick={() => patch({ ssid: net.ssid })}>Join</button>
                <button type="button" class="warn" onClick={() => patch({ forget: net.ssid })}>Forget</button>
              </div>
            </li>
          )) : <li>No remembered networks yet.</li>}
        </ul>
      </section>

      <div class="svc-tree">
        <ServiceNode
          title="Auto-connect"
          info="Retry the last home network if Wi-Fi drops."
          checked={status.autoConnect !== false}
          onChange={(event) => {
            patch({ autoConnect: event.target.checked }).catch(() => { event.target.checked = !event.target.checked; });
          }}
        />
        <ServiceNode
          title="Mixer hotspot"
          info={`SSID ${status.apSsid || "ShowboxMod"}, password ${status.apPassword || "showboxmod"}. After joining, open Chrome or Safari to ${status.apIp || "the hotspot IP"} or showbox.local.`}
          note={status.ap ? `${status.apIp} · ${status.apStations || 0} client(s)` : (status.apWanted ? "Wanted" : "Down")}
          checked={!!status.apWanted || !!status.ap}
          disabled={!status.sta && !status.apAlways}
          onChange={(event) => {
            patch({ ap: event.target.checked }).catch(() => { event.target.checked = !event.target.checked; });
          }}
        />
        <ServiceNode
          title="Auto hotspot"
          info="If home Wi-Fi cannot be reached, the mixer hotspot turns itself back on so a phone can still open this page."
          checked={status.apAuto !== false}
          onChange={(event) => {
            patch({ apAuto: event.target.checked }).catch(() => { event.target.checked = !event.target.checked; });
          }}
        />
        <ServiceNode
          title="Always-on hotspot"
          info="Keeps the mixer hotspot on even while joined to home Wi-Fi. Can make showbox.local unreliable on the LAN when Bluetooth is also up."
          checked={!!status.apAlways}
          onChange={(event) => {
            patch({ apAlways: event.target.checked }).catch(() => { event.target.checked = !event.target.checked; });
          }}
        />
      </div>
    </div>
  );
}
