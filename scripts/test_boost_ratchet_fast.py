#!/usr/bin/env python3
"""Fast: a desynced BOOST_ENABLED flag must not add another +2 steps.

Historical failure: an unboost announcement was missed, the flag stayed
false while hardware was already +2, and the next boost climbed to 60 dB.
"""

from __future__ import annotations

import time

from live_helpers import Checks, api, cc, entity, exit_with, set_var, var, wait_gain


def main() -> None:
    c = Checks()
    auto0 = var("AUTO_BOOST", 1)
    base = 28.0
    boosted = base + 8.0
    ratcheted = boosted + 8.0

    set_var("AUTO_BOOST", 0)
    set_var("BOOST_CHANNEL", 2)
    set_var("BOOST_AMOUNT", 2)
    api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": boosted})
    time.sleep(1.2)
    set_var("REMEMBERED_GAIN", base)
    set_var("BOOST_ENABLED", 0)
    time.sleep(0.3)

    now = entity("INPUT2_GAIN")
    c.check("precondition already +2", now is not None and abs(now - boosted) < 0.05, f"gain={now}")

    cc(105)
    after = wait_gain(base, timeout=5.0)
    c.check(
        "reconcile unboosts instead of ratcheting",
        after is not None and abs(after - base) < 0.05,
        f"gain={after} (want {base}, ratchet would be {ratcheted})",
    )
    c.check(
        "did not climb another +2",
        after is not None and after < ratcheted - 1.0,
        f"gain={after}",
    )

    api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": base})
    time.sleep(1.0)
    set_var("REMEMBERED_GAIN", base)
    set_var("BOOST_ENABLED", 0)
    set_var("AUTO_BOOST", float(auto0) if auto0 is not None else 1)
    exit_with(c)


if __name__ == "__main__":
    main()
