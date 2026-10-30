#!/usr/bin/env python3
"""Fast: reaction vars mutate and survive a burst of NVS writes.

Variable-length JSON used to fragment the NVS partition until
nvs_set_str failed with NOT_ENOUGH_SPACE and BOOST_* state vanished.
Fixed-size 256-byte blobs reuse the same slot; this burst would fail
if that padding/self-heal regresses.
"""

from __future__ import annotations

from live_helpers import Checks, exit_with, set_var, var


REQUIRED = (
    "BOOST_ENABLED",
    "REMEMBERED_GAIN",
    "BOOST_AMOUNT",
    "BOOST_CHANNEL",
    "AUTO_BOOST",
)


def main() -> None:
    c = Checks()
    original = {name: var(name) for name in REQUIRED}
    for name in REQUIRED:
        c.check(f"{name} readable", original[name] is not None, f"value={original[name]}")

    try:
        set_var("BOOST_AMOUNT", 3)
        c.check("BOOST_AMOUNT write", abs(float(var("BOOST_AMOUNT", -1)) - 3) < 0.05,
                f"value={var('BOOST_AMOUNT')}")
        set_var("BOOST_CHANNEL", 2)
        c.check("BOOST_CHANNEL write", abs(float(var("BOOST_CHANNEL", -1)) - 2) < 0.05,
                f"value={var('BOOST_CHANNEL')}")

        marker = 21.0
        for i in range(8):
            set_var("REMEMBERED_GAIN", marker + i * 0.5)
        c.check(
            "burst NVS writes still readable",
            var("REMEMBERED_GAIN") is not None,
            f"value={var('REMEMBERED_GAIN')}",
        )
        set_var("REMEMBERED_GAIN", 27)
        c.check("REMEMBERED_GAIN after burst", abs(float(var("REMEMBERED_GAIN", -1)) - 27) < 0.05,
                f"value={var('REMEMBERED_GAIN')}")
        set_var("BOOST_ENABLED", 1)
        c.check("BOOST_ENABLED write", abs(float(var("BOOST_ENABLED", -1)) - 1) < 0.05)
        set_var("BOOST_ENABLED", 0)
        c.check("BOOST_ENABLED clear", abs(float(var("BOOST_ENABLED", -1))) < 0.05)
    finally:
        for name, value in original.items():
            if value is not None:
                set_var(name, float(value))
        restored = {name: var(name) for name in REQUIRED}
        for name, value in original.items():
            if value is None:
                continue
            c.check(
                f"{name} restored",
                restored[name] is not None and abs(float(restored[name]) - float(value)) < 0.05,
                f"was={value} now={restored[name]}",
            )
    exit_with(c)


if __name__ == "__main__":
    main()
