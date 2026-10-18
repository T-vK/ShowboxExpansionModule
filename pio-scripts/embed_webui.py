Import("env")

from pathlib import Path
import gzip
import os
import re
import shutil
import struct
import subprocess
import zlib

ROOT = Path(env["PROJECT_DIR"])
UI = ROOT / "ui"
PUBLIC = UI / "public"
HEADER = ROOT / "src" / "WebUi" / "Page.h"
ASSET_HEADER = ROOT / "src" / "WebUi" / "WebAssets.h"
PWA_HEADER = ROOT / "src" / "WebUi" / "PwaAssets.h"
DIST_HTML = UI / "dist" / "index.html"


def newest_mtime(paths):
    latest = 0
    for path in paths:
        if path.is_file():
            latest = max(latest, path.stat().st_mtime)
        elif path.is_dir():
            for child in path.rglob("*"):
                if child.is_file() and "node_modules" not in child.parts and "dist" not in child.parts:
                    latest = max(latest, child.stat().st_mtime)
    return latest


def c_bytes(name, data):
    lines = ["static const uint8_t %s[] PROGMEM = {" % name]
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("  " + ", ".join("0x%02x" % b for b in chunk) + ",")
    lines.append("};")
    lines.append("static const size_t %s_LEN = %d;" % (name, len(data)))
    return "\n".join(lines)


def c_raw_string(name, text, delim):
    if (")%s\"" % delim) in text:
        raise ValueError("raw string delimiter %s appears in %s" % (delim, name))
    return "static const char %s[] = R\"%s(%s)%s\";" % (name, delim, text, delim)


def png_rgb(width, height, pixels):
    def chunk(tag, data):
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)

    raw = bytearray()
    for y in range(height):
        raw.append(0)
        row = pixels[y * width:(y + 1) * width]
        for r, g, b in row:
            raw.extend((r, g, b))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


def make_icon(size):
    bg = (11, 12, 16)
    panel = (22, 24, 30)
    groove = (7, 8, 10)
    cap = (90, 97, 112)
    hair = (232, 230, 223)
    accent = (78, 196, 212)
    pix = [bg] * (size * size)

    def setp(x, y, color):
        if 0 <= x < size and 0 <= y < size:
            pix[y * size + x] = color

    def fill(x0, y0, x1, y1, color):
        for y in range(max(0, y0), min(size, y1)):
            for x in range(max(0, x0), min(size, x1)):
                setp(x, y, color)

    margin = max(8, size // 12)
    fill(margin, margin, size - margin, size - margin, panel)
    slots = 4
    levels = (0.78, 0.42, 0.64, 0.26)
    inner = size - 4 * margin
    for i, level in enumerate(levels):
        x = margin * 2 + int(inner * (i + 0.5) / slots)
        gw = max(2, size // 48)
        fill(x - gw, margin * 2, x + gw + 1, size - margin * 2, groove)
        cy = int(margin * 2 + (1.0 - level) * (size - 4 * margin))
        ch = max(6, size // 13)
        cw = max(7, size // 9)
        color = accent if i == 2 else cap
        fill(x - cw, cy - ch // 2, x + cw, cy + ch // 2, color)
        fill(x - cw + 1, cy - max(1, size // 180), x + cw - 1, cy + max(1, size // 180), hair)
    return png_rgb(size, size, pix)


ASSET_TYPES = {
    ".js": "application/javascript",
    ".css": "text/css",
    ".html": "text/html",
    ".svg": "image/svg+xml",
    ".json": "application/json",
    ".woff2": "font/woff2",
}


def write_asset_table(dist_dir, header_path):
    """Embed every dist file (gzipped) with its path so the firmware can
    serve the split build: many small bodies survive the BLE-coex radio
    far better than one 71 KB single file, and each caches separately."""
    files = sorted(
        p for p in dist_dir.rglob("*")
        if p.is_file() and "__CLIENT__" not in p.name
    )
    lines = [
        "#pragma once",
        "#include <Arduino.h>",
        "",
        "struct WebUiAsset {",
        "    const char* path;",
        "    const char* type;",
        "    const uint8_t* gz;",
        "    const size_t len;",
        "};",
        "",
    ]
    count = 0
    for path in files:
        rel = path.relative_to(dist_dir).as_posix()
        if rel == "index.html":
            continue  # served as WEBUI_INDEX_GZ
        if rel in ("manifest.webmanifest", "sw.js"):
            continue  # embedded separately in PwaAssets.h with its own caching
        ext = path.suffix.lower()
        mime = ASSET_TYPES.get(ext)
        if mime is None:
            print("Skipping asset without a mime type: %s" % rel)
            continue
        raw = path.read_bytes()
        gz = gzip.compress(raw, compresslevel=9)
        if len(gz) >= len(raw):
            gz = raw  # tiny files: serve raw
        var = "WEBUI_FILE_%d" % count
        lines.append("static const uint8_t %s[] PROGMEM = {" % var)
        for i in range(0, len(gz), 16):
            lines.append("  " + ", ".join("0x%02x" % b for b in gz[i:i + 16]) + ",")
        lines.append("};")
        lines.append('static const WebUiAsset WEBUI_ASSET_%d = {"/%s", "%s", %s, %d};'
                     % (count, rel, mime, var, len(gz)))
        lines.append("")
        count += 1
    lines.append("static const WebUiAsset* const WEBUI_ASSETS[] = {")
    for i in range(count):
        lines.append("    &WEBUI_ASSET_%d," % i)
    lines.append("};")
    lines.append("static const size_t WEBUI_ASSETS_N = %d;" % count)
    if header_path.parent and not header_path.parent.is_dir():
        header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("Embedded %d Web UI asset files -> %s" % (count, header_path))


BOOTSTRAP = r"""<script>
(function () {
  var CACHE = "showboxmod-v19";
  var fill = document.getElementById("boot-fill");
  var status = document.getElementById("boot-status");
  var card = document.getElementById("boot");
  var MODS = __MODS__;
  var STYLES = __STYLES__;

  function status(msg) {
    if (status) status.textContent = msg;
  }
  function progress(frac, indeterminate) {
    if (!fill) return;
    if (indeterminate) {
      fill.classList.add("indeterminate");
      fill.style.width = "38%";
    } else {
      fill.classList.remove("indeterminate");
      fill.style.width = Math.max(0, Math.min(100, Math.round(frac * 100))) + "%";
    }
  }
  function fail(msg) {
    if (window.__uiFailed) return;
    window.__uiFailed = true;
    card.classList.add("boot-failed");
    status.textContent = msg || "Load failed — the radio link dropped it.";
  }
  setTimeout(function () {
    if (!window.__uiBooted && !window.__uiFailed) {
      fail("Still loading — the radio link is busy. Tap Retry.");
      progress(1, true);
    }
  }, 12000);

  function kb(n) { return (n / 1024).toFixed(0) + " KB"; }
  // Resumable, stall-proof fetch with live byte progress. Range slices are
  // raw gzip bytes; the assembled response keeps Content-Encoding so the
  // browser decodes it exactly once. Warm the service worker cache as we go
  // so the module import afterwards is instant.
  function fetchWithProgress(url, label) {
    var got = 0;
    return new Promise(function (resolve, reject) {
      var attempt = 0;
      function go() {
        attempt++;
        if (attempt > 150) { reject(new Error("too many stalls: " + url)); return; }
        var ctrl = new AbortController();
        var killer = setTimeout(function () { ctrl.abort(); }, 8000);
        var done = false;
        fetch(url, { headers: { Range: "bytes=" + got + "-" }, signal: ctrl.signal, cache: "no-store" })
          .then(function (res) {
            if (res.status !== 200 && res.status !== 206) { throw new Error("http " + res.status); }
            var cr = res.headers.get("Content-Range");
            var total = cr ? parseInt(cr.slice(cr.lastIndexOf("/") + 1), 10) || 0
                           : parseInt(res.headers.get("Content-Length") || "0", 10) || 0;
            var reader = res.body.getReader();
            var pieces = [];
            function pump() {
              var t = setTimeout(function () { ctrl.abort(); }, 3500);
              return reader.read().then(function (r) {
                clearTimeout(t);
                if (r.done) {
                  if (total && got !== total) { throw new Error("short body " + got + "/" + total); }
                  done = true;
                  return;
                }
                got += r.value.length;
                pieces.push(r.value);
                if (total) {
                  progress(got / total);
                  status(label + " " + kb(got) + " / " + kb(total) + " (" + Math.round(got / total * 100) + "%)");
                }
                return pump();
              }, function (e) { clearTimeout(t); throw e; });
            }
            return pump().then(function () {
              var head = new Headers();
              head.set("Content-Type", res.headers.get("Content-Type") || "application/octet-stream");
              head.set("Content-Encoding", res.headers.get("Content-Encoding") || "identity");
              head.set("Content-Length", String(got));
              var out = new Response(new Blob(pieces), { status: 200, headers: head });
              if (window.caches && caches.open) {
                caches.open(CACHE).then(function (c) { c.put(url, out.clone()); }).catch(function () {});
              }
              return out.blob();
            });
          })
          .then(function (blob) {
            clearTimeout(killer);
            resolve(blob);
          })
          .catch(function () {
            clearTimeout(killer);
            if (!done) { setTimeout(go, 600); }
          });
      }
      go();
    });
  }
  function blobUrl(blob, type) {
    return URL.createObjectURL(new Blob([blob], { type: type }));
  }

  // The service worker handles any direct fetches we miss; wait briefly for
  // it to take control, then everything is cached as it arrives.
  var swReady = Promise.resolve();
  if ("serviceWorker" in navigator) {
    swReady = navigator.serviceWorker.register("/sw.js").then(function () {
      return Promise.race([
        navigator.serviceWorker.ready,
        new Promise(function (r) { setTimeout(r, 2000); }),
      ]);
    }).catch(function () {});
  }

  function importWithRetry(src, tries) {
    var attempt = 0;
    function go() {
      attempt++;
      if (attempt > tries) { throw new Error("import kept failing: " + src); }
      var mod = import(src);
      var watchdog = new Promise(function (_, rej) {
        setTimeout(function () { rej(new Error("import timeout")); }, 60000);
      });
      return Promise.race([mod, watchdog]).then(function (v) {
        window.__uiBooted = true;
        progress(1);
        status("Ready");
        return v;
      }, function () {
        return new Promise(function (r) { setTimeout(r, 900); }).then(function () { return go(); });
      });
    }
    return go();
  }

  swReady.then(function () {
    var chain = Promise.resolve();
    STYLES.forEach(function (href, i) {
      chain = chain.then(function () {
        progress(undefined, true);
        status("Loading stylesheet…");
        return fetchWithProgress(href, "stylesheet").then(function (blob) {
          var l = document.createElement("link");
          l.rel = "stylesheet";
          l.href = blobUrl(blob, "text/css");
          document.head.appendChild(l);
        }).catch(function () {
          var l = document.createElement("link");
          l.rel = "stylesheet";
          l.href = href;
          document.head.appendChild(l);
        });
      });
    });
    MODS.forEach(function (src) {
      chain = chain.then(function () {
        return fetchWithProgress(src, "app bundle").then(function (blob) {
          status("Starting mixer…");
          progress(1, true);
          return importWithRetry(src, 3);
        });
      });
    });
    chain.catch(function () { fail(); });
  });
})();
</script>"""


def transform_shell(html):
    """Vite injects <script type=module> / <link rel=stylesheet> tags. A tag
    whose transfer hangs never fires an error event, and the BLE-coex radio
    stalls bodies for tens of seconds — the page would sit at 'Loading'
    forever. Strip the tags and load the bundles with byte-resuming fetches
    instead; the URLs are baked into the bootstrap at build time."""
    mods = re.findall(r'<script type="module"[^>]*src="([^"]*)"', html)
    styles = re.findall(r'<link rel="stylesheet"[^>]*href="([^"]*)"', html)
    html = re.sub(r'<script type="module"[^>]*src="[^"]*"[^>]*></script>\s*', "", html)
    html = re.sub(r'<link rel="modulepreload"[^>]*>\s*', "", html)
    html = re.sub(r'<link rel="stylesheet"[^>]*>\s*', "", html)
    bootstrap = BOOTSTRAP.replace("__MODS__", repr(list(mods))).replace("__STYLES__", repr(list(styles)))
    html = html.replace("</body>", bootstrap + "\n</body>")
    return html


def write_header(gz_bytes):
    HEADER.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "#pragma once",
        "#include <Arduino.h>",
        "",
        "static const uint8_t WEBUI_INDEX_GZ[] PROGMEM = {",
    ]
    for i in range(0, len(gz_bytes), 16):
        chunk = gz_bytes[i:i + 16]
        lines.append("  " + ", ".join("0x%02x" % b for b in chunk) + ",")
    lines.append("};")
    lines.append("static const size_t WEBUI_INDEX_GZ_LEN = %d;" % len(gz_bytes))
    lines.append("")
    HEADER.write_text("\n".join(lines), encoding="utf-8")
    print("Embedded Web UI: %d bytes gzipped -> %s" % (len(gz_bytes), HEADER))


def write_pwa_assets():
    PUBLIC.mkdir(parents=True, exist_ok=True)
    icons = {
        PUBLIC / "icon-192.png": 192,
        PUBLIC / "icon-512.png": 512,
        PUBLIC / "apple-touch-icon.png": 180,
    }
    for path, size in icons.items():
        if not path.is_file():
            path.write_bytes(make_icon(size))
            print("Wrote PWA icon %s" % path)
    manifest = (PUBLIC / "manifest.webmanifest").read_text(encoding="utf-8")
    sw = (PUBLIC / "sw.js").read_text(encoding="utf-8")
    icon192 = (PUBLIC / "icon-192.png").read_bytes()
    icon512 = (PUBLIC / "icon-512.png").read_bytes()
    icon180 = (PUBLIC / "apple-touch-icon.png").read_bytes()
    parts = [
        "#pragma once",
        "#include <Arduino.h>",
        "",
        c_raw_string("PWA_MANIFEST", manifest, "MAN"),
        "",
        c_raw_string("PWA_SW", sw, "SJS"),
        "",
        c_bytes("PWA_ICON_192", icon192),
        "",
        c_bytes("PWA_ICON_512", icon512),
        "",
        c_bytes("PWA_ICON_180", icon180),
        "",
    ]
    PWA_HEADER.parent.mkdir(parents=True, exist_ok=True)
    text = "\n".join(parts)
    if not PWA_HEADER.is_file() or PWA_HEADER.read_text(encoding="utf-8") != text:
        PWA_HEADER.write_text(text, encoding="utf-8")
        print("Embedded PWA assets -> %s" % PWA_HEADER)


def build_ui():
    write_pwa_assets()
    npm = shutil.which("npm")
    if npm is None:
        print("npm not found; using existing Web UI header if present")
        return HEADER.is_file()
    sources_mtime = newest_mtime([
        UI / "package.json",
        UI / "package-lock.json",
        UI / "vite.config.js",
        UI / "index.html",
        UI / "src",
        UI / "public",
        Path(env["PROJECT_DIR"]) / "pio-scripts" / "embed_webui.py",
    ])
    if (
        HEADER.is_file()
        and ASSET_HEADER.is_file()
        and HEADER.stat().st_mtime >= sources_mtime
        and sources_mtime > 0
    ):
        print("Web UI header is current")
        return True
    env_local = os.environ.copy()
    env_local["npm_config_fund"] = "false"
    if not (UI / "node_modules").is_dir():
        print("Installing Web UI npm packages")
        subprocess.check_call([npm, "ci" if (UI / "package-lock.json").is_file() else "install"], cwd=UI, env=env_local)
    print("Building Web UI")
    subprocess.check_call([npm, "run", "build"], cwd=UI, env=env_local)
    html = transform_shell(DIST_HTML.read_text(encoding="utf-8"))
    write_header(gzip.compress(html.encode("utf-8"), compresslevel=9))
    write_asset_table(UI / "dist", ASSET_HEADER)
    return True


if not build_ui():
    raise SystemExit("Web UI header is missing and npm could not build it")
