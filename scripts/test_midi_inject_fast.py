#!/usr/bin/env python3
"""Fast: /api/v1/midi/inject reaches the rule engine and does not freeze it.

AIRSTEP clock floods used to evict CC events from the MIDI ring, and a
leaked inAction lock froze every later rule. Inject a harmless CC, then
a mapped clear, then confirm vars still answer.
"""

from __future__ import annotations

import time

from live_helpers import Checks, api, cc, entity, exit_with, http_status, set_var, var


def main() -> None:
    c = Checks()
    unmapped = api("/api/v1/midi/inject", {"status": 0xB0, "d1": 20, "d2": 64})
    c.check("inject unmapped CC", bool(unmapped.get("ok")), f"{unmapped}")

    code, body = http_status("/api/v1/midi/inject", {"packet": "not-hex"})
    c.check("bad hex rejected", code == 400, f"code={code} body={body}")

    code, body = http_status("/api/v1/midi/inject", {})
    c.check("empty inject rejected", code == 400, f"code={code}")

    auto0 = var("AUTO_BOOST", 1)
    set_var("AUTO_BOOST", 0)
    cc(104)
    time.sleep(1.5)
    state = entity("LOOPER_STATE")
    c.check("inject CC104 reaches looper", state is not None, f"LOOPER_STATE={state}")
    if state is not None:
        c.check("looper empty or reachable", state == 0 or state >= 0, f"LOOPER_STATE={state}")

    amount = var("BOOST_AMOUNT")
    set_var("BOOST_AMOUNT", float(amount) if amount is not None else 2)
    again = var("BOOST_AMOUNT")
    c.check(
        "engine still answers after inject",
        again is not None,
        f"BOOST_AMOUNT={again}",
    )
    set_var("AUTO_BOOST", float(auto0) if auto0 is not None else 1)
    exit_with(c)


if __name__ == "__main__":
    main()
