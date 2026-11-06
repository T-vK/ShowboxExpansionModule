---
name: live-suite-stabilize
description: >-
  Stabilize live L1/L2/L3 suite on real ESP32-S3 hardware. Use when fixing flaky
  suite cases, BLE MIDI / BlueZ / SoftAP fixtures, ActionRules booster races,
  HTTP-after-SPA heap issues, or running gate→streak confirmation.
---

# Live suite stabilize

## Goal

Green fixture gate, then consecutive level passes. Prefer **minimal repro → shared fix → verify** over using full `--level 3` as the debugger.

## Hard constraints (never violate)

- No commit unless asked; author T-vK; never Co-authored-by
- Never hardcode Wi‑Fi PSK (nmcli `pfSenseAP` only)
- Never open `/dev/ttyACM0` with serial/pyserial during tests (DTR resets S3)
- Never start a streak without green fixture gate
- Keep `bluetoothd --noplugin=midi`; destroy PipeWire `bluez_midi.server` if holding MIDI char

## Procedure

1. **Classify** — firmware vs suite vs host fixture. Cite evidence.
2. **Minimal repro** — smallest `--case` chain (&lt;2 min). Example: `ble-midi` then `http-pwa`.
3. **One fix** at shared primitive when cross-cutting (`ensure_ble_midi`, `http_get_retry`, ActionRules).
4. **Verify repro** ≥3× (5× if flake).
5. **Gate** then **one** streak log under `/tmp/`. On STOP: return to step 1 — do not immediately start streak N+1.
6. Actively watch long runs; check logs yourself.

## Subagents

- Delegate streak babysitting / gate revive / noisy iso loops.
- Brief must include IP, done criteria, deliverable: `SUCCESS n/n | STOP at k: case + error + ONE next`.
- Main agent keeps decisions and final edits.

## Commands

```bash
~/.platformio/penv/bin/pio run -e esp32-s3-n16r8 -t upload
python3 scripts/run.py --case NAME --ip 192.168.3.55 --keep-host
python3 scripts/run.py --level 3 --ip 192.168.3.55
```

Typical STA: `192.168.3.55`. SoftAP: `ShowboxMod`.
