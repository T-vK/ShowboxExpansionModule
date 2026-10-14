import { useEffect, useState } from "preact/hooks";
import { asList, getJson, postQuery } from "../api.js";

const ISO = [
  [0, "Product path"],
  [1, "AppleMIDI, no BLE"],
  [2, "DIN + extras, no BLE/Apple"],
  [3, "UART + Wi-Fi only"],
  [4, "UART + DIN"],
  [5, "UART + extras"],
  [10, "BLE idle"],
  [11, "BLE advertising"],
  [12, "BLE + AIRSTEP"],
  [13, "BLE + HID host"]
];

const KEYS = [
  ["escape", "Esc"], ["tab", "Tab"], ["backspace", "Bksp"], ["enter", "Enter"],
  ["space", "Space"], ["insert", "Ins"], ["delete", "Del"], ["home", "Home"],
  ["end", "End"], ["pageup", "PgUp"], ["pagedown", "PgDn"],
  ["up", "↑"], ["down", "↓"], ["left", "←"], ["right", "→"],
  ["ctrl", "Ctrl"], ["shift", "Shift"], ["alt", "Alt"], ["gui", "GUI"],
  ["f1", "F1"], ["f2", "F2"], ["f3", "F3"], ["f4", "F4"],
  ["f5", "F5"], ["f6", "F6"], ["f7", "F7"], ["f8", "F8"],
  ["f9", "F9"], ["f10", "F10"], ["f11", "F11"], ["f12", "F12"]
];

const NOTES = [
  [48, "C3"], [60, "C4"], [64, "E4"], [67, "G4"], [72, "C5"]
];

export function DebugPanel() {
  const [data, setData] = useState(null);
  const [error, setError] = useState("");
  const [keytext, setKeytext] = useState("");
  const [iso, setIso] = useState(0);
  const [soak, setSoak] = useState(null);
  const [dump, setDump] = useState("");
  const [note, setNote] = useState(60);
  const [vel, setVel] = useState(100);
  const [ch, setCh] = useState(1);
  const [cc, setCc] = useState(1);
  const [ccVal, setCcVal] = useState(127);
  const [pc, setPc] = useState(0);

  const liveMix = (data && data.liveHidMix) || { keyboard: false, mouse: false, absPointer: false, digitizer: false };
  const debugInfo = (data && data.debug) || {};
  const transports = asList(data && data.transports);

  async function refresh() {
    const next = await getJson("/api/v1/midi");
    setData(next);
    setError("");
  }

  async function post(url) {
    try {
      await postQuery(url);
      await refresh();
    } catch (err) {
      setError(err.message);
      throw err;
    }
  }

  async function toggle(id, event) {
    const on = event.target.checked;
    try {
      await post("/api/v1/midi/settings?" + id + "=" + (on ? "1" : "0"));
    } catch {
      event.target.checked = !on;
    }
  }

  useEffect(() => {
    let stop = false;
    const tick = () => refresh().catch((err) => { if (!stop) setError(String(err.message || err)); });
    tick();
    getJson("/api/v1/debug/iso").then((row) => setIso(row.iso || 0)).catch(() => {});
    const id = setInterval(tick, 3000);
    return () => { stop = true; clearInterval(id); };
  }, []);

  async function loadDump() {
    try {
      const response = await fetch("/api/v1/debug", { cache: "no-store" });
      setDump(await response.text());
    } catch (err) {
      setError(String(err.message || err));
    }
  }

  async function setIsoLevel(level) {
    try {
      await postQuery("/api/v1/debug/iso?level=" + level);
      setIso(level);
    } catch (err) {
      setError(err.message);
    }
  }

  async function runSoak() {
    try {
      const result = await postQuery("/api/v1/debug/uartsoak?seconds=40");
      setSoak(result);
    } catch (err) {
      setError(err.message);
    }
  }

  useEffect(() => {
    if (!soak || soak.done) return undefined;
    const id = setInterval(() => {
      getJson("/api/v1/debug/uartsoak").then(setSoak).catch(() => {});
    }, 2000);
    return () => clearInterval(id);
  }, [soak && soak.running]);

  return (
    <div class="panel debug-panel">
      <p class="hint">Feature probes only. Pair a HID or MIDI host first, then fire the buttons. Mixer ↔ base keeps running.</p>
      <p id="error">{error}</p>
      <p class="hint">
        HID mix: {[liveMix.keyboard && "keyboard", liveMix.mouse && "mouse", liveMix.absPointer && "absolute mouse", liveMix.digitizer && "touch"].filter(Boolean).join(", ") || "none"}.
        {" "}{data && data.hidReady ? "HID notify ready." : "No HID host connected."}
      </p>
      {transports.length ? (
        <p class="hint">
          Transports: {transports.filter((row) => row.enabled).map((row) => row.id).join(", ") || "none enabled"}.
        </p>
      ) : null}

      <section>
        <h2>Keyboard</h2>
        <form onSubmit={(event) => { event.preventDefault(); post("/api/v1/hid/keyboard?text=" + encodeURIComponent(keytext)).catch(() => {}); }}>
          <input type="text" maxlength="24" placeholder="Text to type" value={keytext} onInput={(event) => setKeytext(event.currentTarget.value)} />
          <button type="submit" disabled={!liveMix.keyboard}>Send keys</button>
        </form>
        <p class="actions">
          {KEYS.map(([id, label]) => (
            <button type="button" key={id} disabled={!liveMix.keyboard} onClick={() => post("/api/v1/hid/keyboard?key=" + id).catch(() => {})}>{label}</button>
          ))}
        </p>
      </section>

      <section>
        <h2>Mouse</h2>
        <p class="actions">
          <button type="button" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?x=-40&y=0").catch(() => {})}>Left</button>
          <button type="button" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?x=40&y=0").catch(() => {})}>Right</button>
          <button type="button" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?x=0&y=-40").catch(() => {})}>Up</button>
          <button type="button" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?x=0&y=40").catch(() => {})}>Down</button>
          <button type="button" class="ghost" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?buttons=1").catch(() => {})}>Left click</button>
          <button type="button" class="ghost" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?buttons=2").catch(() => {})}>Right click</button>
          <button type="button" class="ghost" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?buttons=4").catch(() => {})}>Middle</button>
        </p>
        <p class="actions">
          <button type="button" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?wheel=8").catch(() => {})}>Wheel up</button>
          <button type="button" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?wheel=-8").catch(() => {})}>Wheel down</button>
          <button type="button" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?pan=-8").catch(() => {})}>Wheel left</button>
          <button type="button" disabled={!liveMix.mouse} onClick={() => post("/api/v1/hid/mouse?pan=8").catch(() => {})}>Wheel right</button>
        </p>
      </section>

      <section>
        <h2>Absolute mouse</h2>
        <p class="actions">
          <button type="button" disabled={!liveMix.absPointer} onClick={() => post("/api/v1/hid/touch?action=swipe&x=5000&y=5000&x2=5000&y2=2000&via=abs").catch(() => {})}>Swipe up</button>
          <button type="button" disabled={!liveMix.absPointer} onClick={() => post("/api/v1/hid/touch?action=swipe&x=5000&y=5000&x2=5000&y2=8000&via=abs").catch(() => {})}>Swipe down</button>
          <button type="button" disabled={!liveMix.absPointer} onClick={() => post("/api/v1/hid/touch?action=swipe&x=5000&y=5000&x2=2000&y2=5000&via=abs").catch(() => {})}>Swipe left</button>
          <button type="button" disabled={!liveMix.absPointer} onClick={() => post("/api/v1/hid/touch?action=swipe&x=5000&y=5000&x2=8000&y2=5000&via=abs").catch(() => {})}>Swipe right</button>
          <button type="button" disabled={!liveMix.absPointer} onClick={() => post("/api/v1/hid/touch?action=tap&x=5000&y=5000&via=abs").catch(() => {})}>Tap center</button>
        </p>
      </section>

      <section>
        <h2>Touch</h2>
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
        <h2>MIDI</h2>
        <p class="hint">Sends on every live transport (DIN, BLE, AppleMIDI, USB). Note-off follows 250 ms later. Panic is CC 123 + 120 on the selected channel.</p>
        <p class="actions midi-test-row">
          {NOTES.map(([id, label]) => (
            <button type="button" key={id} class={note === id ? "" : "ghost"} onClick={() => setNote(id)}>{label}</button>
          ))}
          <label class="labeled">Ch
            <select value={ch} onChange={(event) => setCh(Number(event.currentTarget.value))}>
              {Array.from({ length: 16 }, (_, i) => i + 1).map((n) => <option key={n} value={n}>{n}</option>)}
            </select>
          </label>
          <label class="labeled">Vel
            <input type="number" min="1" max="127" value={vel} onInput={(event) => setVel(Number(event.currentTarget.value) || 1)} />
          </label>
          <button type="button" onClick={() => post(`/api/v1/midi/test?kind=note&note=${note}&vel=${vel}&ch=${ch}`).catch(() => {})}>Send note</button>
        </p>
        <p class="actions midi-test-row">
          <label class="labeled">CC
            <input type="number" min="0" max="127" value={cc} onInput={(event) => setCc(Number(event.currentTarget.value) || 0)} />
          </label>
          <label class="labeled">Val
            <input type="number" min="0" max="127" value={ccVal} onInput={(event) => setCcVal(Number(event.currentTarget.value) || 0)} />
          </label>
          <button type="button" onClick={() => post(`/api/v1/midi/test?kind=cc&cc=${cc}&val=${ccVal}&ch=${ch}`).catch(() => {})}>Send CC</button>
          <label class="labeled">PC
            <input type="number" min="0" max="127" value={pc} onInput={(event) => setPc(Number(event.currentTarget.value) || 0)} />
          </label>
          <button type="button" class="ghost" onClick={() => post(`/api/v1/midi/test?kind=pc&pc=${pc}&ch=${ch}`).catch(() => {})}>Program</button>
          <button type="button" class="warn" onClick={() => post(`/api/v1/midi/test?kind=panic&ch=${ch}`).catch(() => {})}>Panic</button>
        </p>
      </section>

      <section>
        <h2>Activity</h2>
        <pre class="logbox">{asList(data && data.log).length ? asList(data && data.log).join("\n") : "No test events yet."}</pre>
      </section>

      <section>
        <h2>Isolation & soak</h2>
        <p class="hint">Iso writes NVS then reboots. UART soak turns Wi-Fi off for a timed mixer↔base stress.</p>
        <label class="labeled">
          <span>Iso {iso}</span>
          <select value={iso} onChange={(event) => setIsoLevel(Number(event.currentTarget.value))}>
            {ISO.map(([id, label]) => <option key={id} value={id}>{id} · {label}</option>)}
          </select>
        </label>
        <p class="actions">
          <button type="button" class="ghost" onClick={runSoak}>UART soak 40s</button>
        </p>
        {soak ? <pre class="logbox">{JSON.stringify(soak, null, 2)}</pre> : null}
      </section>

      <section>
        <h2>Logging</h2>
        <p class="actions">
          <label class="labeled">
            <span>Keep logs until telnet</span>
            <input type="checkbox" checked={!!debugInfo.wifiLogHistory} onChange={(event) => toggle("wifiLogHistory", event)} />
          </label>
          <label class="labeled">
            <span>Log mixer/base packets</span>
            <input type="checkbox" checked={!!debugInfo.uartPackets} onChange={(event) => toggle("uartPackets", event)} />
          </label>
          <label class="labeled">
            <span>Timed USB test</span>
            <input type="checkbox" checked={!!debugInfo.usbSafeTest} onChange={(event) => toggle("usbSafeTest", event)} />
          </label>
        </p>
        <p class="actions"><button type="button" class="ghost" onClick={loadDump}>Load /api/v1/debug</button></p>
        {dump ? <pre class="logbox">{dump}</pre> : null}
      </section>
    </div>
  );
}
