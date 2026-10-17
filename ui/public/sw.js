const CACHE = "showboxmod-v19";

self.addEventListener("install", (event) => {
  self.skipWaiting();
  event.waitUntil(
    caches.open(CACHE).then((cache) => cache.addAll(["/", "/manifest.webmanifest"]))
  );
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(keys.filter((key) => key !== CACHE).map((key) => caches.delete(key)))
    ).then(() => self.clients.claim())
  );
});

// The BLE-coex radio stalls sustained TCP bodies mid-flight (ping of any
// size flows; data+ACK interleaving dies for tens of seconds). Stream the
// body, abort on inactivity, and resume from the exact byte with HTTP
// Range until the whole file is here. Range slices are raw gzip bytes, so
// the assembled response keeps the original Content-Encoding for the
// browser to decode once, at the end.
async function fetchResumable(req, startAt) {
  let got = startAt;
  let total = 0;
  // Accumulate across resume attempts: `got` is the resume offset, the
  // chunk list must hold every byte from the very first attempt.
  const chunks = [];
  for (let i = 0; i < 80; i++) {
    const ctrl = new AbortController();
    const headers = { Range: "bytes=" + got + "-" };
    const killer = setTimeout(() => ctrl.abort(), 8000);
    try {
      const res = await fetch(req, { headers, signal: ctrl.signal, cache: "no-store" });
      clearTimeout(killer);
      if (res.status !== 200 && res.status !== 206) {
        return res;
      }
      if (res.status === 200 && got > startAt) {
        chunks.length = 0;
        got = startAt; // server ignored Range: start over
      }
      const cr = res.headers.get("Content-Range");
      total = cr
        ? parseInt(cr.slice(cr.lastIndexOf("/") + 1), 10) || 0
        : parseInt(res.headers.get("Content-Length") || "0", 10) || 0;
      const reader = res.body.getReader();
      const enc = new Headers(res.headers);
      let mime = enc.get("Content-Type") || "application/octet-stream";
      let isGzip = !!enc.get("Content-Encoding");
      while (true) {
        const t = setTimeout(() => ctrl.abort(), 3500);
        let r;
        try {
          r = await reader.read();
        } finally {
          clearTimeout(t);
        }
        if (r.done) {
          break;
        }
        chunks.push(r.value);
        got += r.value.length;
      }
      if (total && got !== total) {
        throw new Error("short body " + got + "/" + total);
      }
      const out = new Response(new Blob(chunks), {
        status: 200,
        headers: {
          "Content-Type": mime,
          "Content-Length": String(got),
          "Content-Encoding": isGzip ? "gzip" : "identity",
          "Cache-Control": "no-cache",
        },
      });
      const copy = out.clone();
      caches.open(CACHE).then((c) => c.put(req, copy)).catch(() => {});
      return out;
    } catch (err) {
      clearTimeout(killer);
      await new Promise((r) => setTimeout(r, 600));
    }
  }
  return fetch(req);
}

self.addEventListener("fetch", (event) => {
  const req = event.request;
  if (req.method !== "GET") {
    return;
  }
  let url;
  try {
    url = new URL(req.url);
  } catch {
    return;
  }
  if (url.origin !== self.location.origin) {
    return;
  }
  const path = url.pathname;
  if (path.startsWith("/api/") || path === "/ws" || path.startsWith("/ws")) {
    return;
  }
  // Hashed build assets (including module imports and stylesheets): resume
  // across radio stalls, then cache-first forever (names are immutable).
  if (path.startsWith("/assets/")) {
    event.respondWith((async () => {
      const hit = await caches.match(req);
      if (hit) {
        return hit;
      }
      let startAt = 0;
      const rm = req.headers.get("range");
      if (rm && rm.startsWith("bytes=")) {
        const s = parseInt(rm.slice(6), 10);
        if (s > 0 && !isNaN(s)) {
          startAt = s;
        }
      }
      return fetchResumable(req, startAt);
    })());
    return;
  }
  event.respondWith(
    fetch(req)
      .then((res) => {
        if (res.ok && (path === "/" || path.endsWith(".webmanifest") || path.endsWith(".png") || path.endsWith(".js"))) {
          const copy = res.clone();
          caches.open(CACHE).then((cache) => cache.put(req, copy));
        }
        return res;
      })
      .catch(() => caches.match(req).then((hit) => hit || caches.match("/")))
  );
});