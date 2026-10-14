export function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

let busyCount = 0;
export function setBusy(on) {
  busyCount += on ? 1 : -1;
  if (busyCount < 0) busyCount = 0;
  document.body.classList.toggle("busy", busyCount > 0);
}

export async function fetchRetry(url, opts) {
  let lastError = new Error("The module did not answer.");
  const method = opts && opts.method ? String(opts.method).toUpperCase() : "GET";
  const tries = method === "GET" ? 3 : 8;
  for (let i = 0; i < tries; i++) {
    try {
      const response = await fetch(url, opts);
      if (response.status === 500 || response.status === 503) {
        lastError = new Error((await response.text()) || "The module is busy.");
        await sleep(400 * (i + 1));
        continue;
      }
      return response;
    } catch (err) {
      lastError = err;
      await sleep(400 * (i + 1));
    }
  }
  throw lastError;
}

export async function getJson(url) {
  const response = await fetchRetry(url);
  const text = await response.text();
  if (!response.ok) throw new Error(text || "Request failed");
  return text ? JSON.parse(text) : {};
}

export async function postQuery(url) {
  setBusy(true);
  try {
    const response = await fetchRetry(url, { method: "POST" });
    const text = await response.text();
    if (!response.ok) throw new Error(text || "Request failed");
    return text && text[0] === "{" ? JSON.parse(text) : text;
  } finally {
    setBusy(false);
  }
}

export async function postJson(url, body) {
  setBusy(true);
  try {
    const response = await fetchRetry(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body)
    });
    const text = await response.text();
    if (!response.ok) throw new Error(text || "Request failed");
    return text ? JSON.parse(text) : {};
  } finally {
    setBusy(false);
  }
}

export function asList(value) {
  return Array.isArray(value) ? value : [];
}

export function connectLive(onEvent, hooks = {}) {
  let socket = null;
  let closed = false;
  let timer = 0;
  let ping = 0;
  let watchdog = 0;
  function armWatch() {
    clearTimeout(watchdog);
    watchdog = setTimeout(() => { if (socket) socket.close(); }, 12000);
  }
  function open() {
    if (closed) return;
    const proto = location.protocol === "https:" ? "wss" : "ws";
    socket = new WebSocket(`${proto}://${location.host}/ws`);
    socket.onopen = () => {
      if (hooks.onOpen) hooks.onOpen();
      clearInterval(ping);
      ping = setInterval(() => {
        if (socket && socket.readyState === 1) socket.send("p");
      }, 4000);
      armWatch();
    };
    socket.onmessage = (event) => {
      armWatch();
      try {
        onEvent(JSON.parse(event.data));
      } catch {
        /* ignore */
      }
    };
    socket.onclose = () => {
      clearInterval(ping);
      clearTimeout(watchdog);
      socket = null;
      if (hooks.onClose) hooks.onClose();
      if (!closed) timer = setTimeout(open, 1500);
    };
    socket.onerror = () => {
      if (socket) socket.close();
    };
  }
  open();
  return () => {
    closed = true;
    clearTimeout(timer);
    clearInterval(ping);
    clearTimeout(watchdog);
    if (socket) socket.close();
  };
}
