#!/usr/bin/env python3
"""Fast: BLE status exposes names, hosts/peripherals, and n/m slot counts."""

from __future__ import annotations

import time

from live_helpers import Checks, api, exit_with

AIRSTEP = "d9:d7:5b:c1:90:f4"


def same_addr(a: str, b: str) -> bool:
    return a.replace(":", "").lower() == b.replace(":", "").lower()


def main() -> None:
    c = Checks()
    midi = api("/api/v1/midi")
    slots = midi.get("bleSlots") or {}
    c.check("bleSlots present", bool(slots), f"{slots}")
    host_max = int(slots.get("hostMax") or 0)
    peri_max = int(slots.get("peripheralMax") or 0)
    host_used = int(slots.get("hostUsed") or 0)
    peri_used = int(slots.get("peripheralUsed") or 0)
    c.check("host slots configured", host_max > 0, f"{host_used}/{host_max}")
    c.check("peripheral slots configured", peri_max > 0, f"{peri_used}/{peri_max}")
    c.check("used hosts within max", 0 <= host_used <= host_max, f"{host_used}/{host_max}")
    c.check("used peripherals within max", 0 <= peri_used <= peri_max, f"{peri_used}/{peri_max}")
    limit = int(midi.get("bleLinkLimit") or 0)
    c.check("bleLinkLimit is 3", limit == 3, f"bleLinkLimit={limit}")

    hosts = midi.get("hosts") or []
    nearby = midi.get("nearby") or []
    c.check("hosts is a list", isinstance(hosts, list))
    c.check("nearby is a list", isinstance(nearby, list))

    airstep = None
    peris = []
    deadline = time.time() + 8.0
    while time.time() < deadline:
        midi = api("/api/v1/midi")
        peris = midi.get("peripherals") or []
        airstep = None
        for peer in peris:
            name = str(peer.get("name") or "")
            addr = str(peer.get("address") or "")
            if same_addr(addr, AIRSTEP) or name.upper() == "AIRSTEP":
                airstep = peer
                break
        if airstep and airstep.get("connected"):
            break
        time.sleep(0.8)

    c.check("peripherals is a list", isinstance(peris, list))
    named = sum(1 for peer in peris if str(peer.get("name") or "").strip())
    c.check("at least one peripheral has a name", named > 0, f"named={named} of {len(peris)}")
    if airstep is not None:
        c.check("AIRSTEP name present", bool(str(airstep.get("name") or "").strip()),
                f"name={airstep.get('name')!r}")
        c.check("AIRSTEP connected", bool(airstep.get("connected")),
                f"connected={airstep.get('connected')}")
    else:
        c.check("AIRSTEP listed as a peripheral", False, "missing from peripherals")
    exit_with(c)


if __name__ == "__main__":
    main()
