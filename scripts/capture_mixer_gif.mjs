#!/usr/bin/env node
/**
 * Prepare the live mixer (mute MAIN, recall snapshot 4, seed EQ/FX/comp,
 * rec→stop a short loop), then capture mixer HUD views into a GIF.
 *
 * usage:
 *   node scripts/capture_mixer_gif.mjs
 *   node scripts/capture_mixer_gif.mjs --url http://192.168.3.55/ --ui http://127.0.0.1:5173/
 *
 * --url  device API / showbox host (default http://showbox.local/)
 * --ui   optional browser origin for screenshots (fixed local UI proxied to
 *        the device). Defaults to --url.
 *
 * Requires Playwright (repo node_modules) and ImageMagick `magick`/`convert` or ffmpeg.
 */

import { chromium } from "playwright";
import { spawnSync } from "child_process";
import fs from "fs";
import os from "os";
import path from "path";
import { fileURLToPath } from "url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, "..");

function arg(name, def) {
  const i = process.argv.indexOf(name);
  return i > 0 ? process.argv[i + 1] : def;
}

const apiBase = (arg("--url", process.env.SHOWBOX_URL || "http://showbox.local/")).replace(/\/?$/, "/");
const uiBase = (arg("--ui", apiBase)).replace(/\/?$/, "/");
const outGif = path.resolve(ROOT, arg("--out", "docs/webui-mixer.gif"));
const holdMs = Number(arg("--hold-ms", "3000")) || 3000;
const SNAPSHOT_4_SLOT = 3; // SNAPSHOT_4 / UI label "4"

// SM-P615 landscape 2000x1200 @240dpi, ~25% denser CSS viewport.
const SCALE = 1.25;
const BASE_W = Math.round(2000 / 1.5);
const BASE_H = Math.round(1200 / 1.5);
const CSS_W = Math.round(BASE_W / SCALE);
const CSS_H = Math.round(BASE_H / SCALE);
const DPR = 2000 / CSS_W;

/** @type {{ id: string, label: string, kind: "mixer"|"board"|"snap-load", holdMs?: number, before?: string }[]} */
const VIEWS = [
  { id: "full", label: "Full mixer", kind: "mixer" },
  { id: "faders", label: "Faders", kind: "mixer" },
  { id: "inputs", label: "Inputs", kind: "mixer" },
  { id: "fx", label: "FX sends", kind: "board", before: "fx" },
  { id: "eq", label: "EQ", kind: "board" },
  { id: "comp", label: "Compressor", kind: "board" },
  { id: "snap-load", label: "Snapshots Load", kind: "snap-load" },
  { id: "tuner", label: "Tuner", kind: "board", before: "tuner" },
  { id: "loop", label: "Looper", kind: "board" },
];

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function api(pathname, payload) {
  const url = new URL(pathname, apiBase).href;
  const res = await fetch(url, {
    method: payload === undefined ? "GET" : "POST",
    headers: payload === undefined ? undefined : { "Content-Type": "application/json" },
    body: payload === undefined ? undefined : JSON.stringify(payload),
  });
  const text = await res.text();
  if (!res.ok) {
    throw new Error(`${pathname} -> HTTP ${res.status}: ${text.slice(0, 200)}`);
  }
  try {
    return text ? JSON.parse(text) : {};
  } catch {
    return { _raw: text };
  }
}

async function setEntity(id, value) {
  let last;
  for (let attempt = 0; attempt < 6; attempt++) {
    try {
      await api(`/api/v1/showbox/entities/${id}`, { value });
      return;
    } catch (err) {
      last = err;
      await sleep(300 * (attempt + 1));
    }
  }
  throw last || new Error(`setEntity ${id} failed`);
}

async function readMute() {
  const text = await fetch(new URL("/api/v1/showbox/entities/MAIN_MUTE", apiBase).href).then((r) => r.text());
  const t = text.trim().toLowerCase();
  return t === "true" || t === "1";
}

async function muteMain(tag) {
  await setEntity("MAIN_MUTE", true);
  const end = Date.now() + 8000;
  while (Date.now() < end) {
    if (await readMute()) {
      console.log(`MAIN muted (${tag})`);
      return;
    }
    await sleep(200);
  }
  throw new Error(`MAIN_MUTE did not stick after ${tag}`);
}

async function recallSnapshot4() {
  console.log(`recalling snapshot 4 (slot ${SNAPSHOT_4_SLOT})`);
  await api("/api/v1/showbox/action/snapshot", { action: "RECALL", slot: SNAPSHOT_4_SLOT });
  await sleep(5000);
  // Snapshot dumps can wedge large HTTP briefly; wait for API again.
  await waitHttpReady(90000);
}

async function looper(action) {
  await api("/api/v1/showbox/action/looper_button", { action });
}

/** Map 0..1 fader percent to EQ dB in [-15, 15]. */
function eqDbFromPct(pct) {
  return -15 + Math.max(0, Math.min(1, pct)) * 30;
}

async function seedInterestingMix() {
  console.log("seeding EQ / compressor / FX / Ext FX");
  // Per-channel EQ: all three bands in either 0–50% or 50–100% (never mixed).
  const eqPlans = [
    { half: "low", low: 0.35, mid: 0.28, high: 0.42 }, // cut-ish
    { half: "high", low: 0.58, mid: 0.72, high: 0.64 }, // boost-ish
    { half: "low", low: 0.22, mid: 0.40, high: 0.18 },
    { half: "high", low: 0.55, mid: 0.62, high: 0.78 },
  ];
  const fx1Types = [1, 7, 3, 14]; // chorus, overdrive, delay, tremolo
  const fx2Types = [1, 5, 2, 7]; // plate / hall / plate+ / spring (shared pairs)
  // EFFECT21 shared by ch1+ch3, EFFECT22 by ch2+ch4 — set both shared IDs.
  await setEntity("EFFECT21_TYPEID", fx2Types[0]);
  await setEntity("EFFECT22_TYPEID", fx2Types[1]);

  for (let n = 1; n <= 4; n++) {
    const p = `INPUT${n}_`;
    const plan = eqPlans[n - 1];
    await setEntity(`${p}EQ_ENABLE`, true);
    await setEntity(`${p}EQ_LOW_GAIN`, eqDbFromPct(plan.low));
    await setEntity(`${p}EQ_MID_GAIN`, eqDbFromPct(plan.mid));
    await setEntity(`${p}EQ_HIGH_GAIN`, eqDbFromPct(plan.high));
    await setEntity(`${p}COMPRESSOR_ENABLE`, true);
    await setEntity(`${p}COMPRESSOR_AMOUNT`, 0.25 + n * 0.12);
    await setEntity(`EFFECT1${n}_TYPEID`, fx1Types[n - 1]);
    await setEntity(`${p}EFFECT_1_MUTE`, false);
    await setEntity(`${p}EFFECT_1_AMOUNT`, 0.35 + n * 0.08);
    await setEntity(`${p}EFFECT_2_MUTE`, false);
    await setEntity(`${p}EFFECT_2_AMOUNT`, 0.28 + n * 0.07);
    await setEntity(`${p}EXT_FX_MUTE`, false);
    await setEntity(`${p}EXT_FX_SENDS`, 0.2 + n * 0.05);
  }
  await setEntity("STEREO_INPUT1_EQ_ENABLE", true);
  await setEntity("STEREO_INPUT1_EQ_LOW_GAIN", eqDbFromPct(0.3));
  await setEntity("STEREO_INPUT1_EQ_MID_GAIN", eqDbFromPct(0.45));
  await setEntity("STEREO_INPUT1_EQ_HIGH_GAIN", eqDbFromPct(0.2));
}

async function seedLooper() {
  console.log("looper: rec → stop");
  // Clear any leftover loop first.
  await looper("LONG_PRESS");
  await sleep(800);
  await looper("TAP"); // start recording
  await sleep(2200);
  await looper("DOUBLE_PRESS"); // stop
  await sleep(800);
}

function chromePath() {
  const env = process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH;
  if (env && fs.existsSync(env)) return env;
  const home = os.homedir();
  const candidates = [
    path.join(home, ".cache/ms-playwright/chromium-1248/chrome-linux64/chrome"),
    path.join(home, ".cache/ms-playwright/chromium-1161/chrome-linux/chrome"),
  ];
  for (const p of candidates) {
    if (fs.existsSync(p)) return p;
  }
  return undefined;
}

function buildGif(frames, dest) {
  fs.mkdirSync(path.dirname(dest), { recursive: true });
  const delayCs = Math.max(1, Math.round(holdMs / 10));
  const magick =
    spawnSync("magick", ["-version"], { stdio: "ignore" }).status === 0
      ? "magick"
      : spawnSync("convert", ["-version"], { stdio: "ignore" }).status === 0
        ? "convert"
        : null;
  if (magick) {
    const r = spawnSync(magick, ["-delay", String(delayCs), "-loop", "0", ...frames, dest], { stdio: "inherit" });
    if (r.status !== 0) throw new Error(magick + " failed");
    return magick;
  }
  if (spawnSync("ffmpeg", ["-version"], { stdio: "ignore" }).status === 0) {
    const list = path.join(path.dirname(frames[0]), "frames.txt");
    fs.writeFileSync(
      list,
      frames.map((f) => `file '${f}'\nduration ${holdMs / 1000}`).join("\n") + `\nfile '${frames[frames.length - 1]}'\n`
    );
    const r = spawnSync(
      "ffmpeg",
      ["-y", "-f", "concat", "-safe", "0", "-i", list, "-vf", "scale=2000:-1:flags=lanczos", "-loop", "0", dest],
      { stdio: "inherit" }
    );
    if (r.status !== 0) throw new Error("ffmpeg failed");
    return "ffmpeg";
  }
  throw new Error("Need ImageMagick `magick`/`convert` or ffmpeg to build the GIF");
}

async function waitHttpReady(timeoutMs = 120000) {
  const linkUrl = new URL("/api/v1/showbox/link", apiBase).href;
  const spaUrl = new URL("/", apiBase).href;
  const end = Date.now() + timeoutMs;
  let delay = 1000;
  while (Date.now() < end) {
    try {
      const link = await fetch(linkUrl, { signal: AbortSignal.timeout(4000) });
      if (!link.ok) throw new Error(`link ${link.status}`);
      const spa = await fetch(spaUrl, {
        headers: { Range: "bytes=0-2047", Connection: "close" },
        signal: AbortSignal.timeout(12000),
      });
      if (spa.status === 200 || spa.status === 206) {
        console.log("HTTP ready");
        return;
      }
      throw new Error(`spa ${spa.status}`);
    } catch (err) {
      console.log("wait HTTP:", err.message || err);
      await sleep(delay);
      delay = Math.min(5000, delay + 500);
    }
  }
  throw new Error("HTTP/SPA did not become ready in time");
}

async function openViewMenu(page) {
  const btn = page.locator("button.hud-view").first();
  await btn.click();
  await page.waitForSelector(".picker-item", { timeout: 5000 });
}

async function chooseCatalog(page, name) {
  await openViewMenu(page);
  await page.locator(".picker-item", { hasText: new RegExp(`^${name.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}$`) }).first().click();
  await sleep(500);
}

async function showSnapLoad(page) {
  const label = (await page.locator("button.hud-view").first().innerText()).trim();
  if (!/^Snapshots/i.test(label)) {
    await chooseCatalog(page, "Snapshots");
  }
  const back = page.locator("button.tool-off, button.ghost.tool-off").filter({ hasText: /^Back$/i });
  if (await back.count()) {
    await back.first().click();
    await sleep(400);
  }
  await page.locator(".snap-pick-btn").filter({ hasText: /Load/i }).first().click();
  await page.waitForSelector(".snap-bank, .snap-slot", { timeout: 8000 });
  await sleep(400);
}

async function prepareFxBoard(page) {
  // Ensure Ext FX visibility toggle is on (UI also auto-enables it).
  const ext = page.locator(".fx-sends-bar .pad").filter({ hasText: /^Ext FX$/i });
  if (await ext.count()) {
    const cls = (await ext.first().getAttribute("class")) || "";
    if (!/\bon\b/.test(cls)) await ext.first().click();
  }
  await sleep(300);
}

async function prepareTuner(page) {
  // Second channel = index 1.
  const btn = page.locator(".tuner-chan").nth(1);
  await btn.click();
  await sleep(2000);
}

async function main() {
  console.log("api", apiBase);
  console.log("ui ", uiBase);
  console.log("out", outGif);
  console.log("viewport", `${CSS_W}x${CSS_H}`, `@${DPR.toFixed(3)}dpr`);

  await waitHttpReady();

  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "sxm-mixer-gif-"));
  const frames = [];

  const browser = await chromium.launch({
    executablePath: chromePath(),
    headless: true,
  });
  const context = await browser.newContext({
    viewport: { width: CSS_W, height: CSS_H },
    deviceScaleFactor: DPR,
    isMobile: true,
    hasTouch: true,
    userAgent:
      "Mozilla/5.0 (Linux; Android 13; SM-P615) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
  });
  const page = await context.newPage();
  await page.addInitScript(() => {
    try {
      localStorage.setItem("sxm-fs-skip", "1");
      localStorage.setItem("sxm-mixer-view", "full");
      localStorage.setItem("sxm-fx-show-none", "1");
      localStorage.setItem("sxm-tuner-chan", "1");
    } catch {
      /* ignore */
    }
  });

  let booted = false;
  for (let attempt = 0; attempt < 5; attempt++) {
    try {
      await page.goto(uiBase, { waitUntil: "commit", timeout: 60000 });
    } catch (err) {
      console.log(`goto attempt ${attempt + 1}: ${err.message}`);
      await sleep(2000);
      continue;
    }
    booted = await page
      .waitForFunction(() => window.__uiBooted === true || !!document.querySelector(".shell"), { timeout: 60000 })
      .then(() => true)
      .catch(() => false);
    if (booted) break;
    await sleep(2000);
  }
  if (!booted) throw new Error("webui did not boot");
  await page.waitForSelector(".shell, .mixer-hud", { timeout: 30000 });
  for (const t of ["Maybe later", "Don't ask again"]) {
    const el = page.getByText(t, { exact: true });
    if (await el.count()) await el.first().click().catch(() => {});
  }
  await sleep(800);

  await muteMain("pre-snapshot");
  await recallSnapshot4();
  await muteMain("post-snapshot");
  await seedInterestingMix();
  await seedLooper();
  await muteMain("pre-capture");
  await sleep(1000);

  for (let i = 0; i < VIEWS.length; i++) {
    const view = VIEWS[i];
    console.log(`[${i + 1}/${VIEWS.length}] ${view.label}`);
    if (view.kind === "snap-load") {
      await showSnapLoad(page);
    } else {
      await chooseCatalog(page, view.label);
    }
    if (view.before === "fx") await prepareFxBoard(page);
    if (view.before === "tuner") await prepareTuner(page);

    const until = Date.now() + holdMs;
    while (Date.now() < until) {
      if (!(await readMute())) {
        console.log("MAIN unmuted mid-capture — remuting");
        await setEntity("MAIN_MUTE", true);
      }
      await sleep(200);
    }
    const frame = path.join(tmp, `frame_${String(i).padStart(2, "0")}.png`);
    await page.screenshot({
      path: frame,
      fullPage: false,
      animations: "disabled",
      caret: "hide",
      timeout: 90000,
    });
    frames.push(frame);
  }

  await browser.close();
  await muteMain("post-capture");

  const tool = buildGif(frames, outGif);
  const st = fs.statSync(outGif);
  console.log(`GIF via ${tool}: ${outGif} (${st.size} bytes, ${frames.length} frames @ ${holdMs}ms)`);
  fs.rmSync(tmp, { recursive: true, force: true });
}

main().catch((err) => {
  console.error("FAIL:", err.message || err);
  process.exit(1);
});
