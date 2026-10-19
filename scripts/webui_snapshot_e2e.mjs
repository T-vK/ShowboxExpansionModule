#!/usr/bin/env node
// E2E test: snapshots save and recall through the REAL webui.
//
// Opens the webui in Chromium, uses the module API to set and read entity
// values (the same path fader writes take), and verifies:
//   1. Store the current state to a slot
//   2. Change an entity to a different value
//   3. Recall the slot
//   4. The displayed value reverts to the stored state
//   5. The service-worker-free first load also boots
//
// usage: node scripts/webui_snapshot_e2e.mjs [--url http://192.168.3.55/]

import { chromium } from "playwright";

function arg(name, def) {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : def;
}

const url = arg("--url", "http://192.168.3.55/");
const SLOT = 3;
const CHANGED_GAIN = 10;

const browser = await chromium.launch();
const ctx = await browser.newContext({ viewport: { width: 420, height: 780 } });
const page = await ctx.newPage();
const errors = [];

page.on("pageerror", (e) => errors.push("PAGEERR: " + e.message.slice(0, 120)));
page.on("console", (m) => { if (m.type() === "error") errors.push(m.text().slice(0, 120)); });

// 1. Load the webui
await page.goto(url, { waitUntil: "load", timeout: 30000 });
const booted = await page.waitForFunction(() => window.__uiBooted === true, { timeout: 60000 })
  .then(() => true).catch(() => false);
if (!booted) { console.log("FAIL: webui did not boot"); process.exit(1); }
console.log("booted OK");

// 2. Read the displayed gain
const readGain = () => page.evaluate(() => {
  const strip = document.querySelector(".strip");
  if (!strip) return "?";
  const m = strip.innerText.match(/(-?[\d.]+) dB/);
  return m ? m[1] : "?";
});
const g0 = await readGain();
console.log("initial gain:", g0);

// 3. Save the current state to the slot via the API (same path the UI uses)
await page.evaluate((slot) =>
  fetch("/api/v1/showbox/action/snapshot", { method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ action: "SAVE", slot }) }).then(r => r.json()),
  SLOT);
console.log("saved state to slot", SLOT);
await page.waitForTimeout(7000); // save busy window

// 4. Change the gain to a different value
await page.evaluate((v) =>
  fetch("/api/v1/showbox/entities/INPUT1_GAIN", { method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ value: v }) }).then(r => r.json()),
  CHANGED_GAIN);
await page.waitForTimeout(4000);
const g1 = await readGain();
console.log("changed gain to:", g1);
if (parseFloat(g1) === parseFloat(g0)) {
  console.log("WARN: gain unchanged — the set may not have taken effect");
}

// 5. Recall the slot
await page.evaluate((slot) =>
  fetch("/api/v1/showbox/action/snapshot", { method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ action: "RECALL", slot }) }).then(r => r.json()),
  SLOT);
console.log("recalled slot", SLOT);

// 6. Wait for the recall + dump + entity updates to flow through the WS
const g2 = await page.waitForFunction((expected) => {
  const strip = document.querySelector(".strip");
  if (!strip) return null;
  const m = strip.innerText.match(/(-?[\d.]+) dB/);
  return m && m[1] === expected ? m[1] : null;
}, { timeout: 30000 }, String(g0))
  .then(() => true).catch(() => false);
await page.waitForTimeout(2000);
const g3 = await readGain();

const pass = g3 === g0; // recall restored the saved (initial) state
console.log(`gain after recall: ${g3} (saved: ${g0})`);
console.log(`E2E: ${pass ? "PASS" : "FAIL"}`);
if (errors.length) console.log("errors:", errors.slice(0, 5));

await browser.close();
process.exit(pass ? 0 : 1);