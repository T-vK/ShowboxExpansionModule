# Architecture refactor plan

Safe, test-gated cleanup so app modules stop crossing boundaries, and so
`UARTInterceptor` / `MackieShowbox` / `MultiMidi` (and maybe `UltimateBaseFirmware`)
can become separate repos later.

## Non-negotiables

- `main.cpp` is the only composition root for app objects.
- No `extern` of app objects (`LiveSocket`, `BleRadio`, `MackieShowbox`, …) from feature modules.
- Extractable libs may not `#include` Actions / LiveSocket / LanKeep / CaptivePortal / RestApi / Connections / FrontPanel.
- After every phase: flash + minimal suite cases ≥3× before the next phase.

## Library target map

| Unit | Today | Target | Blockers |
|------|-------|--------|----------|
| `lib/UARTInterceptor` | clean | keep as freestanding lib | none |
| `lib/MackieShowbox` | PlatformIO lib | own repo later | logging via ShowboxLog hooks |
| `lib/MultiMidi` | PlatformIO lib | own repo later | `debugLog` weak/app-provided; MidiAppHooks + LanCoex |
| `lib/BleRadio` / `lib/Hid` | libs | stay libs; attach via MultiMidi/Connections | — |
| `lib/UltimateBaseFirmware` | board kit | light PREP then extract | parameterize AP SSID/pass; SoftAP overlap with CaptivePortal |

## Phases

### Phase 1 — Kill hidden app-object `extern`s (low risk) — DONE

1. Inject `LiveSocket*` into `ActionRules` (`setLiveSocket`); null-safe pushes.
2. Inject `BleRadio*` into `RestApiRouter` (`setBleRadio`); drop `extern BleRadio`.
3. Wire both in `main.cpp` only.

**Verify:** `--case websocket --case ws-multiclient --case reaction-engine --case ble-status` (and `/api/v1/debug/iso`).

### Phase 2 — Break extractable-lib back-edges (medium) — DONE

1. `midiInboundSetRules` replaces `MultiMidi.cpp` → `ActionRules.h` include; `main` registers `actionRulesOnMidi`.
2. `MultiMidi/LanCoex.h` port + `BleMidi::setCoex`; `main` supplies LanKeep/HttpsGate hooks. BleMidi no longer includes LanKeep/HttpsGate.
3. Optional later: narrow `MixerPush` port if ActionRules should not know `LiveSocket` by name.

**Verify:** `--case ble-midi --case ble-status --case midi-http-together --case websocket` + hotspot/STA smoke if coex touched.

### Phase 3 — SoftAP ownership — DONE (NetRadio optional later)

1. **Done:** `CaptivePortal::dropApForSta`; LanKeep via `lanSetCaptivePortal`.
2. **Done:** UBF `setSetupPortalAp` / `setSetupPortalTitle` — product SoftAP stays CaptivePortal; WiFiManager portal is optional/dev only.
3. Optional later: full `NetRadio` façade for STA join/reconnect only.

**Verify:** `--case hotspot --case mdns --case ethernet-smoke` (+ wifi cases when `--ip` is STA).

### Phase 4 — Physical library moves — DONE

- `lib/UARTInterceptor` — freestanding
- `lib/MackieShowbox/` — protocol + `constants`/`Packet`/`string_mappings` + `ShowboxLog` hooks (app wires DebugLog/Diag)
- `lib/MultiMidi/` — `LanCoex` + `MidiAppHooks` + `midiInboundSetRules`; `debugLog` declared, defined by app
- `lib/UltimateBaseFirmware` — parameterized setup-portal AP; dead `initializeCaptivePortal` removed
- Sibling libs: `BleRadio`, `Hid`

Future separate repos: publish these PlatformIO libs as git deps (`lib_deps = https://github.com/...`).

**Verify:** core suite ≥3× then `--level 2` once.

## Suite gate (every phase)

Preferred IP: eth `192.168.3.234` with `--keep-host` when BLE peers matter.

```bash
~/.platformio/penv/bin/pio run -e esp32-s3-n16r8 -t upload
python3 scripts/run.py --case <list> --ip 192.168.3.234 --keep-host
# repeat case list ≥3× before advancing
```

Core regression set after any WS/ActionRules/Showbox touch:

`snapshot-mixer`, `websocket`, `ws-multiclient`, `ws-sync-stress`

## Out of scope this pass

- Full repo extraction / publishing packages
- Rewriting ActionRules god-object internals
- Collapsing Diag/HttpsGate extern soup (nice-to-have after Phase 1 pattern exists)
