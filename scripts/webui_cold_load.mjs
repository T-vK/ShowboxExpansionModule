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
// [--url http://showbox.local/]

import { chromium } from "playwright";

function arg(name, def) {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : def;
}

const rounds = parseInt(arg("--rounds", "3"), 10);
const timeoutMs = parseInt(arg("--timeout", "120000"), 10);
const url = arg("--url", "http://showbox.local/");

const results = [];
let hardFailures = 0;

for (let round = 1; round <= rounds; round++) {
  const browser = await chromium.launch();
  const context = await browser.newContext();
  const page = await context.newPage();

  const requests = [];
  const consoleErrors = [];
  page.on("requestfailed", (req) => {
    requests.push({ url: req.url(), state: "FAILED", err: req.failure()?.errorText });
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
  try {
    await page.goto(url, { waitUntil: "commit", timeout: 30000 });
    const deadline = started + timeoutMs;
    while (Date.now() < deadline) {
      booted = await page.evaluate(() => window.__uiBooted === true).catch(() => false);
      if (booted) {
        break;
      }
      shellText = await page
        .evaluate(() => {
          const lead = document.querySelector("#bootbox .lead");
          return lead ? lead.textContent : document.body.innerText.slice(0, 200);
        })
        .catch(() => "");
      await page.waitForTimeout(500);
    }
  } catch (err) {
    consoleErrors.push("navigation: " + err.message);
  }

  const elapsed = ((Date.now() - started) / 1000).toFixed(1);
  const failed = requests.filter((r) => r.state === "FAILED");
  const non200 = requests.filter((r) => r.state !== "FAILED" && !r.state.startsWith("2"));

  if (booted) {
    console.log(`round ${round}: PASS in ${elapsed}s`);
  } else {
    hardFailures++;
    console.log(`round ${round}: FAIL after ${elapsed}s  shell="${shellText}"`);
    console.log(`  requests: ${requests.length} (failed ${failed.length}, non-200 ${non200.length})`);
    for (const r of requests.slice(-12)) {
      console.log(`    ${r.state} ${r.url} ${r.err ?? r.mime ?? ""}`);
    }
    if (consoleErrors.length) {
      console.log("  console errors:");
      for (const e of consoleErrors.slice(0, 8)) {
        console.log(`    ${e.slice(0, 200)}`);
      }
    }
  }
  await browser.close();
  results.push({ round, booted, elapsed, failed, non200 });
}

const passed = results.filter((r) => r.booted).length;
console.log(`\ncold-load: ${passed}/${results.length} rounds booted`);
process.exit(hardFailures === 0 ? 0 : 1);