import { defineConfig } from "vite";
import preact from "@preact/preset-vite";
import { viteSingleFile } from "vite-plugin-singlefile";

function mockShowbox() {
  const store = {};
  function set(id, value) {
    store[id] = { id, value };
  }
  for (let n = 1; n <= 4; n++) {
    set(`INPUT${n}_VOLUME`, n === 3 ? -12 : -80 + n * 8);
    set(`INPUT${n}_GAIN`, 8 + n * 6);
    set(`INPUT${n}_MUTE`, false);
    set(`INPUT${n}_EQ_ENABLE`, n !== 2);
    set(`INPUT${n}_EQ_LOW_GAIN`, n === 1 ? 4 : -2);
    set(`INPUT${n}_EQ_MID_GAIN`, n === 3 ? 3 : -1);
    set(`INPUT${n}_EQ_HIGH_GAIN`, n === 4 ? 6 : 2);
    set(`INPUT${n}_COMPRESSOR_ENABLE`, n === 1);
    set(`INPUT${n}_COMPRESSOR_AMOUNT`, n === 1 ? 0.51 : 0.2);
    set(`INPUT${n}_EFFECT_1_MUTE`, n === 2);
    set(`INPUT${n}_EFFECT_1_AMOUNT`, n === 2 ? 0 : 0.42);
    set(`INPUT${n}_EFFECT_2_MUTE`, false);
    set(`INPUT${n}_EFFECT_2_AMOUNT`, 0.28);
    set(`INPUT${n}_EXT_FX_MUTE`, true);
    set(`INPUT${n}_EXT_FX_SENDS`, 0.12);
    set(`INPUT${n}_CLIP_OL_PRE`, false);
    set(`INPUT${n}_CLIP_OL_POST`, false);
  }
  set("EFFECT11_TYPEID", 1);
  set("EFFECT12_TYPEID", 0);
  set("EFFECT13_TYPEID", 7);
  set("EFFECT14_TYPEID", 3);
  set("EFFECT21_TYPEID", 2);
  set("EFFECT22_TYPEID", 5);
  set("MAIN_MASTER_GAIN", -6);
  set("MAIN_MUTE", false);
  set("MAIN_CLIP_OL", false);
  set("MAIN_HEADPHONE_GAIN", 0);
  set("STEREO_INPUT1_VOLUME", -18);
  set("STEREO_INPUT1_MUTE", false);
  set("STEREO_INPUT1_EQ_ENABLE", true);
  set("STEREO_INPUT1_EQ_LOW_GAIN", -3);
  set("STEREO_INPUT1_EQ_MID_GAIN", 1);
  set("STEREO_INPUT1_EQ_HIGH_GAIN", 4);
  set("LOOPER_LEVEL", 1);
  set("LOOPER_STATE", 2);
  set("AMP_PA_MODE", false);
  set("LOCATION_MODE", false);
  set("FRONT_LED", true);
  set("FEEDBACK_ELIM", 0);
  set("FX_BYPASS", false);
  set("SELECTED_CHAN", 0);

  const analogHold = { phones: true, loop: true };

  return {
    name: "mock-showbox",
    configureServer(server) {
      server.middlewares.use("/api/v1/showbox", (req, res, next) => {
        const url = req.url || "";
        if (url.startsWith("/analog-hold")) {
          if (req.method === "GET") {
            res.setHeader("Content-Type", "application/json");
            res.end(JSON.stringify({ ok: true, ...analogHold }));
            return;
          }
          if (req.method === "POST") {
            let body = "";
            req.on("data", (chunk) => { body += chunk; });
            req.on("end", () => {
              try {
                const parsed = body ? JSON.parse(body) : {};
                if (parsed.on != null) {
                  analogHold.phones = !!parsed.on;
                  analogHold.loop = !!parsed.on;
                }
                if (parsed.phones != null) analogHold.phones = !!parsed.phones;
                if (parsed.loop != null) analogHold.loop = !!parsed.loop;
              } catch {
                /* ignore */
              }
              res.setHeader("Content-Type", "application/json");
              res.end(JSON.stringify({ ok: true, ...analogHold }));
            });
            return;
          }
        }
        if (req.method === "GET" && (url === "/entities" || url.startsWith("/entities?"))) {
          res.setHeader("Content-Type", "application/json");
          res.end(JSON.stringify({ entities: Object.values(store) }));
          return;
        }
        const match = url.match(/^\/entities\/([^/?]+)/);
        if (match && req.method === "POST") {
          const id = decodeURIComponent(match[1]);
          let body = "";
          req.on("data", (chunk) => { body += chunk; });
          req.on("end", () => {
            let value = true;
            try {
              const parsed = body ? JSON.parse(body) : {};
              value = parsed.value;
            } catch {
              /* ignore */
            }
            set(id, value);
            res.setHeader("Content-Type", "application/json");
            res.end(JSON.stringify({ id, value }));
          });
          return;
        }
        if (req.method === "POST" && url.startsWith("/action/")) {
          res.setHeader("Content-Type", "application/json");
          res.end("{}");
          return;
        }
        next();
      });
      const midi = {
        bluetoothRadio: true,
        wifiRadio: true,
        https: false,
        httpsListening: false,
        httpsHasCa: false,
        httpsUrl: "",
        usbJtag: true,
        acceptNewHosts: true,
        showboxBattery: 96,
        showboxLink: { ok: true, parsing: true, baseAgeMs: 12, mixerAgeMs: 8 },
        usbHidMix: { keyboard: true, mouse: true, absPointer: true, digitizer: false },
        bleHidMix: { keyboard: true, mouse: true, absPointer: false, digitizer: true },
        bleSlots: { hostMax: 1, hostUsed: 0, peripheralMax: 2, peripheralUsed: 1 },
        peripherals: [{
          name: "AIRSTEP", address: "d9:d7:5b:c1:90:f4", connected: true,
          midi: true, hid: true, autoConnect: true, pinned: true
        }],
        hosts: [{
          name: "Tablet", address: "9c:b6:d0:94:28:02", connected: false,
          midi: true, hid: true, hidKeyboard: false, hidMouse: false, hidAbs: false, hidDigitizer: true, pinned: false
        }],
        nearby: [],
        transports: [
          { id: "bleHid", available: true, enabled: true, reason: "", note: "" },
          { id: "bleMidiPeripheral", available: true, enabled: true, reason: "", note: "" },
          { id: "bleMidiHost", available: true, enabled: true, reason: "", note: "" },
          { id: "bleClassic", available: false, enabled: false, reason: "No Bluetooth Classic radio.", note: "" },
          { id: "ethernet", available: false, enabled: false, reason: "No W5500 on this board.", note: "" },
          { id: "wifi", available: true, enabled: true, reason: "", note: "" },
          { id: "usbMidiPeripheral", available: true, enabled: false, reason: "", note: "" },
          { id: "usbMidiHost", available: true, enabled: false, reason: "", note: "" },
          { id: "usbHid", available: true, enabled: false, reason: "", note: "" },
          { id: "din", available: true, enabled: true, reason: "", note: "" }
        ]
      };
      const wifi = {
        sta: true, ssid: "Home", ip: "192.168.3.55", rssi: -48,
        ap: false, apWanted: true, apAlways: false, apAuto: true, autoConnect: true,
        apSsid: "ShowboxMod", apPassword: "showboxmod", apIp: "192.168.4.1", apStations: 0,
        portal: false, radio: true,
        remembered: [{ ssid: "Home", saved: true, current: true }, { ssid: "Studio", saved: true, current: false }]
      };
      server.middlewares.use("/api/v1/https", (req, res) => {
        const path = req.url || "";
        if (path.includes("ca.pem")) {
          res.setHeader("Content-Type", "application/x-pem-file");
          res.end("-----BEGIN CERTIFICATE-----\nMIIB mock\n-----END CERTIFICATE-----\n");
          return;
        }
        res.setHeader("Content-Type", "application/json");
        res.end(JSON.stringify({
          ok: true, enabled: false, listening: false, hasCa: false,
          url: "", caUrl: "/api/v1/https/ca.pem", error: ""
        }));
      });
      server.middlewares.use("/api/v1/midi", (req, res) => {
        res.setHeader("Content-Type", "application/json");
        res.end(JSON.stringify(midi));
      });
      server.middlewares.use("/api/v1/wifi", (req, res) => {
        const path = req.url || "";
        if (path.startsWith("/scan")) {
          res.setHeader("Content-Type", "application/json");
          res.end(JSON.stringify({ ok: true, scanning: true }));
          return;
        }
        if (path.startsWith("/networks")) {
          res.setHeader("Content-Type", "application/json");
          res.end(JSON.stringify({ scanning: false, networks: [{ ssid: "Home", rssi: -48, channel: 6, secure: true }] }));
          return;
        }
        if (req.method === "POST") {
          let body = "";
          req.on("data", (chunk) => { body += chunk; });
          req.on("end", () => {
            try {
              const parsed = body ? JSON.parse(body) : {};
              if (parsed.ap != null) wifi.apWanted = !!parsed.ap;
              if (parsed.apAlways != null) wifi.apAlways = !!parsed.apAlways;
              if (parsed.apAuto != null) wifi.apAuto = !!parsed.apAuto;
              if (parsed.autoConnect != null) wifi.autoConnect = !!parsed.autoConnect;
              if (parsed.ssid) {
                wifi.ssid = parsed.ssid;
                wifi.sta = true;
                wifi.remembered = wifi.remembered.filter((row) => row.ssid !== parsed.ssid);
                wifi.remembered.unshift({ ssid: parsed.ssid, saved: true, current: true });
                wifi.remembered.forEach((row) => { row.current = row.ssid === parsed.ssid; });
              }
              if (parsed.forget) {
                wifi.remembered = wifi.remembered.filter((row) => row.ssid !== parsed.forget);
              }
            } catch { /* ignore */ }
            res.setHeader("Content-Type", "application/json");
            res.end(JSON.stringify(wifi));
          });
          return;
        }
        res.setHeader("Content-Type", "application/json");
        res.end(JSON.stringify(wifi));
      });
    }
  };
}

export default defineConfig({
  plugins: [preact(), mockShowbox(), viteSingleFile()],
  build: {
    cssCodeSplit: false,
    assetsInlineLimit: 100000000,
    minify: "esbuild"
  }
});
