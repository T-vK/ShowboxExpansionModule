# AGENTS.md — ShowboxExpansionModule

Vendor-neutral agent instructions ([agents.md](https://agents.md/) standard). Cursor also loads `.cursor/rules/*.mdc` (prefer those for activation control). **Do not add a root `CLAUDE.md`** here — Cursor treats it as always-on and it doubles context tax.

## Project

ESP32-S3 expansion module firmware (PlatformIO) + live Python suite against real hardware (Showbox UART, BLE MIDI, Wi‑Fi STA/SoftAP, web UI).

| Path | Role |
|------|------|
| `src/` | Firmware (Actions, MIDI, REST, Showbox link, …) |
| `scripts/suite/` | Live suite cases / BLE / RTP / HTTP helpers |
| `scripts/run.py` | Suite entry: `--level N`, `--case`, `--ip` |
| `platformio.ini` | `esp32-s3-n16r8` env; NVS partition overlay |
| `.cursor/rules/` | Cursor-scoped mandatory behaviors |

## Commands

- Build/flash: `~/.platformio/penv/bin/pio run -e esp32-s3-n16r8 -t upload`
- Suite: `python3 scripts/run.py --level 1|2|3 --ip 192.168.3.55`
- Single/multi case: `python3 scripts/run.py --case a --case b --ip 192.168.3.55 --keep-host`
- Fixture gate (when present): `/tmp/l3-gate.sh` (or L2 equivalent) before streaks

## Non‑negotiables

- Work as a **senior** by default (see `.cursor/rules/senior-engineer.mdc`). Never wait to be told.
- Debug with **minimal repro → fix → verify repro**; full level streaks are confirmation only.
- Fight **context rot**: keep the main chat for decisions; shove noisy work into subagents that return short verdicts.
- User chat overrides these files for that turn.

## Rule index (Cursor)

| File | When |
|------|------|
| `senior-engineer.mdc` | Always — ownership, efficiency, no junior thrash |
| `senior-debug.mdc` | Always — evidence / repro / verify loop |
| `subagents-context.mdc` | Always — when to spawn, what to return |
| `live-fixture.mdc` | Always — serial / Wi‑Fi / BLE / gate / flash |
| `suite-live.mdc` | `scripts/suite/**`, suite e2e scripts |
| `firmware-actions.mdc` | `src/Actions/**` |

## Skills (on-demand)

| Skill | When |
|-------|------|
| `.cursor/skills/live-suite-stabilize/` | Live L1–L3 / BLE / ActionRules / SPA-heap stabilization |
