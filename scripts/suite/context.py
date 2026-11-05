"""Shared live-test context: websocket feed, console tail, checks."""

from __future__ import annotations

import asyncio
import gzip
import json
import subprocess
import threading
import time
import urllib.error
import urllib.request
from typing import Any, Callable, Optional

import websockets

from live_helpers import HOST, IP, api, entity, var

AIRSTEP = "d9:d7:5b:c1:90:f4"
STALE_MS = 800


class CaseFail(Exception):
    pass


class CaseSkip(Exception):
    pass


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


def telnet_reset(ip: str) -> bool:
    """RemoteDebug `reset` after draining the banner. Recovers :80 when
    ping lives but HTTP is refused (USB RTS / console reboot leftover)."""
    import socket

    try:
        sock = socket.create_connection((ip, 23), 5)
    except OSError:
        return False
    sock.settimeout(0.4)
    buf = b""
    end = time.monotonic() + 7.0
    try:
        while time.monotonic() < end:
            try:
                chunk = sock.recv(8192)
                if not chunk:
                    break
                buf += chunk
                if b"Please type the command" in buf:
                    break
            except socket.timeout:
                if b"Please type the command" in buf:
                    break
        sock.sendall(b"reset\r\n")
        time.sleep(0.3)
    except OSError:
        sock.close()
        return False
    sock.close()
    return True


def http_get(url: str, timeout: float = 8.0, host: Optional[str] = None, ua: str = "showbox-suite") -> tuple[int, bytes]:
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


def _url_ip(url: str) -> str:
    if "://" not in url:
        return ""
    return url.split("/")[2].split(":")[0]


def wait_http(ip: str, timeout: float = 8.0) -> bool:
    """Wait until ICMP and a small GET both work. Do not hammer large bodies."""
    end = time.monotonic() + max(0.2, timeout)
    while time.monotonic() < end:
        if ping_ok(ip):
            code, body = http_get(f"http://{ip}/api/v1/showbox/link", timeout=1.0)
            if code == 200 and len(body) > 20:
                return True
            code, body = http_get(f"http://{ip}/services", timeout=1.0)
            if code == 200 and len(body) > 200:
                return True
        remaining = end - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(0.25, remaining))
    return False


def wait_http_quiet(ip: str, timeout: float = 12.0) -> bool:
    """After a large SPA body, do not SYN-flood :80. One GET, then back off.

    wait_http polls /link every 250 ms. When the listen PCB is rebound
    after SPA+AIRSTEP, that flood wedges it again (L3 att2: SPA 200 then
    PWA manifest code=0).
    """
    end = time.monotonic() + max(0.2, timeout)
    delay = 0.8
    while time.monotonic() < end:
        if ping_ok(ip):
            code, body = http_get(f"http://{ip}/api/v1/showbox/link", timeout=2.0)
            if code == 200 and len(body) > 20:
                return True
        remaining = end - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(delay, remaining))
        delay = min(delay * 1.4, 2.5)
    return False


def http_get_retry(
    url: str,
    timeout: float = 8.0,
    host: Optional[str] = None,
    ua: str = "showbox-suite",
    attempts: int = 3,
    min_bytes: int = 1,
    quiet: bool = False,
) -> tuple[int, bytes]:
    """BLE-coex stalls large TCP bodies; ping still flows. Retry like the PWA shell.

    After a large SPA GET, pass quiet=True: wait_http's 250ms SYN poll re-wedges
    the rebound listen PCB (L3 streak15 att2: /debug bytes=0 for 55s).
    """
    last = (0, b"")
    ip = _url_ip(url)
    for i in range(max(1, attempts)):
        last = http_get(url, timeout=timeout, host=host, ua=ua)
        if last[0] == 200 and len(last[1]) >= min_bytes:
            return last
        if ip:
            if quiet:
                wait_http_quiet(ip, timeout=min(4.0, timeout))
            else:
                wait_http(ip, timeout=min(2.5, timeout))
            time.sleep(0.35 * (i + 1))
    return last


def http_get_resumable(
    url: str,
    timeout: float = 4.0,
    ua: str = "Mozilla/5.0",
    attempts: int = 16,
    min_bytes: int = 1,
    deadline: float = 12.0,
) -> tuple[int, bytes]:
    """GET a large body the way the PWA does: keep partial bytes, Range-resume."""
    got = 0
    chunks: list[bytes] = []
    total = 0
    last_code = 0
    ip = _url_ip(url)
    end = time.monotonic() + max(timeout, deadline)
    for _ in range(max(1, attempts)):
        remaining = end - time.monotonic()
        if remaining <= 0:
            break
        slice_timeout = min(timeout, remaining)
        headers = {
            "Connection": "close",
            "User-Agent": ua,
            "Cache-Control": "no-store",
        }
        if got > 0:
            headers["Range"] = f"bytes={got}-"
        req = urllib.request.Request(url, headers=headers, method="GET")
        try:
            with urllib.request.urlopen(req, timeout=slice_timeout) as resp:
                last_code = resp.getcode() or 0
                if last_code == 200 and got > 0:
                    chunks.clear()
                    got = 0
                cr = resp.headers.get("Content-Range") or ""
                if cr and "/" in cr:
                    try:
                        total = int(cr.rsplit("/", 1)[1])
                    except ValueError:
                        pass
                cl = resp.headers.get("Content-Length")
                if not total and cl and last_code == 200:
                    try:
                        total = int(cl)
                    except ValueError:
                        pass
                while True:
                    buf = resp.read(2048)
                    if not buf:
                        break
                    chunks.append(buf)
                    got += len(buf)
        except Exception:
            if got == 0 and ip:
                wait_http(ip, timeout=min(2.0, end - time.monotonic()))
        assembled = b"".join(chunks)
        complete = bool(total and got >= total) or (
            not total and last_code == 200 and len(assembled) >= min_bytes
        )
        if last_code in (200, 206) and complete and gzip_complete(assembled):
            return 200, assembled
        if got == 0 and ip and not ping_ok(ip):
            wait_http(ip, timeout=min(2.0, end - time.monotonic()))
    assembled = b"".join(chunks)
    if last_code in (200, 206) and len(assembled) >= min_bytes and gzip_complete(assembled):
        return 200, assembled
    return last_code, assembled


def http_text(path: str, timeout: float = 8.0) -> tuple[int, str, dict[str, str]]:
    url = f"http://{IP}{path}"
    req = urllib.request.Request(url, headers={"Connection": "close", "Cache-Control": "no-store"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            headers = {k.lower(): v for k, v in resp.headers.items()}
            return resp.getcode() or 0, resp.read().decode("utf-8", "replace"), headers
    except urllib.error.HTTPError as err:
        headers = {k.lower(): v for k, v in (err.headers.items() if err.headers else [])}
        raw = err.read() if err.fp else b""
        return err.code, raw.decode("utf-8", "replace"), headers
    except Exception:
        return 0, "", {}


def gzip_complete(body: bytes) -> bool:
    if not body:
        return False
    if body[:2] != b"\x1f\x8b":
        return True
    try:
        gzip.decompress(body)
        return True
    except (OSError, EOFError, gzip.BadGzipFile):
        return False


def gunzip_if(body: bytes) -> bytes:
    if body[:2] == b"\x1f\x8b":
        try:
            return gzip.decompress(body)
        except (OSError, EOFError, gzip.BadGzipFile):
            return body
    return body


def same_addr(a: str, b: str) -> bool:
    return (a or "").replace(":", "").lower() == (b or "").replace(":", "").lower()


class LiveWs:
    def __init__(self, ip: str) -> None:
        self.ip = ip
        self.entities: dict[str, Any] = {}
        self.entity_gen: dict[str, int] = {}
        self.entity_at: dict[str, float] = {}
        self.last_link: dict[str, Any] = {}
        self.got_link = threading.Event()
        self.got_entity = threading.Event()
        self.error = ""
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name="live-ws", daemon=True)
        self._thread.start()

    def _run(self) -> None:
        try:
            asyncio.run(self._loop())
        except Exception as exc:
            self.error = str(exc)

    async def _loop(self) -> None:
        uri = f"ws://{self.ip}/ws"
        while not self._stop.is_set():
            try:
                async with websockets.connect(uri, max_size=None, open_timeout=3, close_timeout=1) as ws:
                    while not self._stop.is_set():
                        try:
                            raw = await asyncio.wait_for(ws.recv(), timeout=0.25)
                        except asyncio.TimeoutError:
                            continue
                        try:
                            msg = json.loads(raw)
                        except json.JSONDecodeError:
                            continue
                        kind = msg.get("type")
                        if kind == "entity":
                            eid = str(msg.get("id"))
                            self.entities[eid] = msg.get("value")
                            self.entity_gen[eid] = self.entity_gen.get(eid, 0) + 1
                            self.entity_at[eid] = time.monotonic()
                            self.got_entity.set()
                        elif kind == "link":
                            self.last_link = msg
                            self.got_link.set()
            except Exception as exc:
                self.error = str(exc)
                if self._stop.is_set():
                    return
                await asyncio.sleep(0.2)

    def gen(self, name: str) -> int:
        return self.entity_gen.get(name, 0)

    def wait_entity(
        self,
        name: str,
        pred: Callable[[Any], bool],
        timeout: float = 4.0,
        after_seq: Optional[int] = None,
    ) -> Any:
        end = time.monotonic() + timeout

        def current() -> tuple[Any, int]:
            return self.entities.get(name), self.entity_gen.get(name, 0)

        def matched() -> Any:
            val, seq = current()
            if val is not None and pred(val) and (after_seq is None or seq > after_seq):
                return val
            return None

        while True:
            hit = matched()
            if hit is not None:
                return hit
            remaining = end - time.monotonic()
            if remaining <= 0:
                break
            self.got_entity.clear()
            hit = matched()
            if hit is not None:
                return hit
            self.got_entity.wait(timeout=remaining)
        val, seq = current()
        if after_seq is not None and seq <= after_seq:
            return None
        return val

    def close(self) -> None:
        self._stop.set()
        if self._thread is not None and self._thread.is_alive():
            self._thread.join(timeout=1.5)
            self._thread = None


class ConsoleTail:
    def __init__(self) -> None:
        self.seq = 0
        self.lines: list[str] = []

    def pull(self) -> str:
        path = f"/api/v1/console?since={self.seq}"
        code, text, headers = http_text(path, timeout=4.0)
        if code != 200:
            return ""
        nxt = headers.get("x-console-next")
        if nxt and nxt.isdigit():
            self.seq = int(nxt)
        if text:
            self.lines.extend(ln for ln in text.splitlines() if ln.strip())
        return text

    def since_mark(self) -> int:
        return len(self.lines)

    def new_text(self, mark: int) -> str:
        self.pull()
        return "\n".join(self.lines[mark:])


class Suite:
    def __init__(self, args: Any) -> None:
        self.args = args
        self.ip = getattr(args, "ip", None) or IP
        self.host = getattr(args, "host", None) or HOST
        self.t0 = time.monotonic()
        self.ws = LiveWs(self.ip)
        self.console = ConsoleTail()
        self.ble = None
        self.rtp = None
        self.session = None
        self.module_addr = ""
        self._cc_gen: dict[str, Optional[int]] = {"LOOPER_STATE": None, "INPUT2_GAIN": None}
        self.last_cc_at = 0.0
        self._console_mark = 0

    def stamp(self) -> str:
        return f"{time.monotonic() - self.t0:6.1f}s"

    def check(self, label: str, ok: bool, detail: str = "") -> None:
        mark = "PASS" if ok else "FAIL"
        print(f"[{self.stamp()}] {mark}: {label} {detail}".rstrip())
        if not ok:
            raise CaseFail(f"{label}: {detail}".strip())

    def info(self, msg: str) -> None:
        print(f"[{self.stamp()}] {msg}")

    def start(self) -> None:
        import check_features as cf
        import live_helpers as lh
        import test_mdns_fast as mdns_fast

        lh.IP = self.ip
        lh.HOST = self.host
        mdns_fast.IP = self.ip
        mdns_fast.HOST = self.host
        self.session = cf.Session(self.ip, self.host, STALE_MS)
        end = time.monotonic() + 90.0
        while time.monotonic() < end:
            if ping_ok(self.ip):
                code, body = http_get(f"http://{self.ip}/api/v1/showbox/link", timeout=3.0)
                if code == 200 and body:
                    try:
                        data = json.loads(body.decode("utf-8", "replace"))
                    except (UnicodeDecodeError, json.JSONDecodeError):
                        data = {}
                    if isinstance(data, dict) and data.get("ok") is True:
                        break
        self.ws.start()
        self.console.pull()

    def close(self) -> None:
        if self.rtp is not None:
            try:
                self.rtp.close()
            except Exception:
                pass
            self.rtp = None
        if self.ble is not None:
            try:
                self.ble.close()
            except Exception:
                pass
            self.ble = None
        self.ws.close()

    def midi_status(self) -> dict[str, Any]:
        last: dict[str, Any] = {}
        end = time.monotonic() + 6.0
        for _ in range(6):
            if time.monotonic() >= end:
                break
            code, body = http_get(f"http://{self.ip}/api/v1/midi", timeout=1.0)
            if code == 200 and body:
                try:
                    data = json.loads(body.decode("utf-8", "replace"))
                except json.JSONDecodeError:
                    data = {}
                if isinstance(data, dict) and (data.get("bleSlots") or "hosts" in data):
                    return data
                if isinstance(data, dict) and data:
                    last = data
            if code in (0, 503):
                wait_http(self.ip, timeout=1.0)
                continue
            if not ping_ok(self.ip):
                break
        return last

    def link(self) -> dict[str, Any]:
        return api("/api/v1/showbox/link")

    def _mark_cc(self) -> None:
        self._cc_gen["LOOPER_STATE"] = self.ws.gen("LOOPER_STATE")
        self._cc_gen["INPUT2_GAIN"] = self.ws.gen("INPUT2_GAIN")
        self.last_cc_at = time.monotonic()

    def _entity_now(self, name: str) -> Optional[float]:
        val = self.ws.entities.get(name)
        if val is not None:
            try:
                return float(val)
            except (TypeError, ValueError):
                pass
        try:
            return entity(name)
        except Exception:
            return None

    def looper_now(self) -> Optional[float]:
        return self._entity_now("LOOPER_STATE")

    def refresh_looper(self) -> Optional[float]:
        """HTTP is firmware RAM. WS cache survives a 1005 reconnect without
        the missed rec→play delta, so it can sit at 1 while boost-on-play
        already fired. Pull live LOOPER_STATE and adopt it."""
        try:
            http_val = entity("LOOPER_STATE")
        except Exception:
            http_val = None
        if http_val is None:
            return self.looper_now()
        self.ws.entities["LOOPER_STATE"] = http_val
        self.ws.entity_gen["LOOPER_STATE"] = self.ws.entity_gen.get("LOOPER_STATE", 0) + 1
        self.ws.entity_at["LOOPER_STATE"] = time.monotonic()
        return float(http_val)

    def gain_now(self) -> Optional[float]:
        return self._entity_now("INPUT2_GAIN")

    def wait_next_state(self, timeout: float = 1.2) -> Optional[float]:
        """Next LOOPER_STATE websocket update after the last send_cc."""
        after = self._cc_gen.get("LOOPER_STATE")
        val = self.ws.wait_entity("LOOPER_STATE", lambda _v: True, timeout=timeout, after_seq=after)
        if val is not None:
            try:
                return float(val)
            except (TypeError, ValueError):
                pass
        # HTTP can be a stale dump 0 while the mixer is still at STOP/PLAY.
        return self.looper_now()

    def wait_state(self, want: float, timeout: float = 1.2) -> Optional[float]:
        got = self.wait_next_state(timeout=timeout)
        if got is not None and abs(got - want) < 0.05:
            return got
        return got

    def wait_gain(self, want: float, timeout: float = 2.0) -> Optional[float]:
        def pred(v: Any) -> bool:
            try:
                return abs(float(v) - want) < 0.35
            except (TypeError, ValueError):
                return False

        after = self._cc_gen.get("INPUT2_GAIN")
        end = time.monotonic() + timeout
        last = None
        while True:
            cached = self.ws.entities.get("INPUT2_GAIN")
            seq = self.ws.gen("INPUT2_GAIN")
            if pred(cached) and (after is None or seq > after):
                return float(cached)
            try:
                cur = entity("INPUT2_GAIN")
            except Exception:
                cur = None
            if cur is not None:
                last = cur
                if abs(cur - want) < 0.35:
                    return cur
            remaining = end - time.monotonic()
            if remaining <= 0:
                return last
            self.ws.got_entity.wait(timeout=min(0.2, remaining))
            self.ws.got_entity.clear()

    def wait_var(self, name: str, want: float, timeout: float = 2.0) -> Any:
        end = time.monotonic() + timeout
        last = None
        while True:
            try:
                last = var(name)
            except (urllib.error.URLError, TimeoutError, OSError):
                last = None
            if last is not None and abs(float(last) - want) < 0.05:
                return last
            remaining = end - time.monotonic()
            if remaining <= 0:
                return last
            time.sleep(min(0.05, remaining))

    def peek_console(self) -> None:
        extra = self.console.new_text(self._console_mark)
        if extra.strip():
            preview = " | ".join(extra.splitlines()[-3:])[:160]
            self.info(f"console: {preview}")
            self._console_mark = self.console.since_mark()

    def ble_cc(self, number: int, value: int = 127) -> None:
        if self.ble is None:
            raise CaseFail("BLE MIDI is not connected")
        self._console_mark = self.console.since_mark()
        self._mark_cc()
        self.ble.cc(number, value)

    def rtp_cc(self, number: int, value: int = 127) -> None:
        if self.rtp is None:
            raise CaseFail("AppleMIDI session is not open")
        self._console_mark = self.console.since_mark()
        self._mark_cc()
        self.rtp.cc(number, value)

    def ensure_rtp(self) -> None:
        if self.rtp is not None:
            return
        from suite.rtp_midi import RtpMidi

        last = "AppleMIDI connect failed"
        for _attempt in range(3):
            rtp = RtpMidi(self.ip)
            try:
                detail = rtp.connect()
                self.rtp = rtp
                self.info(f"AppleMIDI session {detail}")
                return
            except Exception as exc:
                last = str(exc)
                try:
                    rtp.close()
                except Exception:
                    pass
        raise CaseFail(last)

    def send_cc(self, number: int, value: int = 127) -> None:
        """Real MIDI: prefer an open RTP session, else BLE."""
        if self.rtp is not None:
            self.rtp_cc(number, value)
            return
        self.ble_cc(number, value)
