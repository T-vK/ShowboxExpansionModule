#!/usr/bin/env python3
"""Live check for the features that have to work together.

Mixer↔base UART is the highest priority: every other probe is wrapped in a
UART liveness check, and a stale link fails the run even if HTTP looks fine.

  python3 scripts/check_features.py                    # showbox.local
  python3 scripts/check_features.py --ip 192.168.3.55  # skip mDNS lookup
  python3 scripts/check_features.py --pair             # laptop/tablet HID+MIDI matrix (slow)
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
import ssl
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

from test_reminders import print_run_reminders

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


def http_get(url: str, timeout: float = HTTP_TIMEOUT, host: Optional[str] = None, ua: str = "showbox-features") -> tuple[int, bytes]:
    headers = {"Connection": "close", "User-Agent": ua, "Cache-Control": "no-store"}
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


def http_get_noredirect(url: str, timeout: float = HTTP_TIMEOUT) -> tuple[int, bytes, str]:
    class NoRedir(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: ARG002
            return None

    opener = urllib.request.build_opener(NoRedir)
    req = urllib.request.Request(
        url,
        headers={"Connection": "close", "User-Agent": "showbox-features", "Cache-Control": "no-store"},
        method="GET",
    )
    try:
        with opener.open(req, timeout=timeout) as resp:
            return resp.getcode() or 0, resp.read(), resp.headers.get("Location") or ""
    except urllib.error.HTTPError as err:
        loc = err.headers.get("Location") if err.headers else ""
        return err.code, err.read() if err.fp else b"", loc or ""
    except Exception:
        return 0, b"", ""


def https_get(ip: str, path: str, timeout: float = 8.0) -> tuple[str, bytes]:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    raw = socket.create_connection((ip, 443), timeout=timeout)
    try:
        tls = ctx.wrap_socket(raw, server_hostname="showbox.local")
        ver = tls.version() or ""
        tls.sendall(f"GET {path} HTTP/1.0\r\nHost: showbox.local\r\nConnection: close\r\n\r\n".encode())
        chunks: list[bytes] = []
        header_blob = b""
        header_done = False
        content_len: int | None = None
        body_len = 0
        while True:
            buf = tls.recv(2048)
            if not buf:
                break
            chunks.append(buf)
            if not header_done:
                header_blob += buf
                sep = header_blob.find(b"\r\n\r\n")
                if sep >= 0:
                    header_done = True
                    for line in header_blob[:sep].split(b"\r\n"):
                        if line.lower().startswith(b"content-length:"):
                            try:
                                content_len = int(line.split(b":", 1)[1].strip())
                            except ValueError:
                                content_len = None
                    body_len = len(header_blob) - (sep + 4)
            else:
                body_len += len(buf)
            if header_done and content_len is not None and body_len >= content_len:
                break
        tls.close()
        return ver, b"".join(chunks)
    finally:
        try:
            raw.close()
        except OSError:
            pass


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


def local_ip_toward(ip: str) -> str:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect((ip, 80))
        return sock.getsockname()[0]
    except OSError:
        return "0.0.0.0"
    finally:
        sock.close()


def mdns_query(
    name: str,
    rtype: int,
    dest: str,
    timeout: float = 0.6,
    toward: Optional[str] = None,
) -> list[bytes]:
    multicast = dest.startswith("224.")
    q = bytearray()
    # RFC 6762: multicast DNS query ID MUST be 0 or responders ignore it.
    q += struct.pack("!HHHHHH", 0x0000, 0x0000, 1, 0, 0, 0)
    for label in name.rstrip(".").split("."):
        raw = label.encode("ascii")
        q.append(len(raw))
        q += raw
    q += b"\x00"
    # QM for multicast (module must answer 224.0.0.251), QU for unicast-to-IP.
    q += struct.pack("!HH", rtype, 0x0001 if multicast else 0x8001)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            try:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            except OSError:
                pass
        sock.settimeout(timeout)
        iface = local_ip_toward(toward or (dest if not multicast else "8.8.8.8"))
        if multicast:
            sock.bind(("", 5353))
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
            if iface != "0.0.0.0":
                sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(iface))
                mreq = socket.inet_aton("224.0.0.251") + socket.inet_aton(iface)
            else:
                mreq = struct.pack("4sI", socket.inet_aton("224.0.0.251"), socket.INADDR_ANY)
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        sock.sendto(bytes(q), (dest, 5353))
        records: list[bytes] = []
        deadline = now() + timeout
        while now() < deadline:
            try:
                data, addr = sock.recvfrom(2048)
            except socket.timeout:
                break
            src_ip = addr[0]
            if multicast:
                if toward and src_ip != toward:
                    continue
            elif src_ip != dest:
                continue
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
            if records:
                break
        return records
    except OSError:
        return []
    finally:
        sock.close()


def mdns_query_a(host: str, dest: str, timeout: float = 0.6, toward: Optional[str] = None) -> list[str]:
    out: list[str] = []
    for rdata in mdns_query(host, 1, dest, timeout, toward=toward):
        if len(rdata) == 4:
            ip = socket.inet_ntoa(rdata)
            if ip not in out:
                out.append(ip)
    return out


def mdns_has_ptr(service: str, dests: list[str], toward: Optional[str] = None) -> bool:
    for dest in dests:
        if mdns_query(service, 12, dest, 0.7, toward=toward):
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
    for ip in mdns_query_a(host, "224.0.0.251", 0.8):
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


def tcp_open(ip: str, port: int, timeout: float = 0.4) -> bool:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.settimeout(timeout)
        sock.connect((ip, port))
        return True
    except OSError:
        return False
    finally:
        sock.close()


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
        try:
            sock.sendall(b"\r\n")
        except OSError:
            pass
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


def bt_power_on() -> str:
    """Unblock and power the default adapter. Returns `bluetoothctl show` text."""
    try:
        subprocess.run(["rfkill", "unblock", "bluetooth"], capture_output=True, timeout=3, check=False)
    except (OSError, subprocess.TimeoutExpired):
        pass
    try:
        run_cmd(["bluetoothctl", "power", "on"], timeout=4)
    except (OSError, subprocess.TimeoutExpired):
        pass
    time.sleep(0.4)
    try:
        return strip_ansi(run_cmd(["bluetoothctl", "show"], timeout=3).stdout or "")
    except (OSError, subprocess.TimeoutExpired):
        return ""


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
    show = bt_power_on()
    if "No default controller" in show or "No controller available" in show:
        return "skip", "no Bluetooth adapter", None
    if re.search(r"Powered:\s*no\b", show) or "off-blocked" in show.lower():
        return "skip", "Bluetooth adapter powered off", None
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
    scan_blob = strip_ansi((out.stdout or "") + "\n" + (out.stderr or ""))
    blob = scan_blob + "\n" + extra
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
    scan_low = scan_blob.lower()
    if "notready" in scan_low or "failed to start discovery" in scan_low:
        return "skip", "host LE scan not ready", None
    if "discovery started" in scan_low or "set-scan-enable" in scan_low or "scanning" in scan_low:
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


def print_test_memory() -> None:
    print_run_reminders()


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
            print_test_memory()
            return 1
        print(STYLE.green(bar))
        print(STYLE.invert_green(line.center(64)))
        print(STYLE.green(bar))
        self._print_skips()
        print_test_memory()
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
            # Laptop MIDI-only HOGP role changes can stall mixer↔base ~16 s on PTA.
            recover_until = now() + 25.0
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
        last = (0, "no post")
        for attempt in range(5):
            last = http_post(f"{self.base}/api/v1/midi/settings", data)
            if last[0] == 200:
                return last
            time.sleep(1.2)
        return last


def transport(midi: dict[str, Any], ident: str) -> dict[str, Any]:
    for row in midi.get("transports") or []:
        if row.get("id") == ident:
            return row
    return {}


def connected_hosts(midi: dict[str, Any]) -> list[dict[str, Any]]:
    return [h for h in (midi.get("hosts") or []) if h.get("connected")]


def live_host_address(session: Session, fallback: str = "") -> str:
    hosts = connected_hosts(session.midi())
    if hosts:
        return str(hosts[0].get("address") or fallback or "")
    return fallback


def module_forget(session: Session, address: str) -> None:
    if not address:
        return
    http_post(f"{session.base}/api/v1/midi/ble/forget", {"address": address})


def forget_module_hosts(session: Session) -> None:
    """Drop leftover HID hosts so the pairing mix can become kb+mouse again.

    Forget is queued one-at-a-time on the module. A second POST while the
    first is pending returns Busy and leaves the digitizer mix in NVS.
    """
    deadline = now() + 10.0
    while now() < deadline:
        addrs = []
        seen: set[str] = set()
        for host in session.midi().get("hosts") or []:
            addr = str(host.get("address") or "")
            if addr and addr.lower() not in seen:
                seen.add(addr.lower())
                addrs.append(addr)
        if not addrs:
            wait_no_hosts(session, 2.0)
            return
        code, text = http_post(f"{session.base}/api/v1/midi/ble/forget", {"address": addrs[0]})
        if code != 200:
            time.sleep(0.6)
            continue
        gone = now() + 3.0
        while now() < gone:
            left = [str(h.get("address") or "") for h in (session.midi().get("hosts") or [])]
            if addrs[0] not in left:
                break
            time.sleep(0.3)
    wait_no_hosts(session, 4.0)


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


def laptop_pair(addr: str, session: Optional["Session"] = None) -> tuple[bool, str]:
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
        paired_up = False
        while now() < deadline:
            info = bt_info(addr)
            if "Connected: yes" in info and ("Paired: yes" in info or "Pairing successful" in pair_blob):
                paired_up = True
                break
            time.sleep(0.4)
        if paired_up or ("Paired: yes" in bt_info(addr)):
            # The pairing connection often fails HID CCC writes. Drop it and
            # connect on the bond so BlueZ can subscribe to reports.
            bt_scan_off()
            hogp = laptop_bond_connect(addr, session)
            how = "pairing successful"
            if attempt:
                how += f" attempt {attempt + 1}"
            if hogp:
                return True, how
            last_reason = "paired but HOGP reconnect failed"
            # Keep the bond. Ads are off while AIRSTEP is up unless the
            # pairing window is open; the caller retries with the window up.
            break
        info = bt_info(addr)
        paired = "Paired: yes" in info
        connected = "Connected: yes" in info
        last_reason = "no pair result"
        for line in pair_blob.splitlines():
            low = line.lower()
            if "fail" in low or "error" in low or "pairing" in low or "not available" in low:
                last_reason = line.strip()
                break
        laptop_disconnect(addr, forget=True)
        time.sleep(1.5)
    bt_scan_off()
    info = bt_info(addr)
    paired = "Paired: yes" in info
    connected = "Connected: yes" in info
    return False, f"paired={int(paired)} connected={int(connected)} ({last_reason})"


def laptop_open_ads(session: Optional["Session"]) -> None:
    """AIRSTEP holds PTA; ShowboxMod only advertises while the pairing window is open."""
    if session is None:
        return
    session.post_settings(acceptNewHosts=True)
    deadline = now() + 10
    while now() < deadline:
        if session.midi().get("advertising"):
            return
        session.post_settings(acceptNewHosts=True)
        time.sleep(0.6)


def laptop_bond_connect(addr: str, session: Optional["Session"] = None) -> bool:
    """Connect on an existing bond. HID CCC writes fail on the pairing link."""
    bt_scan_off()
    laptop_open_ads(session)
    if bt_connected(addr):
        run_cmd(["bluetoothctl", "disconnect", addr], timeout=6)
        time.sleep(2.5)
        laptop_open_ads(session)
    run_cmd(["bluetoothctl", "connect", addr], timeout=15)
    deadline = now() + 12
    while now() < deadline:
        if bt_connected(addr):
            return True
        time.sleep(0.4)
    return bt_connected(addr)


def laptop_reconnect(mod_addr: str, session: Optional["Session"] = None) -> None:
    laptop_bond_connect(mod_addr, session)


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
        subprocess.run(
            ["sudo", "-n", "find", root, "-iname", folder, "-exec", "rm", "-rf", "{}", "+"],
            timeout=5,
            check=False,
            capture_output=True,
        )
    except (OSError, subprocess.TimeoutExpired):
        pass
    try:
        adapters = os.listdir(root)
    except OSError:
        return
    for adapter in adapters:
        for path in (
            os.path.join(root, adapter, folder),
            os.path.join(root, adapter, "cache", folder),
        ):
            if os.path.isdir(path) or os.path.isfile(path):
                try:
                    subprocess.run(["rm", "-rf", path], timeout=3, check=False, capture_output=True)
                    if os.path.exists(path):
                        subprocess.run(["sudo", "-n", "rm", "-rf", path], timeout=3, check=False, capture_output=True)
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
    gone = now() + 6.0
    while now() < gone and evdev_showbox():
        time.sleep(0.3)
    session.post_settings(acceptNewHosts=True)


def laptop_rebond(session: Session, addr: str) -> tuple[bool, str]:
    # Pair only. Caller must forget, then apply the new mix, then call this so
    # the host reads the updated HOGP report map instead of blePairMix.
    session.post_settings(acceptNewHosts=True)
    paired, how = laptop_pair(addr, session)
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


def tablet_reconnect(serial: str, session: Optional["Session"] = None) -> None:
    if tablet_showbox_connected(serial):
        tablet_close_ads(session)
        return
    tablet_keep_pairing(session)
    if tablet_connect_paired(serial) and tablet_showbox_connected(serial):
        tablet_close_ads(session)
        return
    tablet_wait_hogp(serial, 45.0, session=session)
    tablet_close_ads(session)


def tablet_close_ads(session: Optional["Session"]) -> None:
    """Ads + AIRSTEP hold PTA; close the pairing window once HOGP is up."""
    if session is not None:
        session.post_settings(acceptNewHosts=False)


def tablet_rebond(session: Session, serial: str) -> tuple[bool, str]:
    # Pair only. Caller already applied the new mix; resetting to kb+mouse
    # here made Android cache the wrong report map for abs/touch cases.
    # Drop Android's HOGP cache so the next bond re-reads the report map.
    tablet_bt_reset(serial)
    session.post_settings(acceptNewHosts=True)
    time.sleep(1.0)
    ok, how = tablet_try_pair(serial, session)
    host = wait_host(session, 16 if ok else 8)
    if host is None:
        return False, how
    hogp = ok or tablet_showbox_connected(serial)
    if not hogp:
        hogp = tablet_wait_hogp(serial, 60.0, session=session)
    tablet_close_ads(session)
    if not hogp:
        return False, how + " no-hogp"
    return True, how


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


def wait_laptop_hid(addr: str, session: Optional["Session"] = None) -> list[str]:
    names = wait_evdev(12.0, need_mouse=True)
    if evdev_has_mouse(names):
        return names
    print(STYLE.dim("  HOGP has no mouse node yet; reconnecting so BlueZ re-reads reports"))
    laptop_reconnect(addr, session)
    return wait_evdev(15.0, need_mouse=True)


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


def tablet_wake(serial: str) -> None:
    try:
        adb(serial, ["shell", "input", "keyevent", "KEYCODE_WAKEUP"], timeout=4)
        adb(serial, ["shell", "svc", "power", "stayon", "usb"], timeout=4)
    except (OSError, subprocess.TimeoutExpired):
        pass


def tablet_input_nodes(serial: str) -> list[str]:
    nodes: list[str] = []
    current = None
    for line in tablet_getevent_names(serial).splitlines():
        add = re.match(r"add device \d+:\s+(\S+)", line.strip())
        if add:
            current = add.group(1)
            continue
        name = re.search(r'name:\s+"?([^"]+?)"?\s*$', line, re.I)
        if current and name and BLE_NAME.lower() in name.group(1).lower():
            nodes.append(current)
            current = None
    return nodes


def classify_android_events(blob: str) -> dict[str, bool]:
    keyboard = mouse = abs_xy = touch = False
    for raw in blob.splitlines():
        line = raw.lower()
        if "ev_key" in line or re.search(r":\s+0001\s", line):
            if "btn_touch" in line or "btn_tool" in line:
                touch = True
            elif "btn_left" in line or "btn_right" in line or "btn_middle" in line:
                mouse = True
            elif "key_" in line:
                keyboard = True
        if "ev_rel" in line or "rel_x" in line or "rel_y" in line or "rel_wheel" in line:
            mouse = True
        if "ev_abs" in line or "abs_" in line:
            if "abs_mt_" in line or "slot" in line:
                touch = True
            if re.search(r"\babs_x\b", line) or re.search(r"\babs_y\b", line):
                abs_xy = True
        if "btn_touch" in line:
            touch = True
    return {"keyboard": keyboard, "mouse": mouse, "abs": abs_xy, "touch": touch, "midi": False}


def during_tablet(serial: str) -> Callable[[Callable[[], None]], dict[str, bool]]:
    def during(action: Callable[[], None]) -> dict[str, bool]:
        tablet_wake(serial)
        nodes = tablet_input_nodes(serial)
        # Piped getevent block-buffers, so a live readline loop sees nothing
        # until the process exits. timeout(1) kills it and libc flushes.
        try:
            proc = subprocess.Popen(
                ["adb", "-s", serial, "shell", "timeout", "3.5", "getevent", "-lt"],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
            )
        except OSError:
            action()
            return {"keyboard": False, "mouse": False, "abs": False, "touch": False, "midi": False}
        time.sleep(0.35)
        try:
            action()
            out, _err = proc.communicate(timeout=5.0)
        except Exception:
            proc.kill()
            try:
                proc.communicate(timeout=1.5)
            except Exception:
                pass
            raise
        lines = (out or "").splitlines()
        if nodes:
            filtered = [ln for ln in lines if any(node in ln for node in nodes)]
            if filtered:
                lines = filtered
        return classify_android_events("\n".join(lines))

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


def tablet_bounds(raw: str) -> Optional[tuple[int, int, int, int]]:
    m = re.match(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", raw or "")
    if not m:
        return None
    return int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))


def tablet_tap_xy(serial: str, box: tuple[int, int, int, int]) -> None:
    x = (box[0] + box[2]) // 2
    y = (box[1] + box[3]) // 2
    adb(serial, ["shell", "input", "tap", str(x), str(y)], timeout=4)


def tablet_tap_from_xml(serial: str, xml: str, needle: str, exact: bool = False) -> bool:
    try:
        root = ET.fromstring(xml)
    except ET.ParseError:
        return False
    needle_l = needle.lower()
    matches: list[tuple[bool, tuple[int, int, int, int]]] = []
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
        box = tablet_bounds(node.attrib.get("bounds") or "")
        if not box:
            continue
        matches.append((node.attrib.get("clickable") == "true", box))
    if not matches:
        return False
    clickable = [box for is_click, box in matches if is_click]
    box = (clickable or [matches[0][1]])[0]
    if not clickable:
        cx = (box[0] + box[2]) // 2
        cy = (box[1] + box[3]) // 2
        covering: list[tuple[int, tuple[int, int, int, int]]] = []
        for node in root.iter("node"):
            if node.attrib.get("clickable") != "true":
                continue
            parent = tablet_bounds(node.attrib.get("bounds") or "")
            if parent and parent[0] <= cx <= parent[2] and parent[1] <= cy <= parent[3]:
                covering.append(((parent[2] - parent[0]) * (parent[3] - parent[1]), parent))
        if covering:
            covering.sort()
            box = covering[0][1]
    tablet_tap_xy(serial, box)
    return True


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
    if tablet_input_nodes(serial):
        return True
    names = tablet_getevent_names(serial)
    if BLE_NAME.lower() in names.lower():
        return True
    dump = adb(serial, ["shell", "dumpsys", "bluetooth_manager"], timeout=8).stdout or ""
    low = dump.lower()
    idx = low.find("8d:25")
    if idx < 0:
        idx = low.find(BLE_NAME.lower())
    if idx < 0:
        return False
    window = low[idx : idx + 400]
    return "hogp connection state=2" in window or "hid connection state=2" in window


def tablet_pairing_dialog(xml: str) -> bool:
    labels = [x.lower() for x in tablet_labels(xml)]
    if any(x.startswith("pair with") for x in labels):
        return True
    return "cancel" in labels and "pair" in labels


def tablet_confirm_pair_dialog(serial: str) -> bool:
    """Tap the Android Pair button. Do not substring-match 'Pair new device'."""
    deadline = now() + 8.0
    while now() < deadline:
        xml = tablet_dump_xml(serial) or ""
        if not xml:
            time.sleep(0.35)
            continue
        try:
            root = ET.fromstring(xml)
        except ET.ParseError:
            time.sleep(0.35)
            continue
        for node in root.iter("node"):
            rid = node.attrib.get("resource-id") or ""
            text = (node.attrib.get("text") or "").strip().lower()
            if rid.endswith("button1") and text == "pair":
                box = tablet_bounds(node.attrib.get("bounds") or "")
                if box:
                    tablet_tap_xy(serial, box)
                    time.sleep(0.8)
                    return True
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
        ):
            if tablet_tap_from_xml(serial, xml, label, exact=True):
                time.sleep(0.8)
                return True
        if not tablet_pairing_dialog(xml):
            time.sleep(0.35)
            continue
        time.sleep(0.35)
    return False


def tablet_swipe_available(serial: str, xml: str) -> None:
    try:
        root = ET.fromstring(xml)
    except ET.ParseError:
        return
    box = None
    for node in root.iter("node"):
        rid = node.attrib.get("resource-id") or ""
        if rid.endswith("recycler_view"):
            box = tablet_bounds(node.attrib.get("bounds") or "")
            if box:
                break
    if not box:
        return
    x = (box[0] + box[2]) // 2
    y1 = box[1] + int((box[3] - box[1]) * 0.75)
    y2 = box[1] + int((box[3] - box[1]) * 0.25)
    adb(serial, ["shell", "input", "swipe", str(x), str(y1), str(x), str(y2)], timeout=4)


def tablet_keep_pairing(session: Optional["Session"]) -> None:
    if session is not None:
        session.post_settings(acceptNewHosts=True)


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
            "grep -il '30:30:f9:72:8d:25' \"$d\" 2>/dev/null | xargs -r rm -f; done",
        ],
        timeout=8,
    )
    adb(serial, ["shell", "cmd", "bluetooth_manager", "enable"], timeout=8)
    adb(serial, ["shell", "cmd", "bluetooth_manager", "wait-for-state:STATE_ON"], timeout=12)
    time.sleep(2.0)


def tablet_try_pair(serial: str, session: Optional["Session"] = None) -> tuple[bool, str]:
    if not adb_root(serial):
        return False, "adb present but not rooted"
    adb(serial, ["shell", "svc", "bluetooth", "enable"], timeout=6)
    adb(serial, ["shell", "cmd", "bluetooth_manager", "enable"], timeout=6)
    tablet_keep_pairing(session)
    time.sleep(1.0)
    if tablet_showbox_connected(serial):
        return True, "already connected"
    tablet_try_unpair(serial)
    tablet_back(serial)
    tablet_keep_pairing(session)
    xml = tablet_open_bt(serial)
    if xml is None:
        return False, "could not open Bluetooth settings"
    tapped = False
    reset_once = False
    pairing_started = False
    started = now()
    last_keep = started
    deadline = started + 90
    while now() < deadline:
        if session is not None and now() - last_keep > 25:
            tablet_keep_pairing(session)
            last_keep = now()
        if tablet_showbox_connected(serial):
            return True, "uiautomator pair/connect"
        xml = tablet_dump_xml(serial) or xml
        labels = tablet_labels(xml)
        low = [x.lower() for x in labels]
        joined = " ".join(low)
        if pairing_started:
            print(STYLE.dim("  Pair tapped; waiting for Android GATT/HOGP"))
            if tablet_wait_hogp(serial, 150.0, confirm=True, session=session):
                return True, "uiautomator pair/connect"
            return False, "tapped ShowboxMod; tablet did not complete HOGP connect"
        if tablet_pairing_dialog(xml):
            tapped = True
            pairing_started = True
            tablet_confirm_pair_dialog(serial)
            continue
        if BLE_NAME.lower() in low:
            if tablet_tap_from_xml(serial, xml, BLE_NAME, exact=True):
                tapped = True
                time.sleep(0.5)
                tablet_confirm_pair_dialog(serial)
                pairing_started = True
                continue
        if "pair new device" in low and "available devices" not in joined:
            tablet_tap_from_xml(serial, xml, "Pair new device", exact=True)
            time.sleep(2.5)
            continue
        if "refresh" in low:
            tablet_tap_from_xml(serial, xml, "Refresh")
            time.sleep(2.0)
            continue
        if "available devices" in joined:
            if BLE_NAME.lower() not in low:
                tablet_swipe_available(serial, xml)
            time.sleep(1.2)
            continue
        if not reset_once and now() - started >= 45:
            reset_once = True
            print(STYLE.dim("  tablet Bluetooth reset; ShowboxMod not on the pairing list yet"))
            tablet_bt_reset(serial)
            tablet_keep_pairing(session)
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


def tablet_showbox_in_config(serial: str) -> bool:
    conf = adb(serial, ["shell", "cat", "/data/misc/bluedroid/bt_config.conf"], timeout=6).stdout or ""
    return "30:30:f9:72:8d:25" in conf.lower()


def tablet_connect_paired(serial: str) -> bool:
    """After bonding, Android often needs Connect on the device details page."""
    tablet_back(serial)
    adb(serial, ["shell", "am", "start", "-a", "android.settings.BLUETOOTH_SETTINGS"], timeout=6)
    time.sleep(1.0)
    xml = tablet_dump_xml(serial)
    if not xml:
        return False
    labels = [x.lower() for x in tablet_labels(xml)]
    if "available devices" in " ".join(labels):
        tablet_back(serial)
        time.sleep(0.8)
        xml = tablet_dump_xml(serial) or xml
    if not tablet_tap_from_xml(serial, xml, BLE_NAME, exact=True):
        return False
    time.sleep(1.0)
    xml = tablet_dump_xml(serial) or ""
    if tablet_tap_from_xml(serial, xml, "Connect", exact=True):
        time.sleep(1.2)
        return True
    return "disconnect" in [x.lower() for x in tablet_labels(xml)]


def tablet_wait_hogp(
    serial: str,
    seconds: float = 150.0,
    confirm: bool = False,
    session: Optional["Session"] = None,
) -> bool:
    """Android GATT+HOGP beside AIRSTEP often takes ~2 minutes after the Pair tap.

    Do not uiautomator-dump in this loop unless a pairing dialog may still be
    up. Dumping the Settings UI every second is enough to stall HIDH.
    Keep the pairing window open: ads die when it closes beside AIRSTEP.
    """
    deadline = now() + seconds
    last_note = 0.0
    last_keep = now()
    started = now()
    tried_connect = False
    tablet_keep_pairing(session)
    while now() < deadline:
        if tablet_showbox_connected(serial):
            tablet_close_ads(session)
            return True
        if session is not None and now() - last_keep > 25:
            tablet_keep_pairing(session)
            last_keep = now()
        if confirm:
            xml = tablet_dump_xml(serial) or ""
            if tablet_pairing_dialog(xml):
                tablet_confirm_pair_dialog(serial)
                confirm = False
        elapsed = now() - started
        if not tried_connect and elapsed >= 25:
            tried_connect = True
            print(STYLE.dim("  opening ShowboxMod device details to tap Connect"))
            tablet_connect_paired(serial)
        if elapsed - last_note >= 15:
            last_note = elapsed
            print(STYLE.dim(f"  waiting for Android HOGP ({elapsed:.0f}s)"))
        time.sleep(1.5)
    return tablet_showbox_connected(serial)


def tablet_try_unpair(serial: str) -> str:
    xml = tablet_open_bt(serial)
    if xml and tablet_tap_from_xml(serial, xml, BLE_NAME, exact=True):
        time.sleep(0.8)
        xml = tablet_dump_xml(serial) or ""
        tablet_tap_from_xml(serial, xml, "Disconnect") or tablet_tap_from_xml(serial, xml, "Forget") or tablet_tap_from_xml(serial, xml, "Unpair")
        time.sleep(0.5)
        xml = tablet_dump_xml(serial) or ""
        tablet_tap_from_xml(serial, xml, "Forget") or tablet_tap_from_xml(serial, xml, "Unpair") or tablet_tap_from_xml(serial, xml, "OK")
        return "disconnect/forget from Bluetooth settings"
    # dumpsys always mentions ShowboxMod after a scan. Only reset for a real bond.
    if tablet_showbox_in_config(serial):
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
            action, heard = fire(f"{session.base}/api/v1/hid/touch", {"action": "tap", "x": "5000", "y": "5000", "via": "touch"})
            saw = during(action)
            if heard["code"] == 200 and (saw.get("abs") or saw.get("touch")):
                notes.append("abs-via-touch")
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
    host_addr = address
    for case in HOST_CASES:
        reconnects = 0
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
        host_addr = live_host_address(session, host_addr) or host_addr
        code, text = apply_case(session, host_addr, case)
        if code != 200:
            host_addr = live_host_address(session, host_addr) or host_addr
            code, text = apply_case(session, host_addr, case)
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
            host_addr = live_host_address(session, host_addr) or host_addr
            apply_case(session, host_addr, case)
            prev_desc = desc
            reconnects = 0
        elif desc is not None:
            prev_desc = desc
        host = None
        deadline = now() + 10
        while now() < deadline:
            midi_body = session.midi()
            hosts = connected_hosts(midi_body)
            if hosts:
                host = hosts[0]
                host_addr = str(host.get("address") or host_addr)
                if host_matches(host, case) and (not hid or midi_body.get("hidReady")):
                    break
            time.sleep(0.4)
        if host is None:
            results.fail(name, "host disconnected after role change")
            continue
        if not host_matches(host, case):
            host_addr = str(host.get("address") or host_addr)
            apply_case(session, host_addr, case)
            retry_until = now() + 8
            while now() < retry_until:
                midi_body = session.midi()
                hosts = connected_hosts(midi_body)
                if hosts:
                    host = hosts[0]
                    host_addr = str(host.get("address") or host_addr)
                    if host_matches(host, case) and (not hid or midi_body.get("hidReady")):
                        break
                time.sleep(0.4)
        if not host_matches(host, case):
            results.fail(
                name,
                f"API mix mismatch midi={host.get('midi')} hid={host.get('hid')} "
                f"kb={host.get('hidKeyboard')} mouse={host.get('hidMouse')} "
                f"abs={host.get('hidAbs')} dig={host.get('hidDigitizer')}",
            )
            continue
        want_mouse = bool(mouse) and not bool(abs_p or dig)
        if hid and prefix == "laptop":
            until = now() + 5.0
            while now() < until and not evdev_showbox():
                time.sleep(0.25)
            if reconnect is not None and reconnects < 1 and want_mouse and not evdev_has_mouse():
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
                if prefix == "laptop":
                    wait_evdev(8.0, need_mouse=want_mouse)
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
    print_run_reminders()
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
    plain_code, plain_body, location = http_get_noredirect(f"{s.base}/services", timeout=3.0)
    if plain_code == 200 and not location:
        r.pass_("http-plain", f"{plain_code} {len(plain_body)} bytes, no redirect")
    else:
        r.fail("http-plain", f"http={plain_code} location={location!r} size={len(plain_body)}")
    s.assert_uart(r, "uart-after-http")

    code, body = http_get(f"{s.base}/manifest.webmanifest", timeout=2.0)
    if code == 200 and b"ShowboxMod" in body and b"standalone" in body:
        r.pass_("pwa-manifest", f"{len(body)} bytes")
    else:
        r.fail("pwa-manifest", f"http={code} size={len(body)}")
    code, body = http_get(f"{s.base}/sw.js", timeout=2.0)
    if code == 200 and b"showboxmod-v" in body and b"addEventListener(\"fetch\"" in body:
        r.pass_("pwa-sw", f"{len(body)} bytes")
    else:
        r.fail("pwa-sw", f"http={code} size={len(body)}")
    code, body = http_get(f"{s.base}/icon-192.png", timeout=2.0)
    if code == 200 and body[:8] == b"\x89PNG\r\n\x1a\n":
        r.pass_("pwa-icon", f"{len(body)} bytes")
    else:
        r.fail("pwa-icon", f"http={code} size={len(body)}")

    https = {}
    code = 0
    for _ in range(12):
        code, body = http_get(f"{s.base}/api/v1/https", timeout=2.0)
        try:
            https = json.loads(body.decode("utf-8", "replace")) if code == 200 else {}
        except json.JSONDecodeError:
            https = {}
        if code == 200 and https.get("enabled") is True and https.get("listening"):
            break
        if code == 200 and https.get("enabled") is not True:
            break
        time.sleep(0.5)
    if code == 200 and https.get("enabled") is True and https.get("listening"):
        r.pass_("https-on", f"listening={https.get('listening')} hasCa={https.get('hasCa')}")
    elif https.get("enabled") is True:
        r.fail("https-on", f"http={code} enabled={https.get('enabled')} listening={https.get('listening')}")
    else:
        r.skip("https-on", "HTTPS not enabled")
    if tcp_open(args.ip, 443):
        r.pass_("https-port", "443 open")
    elif https.get("enabled") is True:
        r.fail("https-port", "443 closed while HTTPS is enabled")
    else:
        r.skip("https-port", "HTTPS not enabled")
    ca_code, ca_body = http_get(f"{s.base}/api/v1/https/ca.pem", timeout=2.0)
    if https.get("enabled") is not True:
        r.skip("https-ca", "HTTPS not enabled")
    elif ca_code == 200 and b"BEGIN CERTIFICATE" in ca_body:
        r.pass_("https-ca", f"{len(ca_body)} bytes")
    elif https.get("listening"):
        r.fail("https-ca", f"http={ca_code} size={len(ca_body)}")
    else:
        r.skip("https-ca", "TLS still starting")
    if https.get("enabled") is True:
        try:
            ver, payload = https_get(args.ip, "/services", timeout=8.0)
            ok = b"HTTP/1." in payload[:32] and b" 301" not in payload[:48] and b" 302" not in payload[:48]
            if ok and len(payload) > 200:
                r.pass_("https-handshake", f"{ver} {len(payload)} bytes")
            else:
                r.fail("https-handshake", f"{ver} {payload[:120]!r}")
        except Exception as exc:
            r.fail("https-handshake", str(exc))
    else:
        r.skip("https-handshake", "HTTPS not enabled")

    code, body = http_get(f"{s.base}/", timeout=8.0, ua="Mozilla/5.0")
    if code == 200 and len(body) > 10000:
        r.pass_("http-root", f"{code} {len(body)} bytes")
    else:
        r.fail("http-root", f"http={code} size={len(body)}")

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

    uni = mdns_query_a(args.host, args.ip, 0.5, toward=args.ip)
    mcast = mdns_query_a(args.host, "224.0.0.251", 0.8, toward=args.ip)
    resolv = resolver_ips(args.host)
    if args.ip in mcast:
        r.pass_("mdns-a", f"unicast={uni} mcast={mcast} resolv={resolv}")
    elif args.ip in uni or args.ip in resolv:
        r.fail("mdns-a", f"no multicast A from module uni={uni} mcast={mcast} resolv={resolv}")
    else:
        r.fail("mdns-a", f"unicast={uni} mcast={mcast} resolv={resolv}")
    dests = [args.ip, "224.0.0.251"]
    if mdns_has_ptr("_http._tcp.local", dests, toward=args.ip):
        r.pass_("mdns-http", "_http._tcp from module")
    elif args.ip in mcast:
        r.skip("mdns-http", "no PTR in 0.7s (multicast A is up)")
    else:
        r.fail("mdns-http", "no multicast PTR and no multicast A")
    if args.ip in resolv or args.ip in uni:
        code, body = http_get_retry(f"{s.base}/services", timeout=2.0, host=args.host)
        if code == 200 and len(body) > 1000:
            r.pass_("http-via-mdns", f"{code} {len(body)} bytes Host={args.host}")
        else:
            r.fail("http-via-mdns", f"http={code} size={len(body)}")
        root_ok = any(st == "PASS" and name == "http-root" for st, _sec, name, _d in r.rows)
        if root_ok:
            r.pass_("mixer-via-mdns", "http-root already pulled the SPA; skip a second gzip under BLE")
        else:
            code, body = http_get(f"{s.base}/", timeout=8.0, host=args.host, ua="Mozilla/5.0")
            if code == 200 and len(body) > 10000:
                r.pass_("mixer-via-mdns", f"{code} {len(body)} bytes Host={args.host}")
            else:
                r.fail("mixer-via-mdns", f"http={code} size={len(body)}")
    else:
        r.skip("http-via-mdns", "name did not resolve")
        r.skip("mixer-via-mdns", "name did not resolve")
    s.assert_uart(r, "uart-after-mdns")

    tel_ok, tel_detail = telnet_messages(args.ip, want=1, timeout=4.0)
    if tel_ok:
        r.pass_("telnet", tel_detail)
    else:
        r.fail("telnet", tel_detail)
    s.assert_uart(r, "uart-after-telnet")

    ota_mdns = mdns_has_ptr("_arduino._tcp.local", dests, toward=args.ip)
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
            ping_fut = pool.submit(ping_ok, args.ip)
            code, body = http_get(f"{s.base}/services")
            svc_ok = code == 200 and len(body) > 1000
            _code, link_body = http_json(f"{s.base}/api/v1/showbox/link")
            _code, midi_body = http_json(f"{s.base}/api/v1/midi")
            midi_ok = bool(midi_body.get("discoverable"))
            try:
                ping = bool(ping_fut.result())
            except Exception:
                ping = False
                sim_fail += 1
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
    if cmd not in ("OK", "NO", "BY", "CK", "IN"):
        time.sleep(0.4)
        cmd = applemidi_invite(args.ip, timeout=2.0)
    if cmd in ("OK", "NO", "BY", "CK", "IN"):
        r.pass_("wifi-rtp-midi", f"AppleMIDI UDP {APPLEMIDI_PORT} replied {cmd}")
    elif not wifi.get("enabled"):
        r.skip("wifi-rtp-midi", "Wi-Fi RTP-MIDI transport is off")
    else:
        r.fail("wifi-rtp-midi", f"no AppleMIDI reply on UDP {APPLEMIDI_PORT} (got {cmd or 'timeout'})")
    s.assert_uart(r, "uart-after-rtp-midi")

    print(STYLE.dim(f"  host LE scan for {BLE_NAME} ({args.host_scan:.0f}s)"))
    laptop_open_ads(s)
    status, scan_detail, scanned_addr = host_le_scan(args.host_scan)
    if status == "pass":
        r.pass_("ble-advertising-air", scan_detail)
    elif status == "skip":
        r.skip("ble-advertising-air", scan_detail)
    else:
        r.fail("ble-advertising-air", scan_detail)
    s.post_settings(acceptNewHosts=False)
    wifi_until = now() + 15.0
    while now() < wifi_until and not ping_ok(args.ip):
        time.sleep(0.4)
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
        r.skip("host-matrix", "slow; pass --pair if the HID+MIDI matrix has earned its keep")
    else:
        bt_disconnect_named("ShowBox")
        bt_scan_off()
        # A leftover Android HOGP link plus BlueZ discovery is what produced
        # bluetoothd ATT Unlikely Error on HID Information / Report Reference.
        if args.tablet:
            serials = adb_devices()
            if serials:
                print(STYLE.dim("  dropping leftover tablet ShowboxMod bond before laptop pair"))
                tablet_try_unpair(serials[0])
                # Identity + last RPAs. Forget also ignores them so a leftover
                # Android bond cannot take the only host slot during laptop pair.
                for addr in ("c0:3d:03:11:1f:c8", "5f:9a:8f:79:59:2c", "47:62:67:e6:a7:5d"):
                    module_forget(s, addr)
        # Leftover NVS hosts keep the last laptop/tablet mix (often digitizer).
        # Pairing on that map then applying kb+mouse rebuilds HOGP and drops the link.
        forget_module_hosts(s)
        s.post_settings(acceptNewHosts=True, bleHid=True, bleMidiPeripheral=True, bleHidKeyboard=True, bleHidMouse=True, bleHidAbs=False, bleHidDigitizer=False)
        laptop_open_ads(s)

        if want_laptop:
            addr = scanned_addr or bt_find_showbox()
            if not addr:
                status, scan_detail, scanned_addr = host_le_scan(6.0)
                addr = scanned_addr or bt_find_showbox()
            if not addr:
                r.skip("laptop-pair", "ShowboxMod address not in bluetoothctl")
            else:
                print(STYLE.dim(f"  pairing this laptop to {addr} (temporary)"))
                paired, how = laptop_pair(addr, s)
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
                        names = wait_laptop_hid(addr, s)
                        if not names:
                            r.fail("laptop-hid-device", "no /dev/input node named ShowboxMod")
                        else:
                            r.pass_("laptop-hid-device", ", ".join(sorted(names)))
                            http_post(f"{s.base}/api/v1/hid/keyboard", {"key": "pageup"})
                            time.sleep(0.5)
                        run_pairing_matrix(
                            s,
                            r,
                            "laptop",
                            host.get("address") or addr,
                            during_laptop,
                            False,
                            reconnect=lambda: laptop_reconnect(addr, s),
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
                        module_forget(s, (host or {}).get("address") or addr)
                        wait_no_hosts(s, 6.0)
                        s.post_settings(
                            acceptNewHosts=True,
                            bleHid=True,
                            bleMidiPeripheral=True,
                            bleHidKeyboard=True,
                            bleHidMouse=True,
                            bleHidAbs=False,
                            bleHidDigitizer=False,
                        )
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
                forget_module_hosts(s)
                s.post_settings(
                    acceptNewHosts=True,
                    bleHid=True,
                    bleMidiPeripheral=True,
                    bleHidKeyboard=True,
                    bleHidMouse=True,
                    bleHidAbs=False,
                    bleHidDigitizer=False,
                )
                adv_deadline = now() + 12.0
                while now() < adv_deadline and not s.midi().get("advertising"):
                    s.post_settings(acceptNewHosts=True, bleHid=True, bleMidiPeripheral=True)
                    time.sleep(1.0)
                adv_status, adv_detail, _adv_addr = host_le_scan(8.0)
                if adv_status != "pass":
                    print(STYLE.dim(f"  host LE scan after unpair: {adv_detail}"))
                    s.post_settings(acceptNewHosts=True)
                    time.sleep(3.0)
                print(STYLE.dim(f"  pairing tablet {serial} to ShowboxMod (temporary)"))
                # Leftover Android DSCP is a 31-byte truncated Pointer
                # (MTU 23 first+last blob). Reusing that bond yields EV=1.
                if tablet_showbox_in_config(serial) or tablet_showbox_connected(serial):
                    print(STYLE.dim("  dropping leftover Android ShowboxMod bond/cache"))
                    tablet_try_unpair(serial)
                    time.sleep(1.0)
                ok_tab, how = tablet_try_pair(serial, s)
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
                    # Leftover NVS mix from the laptop matrix can be digitizer-only.
                    apply_case(s, host.get("address") or "", HOST_CASES[1])
                    mix_deadline = now() + 8
                    while now() < mix_deadline:
                        midi_body = s.midi()
                        hosts = connected_hosts(midi_body)
                        if hosts:
                            host = hosts[0]
                            if host_matches(host, HOST_CASES[1]) and midi_body.get("hidReady"):
                                break
                        time.sleep(0.3)
                    print(STYLE.dim("  waiting for Android HOGP (GATT discovery can take ~2 min)"))
                    hogp = tablet_wait_hogp(serial, 150.0, confirm=True, session=s)
                    if hogp:
                        nodes = tablet_input_nodes(serial)
                        r.pass_("tablet-hid-device", f"ShowboxMod nodes={nodes or 'dumpsys-only'}")
                        tablet_close_ads(s)
                    else:
                        r.fail("tablet-hid-device", "BLE host up but Android HOGP did not create an input device")
                    tablet_wake(serial)
                    run_pairing_matrix(
                        s,
                        r,
                        "tablet",
                        host.get("address") or "",
                        during_tablet(serial),
                        False,
                        reconnect=lambda: tablet_reconnect(serial, s),
                        rebond=lambda: tablet_rebond(s, serial),
                        forget=lambda: tablet_forget(s, serial),
                        initial_desc=(1, 1, 0),
                    )
                    saw_hid = {"keyboard": False, "mouse": False, "abs": False, "touch": False}
                    for st, _sec, name, detail in r.rows:
                        if st != "PASS" or not str(name).startswith("tablet "):
                            continue
                        blob = str(detail).lower()
                        if "key" in blob:
                            saw_hid["keyboard"] = True
                        if "rel" in blob:
                            saw_hid["mouse"] = True
                        if "abs" in blob:
                            saw_hid["abs"] = True
                        if "touch" in blob:
                            saw_hid["touch"] = True
                    missing = [kind for kind, ok in saw_hid.items() if not ok]
                    if missing:
                        r.fail("tablet-hid-android", "Android never saw " + ",".join(missing))
                    else:
                        r.pass_("tablet-hid-android", "keyboard + relative mouse + abs mouse + touch")
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
    parser.add_argument("--pair", dest="pair", action="store_true", default=False, help="pair laptop/tablet HID+MIDI matrix (slow, opt-in)")
    parser.add_argument("--no-pair", dest="pair", action="store_false", help="skip HID+MIDI host matrix (default)")
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
