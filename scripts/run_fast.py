#!/usr/bin/env python3
"""Run the fast regression suite. Stops on the first failure.

  python3 scripts/run_fast.py           # ~20s, the traps we kept hitting
  python3 scripts/run_fast.py --e2e     # + looper/boost e2e (~2 min)
  python3 scripts/run_fast.py --reboot  # + boost reboot undo (~1 min)

Long soaks (check_live 30s, check_uart, check_features --pair) stay out
of this runner on purpose.
"""

from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent

FAST = (
    "test_link_fast.py",
    "test_mdns_fast.py",
    "test_wifi_status_fast.py",
    "test_ble_status_fast.py",
    "test_nvs_vars_fast.py",
    "test_midi_inject_fast.py",
    "test_gain_tracker_fast.py",
    "test_boost_ratchet_fast.py",
)

E2E = ("test_looper_boost_e2e.py",)
REBOOT = ("test_boost_reboot_e2e.py",)


def run_one(name: str) -> int:
    path = HERE / name
    print()
    print("=" * 60)
    print(name)
    print("=" * 60)
    started = time.monotonic()
    result = subprocess.run([sys.executable, str(path)], cwd=str(HERE))
    elapsed = time.monotonic() - started
    print(f"-- {name} exit {result.returncode} in {elapsed:.1f}s")
    return result.returncode


def main() -> int:
    names = list(FAST)
    if "--e2e" in sys.argv:
        names.extend(E2E)
    if "--reboot" in sys.argv:
        names.extend(REBOOT)
    started = time.monotonic()
    for name in names:
        code = run_one(name)
        if code != 0:
            print()
            print(f"STOP: {name} failed after {time.monotonic() - started:.1f}s")
            return code
    print()
    print(f"ALL FAST PASS in {time.monotonic() - started:.1f}s ({len(names)} scripts)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
