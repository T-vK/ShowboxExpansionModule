#!/usr/bin/env node
/**
 * Mute MAIN, recall snapshot 4, then capture every mixer HUD view into a GIF.
 *
 * usage:
 *   node scripts/capture_mixer_gif.mjs
 *   node scripts/capture_mixer_gif.mjs --url http://192.168.3.55/ --out docs/webui-mixer.gif
 *
 * Requires Playwright (repo node_modules) and ImageMagick `convert` or ffmpeg.
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

const baseUrl = (arg("--url", process.env.SHOWBOX_URL || "http://showbox.local/")).replace(/\/?$/, "/");
const outGif = path.resolve(ROOT, arg("--out", "docs/webui-mixer.gif"));
const holdMs = Number(arg("--hold-ms", "3000")) || 3000;
const SNAPSHOT_4_SLOT = 3; // SNAPSHOT_4 in constants.h / UI slot labeled "4"

// SM-P615 landscape 2000x1200 @240dpi, ~25% denser CSS viewport (matches README shots).
const SCALE = 1.25;
const BASE_W = Math.round(2000 / 1.5);
const BASE_H = Math.round(1200 / 1.5);
const CSS_W = Math.round(BASE_W / SCALE);
const CSS_H = Math.round(BASE_H / SCALE);
const DPR = 2000 / CSS_W;

/** @type {{ id: string, label: string, kind: "mixer"|"board"|"snap-load" }[]} */
const VIEWS = [
  { id: "full", label: "Full mixer", kind: "mixer" },
  { id: "live", label: "Live", kind: "mixer" },
  { id: "faders", label: "Faders", kind: "mixer" },
  { id: "inputs", label: "Inputs", kind: "mixer" },
  { id: "gain", label: "Gain", kind: "board" },
  { id: "fx", label: "FX sends", kind: "board" },
  { id: "eq", label: "EQ", kind: "board" },
  { id: "comp", label: "Compressor", kind: "board" },
  { id: "snap", label: "Snapshots", kind: "board" },
  { id: "snap-load", label: "Snapshots Load", kind: "snap-load" },
  { id: "tuner", label: "Tuner", kind: "board" },
  { id: "loop", label: "Looper", kind: "board" },
];

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function api(pathname, payload) {
  const url = new URL(pathname, baseUrl).href;
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

async function readMute() {
  const text = await fetch(new URL("/api/v1/showbox/entities/MAIN_MUTE", baseUrl).href).then((r) => r.text());
  const t = text.trim().toLowerCase();
  return t === "true" || t === "1";
}

async function muteMain(tag) {
  await api("/api/v1/showbox/entities/MAIN_MUTE", { value: true });
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
  // Mixer snapshot ops take a few seconds; wait then remute.
  await sleep(5000);
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
  const delayCs = Math.max(1, Math.round(holdMs / 10)); // ImageMagick: 1/100s
  const magick = spawnSync("magick", ["-version"], { stdio: "ignore" }).status === 0
    ? "magick"
    : spawnSync("convert", ["-version"], { stdio: "ignore" }).status === 0
      ? "convert"
      : null;
  if (magick) {
    const args = magick === "magick"
      ? ["-delay", String(delayCs), "-loop", "0", ...frames, dest]
      : ["-delay", String(delayCs), "-loop", "0", ...frames, dest];
    const r = spawnSync(magick, args, { stdio: "inherit" });
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
  throw new Error("Need ImageMagick `convert` or ffmpeg to build the GIF");
}

async function openViewMenu(page) {
  const btn = page.locator("button.hud-view").first();
  await btn.click();
  await page.waitForSelector(".picker-item", { timeout: 5000 });
}

async function chooseCatalog(page, name) {
  await openViewMenu(page);
  await page.locator(".picker-item", { hasText: new RegExp(`^${name.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}$`) }).first().click();
  await sleep(400);
}

async function showSnapLoad(page) {
  // Ensure we are on Snapshots pick first.
  const label = (await page.locator("button.hud-view").first().innerText()).trim();
  if (!/^Snapshots$/i.test(label) && !/^Snapshots Load$/i.test(label)) {
    await chooseCatalog(page, "Snapshots");
  }
  // If already on slots, go back to pick then Load.
  const back = page.locator("button.tool-off, button.ghost.tool-off").filter({ hasText: /^Back$/i });
  if (await back.count()) {
    await back.first().click();
    await sleep(400);
  }
  await page.locator(".snap-pick-btn").filter({ hasText: /Load/i }).first().click();
  await page.waitForSelector(".snap-bank, .snap-slot", { timeout: 8000 });
  await sleep(400);
}

async function waitHttpReady(timeoutMs = 120000) {
  const linkUrl = new URL("/api/v1/showbox/link", baseUrl).href;
  const spaUrl = new URL("/", baseUrl).href;
  const end = Date.now() + timeoutMs;
  let delay = 1000;
  while (Date.now() < end) {
    try {
      const link = await fetch(linkUrl, { signal: AbortSignal.timeout(4000) });
      if (!link.ok) throw new Error(`link ${link.status}`);
      // Prefer a small Range probe — full SPA can still be slow right after boot.
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

async function main() {
  console.log("base", baseUrl);
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
    } catch {
      /* ignore */
    }
  });

  // Load the SPA first (large body). Mute/snapshot after the UI is up so we
  // do not wedge :80 with a snapshot dump before the page can transfer.
  let booted = false;
  for (let attempt = 0; attempt < 5; attempt++) {
    try {
      await page.goto(baseUrl, { waitUntil: "commit", timeout: 60000 });
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
  if (!booted) {
    throw new Error("webui did not boot");
  }
  await page.waitForSelector(".shell, .mixer-hud", { timeout: 30000 });
  for (const t of ["Maybe later", "Don't ask again"]) {
    const el = page.getByText(t, { exact: true });
    if (await el.count()) await el.first().click().catch(() => {});
  }
  await sleep(800);

  await muteMain("pre-snapshot");
  await recallSnapshot4();
  await muteMain("post-snapshot");
  // Give the live WS a moment to paint recalled entities.
  await sleep(1500);
  await muteMain("pre-capture");

  for (let i = 0; i < VIEWS.length; i++) {
    const view = VIEWS[i];
    console.log(`[${i + 1}/${VIEWS.length}] ${view.label}`);
    if (view.kind === "snap-load") {
      await showSnapLoad(page);
    } else {
      await chooseCatalog(page, view.label);
    }
    // Hold the view for the requested dwell, remuting if needed.
    const until = Date.now() + holdMs;
    while (Date.now() < until) {
      if (!(await readMute())) {
        console.log("MAIN unmuted mid-capture — remuting");
        await api("/api/v1/showbox/entities/MAIN_MUTE", { value: true });
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
