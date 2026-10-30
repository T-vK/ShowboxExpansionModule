#!/usr/bin/env python3
"""Fast: a user gain move updates REMEMBERED_GAIN and clears BOOST_ENABLED.

If this regresses, the next auto-boost treats the boosted gain as the
base and ratchets +2 steps every play.
"""

from __future__ import annotations

import time

from live_helpers import Checks, api, entity, exit_with, set_var, var, wait_var


def main() -> None:
    c = Checks()
    auto0 = var("AUTO_BOOST", 1)
    gain0 = entity("INPUT2_GAIN")
    c.check("gain readable", gain0 is not None, f"gain={gain0}")
    if gain0 is None:
        exit_with(c)

    base = 28.0
    moved = 40.0
    set_var("AUTO_BOOST", 0)
    api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": base})
    time.sleep(1.2)
    set_var("REMEMBERED_GAIN", base)
    set_var("BOOST_ENABLED", 1)
    time.sleep(0.3)

    api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": moved})
    rem = wait_var("REMEMBERED_GAIN", moved, timeout=5.0)
    c.check(
        "tracker adopts user gain",
        rem is not None and abs(float(rem) - moved) < 0.05,
        f"REMEMBERED_GAIN={rem} want={moved}",
    )
    flag = wait_var("BOOST_ENABLED", 0, timeout=3.0)
    c.check(
        "BOOST_ENABLED cleared by user move",
        flag is not None and abs(float(flag)) < 0.05,
        f"BOOST_ENABLED={flag}",
    )

    api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": base})
    time.sleep(1.0)
    set_var("REMEMBERED_GAIN", base)
    set_var("BOOST_ENABLED", 0)
    set_var("AUTO_BOOST", float(auto0) if auto0 is not None else 1)
    exit_with(c)


if __name__ == "__main__":
    main()
