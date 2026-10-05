#!/usr/bin/env python3
"""Poll the live Showbox module for ~30s and fail if anything flakes.

Checks, every second:
  - ICMP ping to the module IP
  - HTTP /services by IP (must be 200 with a body)
  - HTTP /api/v1/midi JSON: mixer/base ages, linkOk
  - AIRSTEP (or any autoConnect peripheral) stays connected
  - mDNS A record for showbox.local (direct UDP to the module AND multicast)
  - HTTP /services via http://showbox.local
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from typing import Any, Optional

AIRSTEP = "d9:d7:5b:c1:90:f4"
STALE_MS = 800
HTTP_TIMEOUT = 2.0


def now() -> float:
    return time.monotonic()


def ping_ok(ip: str) -> bool:
    try:
        return subprocess.run(
            ["ping", "-c", "1", "-W", "1", ip],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        ).returncode == 0
    except OSError:
        return False


def http_get(url: str, timeout: float = HTTP_TIMEOUT) -> tuple[int, bytes]:
    req = urllib.request.Request(
        url,
        headers={"Connection": "close", "User-Agent": "showbox-check"},
        method="GET",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.getcode() or 0, resp.read()
    except urllib.error.HTTPError as err:
        body = err.read() if err.fp else b""
        return err.code, body
    except Exception:
        return 0, b""


def mdns_query_a(host: str, dest: str, timeout: float = 1.0) -> list[str]:
    """RFC 1035 / mDNS A query. dest is 224.0.0.251 or the module unicast IP."""
    name = host.rstrip(".")
    q = bytearray()
    q += struct.pack("!HHHHHH", 0x1200, 0x0000, 1, 0, 0, 0)
    for label in name.split("."):
        raw = label.encode("ascii")
        q.append(len(raw))
        q += raw
    q += b"\x00"
    q += struct.pack("!HH", 1, 1)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.settimeout(timeout)
        sock.sendto(bytes(q), (dest, 5353))
        answers: list[str] = []
        deadline = now() + timeout
        while now() < deadline:
            try:
                data, _addr = sock.recvfrom(2048)
            except socket.timeout:
                break
            if len(data) < 12:
                continue
            _id, flags, _qd, an, _ns, _ar = struct.unpack("!HHHHHH", data[:12])
            if an == 0 and (flags & 0x8000) == 0:
                continue
            pos = 12
            # skip questions
            qd = struct.unpack("!H", data[4:6])[0]
            for _ in range(qd):
                while pos < len(data) and data[pos] != 0 and (data[pos] & 0xC0) != 0xC0:
                    pos += 1 + data[pos]
                if pos < len(data) and (data[pos] & 0xC0) == 0xC0:
                    pos += 2
                else:
                    pos += 1
                pos += 4
            for _ in range(an):
                if pos >= len(data):
                    break
                if (data[pos] & 0xC0) == 0xC0:
                    pos += 2
                else:
                    while pos < len(data) and data[pos] != 0:
                        pos += 1 + data[pos]
                    pos += 1
                if pos + 10 > len(data):
                    break
                rtype, _rclass, _ttl, rdlen = struct.unpack("!HHIH", data[pos:pos + 10])
                pos += 10
                rdata = data[pos:pos + rdlen]
                pos += rdlen
                if rtype == 1 and rdlen == 4:
                    answers.append(socket.inet_ntoa(rdata))
            if answers:
                break
        return answers
    except OSError:
        return []
    finally:
        sock.close()


def resolver_ips(host: str) -> list[str]:
    # Do not call getaddrinfo: nss-mdns can block for many seconds when
    # showbox.local is dead. Ask avahi / getent with a hard timeout.
    ips: list[str] = []
    for cmd in (
        ["avahi-resolve", "-n", host],
        ["getent", "hosts", host],
    ):
        try:
            out = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=0.7,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired):
            continue
        for line in out.stdout.splitlines():
            for token in line.split():
                if token.count(".") == 3 and token[0].isdigit():
                    if token not in ips:
                        ips.append(token)
    return ips


def same_addr(a: str, b: str) -> bool:
    return a.replace(":", "").lower() == b.replace(":", "").lower()


def find_airstep(status: dict[str, Any]) -> Optional[dict[str, Any]]:
    for group in ("peripherals", "nearby"):
        for peer in status.get(group) or []:
            addr = str(peer.get("address") or "")
            name = str(peer.get("name") or "")
            if same_addr(addr, AIRSTEP) or name.upper() == "AIRSTEP":
                return peer
    return None


def auto_targets(status: dict[str, Any]) -> list[dict[str, Any]]:
    found = []
    for peer in status.get("peripherals") or []:
        if peer.get("autoConnect") and peer.get("midi"):
            found.append(peer)
    return found


def fmt_age(ms: Any) -> str:
    try:
        n = int(ms)
    except (TypeError, ValueError):
        return "?"
    if n < 0 or n >= 0x7FFFFFFF:
        return "never"
    return str(n)


def main() -> int:
    parser = argparse.ArgumentParser(description="Live Showbox health over a window")
    parser.add_argument("--ip", default="192.168.3.55")
    parser.add_argument("--host", default="showbox.local")
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--stale-ms", type=int, default=250)
    args = parser.parse_args()

    print(
        f"checking {args.ip} / {args.host} for {args.seconds:.0f}s "
        f"(every {args.interval:.1f}s, stale>{args.stale_ms}ms is a drop)"
    )
    print(
        f"{'#':>3}  ping  svcIP  midi  link  base  mixr  airstep          "
        f"mdnsU  mdnsM  resolv  svcMDNS"
    )

    ping_fail = 0
    services_ip_fail = 0
    midi_fail = 0
    link_fail = 0
    uart_drops = 0
    airstep_down = 0
    airstep_seen_connected = False
    mdns_unicast_fail = 0
    mdns_mcast_fail = 0
    resolver_fail = 0
    services_mdns_fail = 0
    max_base = 0
    max_mixer = 0
    ages_base: list[int] = []
    ages_mixer: list[int] = []

    started = now()
    deadline = started + args.seconds
    i = 0
    samples = 0
    with ThreadPoolExecutor(max_workers=8) as pool:
      while now() < deadline:
        i += 1
        samples = i
        tick_start = now()
        fut_ping = pool.submit(ping_ok, args.ip)
        fut_svc = pool.submit(http_get, f"http://{args.ip}/services")
        fut_midi = pool.submit(http_get, f"http://{args.ip}/api/v1/midi")
        fut_link = pool.submit(http_get, f"http://{args.ip}/api/v1/showbox/link")
        fut_uni = pool.submit(mdns_query_a, args.host, args.ip, 0.4)
        fut_mcast = pool.submit(mdns_query_a, args.host, "224.0.0.251", 0.4)
        fut_resolv = pool.submit(resolver_ips, args.host)
        ping = fut_ping.result()
        code_ip, body_ip = fut_svc.result()
        code_midi, body_midi = fut_midi.result()
        code_link, body_link = fut_link.result()
        uni = fut_uni.result()
        mcast = fut_mcast.result()
        resolv = fut_resolv.result()
        if not ping:
            ping_fail += 1
        svc_ip_ok = code_ip == 200 and len(body_ip) > 100
        if not svc_ip_ok:
            services_ip_fail += 1

        status: dict[str, Any] = {}
        midi_ok = False
        if code_midi == 200:
            try:
                status = json.loads(body_midi.decode("utf-8", "replace"))
                midi_ok = True
            except json.JSONDecodeError:
                status = {}
        if not midi_ok:
            midi_fail += 1

        link = {}
        if code_link == 200:
            try:
                link = json.loads(body_link.decode("utf-8", "replace"))
            except json.JSONDecodeError:
                link = {}
        ok = bool(link.get("ok"))
        try:
            base = int(link.get("baseAgeMs") or 0xFFFFFFFF)
            mixer = int(link.get("mixerAgeMs") or 0xFFFFFFFF)
        except (TypeError, ValueError):
            base = 0xFFFFFFFF
            mixer = 0xFFFFFFFF
        if base < 0x7FFFFFFF:
            max_base = max(max_base, base)
            ages_base.append(base)
        if mixer < 0x7FFFFFFF:
            max_mixer = max(max_mixer, mixer)
            ages_mixer.append(mixer)
        uart_ok = code_link == 200 and ok and base < args.stale_ms and mixer < args.stale_ms
        if code_link == 200 and not ok:
            link_fail += 1
        if code_link != 200 or not ok or base >= args.stale_ms or mixer >= args.stale_ms:
            uart_drops += 1

        airstep = find_airstep(status)
        autos = auto_targets(status)
        if airstep and airstep.get("connected"):
            airstep_seen_connected = True
            airstep_text = "connected"
        elif airstep:
            airstep_down += 1
            airstep_text = "DOWN auto=" + str(airstep.get("autoConnect"))
        elif autos:
            airstep_down += 1
            airstep_text = "missing"
        else:
            airstep_down += 1
            airstep_text = "no-peer"

        uni_ok = args.ip in uni
        mcast_ok = args.ip in mcast
        resolv_ok = args.ip in resolv
        if not uni_ok:
            mdns_unicast_fail += 1
        if not mcast_ok:
            mdns_mcast_fail += 1
        if not resolv_ok:
            resolver_fail += 1

        if resolv_ok:
            code_mdns, body_mdns = http_get(f"http://{args.host}/services", timeout=1.5)
            svc_mdns_ok = code_mdns == 200 and len(body_mdns) > 100
        else:
            code_mdns, svc_mdns_ok = 0, False
        if not svc_mdns_ok:
            services_mdns_fail += 1

        print(
            f"{i:3d}  {'ok' if ping else 'NO':<4}  "
            f"{code_ip:<5}  {'ok' if midi_ok else code_midi:<4}  "
            f"{'ok' if uart_ok else 'DROP':<4}  "
            f"{fmt_age(base):>4}  {fmt_age(mixer):>4}  "
            f"{airstep_text:<16}  "
            f"{'ok' if uni_ok else 'NO':<5}  "
            f"{'ok' if mcast_ok else 'NO':<5}  "
            f"{'ok' if resolv_ok else 'NO':<6}  "
            f"{code_mdns}"
        )
        sys.stdout.flush()

        remain = args.interval - (now() - tick_start)
        if remain > 0 and now() + remain < deadline:
            time.sleep(remain)

    elapsed = now() - started
    print()
    print(f"window {elapsed:.1f}s, {samples} samples")
    print(f"  ping fail              {ping_fail}/{samples}")
    print(f"  /services via IP fail  {services_ip_fail}/{samples}")
    print(f"  /api/v1/midi fail      {midi_fail}/{samples}")
    print(f"  mixer/base drops       {uart_drops}/{samples}  (linkOk false {link_fail}/{samples})")
    if ages_base:
        print(f"  baseAgeMs              max {max_base}  avg {sum(ages_base)/len(ages_base):.0f}")
    else:
        print("  baseAgeMs              never heard")
    if ages_mixer:
        print(f"  mixerAgeMs             max {max_mixer}  avg {sum(ages_mixer)/len(ages_mixer):.0f}")
    else:
        print("  mixerAgeMs             never heard")
    print(f"  AIRSTEP not connected  {airstep_down}/{samples}  (ever connected: {airstep_seen_connected})")
    print(f"  mDNS unicast to IP     fail {mdns_unicast_fail}/{samples}")
    print(f"  mDNS multicast         fail {mdns_mcast_fail}/{samples}")
    print(f"  local resolver         fail {resolver_fail}/{samples}")
    print(f"  /services via .local   fail {services_mdns_fail}/{samples}")

    failed = []
    if ping_fail:
        failed.append("ping")
    if services_ip_fail:
        failed.append("/services-ip")
    if midi_fail:
        failed.append("midi-api")
    if uart_drops:
        failed.append("mixer-base")
    if airstep_down == samples or not airstep_seen_connected:
        failed.append("airstep-autoconnect")
    if mdns_unicast_fail == samples and mdns_mcast_fail == samples:
        failed.append("mdns-advertise")
    if services_mdns_fail == samples:
        failed.append("showbox.local-http")

    if failed:
        print("FAIL: " + ", ".join(failed))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
