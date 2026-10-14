import { useEffect, useRef, useState } from "preact/hooks";
import { fetchRetry, postJson } from "../api.js";
import { useLive } from "../live/LiveContext.jsx";

export function ConsolePanel() {
  const { esp } = useLive();
  const [log, setLog] = useState("");
  const [cmd, setCmd] = useState("");
  const [follow, setFollow] = useState(true);
  const [error, setError] = useState("");
  const since = useRef(0);
  const box = useRef(null);

  useEffect(() => {
    let stop = false;
    async function pull() {
      try {
        const response = await fetchRetry(`/api/v1/console?since=${since.current}`);
        const text = await response.text();
        const next = Number(response.headers.get("X-Console-Next"));
        if (Number.isFinite(next) && next > 0) since.current = next;
        if (text) {
          setLog((prev) => {
            const joined = prev + text;
            return joined.length > 120000 ? joined.slice(-100000) : joined;
          });
        }
        setError("");
      } catch (err) {
        if (!stop) setError(String(err.message || err));
      }
    }
    pull();
    const id = setInterval(pull, 450);
    return () => { stop = true; clearInterval(id); };
  }, []);

  useEffect(() => {
    if (!follow || !box.current) return;
    box.current.scrollTop = box.current.scrollHeight;
  }, [log, follow]);

  async function send(event) {
    event.preventDefault();
    const line = cmd.trim();
    if (!line) return;
    setCmd("");
    try {
      await postJson("/api/v1/console", { cmd: line });
      if (line.toLowerCase() === "clear") setLog("");
    } catch (err) {
      setError(String(err.message || err));
    }
  }

  return (
    <div class="console-page">
      <p class="hint">
        Same serial history telnet showbox.local 23 sees. Commands run on the module: help, heap, status, clear, reboot, iso [0–13], soak [seconds].
        {esp.connected ? "" : " WebSocket is down; log polling may still work."}
      </p>
      <p id="error">{error}</p>
      <pre class="console-log" ref={box}>{log || "Waiting for log lines…"}</pre>
      <form class="console-bar" onSubmit={send}>
        <input
          type="text"
          value={cmd}
          maxlength="80"
          placeholder="help"
          autocomplete="off"
          spellcheck="false"
          onInput={(event) => setCmd(event.currentTarget.value)}
        />
        <button type="submit">Send</button>
        <label class="console-follow">
          <input type="checkbox" checked={follow} onChange={(event) => setFollow(event.currentTarget.checked)} />
          Follow
        </label>
        <button type="button" class="ghost" onClick={() => setLog("")}>Clear view</button>
      </form>
    </div>
  );
}
