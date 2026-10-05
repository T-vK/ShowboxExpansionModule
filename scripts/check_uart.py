#!/usr/bin/env python3
"""Verify mixer↔base UART the way the mixer does: no reconnect dumps.

The mixer shows "connection lost" and sends ALL_ENTITIES again. Age-only
checks can pass while that loop is running. This script:

  1. Requires the instrumented /link fields (fails on an old image)
  2. Waits until both sides are live
  3. Settles --settle seconds (one handshake dump is allowed)
  4. Fails if mixer ALL_ENTITIES (initM) increases after settle
  5. Fails if either RX age stays >= --stale-ms
  6. Fails if drop/ovf/UART framing-error counters increase
  7. Fails if the parser sees a bad frame after settle
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

REQUIRED = ("initM", "initB", "hbM", "hbB", "frmBase", "frmMix", "goodM", "goodB", "lock")


def fetch(url: str, timeout: float) -> tuple[int, dict]:
    req = urllib.request.Request(url, headers={"Connection": "close", "Cache-Control": "no-store"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            try:
                return resp.getcode() or 0, json.loads(raw.decode("utf-8", "replace"))
            except json.JSONDecodeError:
                return resp.getcode() or 0, {}
    except urllib.error.HTTPError as err:
        return err.code, {}
    except Exception:
        return 0, {}


def age(v) -> int:
    try:
        n = int(v)
    except (TypeError, ValueError):
        return 0x7FFFFFFF
    return 0x7FFFFFFF if n < 0 else n


def num(body: dict, key: str) -> int:
    try:
        return int(body.get(key) or 0)
    except (TypeError, ValueError):
        return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ip", default="192.168.3.55")
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--settle", type=float, default=12)
    parser.add_argument("--wait-live", type=float, default=45)
    parser.add_argument("--interval", type=float, default=0.15)
    parser.add_argument("--stale-ms", type=int, default=2000)
    args = parser.parse_args()

    url = f"http://{args.ip}/api/v1/showbox/link"
    print(
        f"UART reconnect check {url} soak={args.seconds:.0f}s settle={args.settle:.0f}s "
        f"wait-live={args.wait_live:.0f}s stale>{args.stale_ms}ms"
    )
    print(
        f"{'#':>4}  mark   base  mix   rxB    rxM   initM initB  hbM  hbB  "
        f"entM frmB frmM badM"
    )

    started = time.monotonic()
    live_deadline = started + args.wait_live
    live_at = None
    print("WAIT both sides live...")
    while time.monotonic() < live_deadline:
        code, body = fetch(url, timeout=1.0)
        missing = [k for k in REQUIRED if k not in body]
        base = age(body.get("baseAgeMs"))
        mix = age(body.get("mixerAgeMs"))
        if code == 200 and not missing and base < args.stale_ms and mix < args.stale_ms:
            live_at = time.monotonic()
            print(
                f"LIVE iso={num(body, 'iso')} lock={num(body, 'lock')} "
                f"base={base} mix={mix} initM={num(body, 'initM')} initB={num(body, 'initB')} "
                f"hbM={num(body, 'hbM')} hbB={num(body, 'hbB')} pause={body.get('pause')}"
            )
            break
        time.sleep(args.interval)
    if live_at is None:
        print("FAIL: mixer-base UART never came up")
        return 1

    settle_until = live_at + args.settle
    deadline = settle_until + args.seconds
    i = 0
    http_fail = 0
    stale_fail = 0
    reconnect = 0
    wire_fail = 0
    old_fw = 0
    settled = None
    last_rxb = None
    last_rxm = None
    max_base = 0
    max_mix = 0
    events = []

    while time.monotonic() < deadline:
        i += 1
        t0 = time.monotonic()
        is_settled = t0 >= settle_until
        code, body = fetch(url, timeout=1.0)
        missing = [k for k in REQUIRED if k not in body]
        if code != 200:
            http_fail += 1
        if missing:
            old_fw += 1
        base = age(body.get("baseAgeMs"))
        mix = age(body.get("mixerAgeMs"))
        rx_b = num(body, "rxBase")
        rx_m = num(body, "rxMix")
        init_m = num(body, "initM")
        init_b = num(body, "initB")
        hb_m = num(body, "hbM")
        hb_b = num(body, "hbB")
        ent_m = num(body, "entM")
        drop_b = num(body, "dropBase")
        drop_m = num(body, "dropMix")
        ovf_b = num(body, "ovfBase")
        ovf_m = num(body, "ovfMix")
        frm_b = num(body, "frmBase")
        frm_m = num(body, "frmMix")
        bad_m = num(body, "badM")
        bad_b = num(body, "badB")
        if base < 0x7FFFFFFF:
            max_base = max(max_base, base)
        if mix < 0x7FFFFFFF:
            max_mix = max(max_mix, mix)
        if is_settled and settled is None and not missing and code == 200:
            settled = {
                "initM": init_m,
                "initB": init_b,
                "dropB": drop_b,
                "dropM": drop_m,
                "ovfB": ovf_b,
                "ovfM": ovf_m,
                "frmB": frm_b,
                "frmM": frm_m,
                "badM": bad_m,
                "badB": bad_b,
                "rxB": rx_b,
                "rxM": rx_m,
                "hbM": hb_m,
                "hbB": hb_b,
                "iso": num(body, "iso"),
                "lock": num(body, "lock"),
            }
            print(
                f"SETTLE iso={settled['iso']} lock={settled['lock']} "
                f"initM={init_m} initB={init_b} hbM={hb_m} hbB={hb_b} "
                f"rxB={rx_b} rxM={rx_m} frmB={frm_b} frmM={frm_m} "
                f"dropB={drop_b} dropM={drop_m} badM={bad_m} badB={bad_b}"
            )
            sys.stdout.flush()

        silent = (
            code != 200
            or missing
            or base >= args.stale_ms
            or mix >= args.stale_ms
            or rx_b == 0
            or rx_m == 0
        )
        reinit = (
            is_settled
            and settled is not None
            and (init_m > settled["initM"] or init_b > settled["initB"])
        )
        wire = False
        if is_settled and settled is not None:
            wire = (
                drop_b > settled["dropB"]
                or drop_m > settled["dropM"]
                or ovf_b > settled["ovfB"]
                or ovf_m > settled["ovfM"]
                or frm_b > settled["frmB"]
                or frm_m > settled["frmM"]
                or bad_m > settled["badM"]
                or bad_b > settled["badB"]
            )
        last_rxb = rx_b
        last_rxm = rx_m
        if silent:
            stale_fail += 1
        if reinit:
            reconnect += 1
        if wire:
            wire_fail += 1
        mark = "RECON" if reinit else ("WIRE" if wire else ("DROP" if silent else "ok"))
        if reinit or wire or silent or i == 1 or i % 15 == 0:
            def fmt(v):
                return "never" if v >= 0x7FFFFFFF else str(v)
            print(
                f"{i:4d}  {mark:<5} {fmt(base):>5} {fmt(mix):>5} {rx_b:6d} {rx_m:6d} "
                f"{init_m:5d} {init_b:5d} {hb_m:4d} {hb_b:4d} {ent_m:4d} "
                f"{frm_b:4d} {frm_m:4d} {bad_m:4d}"
            )
            sys.stdout.flush()
        if reinit:
            events.append((i, "RECONNECT", init_m, init_b))
        elif wire:
            events.append((i, "WIRE", frm_b, frm_m))
        remain = args.interval - (time.monotonic() - t0)
        if remain > 0 and time.monotonic() + remain < deadline:
            time.sleep(remain)

    elapsed = time.monotonic() - started
    print()
    print(f"window {elapsed:.1f}s, {i} samples")
    print(f"  http fail      {http_fail}/{i}")
    print(f"  old firmware   {old_fw}/{i}")
    print(f"  silent/stale   {stale_fail}/{i}")
    print(f"  mixer reinits  {reconnect}  (initM after settle {None if settled is None else settled['initM']})")
    print(f"  wire errors    {wire_fail}")
    print(f"  baseAgeMs      max {max_base}")
    print(f"  mixerAgeMs     max {max_mix}")
    if settled is not None:
        print(
            f"  settle snapshot iso={settled['iso']} lock={settled['lock']} "
            f"hbM={settled['hbM']} hbB={settled['hbB']}"
        )
    if events:
        print("  events:")
        for row in events[:12]:
            print(f"    sample {row[0]} {row[1]} a={row[2]} b={row[3]}")
    if http_fail or stale_fail or reconnect or wire_fail or old_fw or settled is None:
        print("FAIL: mixer-base UART (reconnect, silence, wire error, or old firmware)")
        return 1
    print("PASS: no mixer reconnect, both sides stayed live, no wire errors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
