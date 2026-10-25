import { useEffect, useRef, useState } from "preact/hooks";
import { getJson } from "../api.js";

const UART_BPS = 11520; // 115200 baud 8N1 payload throughput

function fmtBytes(n) {
  const v = Number(n) || 0;
  if (v >= 1048576) return `${(v / 1048576).toFixed(1)} MB`;
  if (v >= 1024) return `${(v / 1024).toFixed(1)} KB`;
  return `${Math.round(v)} B`;
}

function fmtDur(ms) {
  let s = Math.max(0, Math.floor((Number(ms) || 0) / 1000));
  const d = Math.floor(s / 86400);
  s -= d * 86400;
  const h = Math.floor(s / 3600);
  const m = Math.floor((s - h * 3600) / 60);
  if (d) return `${d}d ${h}h ${m}m`;
  if (h) return `${h}h ${m}m`;
  if (m) return `${m}m ${s % 60}s`;
  return `${s}s`;
}

function pct(used, total) {
  if (!total) return 0;
  return Math.max(0, Math.min(100, (used / total) * 100));
}

function Bar({ value, tone, label, right }) {
  return (
    <div class="diag-bar">
      <span class="diag-bar-head">
        <span>{label}</span>
        <span class="diag-bar-val">{right}</span>
      </span>
      <div class="diag-track">
        <div class={"diag-fill " + (tone || "")} style={{ width: `${Math.min(100, Math.max(0, value))}%` }} />
      </div>
    </div>
  );
}

function Gauge({ title, rows }) {
  return (
    <div class="diag-card">
      <h4>{title}</h4>
      {rows.map(([label, value]) => (
        <div class="diag-row" key={label}>
          <span class="diag-k">{label}</span>
          <span class="diag-v">{value}</span>
        </div>
      ))}
    </div>
  );
}

const KIND_TONE = {
  heap: "bad", link: "warn", uart: "bad", wifi: "warn", loop: "bad",
  boot: "boot", ws: "soft", crash: "bad"
};

function LogRow({ row }) {
  const prev = String(row.m || "").startsWith("[prev-boot]");
  return (
    <li class={"diag-log-row" + (prev ? " prev" : "")}>
      <span class="diag-log-t">{fmtDur(row.t)}</span>
      <span class={"diag-log-k " + (KIND_TONE[row.k] || "soft")}>{row.k}</span>
      <span class="diag-log-m">{String(row.m || "").replace("[prev-boot] ", "")}</span>
    </li>
  );
}

export function DiagnosticsPanel() {
  const [data, setData] = useState(null);
  const [err, setErr] = useState("");
  const [paused, setPaused] = useState(false);
  const [showAllTasks, setShowAllTasks] = useState(false);
  const alive = useRef(true);

  useEffect(() => {
    alive.current = true;
    let timer = null;
    async function poll() {
      if (!alive.current) return;
      try {
        const d = await getJson("/api/v1/diag");
        if (alive.current) {
          setData(d);
          setErr("");
        }
      } catch (e) {
        if (alive.current) setErr(String(e.message || e));
      }
      if (alive.current && !paused) timer = setTimeout(poll, 1000);
    }
    poll();
    return () => {
      alive.current = false;
      clearTimeout(timer);
    };
  }, [paused]);

  if (err && !data) {
    return <section class="page-body diag-page"><p class="banner">Diagnostics unavailable: {err}</p></section>;
  }
  if (!data) {
    return <section class="page-body diag-page"><p class="hint">Loading diagnostics…</p></section>;
  }

  const { boot, mem, flash, net, uart, tasks, load, log, crashLogs } = data;
  const intUsedPct = pct((mem?.int?.size || 0) - (mem?.int?.free || 0), mem?.int?.size);
  const psUsedPct = pct((mem?.psram?.size || 0) - (mem?.psram?.free || 0), mem?.psram?.size);
  const sketchPct = pct(flash?.sketch, flash?.part);
  const uartUtil0 = pct(uart?.rxBps, UART_BPS);
  const uartUtil1 = pct(uart?.rxMps, UART_BPS);
  const visibleTasks = (tasks || [])
    .slice()
    .sort((a, b) => (b.cpu - a.cpu) || (a.n > b.n ? 1 : -1));
  const shownTasks = showAllTasks ? visibleTasks : visibleTasks.filter((t) => t.cpu >= 0.4 || t.hwm < 800);

  // Event log follows the tail unless the reader scrolled up.
  const logRef = useRef(null);
  const logStick = useRef(true);
  useEffect(() => {
    if (logStick.current && logRef.current) {
      logRef.current.scrollTop = logRef.current.scrollHeight;
    }
  }, [data]);

  return (
    <section class="page-body diag-page">
      {err ? <p class="banner">{err}</p> : null}
      <div class="diag-head">
        <div class="diag-title-row">
          <h3>Task manager</h3>
          <span class="diag-meta">up {fmtDur(boot?.uptimeMs)} · boot #{boot?.bootCount} · reset {boot?.reset} · fw {boot?.fw}</span>
          <button type="button" class={paused ? "" : "on"} onClick={() => setPaused((p) => !p)}>
            {paused ? "Resume" : "Pause"}
          </button>
        </div>
        <div class="diag-cores">
          <Bar value={load?.[0] || 0} tone={load?.[0] > 80 ? "bad" : ""} label="Core 0 (WiFi/BT)" right={`${(load?.[0] || 0).toFixed(1)}%`} />
          <Bar value={load?.[1] || 0} tone={load?.[1] > 80 ? "bad" : ""} label="Core 1 (app)" right={`${(load?.[1] || 0).toFixed(1)}%`} />
        </div>
      </div>

      <div class="diag-grid">
        <Gauge title="Memory" rows={[
          ["Internal free", fmtBytes(mem?.int?.free)],
          ["Internal largest", fmtBytes(mem?.int?.largest)],
          ["Internal min ever", fmtBytes(mem?.int?.min)],
          ["PSRAM free", fmtBytes(mem?.psram?.free)],
          ["PSRAM largest", fmtBytes(mem?.psram?.largest)],
        ]} />
        <Gauge title="Flash" rows={[
          ["Sketch", `${fmtBytes(flash?.sketch)} / ${fmtBytes(flash?.part)}`],
          ["Sketch free", fmtBytes(flash?.free)],
          ["NVS used/free", flash?.nvs ? `${flash.nvs.used} / ${flash.nvs.free} entries` : "—"],
        ]} />
        <Gauge title="Network" rows={[
          ["WiFi", net?.wifi?.up ? `up · ${net.wifi.rssi} dBm · ch ${net.wifi.chan}` : "down"],
          ["Gateway RTT", net?.gw?.rttMs ? `${net.gw.rttMs} ms` : "—"],
          ["Gateway misses", String(net?.gw?.misses ?? 0)],
          ["WS clients", String(net?.ws?.clients ?? 0)],
          ["WS out", `${fmtBytes(net?.ws?.txBps || 0)}/s · total ${fmtBytes(net?.ws?.txTotal)}`],
          ["WS send retries", String(net?.ws?.retries ?? 0)],
        ]} />
        <Gauge title="MIDI out" rows={[
          ["Messages", `${fmtInt(net?.midi?.outMsgs)} (${net?.midi?.outMsgsPerS || 0}/s)`],
          ["Bytes", `${fmtBytes(net?.midi?.outBytesPerS || 0)}/s`],
          ["X-Touch surface", net?.midi?.xtouch ? "connected" : "off"],
          ["BLE MIDI hosts", String(net?.midi?.bleHosts ?? 0)],
        ]} />
      </div>

      <div class="diag-card wide">
        <h4>Mixer ↔ base wire ({uart?.baud} baud ≈ {fmtBytes(UART_BPS)}/s each way)</h4>
        <Bar value={uartUtil0} tone={uartUtil0 > 70 ? "warn" : ""} label="Base → module" right={`${fmtBytes(uart?.rxBps || 0)}/s · ${uartUtil0.toFixed(1)}% of wire`} />
        <Bar value={uartUtil1} tone={uartUtil1 > 70 ? "warn" : ""} label="Mixer → module" right={`${fmtBytes(uart?.rxMps || 0)}/s · ${uartUtil1.toFixed(1)}% of wire`} />
        <div class="diag-chips">
          <span class={"diag-chip " + (uart?.link ? "ok" : "bad")}>link {uart?.link ? "live" : "DEAD"}</span>
          <span class="diag-chip">hb {uart?.hbM} / {uart?.hbB} ms</span>
          <span class={"diag-chip " + ((uart?.dropB || uart?.dropM) ? "bad" : "")}>drops {uart?.dropB}/{uart?.dropM}</span>
          <span class={"diag-chip " + ((uart?.ovfB || uart?.ovfM) ? "bad" : "")}>overflows {uart?.ovfB}/{uart?.ovfM}</span>
          <span class={"diag-chip " + ((uart?.frmB || uart?.frmM) ? "bad" : "")}>frame err {uart?.frmB}/{uart?.frmM}</span>
          <span class="diag-chip">parseQ drops {uart?.parseQDrop}</span>
          <span class="diag-chip">analog skipped {fmtInt(uart?.analogSkip)}</span>
          <span class="diag-chip">cut-through {uart?.cutB}/{uart?.cutM}</span>
          <span class="diag-chip">pending tx {uart?.pendB}/{uart?.pendM}</span>
          <span class="diag-chip">dump #{uart?.dumpSeq}</span>
        </div>
      </div>

      <div class="diag-card wide">
        <div class="diag-title-row">
          <h4>Tasks</h4>
          <button type="button" class="ghost" onClick={() => setShowAllTasks((v) => !v)}>
            {showAllTasks ? "Show active" : "Show all"}
          </button>
        </div>
        <div class="diag-tasks-wrap">
          <table class="diag-tasks">
            <thead>
              <tr><th>Task</th><th>Core</th><th>Prio</th><th>State</th><th>CPU</th><th>Stack min</th></tr>
            </thead>
            <tbody>
              {shownTasks.map((t, i) => (
                <tr key={`${t.n}`}>
                  <td>{t.n}</td>
                  <td>{t.c < 0 || t.c > 100 ? "any" : t.c}</td>
                  <td>{t.p}</td>
                  <td>{t.s}</td>
                  <td class={t.cpu > 40 ? "hot" : ""}>{t.cpu.toFixed(1)}%</td>
                  <td class={t.hwm < 512 ? "hot" : ""}>{t.hwm} B</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
        <p class="hint tight">Stack min is the free space ever left on that task's stack; close to zero means near-overflow. CPU is approximate (shared tasks counted separately).</p>
      </div>

      {sketchPct != null ? (
        <div class="diag-card wide">
          <h4>Sketch flash</h4>
          <Bar value={sketchPct} label="Used" right={`${fmtBytes(flash?.sketch)} of ${fmtBytes(flash?.part)}`} />
        </div>
      ) : null}

      <div class="diag-card wide">
        <h4>Event log</h4>
        {crashLogs?.length ? (
          <div class="diag-crash">
            {crashLogs.map((c, i) => <p key={i} class="diag-crash-row">{c}</p>)}
          </div>
        ) : null}
        <ul class="diag-log" ref={logRef} onScroll={(event) => {
          const el = event.currentTarget;
          logStick.current = el.scrollHeight - el.scrollTop - el.clientHeight < 48;
        }}>
          {(log || []).map((row, i) => <LogRow key={`${row.t}-${row.k}-${i}`} row={row} />)}
        </ul>
        <p class="hint tight">Events survive a reboot (RTC memory). Oldest first, newest at the bottom; the view follows the tail unless you scroll up. Crash boots keep a short tail in NVS above.</p>
      </div>
    </section>
  );
}

function fmtInt(n) {
  const v = Number(n) || 0;
  return v.toLocaleString("en-US");
}