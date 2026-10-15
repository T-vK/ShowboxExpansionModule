const CACHE = "showboxmod-v17";

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

// Read the whole body before handing it over: a transfer that dies mid-body
// (the BLE-coex radio stalls big HTTP responses partway) rejects here
// instead of silently delivering a broken asset.
async function fetchWhole(req) {
  const res = await fetch(req);
  if (!res || !res.ok) {
    return res;
  }
  const buf = await res.arrayBuffer();
  const headers = new Headers(res.headers);
  headers.delete("Content-Encoding");
  headers.delete("Content-Length");
  return new Response(buf, { status: res.status, statusText: res.statusText, headers });
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
  // Range/resume fetches (the shell's stall-proof loader) must hit the
  // network directly — sliced bodies are raw gzip and must not be decoded
  // or cached individually.
  if (req.headers.has("range")) {
    return;
  }
  // Hashed build assets are immutable: cache-first, so repeat loads never
  // touch the radio. On a fetch that fails or dies mid-body, retry a few
  // times before giving up.
  if (path.startsWith("/assets/")) {
    event.respondWith((async () => {
      const hit = await caches.match(req);
      if (hit) {
        return hit;
      }
      let last = null;
      // The BLE-coex radio has dead windows of several seconds; spread the
      // attempts across ~15 s so one window cannot exhaust them.
      for (let i = 0; i < 8; i++) {
        try {
          const res = await fetchWhole(req);
          if (res && res.ok) {
            const copy = res.clone();
            caches.open(CACHE).then((cache) => cache.put(req, copy));
            return res;
          }
          last = res;
        } catch (err) {
          last = null;
        }
        await new Promise((r) => setTimeout(r, 1500 + 500 * i));
      }
      return last || fetch(req);
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
