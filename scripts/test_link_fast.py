#!/usr/bin/env python3
"""Fast: mixer/base link, entity dump, reaction engine still answers."""

from __future__ import annotations

from live_helpers import Checks, api, exit_with


def main() -> None:
    c = Checks()
    link = api("/api/v1/showbox/link")
    c.check("link ok", bool(link.get("ok")), f"{link}")
    base = int(link.get("baseAgeMs") or 0xFFFFFFFF)
    mixer = int(link.get("mixerAgeMs") or 0xFFFFFFFF)
    c.check("base age fresh", base < 800, f"baseAgeMs={base}")
    c.check("mixer age fresh", mixer < 800, f"mixerAgeMs={mixer}")

    ents = api("/api/v1/showbox/entities").get("entities") or []
    c.check("entity dump present", len(ents) > 50, f"count={len(ents)}")

    vars_body = api("/api/v1/actions/vars")
    names = {row.get("name") for row in (vars_body.get("vars") or [])}
    for required in ("BOOST_ENABLED", "REMEMBERED_GAIN", "BOOST_AMOUNT", "BOOST_CHANNEL", "AUTO_BOOST"):
        c.check(f"var {required} exists", required in names)

    actions = api("/api/v1/actions")
    rules = actions.get("automations") or actions.get("rules") or actions.get("actions") or []
    c.check("reaction rules loaded", len(rules) > 0, f"count={len(rules)}")
    exit_with(c)


if __name__ == "__main__":
    main()
