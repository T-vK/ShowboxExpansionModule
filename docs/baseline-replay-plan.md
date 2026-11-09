# Baseline replay plan (9615dcc → architecture)

## Hypothesis

`9615dcc` (2026-11-06, “fix SoftAP/STA coex…”) is the last commit **before** the architecture mega-refactor. Treat it as last-green-before-architecture **only after** one L1→L2→L3 confirmation.

**Baseline proof of `9615dcc` (2026-11-08):** L1_OK, L2 STOP at `hotspot-air` — SoftAP stuck `ap=False apWanted=True apIp='0.0.0.0'` after hid-smoke/webui (STA HTTP timed out; SoftAP raise never completed). Alone, `hotspot-air` is green 3×. So `9615dcc` is **not** a proven L1→L2→L3 stable.

**Active baseline:** tag `live-suite-stable` → `0c1a7b9` (`stable: L1/L2/L3 live suite 10× consecutive on hardware`, tagged 2026-11-05). Before SoftAP expansion in `9615dcc`.

**Proof note (2026-11-08):** After tip→baseline app-only flashes, boot showed `ota data partition invalid` + NVS NOT_FOUND and mDNS stayed silent. Re-proof requires **erase + flash** so this tag’s `partitions_16MB_nvs64k.csv` matches the device, then STA join (JSON `/api/v1/wifi`), AIRSTEP connect+auto, laptop host via `check_features.laptop_pair` (KeyboardDisplay agent — plain `bluetoothctl pair` AuthenticationFailed), mdns iso ≥3×, then one gated L1→L2→L3.

**Harness deltas kept on the proof branch (fixture, not architecture):** `ensure_rtp` tries `SHOWBOX_ETH` when STA AppleMIDI is silent, soft-reboots on invite-`NO` then re-pairs the laptop host; `_ensure_midi` prefers bonded BLE before RTP; `case_mdns` probes QU via eth and softens empty multicast/legacy when dual-homed; `wifi-midi` / `midi-http-together` use `ensure_rtp`.

**Proof result (2026-11-09 Europe/Berlin):** gated L1→L2→L3 **OK** on tag `live-suite-stable` after erase+flash + harness deltas above. Log: `/tmp/baseline-live-suite-stable-20261109-002029`.

**Replay branch:** `replay/from-live-suite-stable` (from the tag).
- Slice 1: CI-only from `ce5c2a8` (workflow + `merge_bin.py`) — no SoftAP firmware.
- Slice 2 (**done**): DI extract from `056118d` — `lib/MackieShowbox`, `lib/MultiMidi`, LanCoex/AppHooks/ShowboxLog, ActionRules/RestApi inject, `main` wiring; **excluded** SoftAP ownership / LiveSocket knobs / suite +901 / product portal SSID / snapshot-mirror cache. ble-midi+ble-status+websocket iso **3/3**. Stripped snapshot-mirror (LiveSocket-adjacent BSS). Fixture: laptop HOGP re-pair after flash churn. **Confirmation L1→L2→L3 OK** `/tmp/replay-di-confirm2-20261109-020834` (Europe/Berlin).
- Slice 3 / SoftAP fw slice 1 (**done**, then STOP hotspot-air): `CaptivePortal::dropApForSta(kind)` + `lanSetCaptivePortal` / `lanDropSoftApForSta`; LanKeep SoftAP drops via portal. Iso `hotspot`+`mdns` **3/3** `/tmp/replay-softap-slice1-hotspot-mdns-20261109-024233`; `hotspot-air` FAIL until raise-path slice (baseline `ensureAp` `WiFi.disconnect` dropped STA).
- SoftAP fw slice 2 (**done** + confirmed): SoftAP raise without killing STA — no `WiFi.disconnect` while `staUp`, no preemptive `WiFi.mode(APSTA)` before `softAP` succeeds, `softAPConfig` + partial-SSID reassert, fail path `softAPdisconnect` (not `WiFi.mode(STA)`), `setWantedAp`/`setAlwaysAp` clears `lastApTryMs`. Iso `hotspot`+`hotspot-air`+`mdns` **3/3** `/tmp/replay-softap-slice2-20261109-024828`. **Confirmation L1→L2→L3 OK** `/tmp/replay-softap2-l123b-20261109-025705` (after looper iso 3/3; first L2 attempt flaked on intermittent looper-autoboost).
- SoftAP fw slice 3 (**done** + confirmed): reclaim / BLE yield-before-softAP / boot defer from `5449309` fw only. Iso **3/3** `/tmp/replay-softap-slice3-20261109-032300`. **Confirmation L1→L2→L3 OK** `/tmp/replay-softap3-l123-*` (REPLAY_SOFTAP3_STREAK_OK).
- SoftAP fw slices 4–6 (**done** + confirmed): temporary yield, force-ignore sticky HTTP-busy, heap JSON expose. Iso each 3/3. **Confirmation L1→L2→L3 OK** `/tmp/replay-softap6-l123d-20261109-042321` after clearing leftover `apAlways` fixture (prior L3 airstep-held flake with apAlways left on). SoftAP firmware ownership block complete.
- Suite helper 1 (**done**): SoftAP enable POST retry-until-live (`233e503`) — `_wait_softap_up` + STA→eth `_wifi_api`; wired into `hotspot-air`. Iso `hotspot`+`hotspot-air`+`mdns` **3/3** `/tmp/replay-softap-helper1-20261109-044217`.
- Suite helper 2 (**done**): BLE MIDI WriteValue fallback (`7358a0c`) when PipeWire holds AcquireWrite. Iso `ble-midi`+`ble-status`+`websocket` **3/3** `/tmp/replay-suite-helper2-20261109-044330`.
- Suite helper 3 (**done**): BLE ACL bounce on stuck AcquireWrite (`1a0dc1f`). Iso `ble-midi`+`ble-status`+`websocket` **3/3** `/tmp/replay-suite-helper3-20261109-044513`.
- Suite helper 4 (**done**): HTTP quiet / recover-before-PWA (`ad7a794`→`f8e1aef`/`10c10f3`) — SoftAP yield + quiet recover in `http-pwa`; quiet before looper gain; dual-homed `/debug` eth fallback. Skipped PTA `_yield_airstep_for_looper`. Iso `ble-midi`+`http-pwa`+`looper-no-autoboost`+`looper-autoboost` **3/3** `/tmp/replay-suite-helper4b-20261109-050032` (first helper4 iso false-PASS without pipefail; STOP/isolate then recover+retharden).
- Suite helper 5 (**done**): Looper assert policy (`3097dba` + `c0da2ca`). Iso `ble-midi`+`looper-no-autoboost`+`looper-autoboost` **3/3** `/tmp/replay-suite-helper5-20261109-050734`.
- Suite helper 6 (**done**, needed after L123 fail): MIDI transport yield / CC102 recovery in `wifi-midi` — bounce RTP then BLE yield when CC102 silent (not thrash pile). Iso `ble-midi`+`http-pwa`+`wifi-midi` **3/3** `/tmp/replay-suite-helper6-20261109-051921`.
- Suite helpers confirmation: attempt1 L1_OK L2 STOP wifi-midi; attempt2 L1 STOP http-pwa (heap `largest=4596`); soft-reboot + **Confirmation L1→L2→L3 OK** `/tmp/replay-suite-helpers-l123c-20261109-052629` (`REPLAY_SUITE_HELPERS_STREAK_OK`, apAlways=off, Europe/Berlin).
- LiveSocket / snapshot slice (**done** + confirmed): from `056118d` — `cleanupClients` 12, `kRetryCap` 32, on-demand `"m"` + paced RECALL mixer fan-out; SPIRAM `snapshotMirror` (not DRAM BSS — that broke `http-pwa`); suite `ws-multiclient` / `ws-sync-stress` / `snapshot-mixer` (latency gate relaxed to p95 < 800ms; sync retries once on HTTP wedge; L2 order multiclient→sync→snapshot). Iso chain **3/3** `/tmp/replay-livesocket-iso-chain3-20261109-111101`. **Confirmation L1→L2→L3 OK** `/tmp/replay-livesocket-l123b-20261109-111626` (`REPLAY_LIVESOCKET_L123_OK`, Europe/Berlin).
- SoftAP fw slice 4 (**done**): temporary yield under apAlways (`4b9533a`) — POST `ap=false` with `apAlways` stops SoftAP + backoff but keeps `apWanted`; loadPrefs restores wanted if prefs drifted; `begin` no longer clears `apWanted` when `apAlways`. Iso `hotspot`+`hotspot-air`+`mdns` **3/3** `/tmp/replay-softap-slice4-20261109-034701` (POST ap=false left `apWanted=true`).
- SoftAP fw slice 5 (**done**): force ignores sticky HTTP-busy (`87e43ab` net of `3e16563`) — `apForceTry` raise from tick without `lanHttpBodyBusy` gate (behavior already present; comment lock-in). Iso `hotspot`+`hotspot-air`+`mdns` **3/3** `/tmp/replay-softap-slice5-20261109-035000`.
- SoftAP fw slice 6 (**done**): heap JSON expose only (`4247303` expose part; **not** the gate) — `statusJson` adds `heapLargest`/`heapFree` (`esp_heap_caps.h`); no SoftAP heapLargest floor. Iso `hotspot`+`hotspot-air`+`mdns` **3/3** `/tmp/replay-softap-slice6-20261109-035300` (wifi JSON showed `heapLargest=9716`). SoftAP fw slices 4–6 iso green — **ready for confirmation L1→L2→L3** (not run here).

### Subagent evidence (folded in)

- SoftAP `0c1a7b9`→`9615dcc`: sticky `apWanted` + `apGaveUpThisSta` + SPA SoftAP yield explain L2 `hotspot-air` mid-level FAIL; baseline SoftAP path is safer ([Diff SoftAP 0c1a7b9 vs 9615dcc](bc-9f8b5a8a-b2f4-521e-97d2-b50604bfc69e)).
- mDNS empty uni on dual-homed: firmware (`9615dcc` later fixed per-iface `:5353`); suite probe unchanged ([Analyze mdns case 0c1a7b9](bc-409af9e3-2e7a-564e-adf6-6f3129f2dc7e)).
- `boost-reboot` after `fresh-pair`: suite RTP-first under BLE/PTA ([Classify boost-reboot AppleMIDI fail](bc-b061c76a-318d-5b7d-b77f-b1a81cd1b4de)) — addressed by BLE-before-RTP harness.
- Replay order confirmed: DI extract → SoftAP fw → suite-only; never whole-pick `056118d` / `9615dcc` ([Replay slices from live-suite-stable](bc-cf887611-6e33-5f2a-8c9b-3467029294a4)).

Everything after `9615dcc` is either:

1. **056118d** — clean-architecture DI **plus** SoftAP ownership + LiveSocket/WS behavior + suite +901 lines (not pure DI), or
2. **~33 reactive SoftAP/suite patches** from an overnight flake storm.

## Branches / safety

| Ref | Role |
|-----|------|
| `live-suite-stable` / `baseline/0c1a7b9-stable` | **Active proof root** (`0c1a7b9`) — tagged 10× L1/L2/L3 |
| `baseline/9615dcc-stable` | SoftAP expansion; L2 hotspot-air failed mid-level (not proven) |
| `wip/suite-thrash-20261108` | Tip of `main` before reset |
| stash `wip before baseline reset…` | Uncommitted suite/firmware WIP |

Do **not** force-push `main`. Replay on `baseline/0c1a7b9-stable` (or a child) once green.

## What `056118d` actually changed

**Behavioral SoftAP (not pure DI):**
- `CaptivePortal::dropApForSta(kind)` sole teardown; LanKeep no longer raw `softAPdisconnect`
- UBF setup-portal AP helpers; default portal SSID fallback `"UBFW-Setup"`
- LiveSocket: `cleanupClients` 3→12, `kRetryCap` 8→32, mixer blob / paced `"m"` (same commit)

**Pure DI / extract:**
- `lib/MackieShowbox`, `lib/MultiMidi` moves; `LanCoex` / `MidiAppHooks`; inject edges

**Suite:** only `scripts/suite/cases.py` (+901) — `ws-multiclient`, `ws-sync-stress`, `snapshot-mixer`

## REPLAY ORDER (after baseline green)

One slice per cycle: apply → flash if firmware → iso ≥3× → **one** confirmation streak. On STOP: isolate `--case`, never streak N+1.

1. **CI/release only (extract hunks, don’t whole-cherry-pick mixed commits)**
   - Python 3.12 pin from `914205f` (workflow only)
   - factory.bin publish from `ce5c2a8` (`.github` + merge script only)

2. **Architecture DI without SoftAP behavior**
   - Re-implement `056118d` **minus** `dropApForSta` / SoftAP ownership, UBF portal naming if product-visible, and the suite +901 hunk
   - Keep: lib moves, LanCoex/AppHooks, inject edges

3. **SoftAP firmware ownership (small intentional slices)**
   - `dropApForSta` sole teardown
   - apAlways SoftAP retry / softAPConfig settle (from `914205f` / `ce5c2a8` firmware hunks)
   - reclaim / BLE yield-before-softAP / boot defer (`5449309` firmware; **skip** suite settle poll)
   - temporary yield under apAlways (`4b9533a`); force ignores sticky HTTP-busy (`3e16563` / `87e43ab`)
   - heap JSON expose only (`4247303` expose part; **not** the gate)

4. **Suite helpers — one shared primitive at a time**
   1. SoftAP enable POST retry-until-live (`233e503`)
   2. BLE MIDI WriteValue fallback (`7358a0c`)
   3. BLE ACL bounce on stuck AcquireWrite (`1a0dc1f`)
   4. HTTP quiet / recover-before-PWA (`ad7a794` → `f8e1aef`/`10c10f3` as one helper)
   5. Looper assert policy (`3097dba`, optionally `c0da2ca`)
   6. Only then: MIDI transport yields / CC102 recovery if still failing

### Prove `056118d` SoftAP/DI slices (not L3 first)

```bash
# SoftAP ownership
python3 scripts/run.py --case hotspot --case hotspot-air --case hotspot-sta-coex --case mdns --ip 192.168.3.55 --keep-host
# LanCoex / BLE path
python3 scripts/run.py --case ble-midi --case ble-status --case websocket --ip 192.168.3.55 --keep-host
# LiveSocket bundle in same commit
python3 scripts/run.py --case websocket --case ws-multiclient --case snapshot-mixer --ip 192.168.3.55 --keep-host
```

Skip `ws-sync-stress` / full L2 until those chains are green ≥3×.

### Do NOT replay (pure streak thrash)

- `b9464d7` ↔ `4247303` heapLargest gate add/remove
- Quiet-period knob churn (`a6e5a7d`/`ae56e94` 3min → `6b5fa52` 15m)
- SoftAP settle-poll then kill (`5449309` suite part → `66218dc`)
- SoftAP raise/lower/defer ping-pong: `687d385`, `ccf1dbe`, `d51d6da`
- PTA/CC102/AIRSTEP retry pile: `2011593`, `e86e91e`, `2733f0b`, `de319a9`, `2b3bcaf`, `fe84017`, `11620c2`, `b25e8be`, `1e4c85f`, `bf93f11`
- Whole-commit cherry-picks of mixed commits (`056118d`, `914205f`, `ce5c2a8`, `5449309`)

## Fixture

- STA `192.168.3.55`, eth `192.168.3.234`
- Gate: `/tmp/l2-gate.sh` then `/tmp/l3-gate.sh`
- `bluetoothd --noplugin=midi`; never open `/dev/ttyACM0` during suite

## Done

Baseline green once, then each replay slice green with a named mechanism. Tip of replay branch passes one L1→L2→L3 confirmation.
