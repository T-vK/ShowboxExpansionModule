import { useEffect, useRef, useState } from "preact/hooks";
import { Dropdown } from "../ui/Dropdown.jsx";
import { IconBattery, IconSignalStream } from "../mixer/Icons.jsx";
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

export function StatusCluster() {
  const { esp, link } = useLive();
  const [open, setOpen] = useState(false);
  const [now, setNow] = useState(() => Date.now());
  const linkBtn = useRef(null);
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
    popAnchor.current = which === "batt" ? battBtn.current : linkBtn.current;
    setOpen((v) => !v);
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
      <button
        ref={battBtn}
        type="button"
        class={"batt-chip " + (pct == null ? "unknown" : battClass(pct)) + (link.ok ? "" : " stale")}
        title={pct == null ? "Battery unknown" : `Showbox battery ${pct}%`}
        onClick={() => toggle("batt")}
      >
        <span class="batt-meter">
          <IconBattery pct={pct} />
        </span>
      </button>
      <Dropdown anchorRef={popAnchor} open={open} wide onClose={() => setOpen(false)}>
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
      </Dropdown>
    </>
  );
}
