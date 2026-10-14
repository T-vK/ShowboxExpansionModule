#!/usr/bin/env python3
"""Try to un-wedge a silent Showbox base by injecting recovery packets.

When the module reboots mid-transaction the base's UART state machine wedges:
no heartbeats, no entity traffic, no DATA_REQUEST replies. Only a Showbox
power cycle has cleared it so far. This script tests whether injecting
well-formed packets toward the base can un-wedge it instead.

Candidates cover the two plausible wedge modes:
  - parser stuck mid-packet    -> terminator / zero padding completes or flushes it
  - state machine idle-stuck   -> heartbeat / menu-back / DATA_REQUEST restarts it

For each candidate the script injects one Actions rule (showboxPacket step,
dir=base), runs it, and watches /api/v1/showbox/link for base heartbeats
returning. It deletes the rule afterwards. Stops at the first candidate that
revives the base unless --all is given.
"""

from __future__ import annotations

import argparse
import json
import time
import urllib.error
import urllib.request

LINK = "/api/v1/showbox/link"
STALE_MS = 2000

CANDIDATES = [
    # Heartbeat, module acting as the mixer side
    ("heartbeat", "BE EF 00 FF 00 EF BE", "base"),
    # Menu-back (no-op navigation packet)
    ("menu-back", "BE EF 04 04 00 00 00 00 00 EF BE", "base"),
    # DATA_REQUEST variants (phase 1 and 3 shapes, aimed at the base)
    ("data-req-1", "BE EF 00 09 05 EF BE", "base"),
    ("data-req-3", "BE EF 01 09 00 05 EF BE", "base"),
    # Terminator only: completes a packet the base may hold half of
    ("terminator", "EF BE", "base"),
    # Zero padding: flush a wedged parser looking for a BE EF sync
    ("zero-flush", "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", "base"),
    # Heartbeat to both sides, in case the mixer side is the stuck one
    ("heartbeat-both", "BE EF 00 FF 00 EF BE", "both"),
]


def fetch(url: str, timeout: float = 3.0) -> tuple[int, object]:
    req = urllib.request.Request(url, headers={"Connection": "close", "Cache-Control": "no-store"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8", "replace")
            try:
                return resp.getcode() or 0, json.loads(raw)
            except json.JSONDecodeError:
                return resp.getcode() or 0, raw
    except urllib.error.HTTPError as err:
        return err.code, ""
    except Exception:
        return 0, ""


def post(url: str, body: bytes, timeout: float = 5.0) -> tuple[int, object]:
    req = urllib.request.Request(url, data=body, method="POST",
                                 headers={"Content-Type": "application/json", "Connection": "close"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8", "replace")
            try:
                return resp.getcode() or 0, json.loads(raw)
            except json.JSONDecodeError:
                return resp.getcode() or 0, raw
    except urllib.error.HTTPError as err:
        return err.code, err.read().decode("utf-8", "replace")
    except Exception:
        return 0, ""


def wait_http(ip: str, deadline_s: float) -> bool:
    url = f"http://{ip}{LINK}"
    started = time.monotonic()
    print(f"WAIT http {url} (up to {deadline_s:.0f}s)")
    while time.monotonic() - started < deadline_s:
        code, body = fetch(url, timeout=2.5)
        if code == 200 and isinstance(body, dict):
            return True
        time.sleep(1.5)
    return False


def sample(ip: str) -> dict | None:
    code, body = fetch(f"http://{ip}{LINK}", timeout=2.5)
    if code == 200 and isinstance(body, dict):
        return body
    return None


def num(body: dict, key: str) -> int:
    try:
        v = int(body.get(key) or 0)
    except (TypeError, ValueError):
        v = 0
    return max(0, v)


def age(body: dict, key: str) -> int:
    try:
        v = int(body.get(key) or 0)
    except (TypeError, ValueError):
        return 0x7FFFFFFF
    return 0x7FFFFFFF if v < 0 else v


def looks_wedged(ip: str) -> tuple[bool, dict | None]:
    """Base wedged = base RX stale and hbB flat over ~4 s."""
    samples = []
    for _ in range(3):
        body = sample(ip)
        if body is not None:
            samples.append(body)
        time.sleep(2.0)
    if not samples:
        return False, None
    hb = [num(s, "hbB") for s in samples]
    last = samples[-1]
    base_stale = all(age(s, "baseAgeMs") >= STALE_MS for s in samples)
    rx_stale = all(num(s, "rxBase") == 0 or age(s, "baseAgeMs") >= STALE_MS for s in samples)
    hb_flat = len(set(hb)) == 1
    wedged = base_stale and hb_flat
    return wedged, last


def inject_and_run(ip: str, name: str, hex_str: str, direction: str) -> int | None:
    rule = {"name": name, "on": True,
            "actions": [{"type": "showboxPacket", "hex": hex_str, "dir": direction}]}
    code, body = post(f"http://{ip}/api/v1/actions", json.dumps(rule).encode())
    if code != 200:
        print(f"  save failed http={code} {str(body)[:120]}")
        return None
    # Find the new rule id in the returned list
    rid = None
    if isinstance(body, list):
        for r in body:
            if isinstance(r, dict) and r.get("name") == name:
                rid = r.get("id")
                break
    elif isinstance(body, dict):
        for r in body.get("rules", body.get("actions", [])):
            if isinstance(r, dict) and r.get("name") == name:
                rid = r.get("id")
                break
    if rid is None:
        code, body = fetch(f"http://{ip}/api/v1/actions")
        if code == 200 and isinstance(body, (list, dict)):
            rules = body if isinstance(body, list) else body.get("rules", [])
            for r in rules:
                if isinstance(r, dict) and r.get("name") == name:
                    rid = r.get("id")
                    break
    if rid is None:
        print("  could not resolve rule id after save")
        return None
    code, _ = post(f"http://{ip}/api/v1/actions/run?id={rid}", b"")
    if code != 200:
        print(f"  run failed http={code}")
    return rid


def delete_rule(ip: str, rid: int) -> None:
    try:
        post(f"http://{ip}/api/v1/actions/delete?id={rid}", b"")
    except Exception:
        pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ip", default="192.168.3.55")
    parser.add_argument("--wait-live", type=float, default=180)
    parser.add_argument("--window", type=float, default=8.0, help="seconds to watch after each injection")
    parser.add_argument("--all", action="store_true", help="do not stop at first recovery")
    parser.add_argument("--skip-wedge-check", action="store_true", help="run even if link looks healthy")
    args = parser.parse_args()

    if not wait_http(args.ip, args.wait_live):
        print("FAIL: module http never came up (is WiFi up? is the module reboot-looping?)")
        return 1

    wedged, snap = looks_wedged(args.ip)
    if snap is None:
        print("FAIL: link endpoint unreachable after http came up")
        return 1
    print(f"LINK hbB={num(snap, 'hbB')} hbM={num(snap, 'hbM')} baseAge={age(snap, 'baseAgeMs')} "
          f"mixAge={age(snap, 'mixerAgeMs')} rxBase={num(snap, 'rxBase')} rxMix={num(snap, 'rxMix')} "
          f"initM={num(snap, 'initM')} initB={num(snap, 'initB')}")
    if not wedged and not args.skip_wedge_check:
        print("Base link looks healthy; nothing to recover. Use --skip-wedge-check to test anyway.")
        return 0
    print(f"WEDGED={wedged}: base RX stale, heartbeats flat. Starting candidate injections.\n")

    winner = None
    for i, (name, hex_str, direction) in enumerate(CANDIDATES, 1):
        rule_name = f"uart-recover-{i:02d}-{name}"
        print(f"[{i}/{len(CANDIDATES)}] {name}: {hex_str} dir={direction}")
        before = sample(args.ip)
        if before is None:
            print("  module went away before injection; waiting for http...")
            if not wait_http(args.ip, 120):
                print("  FAIL: module did not come back")
                return 1
            before = sample(args.ip) or {}
        rid = inject_and_run(args.ip, rule_name, hex_str, direction)
        if rid is None:
            continue
        recovered = False
        deadline = time.monotonic() + args.window
        hb0 = num(before, "hbB")
        while time.monotonic() < deadline:
            time.sleep(2.0)
            after = sample(args.ip)
            if after is None:
                continue
            hb_now = num(after, "hbB")
            base_ok = age(after, "baseAgeMs") < STALE_MS
            print(f"  +{int(time.monotonic() - (deadline - args.window)):>2}s hbB {hb0}->{hb_now} "
                  f"baseAge={age(after, 'baseAgeMs')} rxBase={num(after, 'rxBase')}")
            if base_ok and hb_now > hb0:
                recovered = True
                break
        delete_rule(args.ip, rid)
        if recovered:
            print(f"  ==> RECOVERED by '{name}'")
            winner = winner or name
            if not args.all:
                break
        else:
            print("  no response from base\n")
            time.sleep(1.0)

    if winner:
        print(f"\nPASS: base revived with '{winner}'")
        return 0
    print("\nFAIL: no candidate revived the base (power cycle still required, or none of these packets is what it waits for)")
    return 1


if __name__ == "__main__":
    import sys
    sys.exit(main())