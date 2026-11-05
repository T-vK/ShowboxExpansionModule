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
const origin = new URL(url).origin;
const SLOT = 3;

const browser = await chromium.launch();
const ctx = await browser.newContext({ viewport: { width: 420, height: 780 } });
const page = await ctx.newPage();
const errors = [];
const snapEvents = [];
const entities = {};
const entityGen = {};
let dumpSeq = 0;

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
    } else if (msg.type === "link" && msg.dumpSeq != null) {
      dumpSeq = Number(msg.dumpSeq) || dumpSeq;
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
  // Node fetch — Chromium's TCP to :80 dies under SPA largest≈7668 while
  // direct HTTP still works (verify agent: NaN / browser closed in waitGain).
  try {
    const r = await fetch(`${origin}/api/v1/showbox/entities/INPUT1_GAIN`, {
      signal: AbortSignal.timeout(4000),
    });
    if (!r.ok) {
      return NaN;
    }
    return parseFloat(await r.text());
  } catch {
    return NaN;
  }
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

async function waitDumpPast(before, timeoutMs) {
  const end = Date.now() + timeoutMs;
  while (Date.now() < end) {
    if (dumpSeq > before) {
      return dumpSeq;
    }
    await sleep(50);
  }
  return dumpSeq;
}

async function postGain(v) {
  // Under SPA heap pressure a body-less 200 / lost POST left the mixer at
  // 40 while the e2e thought it wrote 4 (L2 streak5 pass9). Require ok and
  // a matching read before continuing. POST via Node, not page.evaluate.
  for (let attempt = 0; attempt < 4; attempt++) {
    let http = { ok: false, status: 0 };
    try {
      const r = await fetch(`${origin}/api/v1/showbox/entities/INPUT1_GAIN`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ value: v }),
        signal: AbortSignal.timeout(5000),
      });
      http = { ok: r.ok, status: r.status };
    } catch (err) {
      console.log(`postGain attempt ${attempt + 1}: ${err.message || err}`);
      await sleep(300);
      continue;
    }
    if (!http.ok) {
      console.log(`postGain attempt ${attempt + 1}: http ${http.status}`);
      await sleep(300);
      continue;
    }
    const got = await waitGain(v, 8000);
    if (Number.isFinite(got) && Math.abs(got - v) < 0.5) {
      return got;
    }
    console.log(`postGain attempt ${attempt + 1}: want ${v} got ${got}`);
    await sleep(400);
  }
  return NaN;
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

async function snapshot(action, slot, opts = {}) {
  // RECALL must not accept bare HTTP 200 — L3 streak1 stopped when httpOnly
  // RECALL left gain at 4 while saved was 40 (WS snapshot never arrived).
  const requireEvidence = opts.requireEvidence === true
    || String(action).toUpperCase() === "RECALL";
  for (let attempt = 0; attempt < 3; attempt++) {
    const before = snapEvents.length;
    const dumpBeforeSnap = dumpSeq;
    let http = { ok: false, status: 0 };
    try {
      const r = await fetch(`${origin}/api/v1/showbox/action/snapshot`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ action, slot }),
        signal: AbortSignal.timeout(8000),
      });
      http = { ok: r.ok, status: r.status };
    } catch (err) {
      console.log(`snapshot ${action} attempt ${attempt + 1}: ${err.message || err}`);
    }
    const msg = await waitSnapshot(action, slot, 8000, before);
    if (msg) {
      console.log(`snapshot ${action}: WS ok slot=${slot}`);
      return msg;
    }
    if (http && http.ok) {
      // Firmware accepted; WS can miss a 1005. For RECALL wait dumpSeq too.
      await sleep(400);
      const late = await waitSnapshot(action, slot, 2000, before);
      if (late) {
        console.log(`snapshot ${action}: late WS ok slot=${slot}`);
        return late;
      }
      const dumpAfterSnap = await waitDumpPast(dumpBeforeSnap, requireEvidence ? 6000 : 1500);
      if (dumpAfterSnap > dumpBeforeSnap) {
        console.log(`snapshot ${action}: dumpSeq ${dumpBeforeSnap}->${dumpAfterSnap}`);
        return { ok: true, action, slot, via: "dumpSeq" };
      }
      if (!requireEvidence && attempt === 2) {
        console.log(`snapshot ${action}: httpOnly fallback slot=${slot}`);
        return { ok: true, action, slot, httpOnly: true };
      }
      console.log(
        `snapshot ${action} attempt ${attempt + 1}: http ${http.status} but no WS/dumpSeq`,
      );
    }
  }
  throw new Error(`no WS/dumpSeq snapshot ${action} slot ${slot} (had ${snapEvents.length} events)`);
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
const dumpBefore = dumpSeq;
try {
  await snapshot("SAVE", SLOT);
} catch (err) {
  console.log("FAIL:", err.message);
  process.exit(1);
}
console.log("saved state to slot", SLOT);
// SAVE clears mixerTouched and re-dumps. Wait for that dump (or entity
// refresh) before writing, or the dump clobbers a concurrent POST.
const dumpAfter = await waitDumpPast(dumpBefore, 6000);
console.log(`dumpSeq after SAVE: ${dumpBefore} -> ${dumpAfter}`);
const dumpUntil = Date.now() + 2500;
while (Date.now() < dumpUntil && (entityGen.INPUT1_GAIN || 0) <= dumpGen) {
  await sleep(50);
}
const settled = await waitGain(g0, 6000);
if (!(Number.isFinite(settled) && Math.abs(settled - g0) < 0.5)) {
  console.log("FAIL: gain drifted after save", settled, "saved", g0);
  process.exit(1);
}

const g1 = await postGain(changed);
console.log("changed gain to:", g1, "(posted", changed + ")");
if (!(Number.isFinite(g1) && Math.abs(g1 - changed) < 0.5)) {
  console.log("FAIL: INPUT1_GAIN did not move to", changed, "got", g1);
  process.exit(1);
}

let g2 = NaN;
for (let recallTry = 0; recallTry < 3; recallTry++) {
  try {
    await snapshot("RECALL", SLOT, { requireEvidence: true });
  } catch (err) {
    console.log(`RECALL attempt ${recallTry + 1}:`, err.message);
    if (recallTry === 2) {
      console.log("FAIL:", err.message);
      process.exit(1);
    }
    await sleep(500);
    continue;
  }
  console.log("recalled slot", SLOT, `(try ${recallTry + 1})`);
  g2 = await waitGain(g0, 8000);
  console.log(`gain after recall: ${g2} (saved: ${g0})`);
  if (Number.isFinite(g2) && Math.abs(g2 - g0) < 0.5) {
    break;
  }
  console.log(`RECALL try ${recallTry + 1}: gain still ${g2}, retrying`);
  await sleep(600);
}
const pass = Number.isFinite(g2) && Math.abs(g2 - g0) < 0.5;
console.log(`E2E: ${pass ? "PASS" : "FAIL"}`);
if (errors.length) console.log("errors:", errors.slice(0, 5));

await browser.close();
process.exit(pass ? 0 : 1);
