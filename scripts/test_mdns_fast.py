#!/usr/bin/env python3
"""Fast: showbox.local answers unicast, multicast, and Android-style one-shot.

The ESP-IDF mDNS component used to ignore multicast 224.0.0.251 queries
in AP+STA mode. Android DnsResolver (12+) sends one-shot queries from a
high port and requires RFC 6762 §6.7 legacy replies (question echoed,
no cache-flush). This probe catches both regressions in seconds.
"""

from __future__ import annotations

import socket
import struct
import time

from check_live import http_get, mdns_query_a
from live_helpers import HOST, IP, Checks, exit_with


def legacy_oneshot_a(host: str, timeout: float = 1.2) -> tuple[list[str], dict]:
    """High-port query — Android one-shot shape (RFC 6762 §6.7 reply).

    Sent unicast to the module IP so the reply is reliably delivered to this
    socket; multicast TX + unicast RX is what Android uses on the air, but
    some desktop stacks drop that path for the probing process.
    """
    labels = host.rstrip(".").split(".")
    q = bytearray(struct.pack("!HHHHHH", 0xA11A, 0x0000, 1, 0, 0, 0))
    for lab in labels:
        raw = lab.encode("ascii")
        q.append(len(raw))
        q += raw
    q += b"\x00" + struct.pack("!HH", 1, 1)  # A IN

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    meta: dict = {"qd": -1, "flush": None, "tid": None}
    ips: list[str] = []
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("", 0))
        sock.settimeout(timeout)
        sock.sendto(bytes(q), (IP, 5353))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                data, addr = sock.recvfrom(2048)
            except socket.timeout:
                break
            if addr[0] != IP or len(data) < 12:
                continue
            tid, flags, qd, an, _ns, _ar = struct.unpack("!HHHHHH", data[:12])
            if tid != 0xA11A:
                continue
            meta["tid"] = tid
            meta["qd"] = qd
            meta["flags"] = flags
            pos = 12

            def skip_name(p: int) -> int:
                while p < len(data):
                    if data[p] == 0:
                        return p + 1
                    if (data[p] & 0xC0) == 0xC0:
                        return p + 2
                    p += 1 + data[p]
                return p

            for _ in range(qd):
                pos = skip_name(pos) + 4
            for _ in range(an):
                pos = skip_name(pos)
                if pos + 10 > len(data):
                    break
                rtype, rclass, _ttl, rdlen = struct.unpack("!HHIH", data[pos : pos + 10])
                pos += 10
                meta["flush"] = bool(rclass & 0x8000)
                rdata = data[pos : pos + rdlen]
                pos += rdlen
                if rtype == 1 and len(rdata) == 4:
                    ip = socket.inet_ntoa(rdata)
                    if ip not in ips:
                        ips.append(ip)
            if ips:
                break
    except OSError as exc:
        meta["error"] = str(exc)
    finally:
        sock.close()
    return ips, meta


def main() -> None:
    c = Checks()
    uni = mdns_query_a(HOST, IP, timeout=1.0)
    c.check("mDNS unicast A", IP in uni, f"answers={uni}")

    mcast = mdns_query_a(HOST, "224.0.0.251", timeout=1.2, toward=IP)
    c.check("mDNS multicast A", IP in mcast, f"answers={mcast}")

    legacy_ips, meta = legacy_oneshot_a(HOST)
    c.check(
        "mDNS legacy one-shot A",
        IP in legacy_ips and meta.get("qd", 0) >= 1 and meta.get("flush") is False,
        f"answers={legacy_ips} meta={meta}",
    )

    code, body = http_get(f"http://{IP}/services", timeout=2.0, host=HOST)
    c.check(
        "HTTP /services with Host showbox.local",
        code == 200 and len(body) > 100,
        f"code={code} bytes={len(body)}",
    )
    exit_with(c)


if __name__ == "__main__":
    main()
