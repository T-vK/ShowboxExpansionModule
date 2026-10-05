#!/usr/bin/env python3
"""Live check for the features that have to work together.

Mixer↔base UART is the highest priority: every other probe is wrapped in a
UART liveness check, and a stale link fails the run even if HTTP looks fine.

  python3 scripts/check_features.py                    # showbox.local
  python3 scripts/check_features.py --ip 192.168.3.55  # skip mDNS lookup
  python3 scripts/check_features.py --no-pair          # skip laptop/tablet HID+MIDI matrix
  python3 scripts/check_features.py --din --ethernet   # only when those cables are plugged in
"""

from __future__ import annotations

import argparse
import json
import os
import random
import re
import select
import socket
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Any, Callable, Optional

BLE_NAME = "ShowboxMod"
AIRSTEP_NAME = "AIRSTEP"
AIRSTEP_ADDR = "d9:d7:5b:c1:90:f4"
MIDI_UUID = "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
HID_UUID = "00001812-0000-1000-8000-00805f9b34fb"
APPLEMIDI_PORT = 5004
TELNET_PORT = 23
OTA_PORT = 3232
STALE_MS = 800
SCAN_STALE_MS = 2000
HTTP_TIMEOUT = 8.0

DEBUG_FATAL = (
    "guru meditation",
    "panic",
    "abort()",
    "brownout",
    "assert failed",
    "corrupt heap",
    "store prohibited",
    "instrfetchprohibited",
    "stack smashing",
    "rst:",
    "guru med",
)

# (id, title, midi, hid, keyboard, mouse, abs, touch)
# Grouped so MIDI on/off can share a HID report map. Unpair only when the
# HOGP descriptor itself changes (mouse vs abs vs touch).
HOST_CASES = (
    ("midi", "MIDI", 1, 0, 0, 0, 0, 0),
    ("hid-kb-mouse", "HID(Keyboard,Mouse)", 0, 1, 1, 1, 0, 0),
    ("midi-hid-kb-mouse", "MIDI+HID(Keyboard,Mouse)", 1, 1, 1, 1, 0, 0),
    ("hid-kb-abs", "HID(Keyboard,AbsMouse)", 0, 1, 1, 0, 1, 0),
    ("midi-hid-kb-abs", "MIDI+HID(Keyboard,AbsMouse)", 1, 1, 1, 0, 1, 0),
    ("hid-touch", "HID(Touch)", 0, 1, 0, 0, 0, 1),
    ("midi-hid-touch", "MIDI+HID(Touch)", 1, 1, 0, 0, 0, 1),
)


def now() -> float:
    return time.monotonic()


def color_enabled(flag: bool) -> bool:
    if not flag:
        return False
    if os.environ.get("NO_COLOR"):
        return False
    if os.environ.get("FORCE_COLOR") or os.environ.get("CLICOLOR_FORCE"):
        return True
    return sys.stdout.isatty()


class Style:
    def __init__(self, enabled: bool) -> None:
        self.on = enabled

    def wrap(self, code: str, text: str) -> str:
        if not self.on:
            return text
        return f"\033[{code}m{text}\033[0m"

    def green(self, text: str) -> str:
        return self.wrap("32", text)

    def red(self, text: str) -> str:
        return self.wrap("31", text)

    def yellow(self, text: str) -> str:
        return self.wrap("33", text)

    def cyan(self, text: str) -> str:
        return self.wrap("36", text)

    def bold(self, text: str) -> str:
        return self.wrap("1", text)

    def dim(self, text: str) -> str:
        return self.wrap("2", text)

    def invert_red(self, text: str) -> str:
        return self.wrap("1;37;41", text)

    def invert_green(self, text: str) -> str:
        return self.wrap("1;37;42", text)


STYLE = Style(False)


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


def http_get(url: str, timeout: float = HTTP_TIMEOUT, host: Optional[str] = None) -> tuple[int, bytes]:
    headers = {"Connection": "close", "User-Agent": "showbox-features", "Cache-Control": "no-store"}
    if host:
        headers["Host"] = host
    req = urllib.request.Request(url, headers=headers, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.getcode() or 0, resp.read()
    except urllib.error.HTTPError as err:
        return err.code, err.read() if err.fp else b""
    except Exception:
        return 0, b""


def http_get_retry(url: str, timeout: float = HTTP_TIMEOUT, host: Optional[str] = None, attempts: int = 3) -> tuple[int, bytes]:
    last = (0, b"")
    for i in range(attempts):
        last = http_get(url, timeout=timeout, host=host)
        if last[0] == 200 and len(last[1]) > 1000:
            return last
        if i + 1 < attempts:
            time.sleep(0.4)
    return last


def http_post(url: str, fields: Optional[dict[str, str]] = None, timeout: float = HTTP_TIMEOUT) -> tuple[int, str]:
    body = urllib.parse.urlencode(fields or {}).encode()
    req = urllib.request.Request(
        url,
        data=body,
        headers={
            "Connection": "close",
            "User-Agent": "showbox-features",
            "Content-Type": "application/x-www-form-urlencoded",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.getcode() or 0, resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as err:
        raw = err.read() if err.fp else b""
        return err.code, raw.decode("utf-8", "replace")
    except Exception as exc:
        return 0, str(exc)


def http_json(url: str, timeout: float = HTTP_TIMEOUT) -> tuple[int, dict[str, Any]]:
    last_code = 0
    last_raw = b""
    for attempt in range(3):
        last_code, last_raw = http_get(url, timeout=timeout)
        if last_code != 200:
            if attempt + 1 < 3:
                time.sleep(0.3)
            continue
        try:
            data = json.loads(last_raw.decode("utf-8", "replace"))
        except json.JSONDecodeError:
            if attempt + 1 < 3:
                time.sleep(0.3)
            continue
        return last_code, data if isinstance(data, dict) else {}
    return last_code, {}


def mdns_query(name: str, rtype: int, dest: str, timeout: float = 0.6) -> list[bytes]:
    q = bytearray()
    q += struct.pack("!HHHHHH", 0x1200, 0x0000, 1, 0, 0, 0)
    for label in name.rstrip(".").split("."):
        raw = label.encode("ascii")
        q.append(len(raw))
        q += raw
    q += b"\x00"
    q += struct.pack("!HH", rtype, 1)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.settimeout(timeout)
        sock.sendto(bytes(q), (dest, 5353))
        records: list[bytes] = []
        deadline = now() + timeout
        while now() < deadline:
            try:
                data, _addr = sock.recvfrom(2048)
            except socket.timeout:
                break
            if len(data) < 12:
                continue
            _id, flags, qd, an, _ns, _ar = struct.unpack("!HHHHHH", data[:12])
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
            for _ in range(an + _ar):
                pos = skip_name(pos)
                if pos + 10 > len(data):
                    break
                got_type, _rclass, _ttl, rdlen = struct.unpack("!HHIH", data[pos:pos + 10])
                pos += 10
                rdata = data[pos:pos + rdlen]
                pos += rdlen
                if got_type == rtype:
                    records.append(rdata)
        return records
    except OSError:
        return []
    finally:
        sock.close()


def mdns_query_a(host: str, dest: str, timeout: float = 0.6) -> list[str]:
    out: list[str] = []
    for rdata in mdns_query(host, 1, dest, timeout):
        if len(rdata) == 4:
            out.append(socket.inet_ntoa(rdata))
    return out


def mdns_has_ptr(service: str, dests: list[str]) -> bool:
    for dest in dests:
        if mdns_query(service, 12, dest, 0.7):
            return True
    return False


def resolver_ips(host: str) -> list[str]:
    ips: list[str] = []
    for cmd in (["avahi-resolve", "-n", host], ["getent", "hosts", host]):
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=1.5, check=False)
        except (OSError, subprocess.TimeoutExpired):
            continue
        for line in out.stdout.splitlines():
            for token in line.split():
                if token.count(".") == 3 and token[0].isdigit() and token not in ips:
                    ips.append(token)
    return ips


def lookup_ipv4(host: str) -> list[str]:
    ips: list[str] = []

    def add(ip: str) -> None:
        if ip and ip not in ips and ip.count(".") == 3 and ip[0].isdigit():
            ips.append(ip)

    for ip in resolver_ips(host):
        add(ip)
    for ip in mdns_query_a(host, "224.0.0.251", 0.7):
        add(ip)
    try:
        for info in socket.getaddrinfo(host, None, family=socket.AF_INET, type=socket.SOCK_STREAM):
            add(info[4][0])
    except OSError:
        pass
    return ips


def pick_target(host: str, ip: Optional[str]) -> tuple[str, str]:
    """Return (connect, note). connect is an IPv4 or the hostname if lookup failed."""
    if ip:
        return ip, f"--ip {ip}"
    found = lookup_ipv4(host)
    if found:
        return found[0], f"{host} → {found[0]}"
    return host, f"{host} (no A record yet, trying the name)"


def applemidi_invite(ip: str, port: int = APPLEMIDI_PORT, timeout: float = 1.5) -> str:
    token = random.randint(1, 0xFFFFFFFE)
    ssrc = random.randint(1, 0xFFFFFFFE)
    pkt = struct.pack("!HHIII", 0xFFFF, 0x494E, 2, token, ssrc) + b"showbox-check\x00"
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.settimeout(timeout)
        sock.sendto(pkt, (ip, port))
        data, _addr = sock.recvfrom(256)
    except socket.timeout:
        return ""
    except OSError as exc:
        return f"error:{exc}"
    finally:
        sock.close()
    if len(data) < 4 or data[0] != 0xFF or data[1] != 0xFF:
        return f"junk:{data[:8]!r}"
    return data[2:4].decode("ascii", "replace")


def udp_open(ip: str, port: int) -> bool:
    """True when a UDP send does not get an immediate ICMP port-unreachable."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.settimeout(0.4)
        sock.sendto(b"9\n", (ip, port))
        try:
            sock.recvfrom(64)
        except socket.timeout:
            return True
        except OSError:
            return False
        return True
    except OSError:
        return False
    finally:
        sock.close()


def telnet_messages(ip: str, port: int = TELNET_PORT, want: int = 2, timeout: float = 6.0) -> tuple[bool, str]:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        sock.connect((ip, port))
        sock.settimeout(1.0)
        chunks = b""
        deadline = now() + timeout
        while now() < deadline:
            try:
                part = sock.recv(512)
            except socket.timeout:
                if chunks.count(b"\n") >= want:
                    break
                continue
            if not part:
                break
            chunks += part
            if chunks.count(b"\n") >= want or len(chunks) >= 400:
                break
        sock.close()
        text = chunks.decode("utf-8", "replace").replace("\r", "")
        lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
        if not lines:
            return False, "connected but no banner or log line"
        preview = " | ".join(lines[:want])[:120]
        return True, f"{len(lines)} line(s): {preview}"
    except OSError as exc:
        try:
            sock.close()
        except OSError:
            pass
        return False, str(exc)


def strip_ansi(text: str) -> str:
    return re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text)


def run_cmd(args: list[str], timeout: float = 8.0, input_text: Optional[str] = None) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            args,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
            input=input_text,
        )
    except subprocess.TimeoutExpired as exc:
        out = exc.stdout or ""
        err = exc.stderr or ""
        if isinstance(out, bytes):
            out = out.decode("utf-8", "replace")
        if isinstance(err, bytes):
            err = err.decode("utf-8", "replace")
        return subprocess.CompletedProcess(args, 124, out, (err + f"\ntimeout after {timeout}s").strip())


def bt_scan_off() -> None:
    """Leave the adapter idle. A leftover LE scan crowds Wi-Fi/UART/HOGP."""
    try:
        run_cmd(["bluetoothctl", "scan", "off"], timeout=3)
    except (OSError, subprocess.TimeoutExpired):
        pass
    try:
        subprocess.run(
            [
                "busctl",
                "--system",
                "call",
                "org.bluez",
                "/org/bluez/hci0",
                "org.bluez.Adapter1",
                "StopDiscovery",
            ],
            capture_output=True,
            timeout=3,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        pass
    try:
        subprocess.run(
            [
                "busctl",
                "--system",
                "call",
                "org.bluez",
                "/org/bluez/hci0",
                "org.bluez.Adapter1",
                "SetDiscoveryFilter",
                "a{sv}",
                "0",
            ],
            capture_output=True,
            timeout=3,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        pass


def bt_disconnect_named(exact: str) -> None:
    """Drop a Classic ACL that would share the adapter with HOGP pairing."""
    try:
        out = run_cmd(["bluetoothctl", "devices", "Connected"], timeout=3).stdout or ""
    except (OSError, subprocess.TimeoutExpired):
        return
    want = exact.lower()
    for line in strip_ansi(out).splitlines():
        m = re.search(r"Device\s+([0-9A-Fa-f:]{17})\s+(.+)$", line)
        if not m:
            continue
        if m.group(2).strip().lower() != want:
            continue
        run_cmd(["bluetoothctl", "disconnect", m.group(1)], timeout=6)


def host_le_scan(seconds: float = 8.0) -> tuple[str, str, Optional[str]]:
    """Scan from this machine for ShowboxMod. Returns (status, detail, address)."""
    try:
        out = subprocess.run(
            ["timeout", f"{seconds + 2:.0f}", "bluetoothctl", "--timeout", f"{seconds:.0f}", "scan", "le"],
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        return "skip", "bluetoothctl not available", None
    try:
        extra = run_cmd(["bluetoothctl", "devices"], timeout=2).stdout or ""
    except (OSError, subprocess.TimeoutExpired):
        extra = ""
    bt_scan_off()
    blob = strip_ansi((out.stdout or "") + "\n" + (out.stderr or "") + "\n" + extra)
    if "No default controller" in blob or "No controller available" in blob:
        return "skip", "no Bluetooth adapter", None
    addr = None
    for line in blob.splitlines():
        m = re.search(r"Device\s+([0-9A-Fa-f:]{17})\s+ShowboxMod\b", line)
        if m:
            addr = m.group(1)
            break
    low = blob.lower()
    if addr or BLE_NAME.lower() in low:
        return "pass", f"saw {BLE_NAME}" + (f" {addr}" if addr else ""), addr
    if MIDI_UUID in low:
        return "pass", "saw BLE MIDI UUID", None
    if "discovery started" in low or "set-scan-enable" in low or "scanning" in low or "device " in low:
        return "fail", f"{BLE_NAME} not seen in {seconds:.0f}s host LE scan", None
    if out.returncode != 0 and "timeout" not in (out.stderr or "").lower():
        return "skip", (out.stderr or out.stdout or "bluetoothctl failed")[:120], None
    return "fail", f"{BLE_NAME} not seen in {seconds:.0f}s host LE scan", None


def parse_heap(debug: str) -> tuple[Optional[int], Optional[int]]:
    heap = None
    largest = None
    for line in debug.splitlines():
        for token in line.split():
            if token.startswith("heap=") and token[5:].isdigit():
                heap = int(token[5:])
            if token.startswith("largest=") and token[8:].isdigit():
                largest = int(token[8:])
    return heap, largest


def age(value: Any) -> int:
    try:
        n = int(value)
    except (TypeError, ValueError):
        return 0x7FFFFFFF
    if n < 0:
        return 0x7FFFFFFF
    return n


def uart_ok(link: dict[str, Any], stale_ms: int) -> tuple[bool, str]:
    if not link:
        return False, "no /link body"
    base = age(link.get("baseAgeMs"))
    mix = age(link.get("mixerAgeMs"))
    hb = age(link.get("heartbeatAgeMs"))
    snap = (
        f"ok={link.get('ok')} base={base} mix={mix} hb={hb} "
        f"rxB={link.get('rxBase')} rxM={link.get('rxMix')} "
        f"dropB={link.get('dropBase')} dropM={link.get('dropMix')} "
        f"ovfB={link.get('ovfBase')} ovfM={link.get('ovfMix')}"
    )
    if base >= stale_ms or mix >= stale_ms:
        return False, f"stale {snap} (>{stale_ms}ms)"
    fifo_b = int(link.get("fifoBase") or 0)
    fifo_m = int(link.get("fifoMix") or 0)
    if fifo_b > 96 or fifo_m > 96:
        return False, f"fifo base={fifo_b} mix={fifo_m} {snap}"
    return True, snap


def uart_ok_sampled(session: Session, stale_ms: int, samples: int = 4, gap: float = 0.2) -> tuple[bool, str]:
    """Pass if any fresh /link sample is live. One HTTP snapshot can land between heartbeats."""
    last = (False, "no /link body")
    for _ in range(max(1, samples)):
        last = uart_ok(session.link(), stale_ms)
        if last[0]:
            return last
        time.sleep(gap)
    return last


class Results:
    def __init__(self) -> None:
        self.rows: list[tuple[str, str, str, str]] = []
        self._section = "core"
        self._section_start = 0
        self.uart_up: Optional[bool] = None

    def section(self, title: str) -> None:
        self._section_footer()
        self._section = title
        self._section_start = len(self.rows)
        print()
        print(STYLE.bold(STYLE.cyan(f"── {title} " + "─" * max(8, 58 - len(title)))))
        sys.stdout.flush()

    def _section_footer(self) -> None:
        chunk = self.rows[self._section_start :]
        if not chunk:
            return
        passed = sum(1 for s, *_ in chunk if s == "PASS")
        failed = sum(1 for s, *_ in chunk if s == "FAIL")
        skipped = sum(1 for s, *_ in chunk if s == "SKIP")
        bits = [STYLE.green(f"{passed} pass")]
        if failed:
            bits.append(STYLE.red(f"{failed} fail"))
        if skipped:
            bits.append(STYLE.yellow(f"{skipped} skip"))
        print(STYLE.dim("   └ ") + "  ".join(bits))
        sys.stdout.flush()

    def _emit(self, status: str, name: str, detail: str) -> None:
        self.rows.append((status, self._section, name, detail))
        if status == "PASS":
            glyph, mark = STYLE.green("✓"), STYLE.green("PASS")
        elif status == "FAIL":
            glyph, mark = STYLE.red("✗"), STYLE.red(STYLE.bold("FAIL"))
        else:
            glyph, mark = STYLE.yellow("○"), STYLE.yellow("SKIP")
        extra = STYLE.dim(detail) if detail else ""
        print(f"  {glyph} {mark}  {name:<34} {extra}".rstrip())
        sys.stdout.flush()

    def pass_(self, name: str, detail: str = "") -> None:
        self._emit("PASS", name, detail)

    def fail(self, name: str, detail: str = "") -> None:
        self._emit("FAIL", name, detail)

    def skip(self, name: str, detail: str = "") -> None:
        self._emit("SKIP", name, detail)

    def failed(self) -> list[str]:
        return [name for status, _sec, name, _d in self.rows if status == "FAIL"]

    def summary(self) -> int:
        self._section_footer()
        passed = sum(1 for s, *_ in self.rows if s == "PASS")
        failed = sum(1 for s, *_ in self.rows if s == "FAIL")
        skipped = sum(1 for s, *_ in self.rows if s == "SKIP")
        print()
        print(STYLE.bold("  By section"))
        seen: list[str] = []
        for _s, sec, _n, _d in self.rows:
            if sec not in seen:
                seen.append(sec)
        for sec in seen:
            chunk = [row for row in self.rows if row[1] == sec]
            p = sum(1 for s, *_ in chunk if s == "PASS")
            f = sum(1 for s, *_ in chunk if s == "FAIL")
            k = sum(1 for s, *_ in chunk if s == "SKIP")
            parts = [STYLE.green(f"{p} pass")]
            if f:
                parts.append(STYLE.red(f"{f} fail"))
            if k:
                parts.append(STYLE.yellow(f"{k} skip"))
            print(f"    {sec:<22} " + "  ".join(parts))
        print()
        bar = "━" * 64
        line = f" {passed} passed   {failed} failed   {skipped} skipped "
        if failed:
            print(STYLE.red(bar))
            print(STYLE.invert_red(line.center(64)))
            print(STYLE.red(bar))
            print()
            print(STYLE.red(STYLE.bold("  What failed")))
            for status, sec, name, detail in self.rows:
                if status == "FAIL":
                    print(STYLE.red(f"    ✗  {sec} / {name}"))
                    if detail:
                        print(STYLE.dim(f"       {detail}"))
            self._print_skips()
            return 1
        print(STYLE.green(bar))
        print(STYLE.invert_green(line.center(64)))
        print(STYLE.green(bar))
        self._print_skips()
        return 0

    def _print_skips(self) -> None:
        skips = [(sec, name, detail) for s, sec, name, detail in self.rows if s == "SKIP"]
        if not skips:
            return
        print()
        print(STYLE.yellow("  Skipped (not a failure)"))
        for sec, name, detail in skips:
            print(STYLE.dim(f"    ○  {sec} / {name}  {detail}"))


class Session:
    def __init__(self, ip: str, host: str, stale_ms: int) -> None:
        self.ip = ip
        self.host = host
        self.stale_ms = stale_ms
        self.base = f"http://{ip}"

    def link(self) -> dict[str, Any]:
        _code, body = http_json(f"{self.base}/api/v1/showbox/link")
        return body

    def midi(self) -> dict[str, Any]:
        _code, body = http_json(f"{self.base}/api/v1/midi")
        return body

    def debug(self) -> str:
        _code, raw = http_get(f"{self.base}/api/v1/debug")
        return raw.decode("utf-8", "replace")

    def assert_uart(
        self,
        results: Results,
        name: str,
        stale_ms: Optional[int] = None,
        quiet: bool = True,
    ) -> bool:
        ok, detail = uart_ok_sampled(self, stale_ms or self.stale_ms)
        if not ok:
            recover_until = now() + 12.0
            while now() < recover_until:
                time.sleep(0.4)
                ok, detail = uart_ok_sampled(self, stale_ms or self.stale_ms, samples=2, gap=0.15)
                if ok:
                    break
        if ok:
            if results.uart_up is False:
                results.pass_(name, "recovered  " + detail)
            elif not quiet:
                results.pass_(name, detail)
            results.uart_up = True
            return True
        if results.uart_up is not False:
            results.fail(name, f"mixer↔base dropped: {detail}")
        results.uart_up = False
        return False

    def post_settings(self, **fields: Any) -> tuple[int, str]:
        data = {k: ("1" if v is True else "0" if v is False else str(v)) for k, v in fields.items()}
        return http_post(f"{self.base}/api/v1/midi/settings", data)


def transport(midi: dict[str, Any], ident: str) -> dict[str, Any]:
    for row in midi.get("transports") or []:
        if row.get("id") == ident:
            return row
    return {}


def connected_hosts(midi: dict[str, Any]) -> list[dict[str, Any]]:
    return [h for h in (midi.get("hosts") or []) if h.get("connected")]


def auto_peripherals(midi: dict[str, Any]) -> list[dict[str, Any]]:
    found = []
    for peer in midi.get("peripherals") or []:
        if peer.get("autoConnect") and peer.get("midi"):
            found.append(peer)
    return found


def find_airstep(midi: dict[str, Any]) -> Optional[str]:
    needle = AIRSTEP_NAME.lower()
    want = AIRSTEP_ADDR.lower()
    for group in ("peripherals", "nearby", "hosts"):
        for row in midi.get(group) or []:
            name = str(row.get("name") or "")
            addr = str(row.get("address") or "")
            if needle in name.lower() or addr.lower() == want:
                return addr or AIRSTEP_ADDR
    try:
        out = run_cmd(["bluetoothctl", "devices"], timeout=3).stdout or ""
    except (OSError, subprocess.TimeoutExpired):
        out = ""
    for line in strip_ansi(out).splitlines():
        m = re.search(r"Device\s+([0-9A-Fa-f:]{17})\s+AIRSTEP\b", line, re.I)
        if m:
            return m.group(1)
    return None


def ensure_airstep(session: Session, results: Results) -> None:
    midi = session.midi()
    up = [p for p in auto_peripherals(midi) if p.get("connected")]
    if up:
        return
    print(STYLE.dim(f"  pairing {AIRSTEP_NAME} as an auto-connect MIDI pedal"))
    addr = find_airstep(midi)
    if not addr:
        http_post(f"{session.base}/api/v1/midi/ble/scan")
        deadline = now() + 10
        while now() < deadline and not addr:
            time.sleep(0.8)
            midi = session.midi()
            addr = find_airstep(midi)
    if not addr:
        addr = AIRSTEP_ADDR
    code, text = http_post(
        f"{session.base}/api/v1/midi/ble/connect",
        {"address": addr, "name": AIRSTEP_NAME, "midi": "1", "hid": "0", "pair": "1"},
    )
    if code != 200:
        results.fail("airstep-pair", f"connect http={code} {text[:80]}")
        return
    http_post(f"{session.base}/api/v1/midi/ble/auto", {"address": addr, "enabled": "1"})
    deadline = now() + 20
    while now() < deadline:
        midi = session.midi()
        up = [p for p in auto_peripherals(midi) if p.get("connected")]
        if up:
            results.pass_("airstep-pair", f"auto-connect { [p.get('name') or p.get('address') for p in up] }")
            return
        time.sleep(0.8)
    results.fail("airstep-pair", f"saved {addr} but not connected after 20s")


def bt_script(lines: list[str], timeout: float = 25.0) -> str:
    payload = "\n".join(lines) + "\nquit\n"
    try:
        proc = subprocess.run(
            ["bluetoothctl"],
            input=payload,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return str(exc)
    return strip_ansi((proc.stdout or "") + "\n" + (proc.stderr or ""))


def bt_find_showbox() -> Optional[str]:
    try:
        out = run_cmd(["bluetoothctl", "devices"], timeout=3).stdout or ""
    except (OSError, subprocess.TimeoutExpired):
        return None
    for line in strip_ansi(out).splitlines():
        m = re.search(r"Device\s+([0-9A-Fa-f:]{17})\s+ShowboxMod\b", line)
        if m:
            return m.group(1)
    return None


def bt_info(addr: str) -> str:
    try:
        return strip_ansi(run_cmd(["bluetoothctl", "info", addr], timeout=4).stdout or "")
    except (OSError, subprocess.TimeoutExpired):
        return ""


def bt_connected(addr: str) -> bool:
    info = bt_info(addr)
    return "Connected: yes" in info


def laptop_pair(addr: str) -> tuple[bool, str]:
    run_cmd(["bluetoothctl", "power", "on"], timeout=4)
    run_cmd(["bluetoothctl", "pairable", "on"], timeout=4)
    run_cmd(["bluetoothctl", "agent", "NoInputNoOutput"], timeout=4)
    run_cmd(["bluetoothctl", "default-agent"], timeout=4)
    last_reason = "no pair result"
    for attempt in range(3):
        bt_disconnect_named("ShowBox")
        info = bt_info(addr)
        if "Paired: yes" in info or "Bonded: yes" in info or "Connected: yes" in info:
            laptop_disconnect(addr, forget=True)
            time.sleep(2.0)
        run_cmd(["bluetoothctl", "scan", "on"], timeout=4)
        seen = now() + 6
        while now() < seen:
            blob = (bt_find_showbox() or "") + "\n" + bt_info(addr)
            if addr.lower() in blob.lower() and ("RSSI" in blob or "ShowboxMod" in blob):
                break
            time.sleep(0.6)
        pair = run_cmd(["bluetoothctl", "pair", addr], timeout=25)
        pair_blob = strip_ansi((pair.stdout or "") + "\n" + (pair.stderr or ""))
        run_cmd(["bluetoothctl", "trust", addr], timeout=4)
        if not bt_connected(addr):
            run_cmd(["bluetoothctl", "connect", addr], timeout=15)
        deadline = now() + 10
        while now() < deadline:
            info = bt_info(addr)
            if "Connected: yes" in info:
                bt_scan_off()
                how = "pair+connect"
                if "Pairing successful" in pair_blob or "Paired: yes" in info:
                    how = "pairing successful"
                if attempt:
                    how += f" attempt {attempt + 1}"
                return True, how
            time.sleep(0.4)
        info = bt_info(addr)
        paired = "Paired: yes" in info
        connected = "Connected: yes" in info
        last_reason = "no pair result"
        for line in pair_blob.splitlines():
            low = line.lower()
            if "fail" in low or "error" in low or "pairing" in low:
                last_reason = line.strip()
                break
        if paired and connected:
            bt_scan_off()
            return True, f"pairing successful attempt {attempt + 1}"
        laptop_disconnect(addr, forget=True)
        time.sleep(1.5)
    bt_scan_off()
    info = bt_info(addr)
    paired = "Paired: yes" in info
    connected = "Connected: yes" in info
    return False, f"paired={int(paired)} connected={int(connected)} ({last_reason})"


def laptop_reconnect(mod_addr: str) -> None:
    bt_scan_off()
    run_cmd(["bluetoothctl", "disconnect", mod_addr], timeout=6)
    time.sleep(1.5)
    run_cmd(["bluetoothctl", "connect", mod_addr], timeout=12)
    deadline = now() + 8
    while now() < deadline:
        if bt_connected(mod_addr):
            return
        time.sleep(0.4)


def laptop_disconnect(addr: str, forget: bool) -> None:
    run_cmd(["bluetoothctl", "disconnect", addr], timeout=8)
    if forget:
        run_cmd(["bluetoothctl", "untrust", addr], timeout=4)
        run_cmd(["bluetoothctl", "remove", addr], timeout=6)
        wipe_hogp_cache(addr)


def wipe_hogp_cache(addr: str) -> None:
    """Drop BlueZ's cached HID report map so the next pair re-reads GATT."""
    folder = addr.upper()
    root = "/var/lib/bluetooth"
    try:
        adapters = os.listdir(root)
    except OSError:
        return
    for adapter in adapters:
        path = os.path.join(root, adapter, folder)
        if os.path.isdir(path):
            try:
                subprocess.run(["rm", "-rf", path], timeout=3, check=False, capture_output=True)
            except (OSError, subprocess.TimeoutExpired):
                pass


def hid_descriptor_key(case: tuple[Any, ...]) -> Optional[tuple[int, int, int]]:
    """HOGP report-map shape. MIDI-only is None: it must not rewrite the map."""
    _cid, _title, _midi, hid, kb, mouse, abs_p, dig = case
    if not hid:
        return None
    return (int(bool(kb)), int(bool(mouse)), int(bool(abs_p or dig)))


def wait_uart_ready(session: Session, stale_ms: int, timeout: float = 25.0) -> tuple[bool, str]:
    deadline = now() + timeout
    last = (False, "no sample")
    while now() < deadline:
        last = uart_ok_sampled(session, stale_ms, samples=2, gap=0.2)
        if last[0]:
            return last
        time.sleep(0.4)
    return last


def wait_no_hosts(session: Session, timeout: float = 8.0) -> bool:
    deadline = now() + timeout
    while now() < deadline:
        if not connected_hosts(session.midi()):
            time.sleep(0.7)
            return True
        time.sleep(0.3)
    return not connected_hosts(session.midi())


def laptop_forget(session: Session, addr: str) -> None:
    """Drop the BlueZ bond and HOGP cache before a new report map is advertised."""
    wait_uart_ready(session, SCAN_STALE_MS, 20.0)
    laptop_disconnect(addr, forget=True)
    bt_scan_off()
    wait_no_hosts(session)
    session.post_settings(acceptNewHosts=True)


def laptop_rebond(session: Session, addr: str) -> tuple[bool, str]:
    # Pair only. Caller must forget, then apply the new mix, then call this so
    # the host reads the updated HOGP report map instead of blePairMix.
    session.post_settings(acceptNewHosts=True)
    paired, how = laptop_pair(addr)
    if not paired:
        return False, how
    host = wait_host(session, 18)
    if host is None:
        return False, f"{how}; module never listed a connected host"
    names = wait_evdev(8.0, need_mouse=False)
    return True, f"{how} evdev={','.join(names) or 'none'}"


def tablet_forget(session: Session, serial: str) -> None:
    wait_uart_ready(session, SCAN_STALE_MS, 20.0)
    tablet_try_unpair(serial)
    wait_no_hosts(session)
    session.post_settings(acceptNewHosts=True)


def tablet_rebond(session: Session, serial: str) -> tuple[bool, str]:
    session.post_settings(acceptNewHosts=True)
    time.sleep(1.0)
    ok, how = tablet_try_pair(serial)
    host = wait_host(session, 16 if ok else 10)
    if host is None:
        return False, how
    hogp_deadline = now() + 12
    while now() < hogp_deadline and not tablet_showbox_connected(serial):
        tablet_confirm_pair_dialog(serial)
        time.sleep(0.8)
    hogp = tablet_showbox_connected(serial)
    return True, how + (" hogp" if hogp else " no-hogp")


def evdev_showbox() -> list[Any]:
    try:
        from evdev import InputDevice, list_devices
    except ImportError:
        return []
    found = []
    for path in list_devices():
        try:
            dev = InputDevice(path)
        except OSError:
            continue
        if BLE_NAME.lower() in (dev.name or "").lower():
            found.append(dev)
    return found


def evdev_names() -> list[str]:
    return [d.name for d in evdev_showbox()]


def evdev_has_mouse(names: Optional[list[str]] = None) -> bool:
    blob = " ".join(n.lower() for n in (names if names is not None else evdev_names()))
    return "mouse" in blob


def wait_evdev(timeout: float, need_mouse: bool = False) -> list[str]:
    deadline = now() + timeout
    names: list[str] = []
    while now() < deadline:
        names = evdev_names()
        if names and (not need_mouse or evdev_has_mouse(names)):
            return names
        time.sleep(0.3)
    return names


def wait_laptop_hid(addr: str) -> list[str]:
    names = wait_evdev(8.0, need_mouse=True)
    if evdev_has_mouse(names):
        return names
    print(STYLE.dim("  HOGP has no mouse node yet; reconnecting so BlueZ re-reads reports"))
    laptop_reconnect(addr)
    return wait_evdev(10.0, need_mouse=True)


def evdev_collect(seconds: float = 2.0) -> list[tuple[str, int, int]]:
    """[(device_name, ev_type, ev_code), ...] while the HID report is in flight."""
    try:
        from evdev import ecodes
    except ImportError:
        return []
    devices = evdev_showbox()
    if not devices:
        return []
    fds = {dev.fd: dev for dev in devices}
    got: list[tuple[str, int, int]] = []
    deadline = now() + seconds
    try:
        while now() < deadline:
            readable, _, _ = select.select(list(fds.keys()), [], [], max(0.05, deadline - now()))
            for fd in readable:
                dev = fds[fd]
                try:
                    for event in dev.read():
                        if event.type in (ecodes.EV_KEY, ecodes.EV_REL, ecodes.EV_ABS):
                            got.append((dev.name, event.type, event.code))
                except OSError:
                    continue
    finally:
        for dev in devices:
            try:
                dev.close()
            except OSError:
                pass
    return got


def classify_hid_events(events: list[tuple[str, int, int]]) -> dict[str, bool]:
    try:
        from evdev import ecodes
    except ImportError:
        return {"keyboard": False, "mouse": False, "abs": False, "touch": False}
    key = rel = abs_xy = mt = False
    for _name, typ, code in events:
        if typ == ecodes.EV_KEY and code not in (ecodes.BTN_TOUCH, getattr(ecodes, "BTN_TOOL_FINGER", -1)):
            key = True
        if typ == ecodes.EV_REL:
            rel = True
        if typ == ecodes.EV_ABS:
            if code in (ecodes.ABS_MT_POSITION_X, ecodes.ABS_MT_POSITION_Y, getattr(ecodes, "ABS_MT_SLOT", -2)):
                mt = True
            if code in (ecodes.ABS_X, ecodes.ABS_Y):
                abs_xy = True
        if typ == ecodes.EV_KEY and code == ecodes.BTN_TOUCH:
            mt = True
    return {"keyboard": key, "mouse": rel, "abs": abs_xy, "touch": mt}


def alsa_midi_ports() -> list[str]:
    try:
        out = run_cmd(["aseqdump", "-l"], timeout=3).stdout or ""
    except (OSError, subprocess.TimeoutExpired):
        return []
    ports = []
    for line in out.splitlines():
        low = line.lower()
        if BLE_NAME.lower() in low or ("ble" in low and "midi" in low):
            ports.append(line.strip())
    return ports


def aseqdump_start(seconds: float = 1.8) -> Optional[subprocess.Popen[str]]:
    ports = alsa_midi_ports()
    args = ["timeout", f"{seconds:.1f}", "aseqdump"]
    if ports:
        m = re.search(r"(\d+:\d+)", ports[0])
        if m:
            args += ["-p", m.group(1)]
    try:
        return subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    except OSError:
        return None


def aseqdump_heard(proc: Optional[subprocess.Popen[str]]) -> bool:
    if proc is None:
        return False
    try:
        out, _err = proc.communicate(timeout=2.5)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _err = proc.communicate()
    blob = (out or "").lower()
    return "note on" in blob or "note off" in blob


def during_laptop(action: Callable[[], None]) -> dict[str, bool]:
    midi_proc = aseqdump_start(2.2)
    pool = ThreadPoolExecutor(max_workers=1)
    hid_fut = pool.submit(evdev_collect, 2.0)
    time.sleep(0.2)
    try:
        action()
    finally:
        hid_events = hid_fut.result()
        pool.shutdown(wait=True)
    saw = classify_hid_events(hid_events)
    saw["midi"] = aseqdump_heard(midi_proc)
    return saw


def during_tablet(serial: str) -> Callable[[Callable[[], None]], dict[str, bool]]:
    def during(action: Callable[[], None]) -> dict[str, bool]:
        try:
            proc = subprocess.Popen(
                ["adb", "-s", serial, "shell", "getevent", "-lt"],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
        except OSError:
            action()
            return {"keyboard": False, "mouse": False, "abs": False, "touch": False, "midi": False}
        time.sleep(0.2)
        try:
            action()
        except Exception:
            proc.kill()
            raise
        deadline = now() + 2.2
        chunks: list[str] = []
        assert proc.stdout is not None
        while now() < deadline:
            ready, _, _ = select.select([proc.stdout], [], [], 0.1)
            if ready:
                line = proc.stdout.readline()
                if not line:
                    break
                chunks.append(line)
        proc.kill()
        try:
            proc.wait(timeout=1)
        except subprocess.TimeoutExpired:
            proc.terminate()
        blob = "".join(chunks)
        low = blob.lower()
        return {
            "keyboard": "key" in low and ("0001" in blob or "ev_key" in low),
            "mouse": "rel" in low or "0002" in blob,
            "abs": "abs" in low or "0003" in blob,
            "touch": "abs" in low or "btn_touch" in low or "slot" in low,
            "midi": False,
        }

    return during


def adb_bin() -> Optional[str]:
    from shutil import which
    return which("adb")


def adb_devices() -> list[str]:
    if not adb_bin():
        return []
    try:
        out = run_cmd(["adb", "devices"], timeout=4).stdout or ""
    except (OSError, subprocess.TimeoutExpired):
        return []
    serials = []
    for line in out.splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            serials.append(parts[0])
    return serials


def adb(serial: str, args: list[str], timeout: float = 8.0) -> subprocess.CompletedProcess[str]:
    return run_cmd(["adb", "-s", serial] + args, timeout=timeout)


def adb_root(serial: str) -> bool:
    try:
        adb(serial, ["root"], timeout=6)
        time.sleep(0.4)
        ident = adb(serial, ["shell", "id"], timeout=4).stdout or ""
        return "uid=0" in ident
    except (OSError, subprocess.TimeoutExpired):
        return False


def tablet_dump_xml(serial: str) -> Optional[str]:
    adb(serial, ["shell", "uiautomator", "dump", "/sdcard/uidump.xml"], timeout=8)
    pull = adb(serial, ["exec-out", "cat", "/sdcard/uidump.xml"], timeout=6)
    raw = pull.stdout or ""
    return raw if "<node" in raw else None


def tablet_labels(xml: str) -> list[str]:
    return [t for t in re.findall(r'(?:text|content-desc)="([^"]+)"', xml) if t]


def tablet_tap_from_xml(serial: str, xml: str, needle: str, exact: bool = False) -> bool:
    try:
        root = ET.fromstring(xml)
    except ET.ParseError:
        return False
    needle_l = needle.lower()
    for node in root.iter("node"):
        label = (node.attrib.get("text") or "").strip() or (node.attrib.get("content-desc") or "").strip()
        if not label:
            continue
        low = label.lower()
        if exact:
            if low != needle_l:
                continue
        elif needle_l not in low:
            continue
        if needle_l == BLE_NAME.lower() and low == "showbox":
            continue
        bounds = node.attrib.get("bounds") or ""
        m = re.match(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", bounds)
        if not m:
            continue
        x = (int(m.group(1)) + int(m.group(3))) // 2
        y = (int(m.group(2)) + int(m.group(4))) // 2
        adb(serial, ["shell", "input", "tap", str(x), str(y)], timeout=4)
        return True
    return False


def tablet_tap_text(serial: str, needle: str, exact: bool = False) -> bool:
    xml = tablet_dump_xml(serial)
    if not xml:
        return False
    return tablet_tap_from_xml(serial, xml, needle, exact=exact)


def tablet_back(serial: str) -> None:
    adb(serial, ["shell", "input", "keyevent", "KEYCODE_BACK"], timeout=4)
    time.sleep(0.6)


def tablet_getevent_names(serial: str) -> str:
    try:
        return adb(serial, ["shell", "getevent", "-p"], timeout=6).stdout or ""
    except (OSError, subprocess.TimeoutExpired):
        return ""


def tablet_showbox_connected(serial: str) -> bool:
    names = tablet_getevent_names(serial)
    if BLE_NAME.lower() in names.lower():
        return True
    dump = adb(serial, ["shell", "dumpsys", "bluetooth_manager"], timeout=8).stdout or ""
    low = dump.lower()
    idx = low.find(BLE_NAME.lower())
    if idx < 0:
        return False
    window = low[idx : idx + 1600]
    markers = (
        "hogp connection state=2",
        "hid connection state=2",
        "connection state=2",
        "connection_state_connected",
        "connection state: connected",
        "connected: true",
        "acl_connected",
    )
    return any(m in window for m in markers)


def tablet_confirm_pair_dialog(serial: str) -> None:
    xml = tablet_dump_xml(serial) or ""
    for label in (
        "Pair & connect",
        "Pair and connect",
        "PAIR & CONNECT",
        "Pair",
        "PAIR",
        "Connect",
        "Allow",
        "OK",
        "Yes",
        "Confirm",
        "Enter",
    ):
        if tablet_tap_from_xml(serial, xml, label, exact=True) or tablet_tap_from_xml(serial, xml, label):
            time.sleep(0.8)
            return
    adb(serial, ["shell", "input", "keyevent", "KEYCODE_TAB"], timeout=4)
    time.sleep(0.2)
    adb(serial, ["shell", "input", "keyevent", "KEYCODE_ENTER"], timeout=4)
    time.sleep(0.6)


def tablet_bt_reset(serial: str) -> None:
    adb(serial, ["shell", "cmd", "bluetooth_manager", "disable"], timeout=8)
    time.sleep(1.5)
    # Ghost LE bonds (BOND_TYPE_UNKNOWN) stay off the pairing list but still
    # hide ShowboxMod from "Pair new device". Drop the cache while BT is off.
    adb(
        serial,
        [
            "shell",
            "sh",
            "-c",
            "rm -rf /data/misc/bluedroid/bt_config.conf.bak "
            "/data/misc/bluetooth/logs 2>/dev/null; "
            "for d in /data/misc/bluedroid /data/misc/bluetooth /data/misc/bluedroid/bt_config.conf "
            "/data/vendor/bluetooth; do [ -e \"$d\" ] || continue; "
            "grep -l 30:30:F9:72:8D:25 \"$d\" 2>/dev/null | xargs -r rm -f; done",
        ],
        timeout=8,
    )
    adb(serial, ["shell", "cmd", "bluetooth_manager", "enable"], timeout=8)
    adb(serial, ["shell", "cmd", "bluetooth_manager", "wait-for-state:STATE_ON"], timeout=12)
    time.sleep(2.0)


def tablet_try_pair(serial: str) -> tuple[bool, str]:
    if not adb_root(serial):
        return False, "adb present but not rooted"
    adb(serial, ["shell", "svc", "bluetooth", "enable"], timeout=6)
    adb(serial, ["shell", "cmd", "bluetooth_manager", "enable"], timeout=6)
    time.sleep(1.0)
    if tablet_showbox_connected(serial):
        return True, "already connected"
    tablet_try_unpair(serial)
    tablet_back(serial)
    xml = tablet_open_bt(serial)
    if xml is None:
        return False, "could not open Bluetooth settings"
    tapped = False
    reset_once = False
    started = now()
    deadline = started + 50
    while now() < deadline:
        if tablet_showbox_connected(serial):
            return True, "uiautomator pair/connect"
        xml = tablet_dump_xml(serial) or xml
        labels = tablet_labels(xml)
        low = [x.lower() for x in labels]
        joined = " ".join(low)
        if BLE_NAME.lower() in low:
            if tablet_tap_from_xml(serial, xml, BLE_NAME, exact=True) or tablet_tap_from_xml(serial, xml, BLE_NAME):
                tapped = True
                time.sleep(1.2)
                tablet_confirm_pair_dialog(serial)
                time.sleep(2.5)
                continue
        if "pair new device" in low:
            tablet_tap_from_xml(serial, xml, "Pair new device")
            time.sleep(2.5)
            continue
        if "refresh" in low:
            tablet_tap_from_xml(serial, xml, "Refresh")
            time.sleep(2.0)
            continue
        if "available devices" in joined or "available" in low:
            adb(serial, ["shell", "input", "swipe", "540", "1400", "540", "700"], timeout=4)
            time.sleep(1.2)
            continue
        if not reset_once and now() - started >= 15:
            reset_once = True
            print(STYLE.dim("  tablet Bluetooth reset; ShowboxMod not on the pairing list yet"))
            tablet_bt_reset(serial)
            xml = tablet_open_bt(serial) or xml
            continue
        time.sleep(1.0)
    if tapped:
        return False, "tapped ShowboxMod; tablet did not complete HOGP connect"
    return False, "ShowboxMod never appeared under Pair new device"


def tablet_open_bt(serial: str) -> Optional[str]:
    adb(serial, ["shell", "am", "start", "-a", "android.settings.BLUETOOTH_SETTINGS"], timeout=6)
    time.sleep(1.2)
    xml = None
    for _ in range(5):
        xml = tablet_dump_xml(serial)
        if not xml:
            time.sleep(0.4)
            continue
        labels = [x.lower() for x in tablet_labels(xml)]
        if "pair new device" in labels or BLE_NAME.lower() in labels or "available devices" in labels:
            return xml
        if "forget" in labels or "device's bluetooth address" in " ".join(labels):
            tablet_back(serial)
            continue
        tablet_back(serial)
    return xml


def tablet_try_unpair(serial: str) -> str:
    xml = tablet_open_bt(serial)
    if xml and tablet_tap_from_xml(serial, xml, BLE_NAME):
        time.sleep(0.8)
        xml = tablet_dump_xml(serial) or ""
        tablet_tap_from_xml(serial, xml, "Disconnect") or tablet_tap_from_xml(serial, xml, "Forget") or tablet_tap_from_xml(serial, xml, "Unpair")
        time.sleep(0.5)
        xml = tablet_dump_xml(serial) or ""
        tablet_tap_from_xml(serial, xml, "Forget") or tablet_tap_from_xml(serial, xml, "Unpair") or tablet_tap_from_xml(serial, xml, "OK")
        return "disconnect/forget from Bluetooth settings"
    dump = adb(serial, ["shell", "dumpsys", "bluetooth_manager"], timeout=8).stdout or ""
    if BLE_NAME.lower() in dump.lower():
        tablet_bt_reset(serial)
        return "Bluetooth reset to drop a leftover ShowboxMod bond"
    return "ShowboxMod row not found to unpair"


def wait_host(session: Session, timeout: float = 20.0) -> Optional[dict[str, Any]]:
    deadline = now() + timeout
    while now() < deadline:
        hosts = connected_hosts(session.midi())
        if hosts:
            return hosts[0]
        time.sleep(0.4)
    return None


def apply_case(session: Session, address: str, case: tuple[Any, ...]) -> tuple[int, str]:
    _cid, _title, midi, hid, kb, mouse, abs_p, dig = case
    fields: dict[str, str] = {
        "address": address,
        "midi": str(midi),
        "hid": str(hid),
    }
    if hid:
        fields.update(
            {
                "hidKeyboard": str(kb),
                "hidMouse": str(mouse),
                "hidAbs": str(abs_p),
                "hidDigitizer": str(dig),
            }
        )
    return http_post(f"{session.base}/api/v1/midi/ble/role", fields)


def host_matches(host: dict[str, Any], case: tuple[Any, ...]) -> bool:
    _cid, _title, midi, hid, kb, mouse, abs_p, dig = case
    if bool(host.get("midi")) != bool(midi):
        return False
    if bool(host.get("hid")) != bool(hid):
        return False
    if hid:
        if bool(host.get("hidKeyboard")) != bool(kb):
            return False
        if bool(host.get("hidMouse")) != bool(mouse):
            return False
        if bool(host.get("hidAbs")) != bool(abs_p):
            return False
        if bool(host.get("hidDigitizer")) != bool(dig):
            return False
    return True


def run_hid_probes(
    session: Session,
    case: tuple[Any, ...],
    during: Callable[[Callable[[], None]], dict[str, bool]],
    require_midi_wire: bool,
) -> tuple[bool, str]:
    _cid, _title, midi, hid, kb, mouse, abs_p, dig = case
    notes: list[str] = []
    ok = True

    def fire(fields_url: str, fields: dict[str, str]) -> tuple[Callable[[], None], dict[str, Any]]:
        heard: dict[str, Any] = {"code": 0, "text": ""}

        def _do() -> None:
            heard["code"], heard["text"] = http_post(fields_url, fields)

        return _do, heard

    if kb:
        action, heard = fire(f"{session.base}/api/v1/hid/keyboard", {"key": "pageup"})
        saw = during(action)
        if heard["code"] != 200:
            ok = False
            notes.append(f"key-http={heard['code']}:{str(heard['text']).strip()[:40]}")
        elif saw.get("keyboard"):
            notes.append("key")
        else:
            action, heard = fire(f"{session.base}/api/v1/hid/keyboard", {"key": "space"})
            saw = during(action)
            if heard["code"] == 200 and saw.get("keyboard"):
                notes.append("key-space")
            else:
                ok = False
                notes.append("no-key")
    if mouse:
        action, heard = fire(f"{session.base}/api/v1/hid/mouse", {"x": "12", "y": "0"})
        saw = during(action)
        if heard["code"] != 200:
            ok = False
            notes.append(f"rel-http={heard['code']}:{str(heard['text']).strip()[:40]}")
        elif saw.get("mouse"):
            notes.append("rel")
        else:
            ok = False
            notes.append("no-rel")
    if abs_p:
        action, heard = fire(f"{session.base}/api/v1/hid/touch", {"action": "tap", "x": "5000", "y": "5000", "via": "abs"})
        saw = during(action)
        if heard["code"] != 200:
            ok = False
            notes.append(f"abs-http={heard['code']}:{str(heard['text']).strip()[:40]}")
        elif saw.get("abs") or saw.get("touch"):
            notes.append("abs")
        else:
            ok = False
            notes.append("no-abs")
    if dig:
        action, heard = fire(f"{session.base}/api/v1/hid/touch", {"action": "tap", "x": "5000", "y": "5000", "via": "touch"})
        saw = during(action)
        if heard["code"] != 200:
            ok = False
            notes.append(f"touch-http={heard['code']}:{str(heard['text']).strip()[:40]}")
        elif saw.get("touch") or saw.get("abs"):
            notes.append("touch")
        else:
            action, heard = fire(f"{session.base}/api/v1/hid/touch", {"action": "tap", "x": "5000", "y": "5000", "via": "abs"})
            saw = during(action)
            if heard["code"] != 200:
                ok = False
                notes.append(f"no-touch then abs-http={heard['code']}")
            elif saw.get("touch") or saw.get("abs"):
                notes.append("touch-via-abs")
            else:
                ok = False
                notes.append("no-touch")
    if midi:
        heard: dict[str, Any] = {"code": 0, "text": ""}

        def send_note() -> None:
            heard["code"], heard["text"] = http_post(f"{session.base}/api/v1/midi/test")

        saw = during(send_note)
        if heard["code"] != 200:
            ok = False
            notes.append(f"midi-http={heard['code']}:{str(heard['text'])[:40]}")
        elif saw.get("midi"):
            notes.append("midi-note")
        elif alsa_midi_ports() and require_midi_wire:
            ok = False
            notes.append("midi-http-ok no ALSA note")
        else:
            notes.append("midi-http")
    if not hid and not midi:
        notes.append("empty-mix")
    return ok, " ".join(notes) or "no probes"


def run_pairing_matrix(
    session: Session,
    results: Results,
    prefix: str,
    address: str,
    during: Callable[[Callable[[], None]], dict[str, bool]],
    require_midi_wire: bool,
    reconnect: Optional[Callable[[], None]] = None,
    rebond: Optional[Callable[[], tuple[bool, str]]] = None,
    forget: Optional[Callable[[], None]] = None,
    initial_desc: Optional[tuple[int, int, int]] = None,
) -> None:
    prev_desc = initial_desc
    reconnects = 0
    for case in HOST_CASES:
        cid, title, midi, hid, kb, mouse, abs_p, dig = case
        name = f"{prefix} {title}"
        session.assert_uart(results, f"uart-before-{prefix}-{cid}", stale_ms=SCAN_STALE_MS)
        desc = hid_descriptor_key(case)
        need_rebond = rebond is not None and desc is not None and desc != prev_desc
        if need_rebond:
            print(STYLE.dim(
                f"  {prefix} {cid}: HID descriptor {prev_desc} → {desc}; "
                "unpair so the host re-reads the report map"
            ))
            if forget is not None:
                forget()
        code, text = apply_case(session, address, case)
        if code != 200:
            results.fail(name, f"role http={code} {text[:80]}")
            continue
        if need_rebond:
            time.sleep(1.2)
            ok_b, how = rebond()
            if not ok_b:
                results.fail(name, f"re-pair after descriptor change failed: {how}")
                prev_desc = desc
                continue
            print(STYLE.dim(f"  re-paired: {how}"))
            apply_case(session, address, case)
            prev_desc = desc
        elif desc is not None:
            prev_desc = desc
        host = None
        deadline = now() + 10
        while now() < deadline:
            midi_body = session.midi()
            hosts = connected_hosts(midi_body)
            if hosts:
                host = hosts[0]
                if host_matches(host, case) and (not hid or midi_body.get("hidReady")):
                    break
            time.sleep(0.4)
        if host is None:
            results.fail(name, "host disconnected after role change")
            continue
        if not host_matches(host, case):
            results.fail(
                name,
                f"API mix mismatch midi={host.get('midi')} hid={host.get('hid')} "
                f"kb={host.get('hidKeyboard')} mouse={host.get('hidMouse')} "
                f"abs={host.get('hidAbs')} dig={host.get('hidDigitizer')}",
            )
            continue
        if hid and prefix == "laptop":
            until = now() + 5.0
            while now() < until and not evdev_showbox():
                time.sleep(0.25)
            if reconnect is not None and reconnects < 1 and not evdev_has_mouse():
                print(STYLE.dim(f"  {prefix} {cid}: no mouse node after mix change, reconnecting"))
                reconnects += 1
                reconnect()
                wait_evdev(8.0, need_mouse=True)
            time.sleep(0.6)
        elif hid:
            time.sleep(0.8)
        if midi and prefix == "laptop":
            until = now() + 4.0
            while now() < until and not alsa_midi_ports():
                time.sleep(0.25)
        hid_nodes = evdev_names()
        midi_ports = alsa_midi_ports()
        ok, detail = run_hid_probes(session, case, during, require_midi_wire)
        if not ok and hid:
            time.sleep(0.4)
            retry_ok, retry_detail = run_hid_probes(session, case, during, require_midi_wire)
            if retry_ok:
                ok, detail = True, retry_detail + " retry"
            elif reconnect is not None and reconnects < 1:
                reconnects += 1
                print(STYLE.dim(f"  {prefix} {cid}: HID probes missed events, reconnecting"))
                reconnect()
                wait_evdev(8.0, need_mouse=True)
                time.sleep(0.5)
                retry_ok, retry_detail = run_hid_probes(session, case, during, require_midi_wire)
                if retry_ok:
                    ok, detail = True, retry_detail + " after-reconnect"
                else:
                    detail = f"{detail} then {retry_detail}"
        extra = []
        hid_nodes = evdev_names()
        if prefix == "laptop" and hid and not hid_nodes:
            extra.append("no-evdev")
        if midi and require_midi_wire and not midi_ports:
            extra.append("no-alsa-port")
        if extra:
            detail = (detail + " " + " ".join(extra)).strip()
            if prefix == "laptop" and hid and not hid_nodes:
                ok = False
        if ok:
            results.pass_(name, detail)
        else:
            results.fail(name, detail + f" evdev={hid_nodes or '-'}")
        session.assert_uart(results, f"uart-after-{prefix}-{cid}", stale_ms=SCAN_STALE_MS)


def run(args: argparse.Namespace) -> int:
    global STYLE
    STYLE = Style(color_enabled(not args.no_color))
    ip, how = pick_target(args.host, args.ip)
    args.ip = ip
    s = Session(ip, args.host, args.stale_ms)
    r = Results()
    print()
    print(STYLE.bold("Showbox feature check"))
    print(STYLE.dim(f"  {how}"))
    print(STYLE.dim("  mixer↔base UART has priority — a stale link fails the run"))
    print(STYLE.dim("  ") + STYLE.green("✓ PASS") + "   " + STYLE.red("✗ FAIL") + "   " + STYLE.yellow("○ SKIP") + STYLE.dim(" (not wired / not requested)"))

    r.section("Core")
    if ping_ok(args.ip):
        r.pass_("icmp-ping")
    else:
        r.fail("icmp-ping", "no reply")
        print("Cannot reach the module. Stop.")
        return r.summary()

    link: dict[str, Any] = {}
    ok, detail = False, "no /link body"
    ready_deadline = now() + 90.0
    while now() < ready_deadline:
        ok, detail = uart_ok_sampled(s, args.stale_ms, samples=3, gap=0.25)
        if ok:
            link = s.link()
            break
        time.sleep(0.4)
    if ok:
        r.uart_up = True
        r.pass_("mixer-base", detail + f" setup={link.get('setup')}")
    else:
        r.uart_up = False
        r.fail("mixer-base", detail)
    if link.get("setup") is False:
        r.fail("setup-finished", "setup() has not returned")
    elif link.get("setup") is True:
        r.pass_("setup-finished")

    code, body = http_get_retry(f"{s.base}/services", timeout=3.0, attempts=2)
    if code == 200 and len(body) > 1000:
        r.pass_("http-services", f"{code} {len(body)} bytes")
    else:
        r.fail("http-services", f"http={code} size={len(body)}")
    s.assert_uart(r, "uart-after-http")

    code, _ = http_get(f"{s.base}/")
    if code == 200:
        r.pass_("http-root", str(code))
    else:
        r.fail("http-root", f"http={code}")

    midi = s.midi()
    if midi:
        r.pass_("http-midi-api", f"discoverable={midi.get('discoverable')} links={midi.get('bleLinkCount')}")
    else:
        r.fail("http-midi-api", "empty or not JSON")
        midi = {}

    debug = s.debug()
    if debug.strip():
        r.pass_("http-debug", f"{len(debug)} bytes")
    else:
        r.fail("http-debug", "empty")
        debug = ""
    fatal = [p for p in DEBUG_FATAL if p in debug.lower()]
    if fatal:
        r.fail("debug-log-errors", "found " + ", ".join(fatal))
    else:
        r.pass_("debug-log-errors", "no panic/assert/heap-corrupt lines")
    heap, largest = parse_heap(debug)
    if largest is not None and largest < 1024:
        r.fail("heap", f"largest={largest} heap={heap}")
    elif heap is None:
        r.skip("heap", "no heap= lines in /debug")
    else:
        r.pass_("heap", f"heap={heap} largest={largest}")

    uni = mdns_query_a(args.host, args.ip, 0.5)
    mcast = mdns_query_a(args.host, "224.0.0.251", 0.5)
    resolv = resolver_ips(args.host)
    if args.ip in uni or args.ip in mcast or args.ip in resolv:
        r.pass_("mdns-a", f"unicast={uni} mcast={mcast} resolv={resolv}")
    else:
        r.fail("mdns-a", f"unicast={uni} mcast={mcast} resolv={resolv}")
    dests = [args.ip, "224.0.0.251"]
    if mdns_has_ptr("_http._tcp.local", dests) or args.ip in resolv:
        r.pass_("mdns-http", "_http._tcp or A record for showbox.local")
    else:
        r.skip("mdns-http", "no PTR in 0.7s (A record may still work)")
    if args.ip in resolv or args.ip in uni:
        code, body = http_get_retry(f"{s.base}/services", timeout=2.0, host=args.host)
        if code == 200 and len(body) > 1000:
            r.pass_("http-via-mdns", f"{code} {len(body)} bytes Host={args.host}")
        else:
            r.fail("http-via-mdns", f"http={code} size={len(body)}")
    else:
        r.skip("http-via-mdns", "name did not resolve")
    s.assert_uart(r, "uart-after-mdns")

    tel_ok, tel_detail = telnet_messages(args.ip, want=2)
    if tel_ok:
        r.pass_("telnet", tel_detail)
    else:
        r.fail("telnet", tel_detail)
    s.assert_uart(r, "uart-after-telnet")

    ota_mdns = mdns_has_ptr("_arduino._tcp.local", dests)
    ota_udp = udp_open(args.ip, OTA_PORT)
    if ota_mdns or ota_udp:
        r.pass_("ota-available", f"mDNS _arduino._tcp={int(ota_mdns)} UDP {OTA_PORT}={int(ota_udp)} (not flashing)")
    else:
        r.fail("ota-available", f"no _arduino._tcp and UDP {OTA_PORT} not reachable")
    s.assert_uart(r, "uart-after-ota")

    if midi.get("discoverable"):
        r.pass_("ble-advertising-api", "discoverable=true")
    else:
        r.fail("ble-advertising-api", "discoverable=false")

    r.section("Pedals")
    ensure_airstep(s, r)
    midi = s.midi()
    autos = auto_peripherals(midi)
    if not autos:
        r.skip("ble-auto-connect", "no autoConnect MIDI peripheral saved")
    else:
        down = [p.get("name") or p.get("address") for p in autos if not p.get("connected")]
        up = [p.get("name") or p.get("address") for p in autos if p.get("connected")]
        if down:
            r.fail("ble-auto-connect", f"saved but not connected: {down}; up={up}")
        else:
            r.pass_("ble-auto-connect", f"connected {up}")
    s.assert_uart(r, "uart-after-autoconnect-check")

    r.section("Load")
    print(STYLE.dim("  simultaneous ping + HTTP + UART, advertising stays up"))
    sim_fail = 0
    sim_n = 0
    deadline = now() + args.simultaneous
    with ThreadPoolExecutor(max_workers=6) as pool:
        while now() < deadline:
            sim_n += 1
            futs = {
                pool.submit(ping_ok, args.ip): "ping",
                pool.submit(http_get, f"{s.base}/services"): "services",
                pool.submit(http_json, f"{s.base}/api/v1/showbox/link"): "link",
                pool.submit(http_json, f"{s.base}/api/v1/midi"): "midi",
            }
            ping = True
            link_body: dict[str, Any] = {}
            svc_ok = False
            midi_ok = False
            for fut in as_completed(futs):
                kind = futs[fut]
                try:
                    val = fut.result()
                except Exception:
                    sim_fail += 1
                    continue
                if kind == "ping":
                    ping = bool(val)
                elif kind == "services":
                    code, body = val
                    svc_ok = code == 200 and len(body) > 1000
                elif kind == "link":
                    _code, link_body = val
                elif kind == "midi":
                    _code, midi_body = val
                    midi_ok = bool(midi_body.get("discoverable"))
            live, why = uart_ok(link_body, SCAN_STALE_MS)
            if not live:
                live, why = uart_ok_sampled(s, SCAN_STALE_MS)
            lost = []
            if not ping:
                lost.append("ping")
            if not svc_ok:
                lost.append("http")
            if not live:
                lost.append("uart")
            if not midi_ok:
                lost.append("adv")
            if lost:
                sim_fail += 1
                print(STYLE.dim(f"    tick {sim_n} lost {','.join(lost)}  uart={why}"))
            time.sleep(0.25)
    if sim_n == 0 or sim_fail:
        r.fail("simultaneous", f"{sim_fail}/{sim_n} ticks lost ping, HTTP, advertising, or UART")
    else:
        r.pass_("simultaneous", f"{sim_n} ticks ping+HTTP+/services+UART+advertising")
    s.assert_uart(r, "uart-after-simultaneous")

    r.section("Radios")
    wifi = transport(s.midi(), "wifi")
    cmd = applemidi_invite(args.ip)
    if cmd in ("OK", "NO", "BY", "CK", "IN"):
        r.pass_("wifi-rtp-midi", f"AppleMIDI UDP {APPLEMIDI_PORT} replied {cmd}")
    elif not wifi.get("enabled"):
        r.skip("wifi-rtp-midi", "Wi-Fi RTP-MIDI transport is off")
    else:
        r.fail("wifi-rtp-midi", f"no AppleMIDI reply on UDP {APPLEMIDI_PORT} (got {cmd or 'timeout'})")
    s.assert_uart(r, "uart-after-rtp-midi")

    print(STYLE.dim(f"  host LE scan for {BLE_NAME} ({args.host_scan:.0f}s)"))
    status, scan_detail, scanned_addr = host_le_scan(args.host_scan)
    if status == "pass":
        r.pass_("ble-advertising-air", scan_detail)
    elif status == "skip":
        r.skip("ble-advertising-air", scan_detail)
    else:
        r.fail("ble-advertising-air", scan_detail)
    s.assert_uart(r, "uart-after-host-le-scan", stale_ms=SCAN_STALE_MS)

    print(STYLE.dim("  module BLE scan (crowds the radio; UART must still move)"))
    code, text = http_post(f"{s.base}/api/v1/midi/ble/scan")
    if code != 200:
        r.fail("ble-scanning", f"scan POST http={code} {text[:80]}")
    else:
        saw_scan = False
        nearby = 0
        scan_ok = True
        deadline = now() + 12
        while now() < deadline:
            live, why = uart_ok(s.link(), SCAN_STALE_MS)
            if not live:
                if r.uart_up is not False:
                    r.fail("ble-scanning", f"UART dropped during scan: {why}")
                    r.uart_up = False
                    scan_ok = False
                    break
            mid = s.midi()
            if mid.get("scanning"):
                saw_scan = True
            nearby = max(nearby, len(mid.get("nearby") or []))
            if saw_scan and not mid.get("scanning"):
                break
            time.sleep(0.6)
        if scan_ok:
            r.pass_("ble-scanning", f"POST ok, scanning={saw_scan} nearby={nearby}")
        idle = now() + 8
        while now() < idle and s.midi().get("scanning"):
            time.sleep(0.4)
    s.assert_uart(r, "uart-after-module-scan", stale_ms=SCAN_STALE_MS)

    r.section("Host HID + MIDI")
    want_laptop = args.pair and args.laptop
    want_tablet = args.pair and args.tablet
    if not args.pair:
        r.skip("host-matrix", "--no-pair")
    else:
        bt_disconnect_named("ShowBox")
        bt_scan_off()
        s.post_settings(acceptNewHosts=True, bleHid=True, bleMidiPeripheral=True, bleHidKeyboard=True, bleHidMouse=True, bleHidAbs=False, bleHidDigitizer=False)
        time.sleep(0.3)

        if want_laptop:
            addr = scanned_addr or bt_find_showbox()
            if not addr:
                status, scan_detail, scanned_addr = host_le_scan(6.0)
                addr = scanned_addr or bt_find_showbox()
            if not addr:
                r.skip("laptop-pair", "ShowboxMod address not in bluetoothctl")
            else:
                print(STYLE.dim(f"  pairing this laptop to {addr} (temporary)"))
                paired, how = laptop_pair(addr)
                if not paired:
                    r.fail("laptop-pair", how)
                else:
                    host = wait_host(s, 18)
                    if host is None:
                        r.fail("laptop-pair", f"{how}; module never listed a connected host")
                    else:
                        r.pass_("laptop-pair", f"{host.get('name') or host.get('address')} via {how}")
                        # Leftover NVS mix from the last matrix case can be digitizer-only.
                        apply_case(s, host.get("address") or addr, HOST_CASES[1])
                        mix_deadline = now() + 8
                        while now() < mix_deadline:
                            midi_body = s.midi()
                            hosts = connected_hosts(midi_body)
                            if hosts and host_matches(hosts[0], HOST_CASES[1]) and midi_body.get("hidReady"):
                                break
                            time.sleep(0.3)
                        names = wait_laptop_hid(addr)
                        if not names:
                            r.fail("laptop-hid-device", "no /dev/input node named ShowboxMod")
                        else:
                            r.pass_("laptop-hid-device", ", ".join(sorted(names)))
                        run_pairing_matrix(
                            s,
                            r,
                            "laptop",
                            host.get("address") or addr,
                            during_laptop,
                            False,
                            rebond=lambda: laptop_rebond(s, addr),
                            forget=lambda: laptop_forget(s, addr),
                            initial_desc=(1, 1, 0),
                        )
                        autos_later = auto_peripherals(s.midi())
                        down = [p.get("name") for p in autos_later if not p.get("connected")]
                        if down:
                            r.fail("airstep-during-laptop", f"pedal dropped while laptop was host: {down}")
                        elif autos_later:
                            r.pass_("airstep-during-laptop", "auto-connect pedal still up")
                    if not args.keep_host:
                        laptop_disconnect(addr, forget=True)
                        bt_scan_off()
                        s.post_settings(acceptNewHosts=True, bleHid=True, bleMidiPeripheral=True)
                        r.pass_("laptop-unpair", "disconnected and removed")
                        time.sleep(2.0)
                    else:
                        laptop_disconnect(addr, forget=False)
                        bt_scan_off()
                        s.post_settings(acceptNewHosts=True)
                        r.pass_("laptop-disconnect", "disconnected, bond kept")
                        time.sleep(1.0)
        else:
            r.skip("laptop-pair", "--no-laptop")

        if want_tablet:
            serials = adb_devices()
            if not serials:
                r.skip("tablet-pair", "no adb device")
            else:
                serial = serials[0]
                print(STYLE.dim("  waiting for ShowboxMod to advertise again after laptop unpair"))
                adv_status, adv_detail, _adv_addr = host_le_scan(8.0)
                if adv_status != "pass":
                    print(STYLE.dim(f"  host LE scan after unpair: {adv_detail}"))
                    time.sleep(3.0)
                print(STYLE.dim(f"  pairing tablet {serial} to ShowboxMod (temporary)"))
                ok_tab, how = tablet_try_pair(serial)
                host = wait_host(s, 16 if ok_tab else 10)
                if host is None and not ok_tab:
                    if "tapped ShowboxMod" in how:
                        r.fail("tablet-pair", how)
                    else:
                        r.skip("tablet-pair", how)
                elif host is None:
                    r.fail("tablet-pair", f"{how}; module never listed a connected host")
                else:
                    r.pass_("tablet-pair", f"{host.get('name') or host.get('address')} {how}")
                    hogp_deadline = now() + 18
                    while now() < hogp_deadline and not tablet_showbox_connected(serial):
                        tablet_confirm_pair_dialog(serial)
                        time.sleep(1.0)
                    if tablet_showbox_connected(serial):
                        r.pass_("tablet-hid-device", "ShowboxMod in tablet getevent/dumpsys")
                    else:
                        r.fail("tablet-hid-device", "BLE host up but Android HOGP did not create an input device")
                    run_pairing_matrix(
                        s,
                        r,
                        "tablet",
                        host.get("address") or "",
                        during_tablet(serial),
                        False,
                        rebond=lambda: tablet_rebond(s, serial),
                        forget=lambda: tablet_forget(s, serial),
                        initial_desc=(1, 1, 0),
                    )
                    if not args.keep_host:
                        note = tablet_try_unpair(serial)
                        r.pass_("tablet-unpair", note)
        else:
            r.skip("tablet-pair", "--no-tablet")

    r.section("Wired transports")
    midi = s.midi()
    din = transport(midi, "din")
    eth = transport(midi, "ethernet")
    usb_dev = transport(midi, "usbMidiPeripheral")
    usb_host = transport(midi, "usbMidiHost")
    if args.din:
        if din.get("enabled"):
            code, text = http_post(f"{s.base}/api/v1/midi/test")
            if code == 200:
                r.pass_("din-midi", "API enabled; test note sent (listen on the DIN jack)")
            else:
                r.fail("din-midi", f"test note http={code} {text[:80]}")
        else:
            r.fail("din-midi", "DIN is disabled in /midi")
        s.assert_uart(r, "uart-after-din")
    else:
        r.skip("din-midi", f"API enabled={din.get('enabled')} — pass --din when a 5-pin loopback is plugged in")
    if args.ethernet:
        note = (eth.get("note") or "").lower()
        if not eth.get("enabled"):
            r.fail("ethernet-midi", "Ethernet MIDI is disabled in /midi")
        elif "waiting" in note or "no link" in note or "down" in note:
            r.fail("ethernet-midi", eth.get("note") or "no W5500 link")
        else:
            r.pass_("ethernet-midi", eth.get("note") or "link up")
        s.assert_uart(r, "uart-after-ethernet")
    else:
        r.skip("ethernet-midi", f"{eth.get('note') or 'W5500'} — pass --ethernet when a cable is plugged in")
    if args.usb:
        r.skip("usb-midi", "USB MIDI probe not implemented as a cable test yet")
    else:
        r.skip("usb-midi-device", f"API enabled={usb_dev.get('enabled')} — nothing on USB")
        r.skip("usb-midi-host", f"API enabled={usb_host.get('enabled')} — nothing on USB")

    r.section("Soak")
    uart_ok_sampled(s, SCAN_STALE_MS, samples=6, gap=0.4)
    print(STYLE.dim(f"  {args.soak:.0f}s ping + HTTP + UART"))
    soak_fail = 0
    soak_n = 0
    heaps: list[int] = []
    deadline = now() + args.soak
    while now() < deadline:
        soak_n += 1
        ping = ping_ok(args.ip)
        code, body = http_get(f"{s.base}/services")
        live, why = uart_ok_sampled(s, SCAN_STALE_MS)
        dbg = s.debug()
        _h, largest_now = parse_heap(dbg)
        if largest_now is not None:
            heaps.append(largest_now)
        if not ping or code != 200 or not live:
            soak_fail += 1
            print(STYLE.dim(f"    soak {soak_n} ping={ping} services={code} uart={why}"))
        time.sleep(max(0.4, args.interval))
    if soak_n == 0 or soak_fail:
        r.fail("soak", f"{soak_fail}/{soak_n} ticks")
    else:
        r.pass_("soak", f"{soak_n} ticks")
    if len(heaps) >= 4 and heaps[-1] + 8192 < heaps[0] and heaps[-1] < 8192:
        r.fail("heap-soak", f"largest fell {heaps[0]} → {heaps[-1]}")
    elif heaps:
        r.pass_("heap-soak", f"largest start {heaps[0]} end {heaps[-1]}")
    s.assert_uart(r, "uart-final", quiet=False)
    return r.summary()


def main() -> int:
    parser = argparse.ArgumentParser(description="Showbox live feature check")
    parser.add_argument("--ip", default=None, help="module IPv4 (default: resolve --host / showbox.local)")
    parser.add_argument("--host", default="showbox.local", help="mDNS name used when --ip is omitted")
    parser.add_argument("--stale-ms", type=int, default=STALE_MS)
    parser.add_argument("--simultaneous", type=float, default=8.0, help="seconds of parallel ping+HTTP+UART")
    parser.add_argument("--host-scan", type=float, default=8.0, help="seconds for local LE advertisement scan")
    parser.add_argument("--soak", type=float, default=12.0, help="final soak seconds")
    parser.add_argument("--interval", type=float, default=0.8)
    parser.add_argument("--pair", dest="pair", action="store_true", default=True, help="pair laptop/tablet (default)")
    parser.add_argument("--no-pair", dest="pair", action="store_false", help="skip HID+MIDI host matrix")
    parser.add_argument("--laptop", dest="laptop", action="store_true", default=True)
    parser.add_argument("--no-laptop", dest="laptop", action="store_false")
    parser.add_argument("--tablet", dest="tablet", action="store_true", default=True)
    parser.add_argument("--no-tablet", dest="tablet", action="store_false")
    parser.add_argument("--keep-host", action="store_true", help="leave the laptop/tablet bonded after the matrix")
    parser.add_argument("--din", action="store_true", help="send a DIN MIDI test note (needs a 5-pin cable)")
    parser.add_argument("--ethernet", action="store_true", help="require a live W5500 Ethernet MIDI link")
    parser.add_argument("--usb", action="store_true", help="reserved USB MIDI cable tests")
    parser.add_argument("--no-color", action="store_true")
    return run(parser.parse_args())


if __name__ == "__main__":
    sys.exit(main())
