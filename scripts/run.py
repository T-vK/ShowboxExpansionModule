#!/usr/bin/env python3
"""Leveled live test runner.

  python3 scripts/run.py              # level 1 smoke
  python3 scripts/run.py --level 2
  python3 scripts/run.py --level 3
  python3 scripts/run.py --list
  python3 scripts/run.py --case mdns

MIDI is always real BLE or AppleMIDI/RTP. /api/v1/midi/inject is not used.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

from suite.cases import all_cases, cases_for_level
from suite.context import CaseFail, CaseSkip, Suite
from live_helpers import HOST, IP

SOFTAP_IP = "192.168.4.1"
NET_CHOICES = ("softap", "sta", "ethernet")


def apply_net_defaults(args: argparse.Namespace) -> None:
    """Resolve --ip/--host from --net when the CLI did not override them."""
    if args.ip is None:
        if args.net == "softap":
            args.ip = os.environ.get("SHOWBOX_IP", SOFTAP_IP)
        elif args.net == "ethernet":
            args.ip = os.environ.get("SHOWBOX_ETH", os.environ.get("SHOWBOX_IP", IP))
        else:
            args.ip = os.environ.get("SHOWBOX_IP", IP)
    if args.host is None:
        args.host = os.environ.get("SHOWBOX_HOST", HOST)


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Showbox leveled live tests")
    p.add_argument("--level", type=int, default=1, choices=(1, 2, 3), help="1 smoke, 2 correctness, 3 load")
    p.add_argument("--case", action="append", default=[], help="run only these named cases (repeatable)")
    p.add_argument("--list", action="store_true", help="print case names and exit")
    net_default = os.environ.get("SHOWBOX_NET", "softap")
    if net_default not in NET_CHOICES:
        net_default = "softap"
    p.add_argument("--net", choices=NET_CHOICES, default=net_default, help="suite network path (default softap)")
    p.add_argument("--ip", default=None, help="module IPv4 (default depends on --net)")
    p.add_argument("--host", default=None, help="mDNS / Host header (default SHOWBOX_HOST or showbox.local)")
    p.add_argument("--keep-host", dest="keep_host", action="store_true", default=True)
    p.add_argument("--no-keep-host", dest="keep_host", action="store_false")
    p.add_argument("--https", action="store_true", help="require HTTPS checks")
    p.add_argument("--ap", action="store_true", help="enable the hotspot to scan the SSID")
    p.add_argument("--din", action="store_true")
    p.add_argument("--ethernet", action="store_true")
    p.add_argument("--usb", action="store_true")
    p.add_argument("--tablet", action="store_true")
    args = p.parse_args(argv)
    apply_net_defaults(args)
    return args


def main_cases(*names: str, ip: str | None = None, host: str | None = None, extra: list[str] | None = None) -> int:
    """Entry used by the folded fast-script wrappers."""
    argv: list[str] = []
    if ip:
        argv.extend(["--ip", ip])
    if host:
        argv.extend(["--host", host])
    for name in names:
        argv.extend(["--case", name])
    if extra:
        argv.extend(extra)
    return main(argv)


def selected_cases(args: argparse.Namespace) -> list[tuple]:
    catalog = all_cases() if args.case else cases_for_level(args.level)
    if not args.case:
        return catalog
    wanted = [name.replace("_", "-") for name in args.case]
    by_name = {name: fn for name, fn in all_cases()}
    missing = [name for name in wanted if name not in by_name]
    if missing:
        print("unknown cases:", ", ".join(missing))
        print("known:", ", ".join(name for name, _fn in all_cases()))
        sys.exit(2)
    return [(name, by_name[name]) for name in wanted]


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    if args.list:
        print("level 1:")
        for name, _fn in cases_for_level(1):
            print(f"  {name}")
        print("level 2 extra:")
        l1 = {name for name, _fn in cases_for_level(1)}
        for name, _fn in cases_for_level(2):
            if name not in l1:
                print(f"  {name}")
        print("level 3 extra:")
        l2 = {name for name, _fn in cases_for_level(2)}
        for name, _fn in cases_for_level(3):
            if name not in l2:
                print(f"  {name}")
        return 0

    os.environ["SHOWBOX_IP"] = args.ip
    os.environ["SHOWBOX_HOST"] = args.host

    from test_reminders import print_run_reminders

    print_run_reminders()
    cases = selected_cases(args)
    print(
        f"Showbox live suite  net={args.net}  level={args.level if not args.case else 'pick'}  "
        f"ip={args.ip}  cases={len(cases)}"
    )
    print("MIDI path: real BLE / AppleMIDI RTP  (inject is not a test)")
    suite = Suite(args)
    started = time.monotonic()
    passed = skipped = 0
    try:
        suite.start()
        for name, fn in cases:
            print()
            print("=" * 60)
            print(name)
            print("=" * 60)
            t0 = time.monotonic()
            try:
                fn(suite)
            except CaseSkip as exc:
                elapsed = time.monotonic() - t0
                print(f"-- SKIP {name} in {elapsed:.1f}s  ({exc})")
                skipped += 1
                continue
            except CaseFail as exc:
                elapsed = time.monotonic() - t0
                print(f"-- FAIL {name} in {elapsed:.1f}s  {exc}")
                print()
                print(f"STOP after {time.monotonic() - started:.1f}s")
                return 1
            except Exception as exc:
                elapsed = time.monotonic() - t0
                print(f"-- ERROR {name} in {elapsed:.1f}s  {type(exc).__name__}: {exc}")
                print()
                print(f"STOP after {time.monotonic() - started:.1f}s")
                return 1
            elapsed = time.monotonic() - t0
            print(f"-- PASS {name} in {elapsed:.1f}s")
            passed += 1
    finally:
        suite.close()

    print()
    print(f"ALL PASS in {time.monotonic() - started:.1f}s  ({passed} passed, {skipped} skipped)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
