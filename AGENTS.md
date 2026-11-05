# Agent instructions (ShowboxExpansionModule)

Cursor loads `.cursor/rules/*.mdc` automatically. This file is the index.

## Always-on rules

| Rule | Purpose |
|------|---------|
| [`.cursor/rules/senior-debug.mdc`](.cursor/rules/senior-debug.mdc) | Evidence → minimal repro → fix → verify that repro; no streak-as-debugger |
| [`.cursor/rules/live-fixture.mdc`](.cursor/rules/live-fixture.mdc) | Hardware suite constraints (gate, serial, Wi‑Fi, BLE, flash discipline) |

If a rule conflicts with an explicit user instruction in the current turn, follow the user for that turn, then restore the rule.
