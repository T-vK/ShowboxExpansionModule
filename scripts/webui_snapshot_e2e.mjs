#!/usr/bin/env node
// E2E: snapshots save and recall INPUT1_GAIN through the real webui.
// Pass/fail is mixer truth (entity API / live WS), not the strip dB label.
// Completion is the live-socket `type:snapshot` event, not a hope-sleep.
//
// usage: node scripts/webui_snapshot_e2e.mjs [--url http://192.168.3.55/]

import { chromium } from "playwright";

function arg(name, def) {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : def;
}

const url = arg("--url", "http://192.168.3.55/");
const SLOT = 3;

const browser = await chromium.launch();
const ctx = await browser.newContext({ viewport: { width: 420, height: 780 } });
const page = await ctx.newPage();
const errors = [];
const snapEvents = [];
const entities = {};
const entityGen = {};

page.on("pageerror", (e) => errors.push("PAGEERR: " + e.message.slice(0, 120)));
page.on("console", (m) => { if (m.type() === "error") errors.push(m.text().slice(0, 120)); });
page.on("websocket", (ws) => {
  ws.on("framereceived", (ev) => {
    let msg;
    try {
      msg = JSON.parse(String(ev.payload));
    } catch {
      return;
    }
    if (msg.type === "snapshot") {
      snapEvents.push(msg);
    } else if (msg.type === "entity" && msg.id) {
      entities[msg.id] = msg.value;
      entityGen[msg.id] = (entityGen[msg.id] || 0) + 1;
    }
  });
});

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

// Same boot gate as webui_cold_load.mjs. waitUntil:"load" waits for every
// icon/asset; BLE-coex can stall one of those for 30s and drop STA while
// the mixer already mounted (L3: cold-load PASS, snapshot goto timeout,
// ping dead). Retry commit — cold-load can leave SYN-ACK-no-body PCBs.
let booted = false;
for (let attempt = 0; attempt < 3; attempt++) {
  try {
    await page.goto(url, { waitUntil: "commit", timeout: 30000 });
  } catch (err) {
    console.log(`goto attempt ${attempt + 1}: ${err.message}`);
    await sleep(1500);
    continue;
  }
  booted = await page.waitForFunction(() => window.__uiBooted === true, { timeout: 45000 })
    .then(() => true).catch(() => false);
  if (booted) {
    break;
  }
  console.log(`boot attempt ${attempt + 1}: __uiBooted never set`);
  await sleep(1500);
}
if (!booted) {
  console.log("FAIL: webui did not boot");
  process.exit(1);
}
console.log("booted OK");

async function apiGain() {
  return page.evaluate(async () => {
    const t = await fetch("/api/v1/showbox/entities/INPUT1_GAIN").then((r) => r.text());
    return parseFloat(t);
  });
}

async function currentGain() {
  const api = await apiGain();
  if (Number.isFinite(api)) {
    return api;
  }
  const ws = Number(entities.INPUT1_GAIN);
  return Number.isFinite(ws) ? ws : NaN;
}

async function waitGain(want, timeoutMs) {
  const end = Date.now() + timeoutMs;
  let last = NaN;
  while (Date.now() < end) {
    last = await currentGain();
    if (Number.isFinite(last) && Math.abs(last - want) < 0.5) {
      return last;
    }
    await sleep(50);
  }
  return last;
}

async function waitSnapshot(action, slot, timeoutMs, afterIndex) {
  const want = String(action).toUpperCase();
  const end = Date.now() + timeoutMs;
  while (Date.now() < end) {
    for (let i = afterIndex; i < snapEvents.length; i++) {
      const msg = snapEvents[i];
      if (
        msg && msg.ok
        && String(msg.action || "").toUpperCase() === want
        && Number(msg.slot) === Number(slot)
      ) {
        return msg;
      }
    }
    await sleep(50);
  }
  return null;
}

async function snapshot(action, slot) {
  for (let attempt = 0; attempt < 3; attempt++) {
    const before = snapEvents.length;
    const http = await page.evaluate(async ({ action, slot }) => {
      const r = await fetch("/api/v1/showbox/action/snapshot", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ action, slot }),
      });
      return { ok: r.ok, status: r.status };
    }, { action, slot });
    const msg = await waitSnapshot(action, slot, 8000, before);
    if (msg) {
      return msg;
    }
    if (http && http.ok) {
      // Firmware accepted SAVE/RECALL; WS snapshot can miss a 1005.
      await sleep(400);
      const late = await waitSnapshot(action, slot, 2000, before);
      if (late) {
        return late;
      }
      if (attempt === 2) {
        return { ok: true, action, slot, httpOnly: true };
      }
    }
  }
  throw new Error(`no WS snapshot ${action} slot ${slot} (had ${snapEvents.length} events)`);
}

// Cold-load SPA can leave :80 wedged for a beat (BLE coex). Retry the
// entity GET before declaring the mixer dump dead.
let g0 = NaN;
for (let i = 0; i < 8; i++) {
  g0 = await apiGain();
  if (Number.isFinite(g0)) {
    break;
  }
  await sleep(250);
}
if (!Number.isFinite(g0)) {
  console.log("FAIL: INPUT1_GAIN unreadable");
  process.exit(1);
}
// Mixer faders quantize (POST 10 reads back as 8). Pick a far step.
const changed = g0 < 16 ? 28 : 4;
console.log("initial gain:", g0);

const dumpGen = entityGen.INPUT1_GAIN || 0;
try {
  await snapshot("SAVE", SLOT);
} catch (err) {
  console.log("FAIL:", err.message);
  process.exit(1);
}
console.log("saved state to slot", SLOT);
const dumpUntil = Date.now() + 2500;
while (Date.now() < dumpUntil && (entityGen.INPUT1_GAIN || 0) <= dumpGen) {
  await sleep(50);
}
const settled = await waitGain(g0, 4000);
if (!(Number.isFinite(settled) && Math.abs(settled - g0) < 0.5)) {
  console.log("FAIL: gain drifted after save", settled, "saved", g0);
  process.exit(1);
}

await page.evaluate(async (v) => {
  await fetch("/api/v1/showbox/entities/INPUT1_GAIN", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ value: v }),
  });
}, changed);
const g1 = await waitGain(changed, 4000);
console.log("changed gain to:", g1, "(posted", changed + ")");
if (!(Number.isFinite(g1) && Math.abs(g1 - changed) < 0.5)) {
  console.log("FAIL: INPUT1_GAIN did not move to", changed, "got", g1);
  process.exit(1);
}

try {
  await snapshot("RECALL", SLOT);
} catch (err) {
  console.log("FAIL:", err.message);
  process.exit(1);
}
console.log("recalled slot", SLOT);
const g2 = await waitGain(g0, 8000);
console.log(`gain after recall: ${g2} (saved: ${g0})`);
const pass = Number.isFinite(g2) && Math.abs(g2 - g0) < 0.5;
console.log(`E2E: ${pass ? "PASS" : "FAIL"}`);
if (errors.length) console.log("errors:", errors.slice(0, 5));

await browser.close();
process.exit(pass ? 0 : 1);
