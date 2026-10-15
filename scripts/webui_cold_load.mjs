#!/usr/bin/env node
// Real-browser cold-load test for the ShowboxMod web UI.
//
// A curl one-shot proves almost nothing here: a cold browser load is several
// sequential transfers (shell, hashed assets, module imports) racing the
// BLE-coex radio's multi-second dead windows, and the service worker changes
// how failures look (an old worker can even answer a failed asset fetch with
// the cached shell HTML, which then fails the module MIME check).
//
// This test drives headless Chromium in a FRESH context per round (no cache,
// no cookies, no service worker — exactly an incognito window), loads
// http://showbox.local/, and requires window.__uiBooted === true (set by
// main.jsx once the app has actually mounted). Every round must pass.
//
// usage: node scripts/webui_cold_load.mjs [--rounds 3] [--timeout 120000]
// [--url http://showbox.local/] [--stale-shell]

import { chromium, firefox } from "playwright";

function arg(name, def) {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : def;
}
function flag(name) {
  return process.argv.includes(name);
}

const rounds = parseInt(arg("--rounds", "3"), 10);
const timeoutMs = parseInt(arg("--timeout", "120000"), 10);
const url = arg("--url", "http://showbox.local/");
const staleShell = flag("--stale-shell") || flag("--all-scenarios");
const engineName = arg("--engine", flag("--all-scenarios") ? "both" : "chromium");

function launchEngine(name) {
  if (name === "firefox") {
    return firefox.launch();
  }
  return chromium.launch();
}

function pickEngine(round) {
  if (engineName === "both") {
    // stale-shell rounds run on Firefox (the field-failure browser, and the
    // engine without Private Network Access enforcement); normal rounds on
    // Chromium.
    return staleShell && round % 2 === 0 ? "firefox" : "chromium";
  }
  return engineName;
}

// Playwright's Firefox bundles its own DNS stack without mDNS, so
// showbox.local fails there with NS_ERROR_UNKNOWN_HOST; use the IP.
function pickUrl(engine) {
  if (engine === "firefox" && url.includes("showbox.local")) {
    return "http://192.168.3.55/";
  }
  return url;
}

function withTimeout(promise, ms, tag) {
  return Promise.race([
    promise,
    new Promise((_, rej) => setTimeout(() => rej(new Error(tag + " timeout")), ms)),
  ]).catch(() => undefined);
}

// The update-deadlock scenario: a browser that cached an older shell keeps
// loading that shell (network failures fall back to cache; HTTP cache under
// the old max-age), and the old shell's script tag points at an asset hash
// that no longer exists after an update. That browser showed 'Loading
// mixer…' forever. The shell below replicates it exactly — bare, unstyled,
// stale hash, no bootstrap, no retry.
const STALE_SHELL = `<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><title>ShowboxMod</title></head>
<body>
  <div id="app">
    <div class="shell">
      <header class="topbar">
        <div>
          <h1>ShowboxMod</h1>
          <p class="lead">Loading mixer…</p>
        </div>
      </header>
    </div>
  </div>
  <script type="module" crossorigin src="/assets/index-BDP4IidH.js"></script>
  <link rel="stylesheet" crossorigin href="/assets/style-BSp0B7vh.css">
</body></html>`;

const results = [];
let hardFailures = 0;

for (let round = 1; round <= rounds; round++) {
  const engine = pickEngine(round);
  const useStale = staleShell && (engineName === "both" ? engine === "firefox" : round % 2 === 0);
  const browser = await launchEngine(engine);
  const context = await browser.newContext();
  const page = await context.newPage();
  if (useStale) {
    // Serve the stale shell for the navigation only; everything else (the
    // dead-hash asset request) goes to the module for real.
    let served = false;
    await context.route("**/", (route) => {
      if (!served && route.request().url().replace(/\/$/, "") === targetUrl.replace(/\/$/, "")) {
        served = true;
        route.fulfill({ status: 200, contentType: "text/html", body: STALE_SHELL });
        return;
      }
      route.continue();
    });
  }

  const requests = [];
  const consoleErrors = [];
  const unfinished = new Set();
  page.on("requestfailed", (req) => {
    unfinished.delete(req.url());
    requests.push({ url: req.url(), state: "FAILED", err: req.failure()?.errorText });
  });
  page.on("requestfinished", (req) => {
    unfinished.delete(req.url());
  });
  page.on("request", (req) => {
    unfinished.add(req.url());
  });
  page.on("response", (res) => {
    requests.push({ url: res.url(), state: String(res.status()), mime: res.headers()["content-type"] });
  });
  page.on("console", (msg) => {
    if (msg.type() === "error") {
      consoleErrors.push(msg.text());
    }
  });
  page.on("pageerror", (err) => {
    consoleErrors.push("pageerror: " + err.message);
  });

  const started = Date.now();
  let booted = false;
  let shellText = "";
  let reloaded = false;
  const reloadAtMs = timeoutMs / 3;
  const targetUrl = pickUrl(engine);
  try {
    await page.goto(targetUrl, { waitUntil: "commit", timeout: 30000 });
    const deadline = started + timeoutMs;
    while (Date.now() < deadline) {
      booted = (await withTimeout(
        page.evaluate(() => window.__uiBooted === true), 5000, "boot-poll")) === true;
      if (booted) {
        break;
      }
      // A user taps reload when the page sits at 'Loading': on reload, "/"
      // goes to the network (network-first SW, no-cache shell) and gets the
      // current shell with the resumable bootstrap.
      if (!reloaded && Date.now() - started > reloadAtMs) {
        reloaded = true;
        await withTimeout(page.reload({ waitUntil: "commit", timeout: 30000 }), 32000, "reload");
      }
      shellText = (await withTimeout(
        page
          .evaluate(() => {
            const lead = document.querySelector("#bootbox .lead");
            if (lead) return lead.textContent;
            const fallback = document.querySelector(".lead");
            return fallback ? fallback.textContent : document.body.innerText.slice(0, 200);
          }),
        5000, "shell-poll")) ?? "";
      await page.waitForTimeout(500);
    }
  } catch (err) {
    consoleErrors.push("navigation: " + err.message);
  }

  const elapsed = ((Date.now() - started) / 1000).toFixed(1);
  const failed = requests.filter((r) => r.state === "FAILED");
  const non200 = requests.filter((r) => r.state !== "FAILED" && !r.state.startsWith("2"));

  if (booted) {
    console.log(`round ${round} [${engine}${useStale ? ", stale-shell" : ""}${reloaded ? ", reloaded" : ""}]: PASS in ${elapsed}s`);
  } else {
    hardFailures++;
    console.log(`round ${round} [${engine}${useStale ? ", stale-shell" : ""}${reloaded ? ", reloaded" : ""}]: FAIL after ${elapsed}s  shell="${shellText}"`);
    console.log(`  requests: ${requests.length} (failed ${failed.length}, non-200 ${non200.length})`);
    for (const r of requests.slice(-12)) {
      console.log(`    ${r.state} ${r.url} ${r.err ?? r.mime ?? ""}`);
    }
    if (unfinished.size) {
      console.log(`  requests never finished (stalled bodies): ${[...unfinished].slice(0, 4).join(", ")}`);
    }
    if (consoleErrors.length) {
      console.log("  console errors:");
      for (const e of consoleErrors.slice(0, 8)) {
        console.log(`    ${e.slice(0, 500)}`);
      }
    }
  }
  await browser.close();
  results.push({ round, engine, useStale, booted, elapsed, failed, non200 });
}

const passed = results.filter((r) => r.booted).length;
console.log(`\ncold-load: ${passed}/${results.length} rounds booted`);
process.exit(hardFailures === 0 ? 0 : 1);