#!/usr/bin/env python3
"""Compatibility wrapper. The suite lives in run.py now.

  python3 scripts/run.py            # level 1
  python3 scripts/run.py --level 2
  python3 scripts/run.py --level 3
"""

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

from run import main as run_main


def main() -> int:
    extra = [a for a in sys.argv[1:] if a not in ("--e2e", "--reboot")]
    level = 1
    if "--reboot" in sys.argv or "--e2e" in sys.argv:
        level = 2
    print("run_fast.py is a wrapper; use python3 scripts/run.py [--level 1|2|3]")
    return run_main(["--level", str(level), *extra])


if __name__ == "__main__":
    sys.exit(main())
