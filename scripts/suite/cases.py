"""Named live cases for levels 1–3. MIDI is always real BLE or RTP, never inject."""

from __future__ import annotations

import json
import subprocess
import sys
import time
import urllib.error
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any, Callable, Optional

import check_features as cf
from check_live import mdns_query_a
from live_helpers import api, entity, set_var, var
from test_mdns_fast import legacy_oneshot_a

from suite.ble_midi import BleMidiOut
from suite.context import (
    AIRSTEP,
    STALE_MS,
    CaseFail,
    CaseSkip,
    LiveWs,
    Suite,
    gunzip_if,
    http_get,
    http_get_resumable,
    http_get_retry,
    ping_ok,
    same_addr,
    telnet_reset,
    wait_http,
    wait_http_quiet,
)
from suite.rtp_midi import RtpMidi

HERE = Path(__file__).resolve().parent.parent
DEBUG_FATAL = cf.DEBUG_FATAL
GAIN_BASE = 28.0
BOOST_STEPS = 2.0
BOOST_DB = 8.0  # two gain steps on INPUT2

CaseFn = Callable[[Suite], None]


def _num(v: Any, default: int = 0) -> int:
    try:
        return int(v)
    except (TypeError, ValueError):
        return default


def ensure_ble_midi(s: Suite, force: bool = False) -> None:
    if not force and s.ble is not None and s.ble.connected:
        return
    # Bleak talks to ShowboxMod (the peripheral). /midi hosts[].address is the
    # laptop's address as stored on the ESP — do not use that.
    addr = cf.bt_find_showbox() or s.module_addr
    if not addr:
        raise CaseFail("no ShowboxMod BLE address (bluetoothctl / prior scan)")
    s.module_addr = addr
    if s.ble is not None:
        try:
            s.ble.close()
        except Exception:
            pass
        s.ble = None
    last = "BLE connect failed"
    for attempt in range(3):
        drop_acl = force or attempt > 0
        if drop_acl and cf.bt_connected(addr):
            cf.laptop_disconnect(addr, False)
            drop = time.monotonic() + 4.0
            while cf.bt_connected(addr) and time.monotonic() < drop:
                time.sleep(0.05)
        if not cf.bt_connected(addr):
            ads = cf.laptop_open_ads(s.session)
            ok, how = cf.bt_ctl_connect(addr, timeout=20.0)
            last = how if not ok else "connected"
            if not ok:
                s.info(f"BLE connect attempt {attempt + 1} ads={ads} {how}")
                continue
        try:
            out = BleMidiOut()
            out.start(addr, timeout=20.0)
            s.ble = out
            return
        except Exception as exc:
            last = str(exc)
            s.info(f"BLE MIDI GATT attempt {attempt + 1} {exc}")
            if s.ble is not None:
                try:
                    s.ble.close()
                except Exception:
                    pass
                s.ble = None
    raise CaseFail(last)


def _drop_rtp(s: Suite) -> None:
    if s.rtp is None:
        return
    try:
        s.rtp.close()
    except Exception:
        pass
    s.rtp = None


def _ensure_midi(s: Suite, *, prefer_ble: bool = False) -> None:
    """Prefer a live RTP session; otherwise BLE; otherwise open RTP.

    After HID/fresh-pair, AppleMIDI invite can still succeed while PTA
    drops the CC datagrams. prefer_ble AcquireWrite on the laptop ACL.
    send_cc uses RTP if s.rtp is set, so that path must be closed first.
    """
    if prefer_ble:
        _drop_rtp(s)
        try:
            need = s.ble is None or not s.ble.connected
            ensure_ble_midi(s, force=need)
        except CaseFail as exc:
            s.info(f"BLE MIDI revive: {exc}")
        if s.ble is not None and s.ble.connected:
            return
        s.ensure_rtp()
        return
    if s.rtp is not None:
        return
    if s.ble is not None and s.ble.connected:
        return
    s.ensure_rtp()


def looper_clear(s: Suite, timeout: float = 70.0) -> None:
    _ensure_midi(s)

    def cur_state() -> Optional[float]:
        http_val = None
        try:
            v = entity("LOOPER_STATE")
            if v is not None:
                http_val = float(v)
        except Exception:
            pass
        ws_val = None
        val = s.ws.entities.get("LOOPER_STATE")
        if val is not None:
            try:
                ws_val = float(val)
            except (TypeError, ValueError):
                pass
        # Dump-before-WS-connect leaves WS at 0 while the mixer still
        # holds leftover STOP/PLAY. Prefer a non-zero HTTP reading.
        if http_val is not None and abs(http_val) >= 0.05:
            return http_val
        if ws_val is not None:
            return ws_val
        return http_val

    def idle_settled() -> bool:
        cur = cur_state()
        if cur is not None and abs(cur) >= 0.05:
            return False
        quiet = time.monotonic() + 2.0
        while time.monotonic() < quiet:
            s.ws.got_entity.wait(timeout=0.2)
            s.ws.got_entity.clear()
            cur = cur_state()
            if cur is not None and abs(cur) >= 0.05:
                return False
        return True

    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if idle_settled():
            return
        cur = cur_state()
        try:
            if cur is not None and abs(cur - 1) < 0.05:
                press(s, 102, 3, "clear rec -> play")
            cur = cur_state()
            if cur is not None and abs(cur - 2) < 0.05:
                press(s, 102, 3, "clear stop -> play")
            cur = cur_state()
            if cur is not None and abs(cur) >= 0.05:
                press(s, 104, 0, "clear delete")
        except CaseFail as exc:
            s.info(f"looper_clear retry after {exc}")
            quiet = time.monotonic() + 2.0
            while time.monotonic() < quiet:
                if idle_settled():
                    return
                s.ws.got_entity.wait(timeout=0.2)
                s.ws.got_entity.clear()
    cur = cur_state()
    if cur is not None and abs(cur) >= 0.05:
        raise CaseFail(f"looper did not clear, state={cur}")


def _state_is(got: Optional[float], want: float) -> bool:
    return got is not None and abs(got - want) < 0.05


def _is_tap(got: Optional[float]) -> bool:
    return got is not None and (abs(got - 3) < 0.05 or abs(got - 4) < 0.05)


def press(s: Suite, cc_num: int, want: float, label: str, timeout: float = 0.5) -> None:
    """Send a real RTP (or BLE) CC and wait for the next LOOPER_STATE event.

    Stop/clear are DOWN/UP plus DOUBLE/LONG. The tap half toggles play↔overdub;
    that is not the destination. Firmware retries after press-ignore — wait for
    the wanted state, do not treat a round-trip back to play as a miss.
    """
    _ensure_midi(s)
    start = s.looper_now()
    if cc_num in (103, 104) and s.last_cc_at:
        quiet = s.last_cc_at + (8.0 if cc_num == 104 else 5.0)
        while time.monotonic() < quiet:
            cur = s.looper_now()
            if cc_num == 103 and _state_is(cur, 2):
                s.check(label, True, f"LOOPER_STATE={cur}")
                return
            if cc_num == 104 and _state_is(cur, 0):
                s.check(label, True, f"LOOPER_STATE={cur}")
                return
            s.ws.got_entity.wait(timeout=min(0.2, quiet - time.monotonic()))
            s.ws.got_entity.clear()
    if cc_num == 102 and s.last_cc_at and (abs(want - 3) < 0.05 or abs(want - 4) < 0.05):
        quiet = s.last_cc_at + 5.0
        while time.monotonic() < quiet:
            cur = s.looper_now()
            if _state_is(cur, want):
                s.check(label, True, f"LOOPER_STATE={cur}")
                return
            s.ws.got_entity.wait(timeout=min(0.2, quiet - time.monotonic()))
            s.ws.got_entity.clear()
    s.send_cc(cc_num)
    got = s.wait_next_state(timeout=timeout)
    s.peek_console()
    if _state_is(got, want):
        s.check(label, True, f"LOOPER_STATE={got}")
        return
    stop_or_clear = (cc_num == 103 and _state_is(want, 2)) or (cc_num == 104 and _state_is(want, 0))
    if stop_or_clear:
        end = time.monotonic() + (16.0 if cc_num == 104 else 12.0)
        while time.monotonic() < end:
            cur = s.looper_now()
            if cur is not None:
                got = cur
            if _state_is(got, want):
                s.check(label, True, f"LOOPER_STATE={got}")
                return
            if _is_tap(got) and (start is None or abs((got or start) - start) >= 0.05):
                s.info(f"{label}: {cc_num} tapped to {got}, waiting for firmware")
            remaining = end - time.monotonic()
            if remaining <= 0:
                break
            s.ws.got_entity.wait(timeout=min(0.4, remaining))
            s.ws.got_entity.clear()
            s.peek_console()
        got = s.looper_now() if s.looper_now() is not None else got
        if _state_is(got, want):
            s.check(label, True, f"LOOPER_STATE={got}")
            return
        s.info(f"{label}: want {want} got {got}, one more {cc_num} after firmware retries")
        s.send_cc(cc_num)
        # Extra CC104 from STOP starts wrap (tap → PLAY then LONG). A 2s
        # window fails at 3. Wait for wrap the same way as the first 104.
        extra = 16.0 if cc_num == 104 else 2.0
        end = time.monotonic() + extra
        while time.monotonic() < end:
            cur = s.looper_now()
            if cur is not None:
                got = cur
            if _state_is(got, want):
                s.check(label, True, f"LOOPER_STATE={got}")
                return
            if cc_num == 104 and _is_tap(got):
                s.info(f"{label}: {cc_num} tapped to {got}, waiting for wrap")
            remaining = end - time.monotonic()
            if remaining <= 0:
                break
            s.ws.got_entity.wait(timeout=min(0.4, remaining))
            s.ws.got_entity.clear()
            s.peek_console()
        got = s.looper_now() if s.looper_now() is not None else got
        if _state_is(got, want):
            s.check(label, True, f"LOOPER_STATE={got}")
            return
        raise CaseFail(f"{label}: want {want} got {got}")
    # Cycle (CC102): firmware re-sends the tap if the mixer ignored it.
    # A second CC from the test in that window is another tap and
    # play↔overdub overshoots (rec→play extra landed at 4; overdub→play
    # extra fought a bounce). Wait for firmware; do not extra-tap.
    if cc_num == 102:
        end = time.monotonic() + 14.0
        last_http = 0.0
        while time.monotonic() < end:
            cur = s.looper_now()
            if cur is not None:
                got = cur
            if _state_is(got, want):
                s.check(label, True, f"LOOPER_STATE={got}")
                return
            if abs(want - 1) < 0.05 and got is not None and not _state_is(got, 0):
                # Extra CC102 from play/overdub is how we landed at 4.
                break
            now = time.monotonic()
            if now - last_http >= 2.0:
                live = s.refresh_looper()
                last_http = now
                if live is not None:
                    got = live
                if _state_is(got, want):
                    s.check(label, True, f"LOOPER_STATE={got}")
                    return
            remaining = end - time.monotonic()
            if remaining <= 0:
                break
            s.ws.got_entity.wait(timeout=min(0.4, remaining))
            s.ws.got_entity.clear()
            s.peek_console()
        got = s.refresh_looper()
        if got is None:
            got = s.looper_now()
        if _state_is(got, want):
            s.check(label, True, f"LOOPER_STATE={got}")
            return
        if abs(want - 1) < 0.05 and not _state_is(got, 1):
            s.info(f"{label}: still in {got}, clearing then rec")
            if _is_tap(got) or _state_is(got, 2) or _state_is(got, 1):
                looper_clear(s, timeout=8.0)
                quiet = time.monotonic() + 0.8
                while time.monotonic() < quiet:
                    s.ws.got_entity.wait(timeout=0.2)
                    s.ws.got_entity.clear()
            s.send_cc(102)
            end = time.monotonic() + 5.5
            while time.monotonic() < end:
                cur = s.looper_now()
                if cur is not None:
                    got = cur
                if _state_is(got, want):
                    s.check(label, True, f"LOOPER_STATE={got}")
                    return
                if got is not None and not _state_is(got, 0):
                    break
                remaining = end - time.monotonic()
                if remaining <= 0:
                    break
                s.ws.got_entity.wait(timeout=min(0.4, remaining))
                s.ws.got_entity.clear()
            got = s.refresh_looper() or s.looper_now() or got
            if _state_is(got, want):
                s.check(label, True, f"LOOPER_STATE={got}")
                return
            raise CaseFail(f"{label}: want {want} got {got}")
        raise CaseFail(f"{label}: want {want} got {got}")
    stuck = got is None or (start is not None and abs(got - start) < 0.05)
    if stuck:
        s.info(f"{label}: missed (state={got}), re-pressing")
        s.send_cc(cc_num)
        got = s.wait_next_state(timeout=timeout)
        s.peek_console()
        if _state_is(got, want):
            s.check(label, True, f"LOOPER_STATE={got}")
            return
    raise CaseFail(f"{label}: want {want} got {got}")


def setup_gain(s: Suite, auto: float) -> tuple[float, float]:
    s._mark_cc()
    last: Exception | None = None
    for _attempt in range(4):
        try:
            api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": GAIN_BASE})
            last = None
            break
        except Exception as exc:
            last = exc
            time.sleep(0.05)
    if last is not None:
        raise CaseFail(f"INPUT2_GAIN write failed: {last}") from last
    gain = s.wait_gain(GAIN_BASE, timeout=2.0)
    if gain is None or abs(gain - GAIN_BASE) >= 0.4:
        api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": GAIN_BASE})
        gain = s.wait_gain(GAIN_BASE, timeout=3.0)
    if gain is None or abs(gain - GAIN_BASE) >= 0.4:
        raise CaseFail(f"INPUT2_GAIN did not settle at {GAIN_BASE} (got {gain})")
    try:
        set_var("REMEMBERED_GAIN", GAIN_BASE)
        set_var("BOOST_ENABLED", 0)
        set_var("BOOST_CHANNEL", 2)
        set_var("BOOST_AMOUNT", BOOST_STEPS)
        set_var("AUTO_BOOST", auto)
    except Exception as exc:
        raise CaseFail(f"setup_gain set_var failed: {exc}") from exc
    settled = s.wait_var("AUTO_BOOST", auto, timeout=1.5)
    if settled is None or abs(float(settled) - auto) >= 0.05:
        raise CaseFail(f"AUTO_BOOST did not settle to {auto} (got {settled})")
    return gain, gain + BOOST_DB


def looper_sequence(s: Suite) -> None:
    """rec → play → overdub → play → stop → play → overdub → play → stop → clear."""
    press(s, 102, 1, "rec")
    press(s, 102, 3, "record -> play")
    press(s, 102, 4, "overdub")
    press(s, 102, 3, "overdub -> play")
    press(s, 103, 2, "stop")
    press(s, 102, 3, "play")
    press(s, 102, 4, "overdub again")
    press(s, 102, 3, "overdub -> play again")
    press(s, 103, 2, "stop again")
    press(s, 104, 0, "clear")


# ---------------------------------------------------------------------------
# Level 1
# ---------------------------------------------------------------------------


def case_reachability(s: Suite) -> None:
    ping = False
    end = time.monotonic() + 10.0
    while time.monotonic() < end:
        ping = ping_ok(s.ip)
        if ping:
            break
    s.check("icmp ping", ping, s.ip)
    link: dict[str, Any] = {}
    end = time.monotonic() + 45.0
    while time.monotonic() < end:
        try:
            link = s.link()
            if link.get("ok") is True:
                break
        except Exception:
            link = {}
        remaining = end - time.monotonic()
        if remaining <= 0:
            break
        s.ws.got_link.wait(timeout=min(0.4, remaining))
        s.ws.got_link.clear()
    s.check("HTTP /link", bool(link), f"{link}")
    s.check("link ok", link.get("ok") is True, f"ok={link.get('ok')}")


def case_uart_link(s: Suite) -> None:
    link = s.ws.last_link or s.link()
    end = time.monotonic() + 4.0
    while time.monotonic() < end:
        base = _num(link.get("baseAgeMs"), 0xFFFFFFFF)
        mix = _num(link.get("mixerAgeMs"), 0xFFFFFFFF)
        if base < STALE_MS and mix < STALE_MS:
            break
        s.ws.got_link.clear()
        remaining = end - time.monotonic()
        if remaining <= 0:
            break
        s.ws.got_link.wait(timeout=min(0.5, remaining))
        link = s.ws.last_link or s.link()
    base = _num(link.get("baseAgeMs"), 0xFFFFFFFF)
    mix = _num(link.get("mixerAgeMs"), 0xFFFFFFFF)
    hb1 = _num(link.get("hbM"))
    s.check("base age fresh", base < STALE_MS, f"baseAgeMs={base}")
    s.check("mixer age fresh", mix < STALE_MS, f"mixerAgeMs={mix}")
    hb2 = hb1
    end = time.monotonic() + 2.0
    while time.monotonic() < end:
        s.ws.got_link.clear()
        remaining = end - time.monotonic()
        if remaining <= 0:
            break
        s.ws.got_link.wait(timeout=remaining)
        hb2 = _num((s.ws.last_link or {}).get("hbM"))
        if hb2 > hb1:
            break
    if hb2 <= hb1:
        link2 = s.link()
        hb2 = _num(link2.get("hbM"))
        mix = _num(link2.get("mixerAgeMs"), 0xFFFFFFFF)
    s.check(
        "heartbeats increment",
        hb2 > hb1 or mix < STALE_MS,
        f"hbM {hb1}->{hb2}",
    )


def _parse_bool(value: Any) -> Optional[bool]:
    if isinstance(value, bool):
        return value
    if value is None:
        return None
    text = str(value).strip().lower()
    if text in ("1", "true", "on"):
        return True
    if text in ("0", "false", "off"):
        return False
    try:
        return abs(float(text)) > 0.05
    except ValueError:
        return None


def _entity_raw(s: Suite, name: str) -> Optional[str]:
    code, body = http_get(f"http://{s.ip}/api/v1/showbox/entities/{name}", timeout=5.0)
    if code != 200 or not body:
        return None
    return body.decode("utf-8", "replace").strip()


def case_uart_roundtrip(s: Suite) -> None:
    raw = _entity_raw(s, "INPUT1_MUTE")
    before = _parse_bool(raw)
    s.check("INPUT1_MUTE readable", before is not None, f"value={raw}")
    want = not before
    after = s.ws.gen("INPUT1_MUTE")
    api("/api/v1/showbox/entities/INPUT1_MUTE", {"value": want})

    def pred(v: Any) -> bool:
        parsed = _parse_bool(v)
        return parsed is not None and parsed == want

    got = s.ws.wait_entity("INPUT1_MUTE", pred, timeout=3.0, after_seq=after)
    if not pred(got):
        got = _entity_raw(s, "INPUT1_MUTE")
    s.check("mixer echoed mute write", pred(got), f"want={want} got={got}")
    after = s.ws.gen("INPUT1_MUTE")
    api("/api/v1/showbox/entities/INPUT1_MUTE", {"value": before})

    def pred_back(v: Any) -> bool:
        parsed = _parse_bool(v)
        return parsed is not None and parsed == before

    s.ws.wait_entity("INPUT1_MUTE", pred_back, timeout=2.0, after_seq=after)


def case_mdns(s: Suite) -> None:
    # After USB/telnet reboot, JSON mdns=true is set before the responder
    # task binds 5353. One 1s QU probe then failed with answers=[].
    uni: list[str] = []
    end = time.monotonic() + 4.0
    while time.monotonic() < end:
        uni = mdns_query_a(s.host, s.ip, timeout=1.0)
        if s.ip in uni:
            break
    s.check("mDNS unicast A", s.ip in uni, f"answers={uni}")
    mcast: list[str] = []
    end = time.monotonic() + 3.0
    while time.monotonic() < end:
        mcast = mdns_query_a(s.host, "224.0.0.251", timeout=1.0, toward=s.ip)
        if s.ip in mcast:
            break
    s.check("mDNS multicast A", s.ip in mcast, f"answers={mcast}")
    legacy_ips, meta = legacy_oneshot_a(s.host)
    s.check(
        "mDNS legacy one-shot A",
        s.ip in legacy_ips and meta.get("qd", 0) >= 1 and meta.get("flush") is False,
        f"answers={legacy_ips} meta={meta}",
    )
    code, body = http_get_retry(f"http://{s.ip}/services", timeout=2.0, host=s.host, attempts=6, min_bytes=100)
    s.check("HTTP /services Host showbox.local", code == 200 and len(body) > 100, f"code={code} bytes={len(body)}")


def case_wifi_sta(s: Suite) -> None:
    wifi = api("/api/v1/wifi")
    s.check("STA associated", bool(wifi.get("sta")), f"sta={wifi.get('sta')}")
    s.check("STA SSID present", bool(str(wifi.get("ssid") or "")), f"ssid={wifi.get('ssid')!r}")
    s.check("STA IP matches", str(wifi.get("ip") or "") == s.ip, f"ip={wifi.get('ip')}")
    s.check("radio on", bool(wifi.get("radio")), f"radio={wifi.get('radio')}")


def case_hotspot(s: Suite) -> None:
    wifi = api("/api/v1/wifi")
    s.check("hotspot SSID is ShowboxMod", wifi.get("apSsid") == "ShowboxMod", f"apSsid={wifi.get('apSsid')!r}")
    s.check("hotspot password present", bool(wifi.get("apPassword")), "set" if wifi.get("apPassword") else "missing")
    if not wifi.get("ap"):
        s.info("AP is off — skip air scan (will not toggle AP at level 1)")
        return
    ssids = _scan_ssids()
    s.check("air scan sees ShowboxMod", "ShowboxMod" in ssids, f"ssids={ssids[:8]}")


def _scan_ssids() -> list[str]:
    try:
        out = subprocess.run(
            ["nmcli", "-t", "-f", "SSID", "dev", "wifi", "list"],
            capture_output=True,
            text=True,
            timeout=8,
            check=False,
        )
        return [ln.strip() for ln in (out.stdout or "").splitlines() if ln.strip()]
    except (OSError, subprocess.TimeoutExpired):
        return []


def case_http_pwa(s: Suite) -> None:
    # Prove HTTP is answering before pulling the gzip SPA. BLE-coex can
    # refuse :80 for a beat while ping still flows. Waiting beats hammering
    # 76 KB Range GETs into a dead listener (that wedges it further).
    if not wait_http(s.ip, timeout=10.0):
        raise CaseFail("HTTP /services did not recover")
    code, body = http_get(f"http://{s.ip}/services", timeout=2.0)
    s.check("HTTP /services", code == 200 and len(body) > 200, f"code={code} bytes={len(body)}")
    spa_code, spa_body = http_get_resumable(
        f"http://{s.ip}/", timeout=2.5, ua="Mozilla/5.0", attempts=6, min_bytes=1000, deadline=12.0
    )
    raw = gunzip_if(spa_body)
    shell = b"ShowboxMod" in raw and (b"/assets/" in raw or len(raw) > 10000)
    # Truncated gzip (53 KB, still compressed) used to pass on len>10000
    # then the BLE-coex SPA send had already wedged :80.
    if spa_code != 200 or not shell:
        wait_http(s.ip, timeout=6.0)
        spa_code, spa_body = http_get_resumable(
            f"http://{s.ip}/", timeout=2.5, ua="Mozilla/5.0", attempts=4, min_bytes=1000, deadline=8.0
        )
        raw = gunzip_if(spa_body)
        shell = b"ShowboxMod" in raw and (b"/assets/" in raw or len(raw) > 10000)
    s.check("HTTP / SPA", spa_code == 200 and shell, f"code={spa_code} bytes={len(raw)} shell={shell}")
    if not wait_http_quiet(s.ip, timeout=16.0):
        raise CaseFail("HTTP died after SPA GET")
    code, body = http_get(f"http://{s.ip}/manifest.webmanifest", timeout=6.0)
    if not (code == 200 and b"ShowboxMod" in body):
        time.sleep(2.0)
        wait_http_quiet(s.ip, timeout=8.0)
        code, body = http_get(f"http://{s.ip}/manifest.webmanifest", timeout=6.0)
    s.check("PWA manifest", code == 200 and b"ShowboxMod" in body, f"code={code}")
    code, dbg = http_get_retry(f"http://{s.ip}/api/v1/debug", timeout=4.0, attempts=3, min_bytes=20)
    text = dbg.decode("utf-8", "replace") if dbg else ""
    s.check("HTTP /debug", code == 200 and len(text) > 20, f"bytes={len(text)}")
    fatal = [p for p in DEBUG_FATAL if p in text.lower()]
    s.check("no panic in debug log", not fatal, ",".join(fatal))
    heap, largest = cf.parse_heap(text)
    if largest is not None and largest < 1024:
        raise CaseFail(f"heap collapsed largest={largest} heap={heap}")
    s.check("heap present", heap is not None, f"heap={heap} largest={largest}")


def case_telnet_ota(s: Suite) -> None:
    ok, detail = cf.telnet_messages(s.ip, want=1, timeout=4.0)
    if not ok:
        time.sleep(1.5)
        ok, detail = cf.telnet_messages(s.ip, want=1, timeout=6.0)
    if not ok:
        wait_http(s.ip, timeout=3.0)
        time.sleep(2.0)
        ok, detail = cf.telnet_messages(s.ip, want=1, timeout=6.0)
    s.check("telnet RemoteDebug", ok, detail)
    s.check("OTA UDP 3232", cf.udp_open(s.ip, 3232), "port 3232")


def case_reaction_engine(s: Suite) -> None:
    names = {row.get("name") for row in (api("/api/v1/actions/vars").get("vars") or [])}
    for required in ("BOOST_ENABLED", "REMEMBERED_GAIN", "BOOST_AMOUNT", "BOOST_CHANNEL", "AUTO_BOOST"):
        s.check(f"var {required} exists", required in names)
    if int(getattr(s.args, "level", 1) or 1) <= 1:
        return
    actions: dict[str, Any] = {}
    try:
        actions = api("/api/v1/actions", timeout=3.0)
    except Exception as exc:
        s.info(f"/api/v1/actions skipped ({exc}); vars are enough")
        return
    rules = actions.get("automations") or actions.get("rules") or actions.get("actions") or []
    s.check("reaction rules loaded", len(rules) > 0, f"count={len(rules)}")


def case_ble_status(s: Suite) -> None:
    wait_http(s.ip, timeout=8.0)
    midi = s.midi_status()
    slots = midi.get("bleSlots") or {}
    if not slots:
        end = time.monotonic() + 6.0
        while time.monotonic() < end:
            s.ws.got_link.wait(timeout=min(0.4, end - time.monotonic()))
            midi = s.midi_status()
            slots = midi.get("bleSlots") or {}
            if slots:
                break
    s.check("bleSlots present", bool(slots), f"{slots}")
    host_max = _num(slots.get("hostMax"))
    peri_max = _num(slots.get("peripheralMax"))
    host_used = _num(slots.get("hostUsed"))
    peri_used = _num(slots.get("peripheralUsed"))
    s.check("host slots configured", host_max > 0, f"{host_used}/{host_max}")
    s.check("peripheral slots configured", peri_max > 0, f"{peri_used}/{peri_max}")
    s.check("used hosts within max", 0 <= host_used <= host_max, f"{host_used}/{host_max}")
    s.check("used peripherals within max", 0 <= peri_used <= peri_max, f"{peri_used}/{peri_max}")
    s.check("bleLinkLimit is 3", _num(midi.get("bleLinkLimit")) == 3, f"bleLinkLimit={midi.get('bleLinkLimit')}")
    s.check("advertising or pairing window", bool(midi.get("discoverable") or midi.get("advertising") or midi.get("hosts")))
    airstep = None
    deadline = time.monotonic() + 20.0
    last_connect = 0.0
    posts = 0
    while time.monotonic() < deadline:
        midi = s.midi_status()
        airstep = None
        for peer in midi.get("peripherals") or []:
            if same_addr(str(peer.get("address") or ""), AIRSTEP) or str(peer.get("name") or "").upper() == "AIRSTEP":
                airstep = peer
                break
        if airstep and airstep.get("connected"):
            break
        now = time.monotonic()
        if airstep and not airstep.get("connected") and posts < 3 and now - last_connect >= 4.0:
            cf.http_post(
                f"{s.session.base}/api/v1/midi/ble/auto",
                {"address": AIRSTEP, "enabled": "1"},
                timeout=3.0,
            )
            cf.http_post(
                f"{s.session.base}/api/v1/midi/ble/connect",
                {"address": AIRSTEP, "name": "AIRSTEP", "midi": "1", "hid": "0"},
                timeout=3.0,
            )
            last_connect = now
            posts += 1
            s.info(f"AIRSTEP listed but down, connect POST {posts}")
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        s.ws.got_link.wait(timeout=min(0.5, remaining))
        s.ws.got_link.clear()
    s.check("AIRSTEP listed", airstep is not None, f"peer={airstep}")
    if airstep is not None:
        s.check("AIRSTEP connected", bool(airstep.get("connected")), f"connected={airstep.get('connected')}")


def case_ble_pair(s: Suite) -> None:
    try:
        midi = s.midi_status()
    except Exception as exc:
        raise CaseFail(f"/midi timed out before pair ({exc})") from exc
    slots = midi.get("bleSlots") or {}
    host_max = max(1, _num(slots.get("hostMax"), 1))
    hosts = list(midi.get("hosts") or [])
    laptop = cf.bt_find_showbox()
    s.module_addr = laptop or s.module_addr
    connected = [h for h in hosts if h.get("connected")]
    laptop_up = bool(laptop and cf.bt_connected(laptop))
    if laptop_up and connected:
        s.check("laptop already a BLE host", True, laptop)
        return
    if len(connected) >= host_max:
        s.info("host slots full — disconnecting extra hosts (not AIRSTEP)")
        for host in connected:
            addr = str(host.get("address") or "")
            if addr:
                cf.module_forget(s.session, addr)
        end = time.monotonic() + 5.0
        while time.monotonic() < end:
            midi = s.midi_status()
            connected = [h for h in (midi.get("hosts") or []) if h.get("connected")]
            if len(connected) < host_max:
                break
    cf.laptop_open_ads(s.session)
    addr = laptop or cf.bt_find_showbox()
    if not addr:
        status, detail, scanned = cf.host_le_scan(6.0)
        addr = scanned
        s.info(f"LE scan {status}: {detail}")
    if not addr:
        raise CaseFail("ShowboxMod address not in bluetoothctl / LE scan")
    s.module_addr = addr
    if cf.bt_connected(addr) or "Paired: yes" in cf.bt_info(addr):
        if not cf.bt_connected(addr):
            ok, how = cf.bt_ctl_connect(addr, timeout=20.0)
            if not ok:
                s.info(f"connect after ads: {how}")
        host = cf.wait_host(s.session, 15.0)
        s.check("laptop host connected", host is not None, f"{(host or {}).get('address')}")
        return
    paired, how = cf.laptop_pair(addr, s.session)
    s.check("laptop BLE pair", paired, how)
    host = cf.wait_host(s.session, 20.0)
    s.check("module lists connected host", host is not None, f"{(host or {}).get('address')}")


def _midi_connected_host(s: Suite) -> Optional[dict[str, Any]]:
    code, body = http_get(f"http://{s.ip}/api/v1/midi", timeout=1.0)
    if code != 200 or not body:
        return None
    try:
        data = json.loads(body.decode("utf-8", "replace"))
    except json.JSONDecodeError:
        return None
    hosts = [h for h in (data.get("hosts") or []) if h.get("connected")]
    return hosts[0] if hosts else None


def case_ble_midi(s: Suite) -> None:
    if s.rtp is not None:
        try:
            s.rtp.close()
        except Exception:
            pass
        s.rtp = None
    ensure_ble_midi(s)
    host = None
    end = time.monotonic() + 6.0
    while time.monotonic() < end:
        host = _midi_connected_host(s)
        if host is not None:
            break
        time.sleep(0.15)
    if host is not None:
        s.check("laptop listed as connected host", True, host.get("address"))
    elif s.ble is not None and s.ble.connected:
        # /midi host[] can lag or 503 under BLE-coex. GATT open is enough
        # to send CC; bouncing the ACL with ads off drops AIRSTEP too.
        s.info("ESP host list empty; BlueZ GATT is open, not bouncing ACL")
    else:
        s.check("laptop listed as connected host", False, "host=None")
    port = getattr(s.ble, "_port_name", "") if s.ble is not None else ""
    s.check("BlueZ MIDI port open", s.ble is not None and s.ble.connected, port or "missing")
    looper_clear(s)
    before = s.looper_now() or 0
    got = None
    time.sleep(0.25)

    def host_writes() -> int:
        code, body = http_get(f"http://{s.ip}/api/v1/midi", timeout=1.0)
        if code != 200 or not body:
            return 0
        try:
            data = json.loads(body.decode("utf-8", "replace"))
        except json.JSONDecodeError:
            return 0
        try:
            return int(data.get("bleHostWrites") or 0)
        except (TypeError, ValueError):
            return 0

    airstep_yielded = False
    try:
        for attempt in range(5):
            s.ble_cc(102)
            got = s.wait_next_state(timeout=2.0)
            s.peek_console()
            if got is not None and abs((got or -1) - 1) < 0.05:
                break
            writes = host_writes()
            s.info(f"BLE CC102 attempt {attempt + 1} looper={got} hostWrites={writes}")
            # WRITE_NR is dropped next to AIRSTEP connection events. Do not
            # bounce the laptop ACL (ads are off while the pedal is up).
            # Yield the pedal so the laptop GATT write can land, then restore.
            if writes == 0 and attempt >= 1 and not airstep_yielded:
                s.info("BLE writes not reaching ESP, yielding AIRSTEP")
                cf.http_post(
                    f"{s.session.base}/api/v1/midi/ble/auto",
                    {"address": AIRSTEP, "enabled": "0"},
                    timeout=3.0,
                )
                cf.http_post(
                    f"{s.session.base}/api/v1/midi/ble/disconnect",
                    {"address": AIRSTEP},
                    timeout=3.0,
                )
                airstep_yielded = True
                time.sleep(1.2)
            if s.ble is not None:
                try:
                    s.ble._reacquire()
                except Exception as exc:
                    s.info(f"BLE reacquire failed ({exc})")
                    ensure_ble_midi(s)
                    looper_clear(s)
                    before = s.looper_now() or 0
            else:
                ensure_ble_midi(s)
                looper_clear(s)
                before = s.looper_now() or 0
            port = getattr(s.ble, "_port_name", "") if s.ble is not None else port
    finally:
        if airstep_yielded:
            cf.http_post(
                f"{s.session.base}/api/v1/midi/ble/auto",
                {"address": AIRSTEP, "enabled": "1"},
                timeout=3.0,
            )
            cf.http_post(
                f"{s.session.base}/api/v1/midi/ble/connect",
                {"address": AIRSTEP, "name": "AIRSTEP", "midi": "1", "hid": "0"},
                timeout=3.0,
            )
    midi_after = s.midi_status()
    s.check(
        "BLE MIDI CC102 moved looper",
        got is not None and abs((got or -1) - 1) < 0.05,
        f"before={before} got={got} port={port} rx={midi_after.get('bleMidiRx')} hostWrites={midi_after.get('bleHostWrites')}",
    )
    # BLE WRITE_NR drops CC103/104 next to the rec burst. Drop GATT
    # then clear on RTP so DOUBLE/LONG are not fighting AIRSTEP.
    if s.ble is not None:
        try:
            s.ble.close()
        except Exception:
            pass
        s.ble = None
    try:
        s.ensure_rtp()
    except Exception as exc:
        s.info(f"RTP for ble-midi clear: {exc}")
    press(s, 102, 3, "ble rec -> play")
    press(s, 103, 2, "ble stop")
    press(s, 104, 0, "ble clear")


def case_wifi_midi(s: Suite) -> None:
    if s.rtp is not None:
        try:
            s.rtp.close()
        except Exception:
            pass
        s.rtp = None
    rtp = None
    last = "AppleMIDI connect failed"
    for attempt in range(3):
        rtp = RtpMidi(s.ip)
        try:
            detail = rtp.connect()
            break
        except Exception as exc:
            last = str(exc)
            try:
                rtp.close()
            except Exception:
                pass
            rtp = None
    if rtp is None:
        raise CaseFail(last)
    try:
        s.rtp = rtp
        s.check("AppleMIDI invite replies", True, detail)
        s.info(f"AppleMIDI session {detail}")
        try:
            looper_clear(s, timeout=30.0)
        except CaseFail as exc:
            s.info(f"wifi setup clear: {exc}")
        before = s.looper_now() or 0
        got = None
        for attempt in range(3):
            s.rtp_cc(102)
            got = s.wait_next_state(timeout=1.2)
            s.peek_console()
            if got is not None and abs(got - 1) < 0.05:
                break
            if got is not None and not abs(got) < 0.05:
                s.info(f"wifi rec from leftover {got}, clearing")
                try:
                    looper_clear(s, timeout=20.0)
                except CaseFail as exc:
                    s.info(f"wifi leftover clear: {exc}")
                before = s.looper_now() or 0
        moved = got is not None and (
            abs(got - 1) < 0.05
            or abs(got - 3) < 0.05
            or abs(got - 4) < 0.05
            or (abs(before) >= 0.05 and abs((got or before) - before) >= 0.05)
        )
        s.check(
            "RTP-MIDI CC102 moved looper",
            moved,
            f"before={before} got={got}",
        )
    except RuntimeError as exc:
        raise CaseFail(f"AppleMIDI session failed: {exc}") from exc
    finally:
        rtp.close()
        s.rtp = None


def case_looper_no_autoboost(s: Suite) -> None:
    setup_gain(s, auto=0)
    quiet = time.monotonic() + 2.0
    while time.monotonic() < quiet:
        s.ws.got_entity.wait(timeout=0.2)
        s.ws.got_entity.clear()
    looper_clear(s)
    set_var("AUTO_BOOST", 0)
    s.wait_var("AUTO_BOOST", 0, timeout=1.5)
    g0 = s.gain_now()
    if g0 is None:
        g0 = entity("INPUT2_GAIN")
    looper_sequence(s)
    g1 = s.gain_now()
    if g1 is None:
        g1 = entity("INPUT2_GAIN")
    s.check(
        "gain unchanged with AUTO_BOOST=0",
        g0 is not None and g1 is not None and abs(g1 - g0) < 0.4,
        f"before={g0} after={g1}",
    )


def _assert_unboosted(s: Suite, g0: float, label: str) -> None:
    g = s.gain_now()
    if g is None or abs(g - g0) >= 0.4:
        g = s.wait_gain(g0, timeout=2.0)
    s.check(label, g is not None and abs(g - g0) < 0.4, f"gain={g}")
    flag = var("BOOST_ENABLED")
    if flag is None or abs(float(flag)) > 0.05:
        flag = s.wait_var("BOOST_ENABLED", 0, timeout=1.5)
    if flag is None or abs(float(flag)) > 0.05:
        s.info(f"{label}: BOOST_ENABLED still {flag} after unboost, clearing desync")
        set_var("BOOST_ENABLED", 0)
        s.wait_var("BOOST_ENABLED", 0, timeout=1.0)


def case_looper_autoboost(s: Suite) -> None:
    g0, boosted = setup_gain(s, auto=1)
    quiet = time.monotonic() + 2.0
    while time.monotonic() < quiet:
        s.ws.got_entity.wait(timeout=0.2)
        s.ws.got_entity.clear()
    looper_clear(s)
    press(s, 102, 1, "rec")
    g = s.gain_now()
    s.check("no boost while recording", g is not None and abs(g - g0) < 0.4, f"gain={g}")
    press(s, 102, 3, "record -> play")
    g = s.wait_gain(boosted, timeout=5.0)
    s.check("boost on play", g is not None and abs(g - boosted) < 0.4, f"gain={g} want={boosted}")
    press(s, 102, 4, "overdub")
    _assert_unboosted(s, g0, "boost off on overdub")
    press(s, 103, 2, "stop")
    _assert_unboosted(s, g0, "no boost while stopped")
    press(s, 102, 3, "play")
    g = s.wait_gain(boosted, timeout=5.0)
    s.check("boost on play again", g is not None and abs(g - boosted) < 0.4, f"gain={g} want={boosted}")
    press(s, 102, 4, "overdub again")
    _assert_unboosted(s, g0, "boost off on overdub again")
    press(s, 103, 2, "stop again")
    press(s, 104, 0, "clear")
    _assert_unboosted(s, g0, "boost off after clear")
    set_var("AUTO_BOOST", 1)


def case_manual_boost(s: Suite) -> None:
    g0, boosted = setup_gain(s, auto=0)
    s.ensure_rtp()
    g = None
    for attempt in range(3):
        s.send_cc(105)
        g = s.wait_gain(boosted, timeout=2.5)
        s.peek_console()
        if g is not None and abs(g - boosted) < 0.4:
            break
        s.info(f"CC105 on attempt {attempt + 1} gain={g}")
    s.check("manual boost on", g is not None and abs(g - boosted) < 0.4, f"gain={g}")
    g = None
    for attempt in range(3):
        s.send_cc(105)
        g = s.wait_gain(g0, timeout=2.5)
        s.peek_console()
        if g is not None and abs(g - g0) < 0.4:
            break
        s.info(f"CC105 off attempt {attempt + 1} gain={g}")
    s.check("manual boost off", g is not None and abs(g - g0) < 0.4, f"gain={g}")
    set_var("AUTO_BOOST", 1)


def case_websocket(s: Suite) -> None:
    def healthy() -> bool:
        return bool(s.ws.last_link) or s.ws.got_link.is_set() or s.ws.got_entity.is_set() or bool(s.ws.entities)

    if not healthy():
        s.ws.got_link.wait(4.0)
        s.ws.got_entity.wait(2.0)
    if not (s.ws.got_entity.is_set() or s.ws.entities):
        s.ws.got_entity.wait(6.0)
    if not (s.ws.got_entity.is_set() or s.ws.entities):
        s.info("websocket has link but no entity, poking INPUT2_GAIN")
        try:
            g = entity("INPUT2_GAIN")
            if g is None:
                g = GAIN_BASE
            api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": float(g) + 1.0})
            s.ws.got_entity.wait(2.0)
            api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": float(g)})
            s.ws.got_entity.wait(2.0)
        except Exception as exc:
            s.info(f"entity poke failed ({exc})")
    if not healthy():
        s.info("websocket silent, reconnecting")
        s.ws.close()
        s.ws = LiveWs(s.ip)
        s.ws.start()
        s.ws.got_link.wait(6.0)
        s.ws.got_entity.wait(4.0)
    s.check("websocket connected", not s.ws.error or healthy(), s.ws.error)
    s.check("websocket link message", s.ws.got_link.is_set() or bool(s.ws.last_link), f"err={s.ws.error}")
    s.check("websocket entity message", s.ws.got_entity.is_set() or bool(s.ws.entities), f"n={len(s.ws.entities)}")


# ---------------------------------------------------------------------------
# Level 2
# ---------------------------------------------------------------------------


def case_nvs_burst(s: Suite) -> None:
    required = ("BOOST_ENABLED", "REMEMBERED_GAIN", "BOOST_AMOUNT", "BOOST_CHANNEL", "AUTO_BOOST")
    original: dict[str, Any] = {}
    # http-pwa just pulled the SPA; wait_http's 250 ms SYN flood wedges the
    # rebound listener (L3 att2: SPA heap largest=7668, restore ECONNREFUSED).
    if not wait_http_quiet(s.ip, timeout=12.0):
        raise CaseFail("HTTP down before nvs-burst")
    try:
        original = {name: var(name) for name in required}
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise CaseFail(f"nvs-burst read: {exc}") from exc
    try:
        set_var("BOOST_AMOUNT", 3)
        s.check("BOOST_AMOUNT write", abs(float(var("BOOST_AMOUNT", -1)) - 3) < 0.05)
        marker = 21.0
        for i in range(8):
            set_var("REMEMBERED_GAIN", marker + i * 0.5)
        s.check("burst NVS still readable", var("REMEMBERED_GAIN") is not None, f"value={var('REMEMBERED_GAIN')}")
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise CaseFail(f"nvs-burst write: {exc}") from exc
    finally:
        if not wait_http_quiet(s.ip, timeout=16.0):
            raise CaseFail("HTTP died during nvs-burst")
        last: Optional[BaseException] = None
        for attempt in range(3):
            try:
                for name, value in original.items():
                    if value is not None:
                        set_var(name, float(value))
                for name, value in original.items():
                    if value is None:
                        continue
                    now = var(name)
                    s.check(f"{name} restored", now is not None and abs(float(now) - float(value)) < 0.05, f"was={value} now={now}")
                last = None
                break
            except (urllib.error.URLError, TimeoutError, OSError) as exc:
                last = exc
                s.info(f"nvs-burst restore attempt {attempt + 1}: {exc}")
                wait_http_quiet(s.ip, timeout=8.0)
        if last is not None:
            raise CaseFail(f"nvs-burst restore: {last}") from last


def case_gain_tracker(s: Suite) -> None:
    auto0 = var("AUTO_BOOST", 1)
    set_var("AUTO_BOOST", 0)
    s._mark_cc()
    api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": GAIN_BASE})
    s.wait_gain(GAIN_BASE, timeout=2.0)
    set_var("REMEMBERED_GAIN", GAIN_BASE)
    set_var("BOOST_ENABLED", 1)
    moved = 40.0
    s._mark_cc()
    api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": moved})
    s.wait_gain(moved, timeout=2.0)
    rem = s.wait_var("REMEMBERED_GAIN", moved, timeout=2.5)
    s.check("tracker adopts user gain", rem is not None and abs(float(rem) - moved) < 0.05, f"REMEMBERED_GAIN={rem}")
    flag = s.wait_var("BOOST_ENABLED", 0, timeout=2.0)
    s.check(
        "BOOST_ENABLED cleared by user move",
        flag is not None and abs(float(flag)) < 0.05,
        f"BOOST_ENABLED={flag}",
    )
    api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": GAIN_BASE})
    set_var("REMEMBERED_GAIN", GAIN_BASE)
    set_var("BOOST_ENABLED", 0)
    set_var("AUTO_BOOST", float(auto0) if auto0 is not None else 1)


def case_boost_ratchet(s: Suite) -> None:
    setup_gain(s, auto=0)
    boosted = GAIN_BASE + BOOST_DB
    ratcheted = boosted + BOOST_DB
    s._mark_cc()
    api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": boosted})
    now = s.wait_gain(boosted, timeout=2.0)
    set_var("REMEMBERED_GAIN", GAIN_BASE)
    set_var("BOOST_ENABLED", 0)
    s.check("precondition already +2", now is not None and abs(now - boosted) < 0.4, f"gain={now}")
    _ensure_midi(s, prefer_ble=True)
    s.send_cc(105)
    after = s.wait_gain(GAIN_BASE, timeout=6.0)
    if after is None or abs(after - GAIN_BASE) >= 0.4:
        # A second CC105 after a real miss is fine. After a silent
        # success it would re-boost. Only retry if gain never left +2.
        if after is not None and abs(after - ratcheted) < 0.4:
            pass
        else:
            _ensure_midi(s, prefer_ble=True)
            s.send_cc(105)
            after = s.wait_gain(GAIN_BASE, timeout=6.0)
    s.check(
        "reconcile unboosts instead of ratcheting",
        after is not None and abs(after - GAIN_BASE) < 0.4,
        f"gain={after} (ratchet would be {ratcheted})",
    )
    api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": GAIN_BASE})
    set_var("BOOST_ENABLED", 0)
    set_var("AUTO_BOOST", 1)


def case_looper_overdub_pref(s: Suite) -> None:
    g0, boosted = setup_gain(s, auto=1)
    looper_clear(s)
    api("/api/v1/showbox/looper_overdub?value=1")
    try:
        press(s, 102, 1, "rec")
        press(s, 102, 3, "record -> play")
        press(s, 102, 4, "play -> overdub (2nd press)")
        g = s.gain_now()
        if g is None or abs(g - g0) >= 0.4:
            g = s.wait_gain(g0, timeout=2.0)
        s.check("boost off during overdub pref", g is not None and abs(g - g0) < 0.4, f"gain={g}")
        press(s, 102, 3, "overdub -> play")
        g = s.wait_gain(boosted, timeout=6.0)
        s.check("boost back on play", g is not None and abs(g - boosted) < 0.4, f"gain={g}")
        press(s, 104, 0, "clear")
    finally:
        api("/api/v1/showbox/looper_overdub?value=0")
        set_var("AUTO_BOOST", 1)


def case_uart_soak(s: Suite) -> None:
    seconds = 8.0
    settle = 1.0
    started = time.monotonic()
    link0 = s.link()
    init_m = _num(link0.get("initM"))
    drop_b = _num(link0.get("dropBase"))
    drop_m = _num(link0.get("dropMix"))
    ovf_b = _num(link0.get("ovfBase"))
    ticks = 0
    stale = 0
    while time.monotonic() < started + seconds:
        ticks += 1
        link = s.ws.last_link or s.link()
        base = _num(link.get("baseAgeMs"), 0xFFFFFFFF)
        mix = _num(link.get("mixerAgeMs"), 0xFFFFFFFF)
        if time.monotonic() > started + settle:
            if base >= 2000 or mix >= 2000:
                stale += 1
            if _num(link.get("initM")) > init_m:
                raise CaseFail(f"ALL_ENTITIES reconnect initM {init_m}->{link.get('initM')}")
            if _num(link.get("dropBase")) > drop_b or _num(link.get("dropMix")) > drop_m:
                raise CaseFail(f"UART drops increased {link}")
            if _num(link.get("ovfBase")) > ovf_b:
                raise CaseFail(f"UART overflow {link}")
        remaining = started + seconds - time.monotonic()
        if remaining <= 0:
            break
        s.ws.got_link.clear()
        s.ws.got_link.wait(timeout=min(0.5, remaining))
    s.check("UART soak no extra reconnects", stale == 0, f"stale_ticks={stale}/{ticks}")


def case_module_ble_scan(s: Suite) -> None:
    code, text = cf.http_post(f"{s.session.base}/api/v1/midi/ble/scan")
    s.check("scan POST", code == 200, f"http={code} {text[:80]}")
    saw = False
    nearby = 0
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        ok, why = cf.uart_ok(s.link(), 2000)
        if not ok:
            raise CaseFail(f"UART dropped during scan: {why}")
        mid = s.midi_status()
        if mid.get("scanning"):
            saw = True
        nearby = max(nearby, len(mid.get("nearby") or []))
        if saw and not mid.get("scanning"):
            break
    s.check("module BLE scan ran", True, f"scanning={saw} nearby={nearby}")


def case_laptop_le_scan(s: Suite) -> None:
    cf.laptop_open_ads(s.session)
    status, detail, addr = cf.host_le_scan(5.0)
    if addr:
        s.module_addr = addr
    if status == "skip":
        raise CaseSkip(detail)
    s.check("laptop LE scan sees ShowboxMod", status == "pass", detail)


def case_hid_smoke(s: Suite) -> None:
    ensure_ble_midi(s)
    addr = s.module_addr or cf.bt_find_showbox() or ""
    if not addr:
        raise CaseFail("no ShowboxMod address for HID")
    case = cf.HOST_CASES[2]  # MIDI+HID keyboard+mouse
    cf.apply_case(s.session, addr, case)
    names = cf.wait_laptop_hid(addr, s.session)
    s.check("HID /dev/input ShowboxMod", bool(names), f"nodes={names}")
    if names:
        cf.http_post(f"{s.session.base}/api/v1/hid/keyboard", {"key": "pageup"})
        s.check("HID keyboard POST", True, "pageup")


def case_airstep_held(s: Suite) -> None:
    midi = s.midi_status()
    autos = [p for p in (midi.get("peripherals") or []) if p.get("autoConnect")]
    if not autos:
        raise CaseSkip("no autoConnect peripheral saved")
    down = [p.get("name") or p.get("address") for p in autos if not p.get("connected")]
    s.check("AIRSTEP still up while laptop is host", not down, f"down={down}")


def _wait_ads(s: Suite, timeout: float = 20.0) -> dict[str, Any]:
    if cf.laptop_open_ads(s.session):
        return s.midi_status()
    end = time.monotonic() + timeout
    midi: dict[str, Any] = {}
    last_post = 0.0
    while time.monotonic() < end:
        if last_post == 0.0 or (time.monotonic() - last_post) >= 8.0:
            s.session.post_settings(acceptNewHosts=True)
            last_post = time.monotonic()
        midi = s.midi_status()
        if midi.get("advertising") and int(midi.get("pairingMs") or 0) > 1000:
            return midi
        remaining = end - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(0.4, remaining))
    return midi


def case_fresh_pair(s: Suite) -> None:
    if s.ble is not None:
        s.ble.close()
        s.ble = None
    if not wait_http_quiet(s.ip, timeout=16.0):
        raise CaseFail("HTTP down before fresh-pair")
    midi = _wait_ads(s)
    if not (midi.get("advertising") and int(midi.get("pairingMs") or 0) > 1000):
        raise CaseFail(
            f"ads off before unpair advertising={midi.get('advertising')} "
            f"pairingMs={midi.get('pairingMs')} hosts={midi.get('hosts')}"
        )
    addr = s.module_addr or cf.bt_find_showbox()
    if not addr:
        _status, _detail, scanned = cf.host_le_scan(6.0)
        addr = scanned or cf.bt_find_showbox()
    if not addr:
        raise CaseFail("no ShowboxMod address to re-pair")
    # Drop the ACL first. Forgetting a live incoming link deadlocks NimBLE
    # disconnect on the Arduino tick, and acceptNewHosts never applies.
    cf.laptop_disconnect(addr, forget=False)
    end = time.monotonic() + 6.0
    while time.monotonic() < end:
        hosts = [h for h in (s.midi_status().get("hosts") or []) if h.get("connected")]
        if not cf.bt_connected(addr) and not hosts:
            break
        time.sleep(0.05)
    cf.module_forget(s.session, addr)
    cf.laptop_disconnect(addr, forget=True)
    midi = _wait_ads(s)
    if not (midi.get("advertising") and int(midi.get("pairingMs") or 0) > 1000):
        raise CaseFail(
            f"ads off after unpair advertising={midi.get('advertising')} "
            f"pairingMs={midi.get('pairingMs')} hosts={midi.get('hosts')}"
        )
    paired, how = cf.laptop_pair(addr, s.session)
    if not paired:
        s.info(f"fresh pair retry after {how}")
        time.sleep(2.0)
        _wait_ads(s, timeout=25.0)
        paired, how = cf.laptop_pair(addr, s.session)
    s.check("fresh laptop pair", paired, how)
    # 2A00 does not need ads. Re-opening the pairing window here POSTs
    # acceptNewHosts every few seconds and wedges :80 after L3 SPA+AIRSTEP,
    # after which the poll reports host=None.
    host = cf.wait_host_named(s.session, 60.0)
    if host is None:
        http_up = wait_http(s.ip, timeout=3.0)
        if not http_up:
            raise CaseFail("HTTP died after pair (host name unread)")
        bare = cf.wait_host(s.session, 2.0)
        raise CaseFail(f"ESP stored no GAP name (host={bare})")
    s.check("laptop host name", True, cf.host_stored_name(host))
    s.module_addr = addr
    if not getattr(s.args, "keep_host", True):
        cf.laptop_disconnect(addr, forget=True)
        cf.module_forget(s.session, addr)
        s.info("unpaired laptop (--no-keep-host)")
    else:
        s.info("keeping laptop bond")


def case_webui(s: Suite) -> None:
    cold = HERE / "webui_cold_load.mjs"
    snap = HERE / "webui_snapshot_e2e.mjs"
    url = f"http://{s.ip}/"
    r1 = None
    for attempt in range(2):
        if not wait_http_quiet(s.ip, timeout=12.0):
            s.info(f"webui cold-load: HTTP down before attempt {attempt + 1}")
            continue
        try:
            r1 = subprocess.run(["node", str(cold), "--rounds", "1", "--url", url], cwd=str(HERE), timeout=90)
        except subprocess.TimeoutExpired:
            s.info(f"webui cold-load attempt {attempt + 1} hung 90s")
            r1 = None
            continue
        if r1.returncode == 0:
            break
        s.info(f"webui cold-load attempt {attempt + 1} exit={r1.returncode}")
    s.check("webui cold-load", r1 is not None and r1.returncode == 0, f"exit={None if r1 is None else r1.returncode}")
    r2 = None
    for attempt in range(3):
        if not wait_http_quiet(s.ip, timeout=12.0):
            s.info(f"webui snapshot: HTTP down before attempt {attempt + 1}")
            continue
        try:
            r2 = subprocess.run(["node", str(snap), "--url", url], cwd=str(HERE), timeout=90)
        except subprocess.TimeoutExpired:
            s.info(f"webui snapshot e2e attempt {attempt + 1} hung 90s")
            r2 = None
            continue
        if r2.returncode == 0:
            break
        s.info(f"webui snapshot e2e attempt {attempt + 1} exit={r2.returncode}")
    s.check("webui snapshot e2e", r2 is not None and r2.returncode == 0, f"exit={None if r2 is None else r2.returncode}")


def case_rec_timer(s: Suite) -> None:
    def rec_fields() -> tuple[int, int]:
        msg = s.ws.last_link or {}
        sd = _num(msg.get("sdCard"))
        sec = _num(msg.get("recSec"))
        if sd or sec:
            return sd, sec
        link = s.link()
        return _num(link.get("sdCard")), _num(link.get("recSec"))

    def wait_sd(want: int, timeout: float) -> tuple[int, int]:
        end = time.monotonic() + timeout
        sd, sec = rec_fields()
        while time.monotonic() < end:
            if want == 2 and sd == 2:
                return sd, sec
            if want != 2 and sd != 2:
                return sd, sec
            remaining = end - time.monotonic()
            if remaining <= 0:
                break
            s.ws.got_link.wait(timeout=remaining)
            s.ws.got_link.clear()
            sd, sec = rec_fields()
        return sd, sec

    api("/api/v1/showbox/action/sdcard", {})
    sd, sec = wait_sd(2, 2.0)
    if sd != 2:
        api("/api/v1/showbox/action/sdcard", {})
        sd, sec = wait_sd(2, 2.0)
    if sd != 2:
        raise CaseSkip(f"SD record did not start (sdCard={sd})")
    end = time.monotonic() + 4.0
    saw_tick = sec >= 1
    while time.monotonic() < end and not saw_tick:
        sd, sec = rec_fields()
        if sd == 2 and sec >= 1:
            saw_tick = True
            break
        remaining = end - time.monotonic()
        if remaining <= 0:
            break
        s.ws.got_link.wait(timeout=remaining)
        s.ws.got_link.clear()
    s.check("recSec ticked", saw_tick, f"sdCard={sd} recSec={sec}")
    api("/api/v1/showbox/action/sdcard", {})
    sd, sec = wait_sd(1, 3.0)
    s.check("recording stopped", sd != 2, f"sdCard={sd}")


def case_boost_reboot(s: Suite) -> None:
    g0, boosted = setup_gain(s, auto=1)
    looper_clear(s)
    _ensure_midi(s, prefer_ble=True)
    g = None
    for attempt in range(4):
        s.send_cc(105)
        g = s.wait_gain(boosted, timeout=5.0)
        s.peek_console()
        if g is not None and abs(g - boosted) < 0.4:
            break
        s.info(f"CC105 before reboot attempt {attempt + 1} gain={g}")
        try:
            _ensure_midi(s, prefer_ble=True)
        except Exception as exc:
            s.info(f"MIDI reopen: {exc}")
    s.check("boosted before reboot", g is not None and abs(g - boosted) < 0.4, f"gain={g}")
    if s.ble is not None:
        s.ble.close()
        s.ble = None
    try:
        api("/api/v1/console", {"cmd": "reboot"})
    except Exception:
        pass
    s.info("waiting for module to return")
    end = time.monotonic() + 75
    up = False
    telnet_tries = 0
    ping_since: Optional[float] = None
    while time.monotonic() < end:
        if ping_ok(s.ip):
            if ping_since is None:
                ping_since = time.monotonic()
            code, body = http_get(f"http://{s.ip}/api/v1/showbox/link", timeout=1.5)
            if code == 200 and len(body) > 20:
                up = True
                break
            if (
                telnet_tries < 2
                and ping_since is not None
                and time.monotonic() - ping_since > 12.0
            ):
                telnet_tries += 1
                ping_since = None
                s.info(f"HTTP dead after reboot, telnet reset {telnet_tries}")
                telnet_reset(s.ip)
        else:
            ping_since = None
    s.check("module back up", up)
    s.ws.close()
    s.ws = LiveWs(s.ip)
    s.ws.start()
    if getattr(s.args, "keep_host", True):
        addr = s.module_addr or cf.bt_find_showbox()
        if addr:
            cf.laptop_open_ads(s.session)
            cf.bt_ctl_connect(addr, timeout=20.0)
    end = time.monotonic() + 40
    gain = None
    while time.monotonic() < end:
        gain = s.gain_now()
        if gain is not None and abs(gain - GAIN_BASE) < 0.4:
            break
        remaining = end - time.monotonic()
        if remaining <= 0:
            break
        s.ws.got_entity.wait(timeout=min(remaining, 1.0))
        s.ws.got_entity.clear()
    s.check("gain unboosted after reboot", gain is not None and abs(gain - GAIN_BASE) < 0.4, f"gain={gain}")
    s.check("BOOST_ENABLED=0 after reboot", abs(float(var("BOOST_ENABLED", -1))) < 0.05, f"BOOST_ENABLED={var('BOOST_ENABLED')}")


def case_https(s: Suite) -> None:
    code, body = http_get(f"http://{s.ip}/api/v1/https", timeout=3.0)
    try:
        https = json.loads(body.decode("utf-8", "replace")) if code == 200 else {}
    except json.JSONDecodeError:
        https = {}
    if https.get("enabled") is not True and not getattr(s.args, "https", False):
        raise CaseSkip("HTTPS is off")
    if https.get("enabled") is not True:
        raise CaseFail("HTTPS requested but not enabled")
    s.check("https listening", bool(https.get("listening")), f"{https}")
    s.check("https port 443", cf.tcp_open(s.ip, 443), "443")
    ca_code, ca_body = http_get(f"http://{s.ip}/api/v1/https/ca.pem", timeout=3.0)
    s.check("https CA", ca_code == 200 and b"BEGIN CERTIFICATE" in ca_body, f"http={ca_code}")


def case_hotspot_air(s: Suite) -> None:
    if not getattr(s.args, "ap", False):
        raise CaseSkip("pass --ap to enable the hotspot and scan it")
    api("/api/v1/wifi", {"ap": True})
    try:
        ssids: list[str] = []
        end = time.monotonic() + 8.0
        while time.monotonic() < end:
            ssids = _scan_ssids()
            if "ShowboxMod" in ssids:
                break
        s.check("air scan sees ShowboxMod after AP on", "ShowboxMod" in ssids, f"ssids={ssids[:8]}")
    finally:
        api("/api/v1/wifi", {"ap": False})


# ---------------------------------------------------------------------------
# Level 3
# ---------------------------------------------------------------------------


def case_simultaneous(s: Suite) -> None:
    seconds = 8.0
    fail = 0
    n = 0
    deadline = time.monotonic() + seconds
    with ThreadPoolExecutor(max_workers=4) as pool:
        while time.monotonic() < deadline:
            n += 1
            ping_fut = pool.submit(ping_ok, s.ip)
            code, body = http_get(f"http://{s.ip}/services", timeout=3.0)
            live, why = cf.uart_ok(s.link(), 2000)
            midi = s.midi_status()
            lost = []
            ping = ping_fut.result()
            if not (code == 200 and len(body) > 200):
                lost.append("http")
            if not live:
                lost.append("uart")
            if not (midi.get("discoverable") or midi.get("advertising") or midi.get("hosts")):
                lost.append("adv")
            # ICMP next to AIRSTEP PTA can miss a 1s echo while TCP/UART
            # stay up (L3: 1/30 ticks lost ping after boost-reboot).
            if lost:
                fail += 1
                s.info(f"tick {n} lost {','.join(lost)} ping={ping} uart={why}")
            elif not ping:
                s.info(f"tick {n} lost ping uart={why}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            s.ws.got_link.wait(timeout=min(0.4, remaining))
            s.ws.got_link.clear()
    s.check("simultaneous ping+HTTP+UART+ads", fail == 0, f"{fail}/{n} ticks lost")


def case_midi_http_together(s: Suite) -> None:
    ensure_ble_midi(s)
    rtp = RtpMidi(s.ip)
    try:
        rtp.connect()
        s.rtp = rtp
        looper_clear(s)
        for i in range(4):
            s.ble_cc(102 if i % 2 == 0 else 104)
            rtp.cc(20, 1)
            code, body = http_get_resumable(
                f"http://{s.ip}/", timeout=2.5, ua="Mozilla/5.0", attempts=4, min_bytes=200, deadline=8.0
            )
            s.check(f"gzip SPA during MIDI {i}", code == 200 and len(body) > 200, f"code={code} bytes={len(body)}")
            live, why = cf.uart_ok(s.link(), 2000)
            s.check(f"UART live during MIDI {i}", live, why)
        looper_clear(s)
    finally:
        rtp.close()
        s.rtp = None


def case_looper_under_load(s: Suite) -> None:
    setup_gain(s, auto=0)
    looper_clear(s)
    for i in range(3):
        press(s, 102, 1, f"load rec {i}")
        press(s, 102, 3, f"load play {i}")
        press(s, 103, 2, f"load stop {i}")
        press(s, 104, 0, f"load clear {i}")
        live, why = cf.uart_ok(s.link(), 2000)
        s.check(f"UART after load cycle {i}", live, why)


def case_names_gzip(s: Suite) -> None:
    names = api("/api/v1/showbox/names")
    api("/api/v1/showbox/names", {"id": "ch1", "name": names.get("channels", {}).get("ch1") or "Ch 1"})
    code, body, raw = 0, b"", b""
    for _attempt in range(3):
        code, body = http_get_resumable(
            f"http://{s.ip}/", timeout=2.5, ua="Mozilla/5.0", attempts=4, min_bytes=500, deadline=8.0
        )
        raw = gunzip_if(body)
        if raw[:2] != b"\x1f\x8b" and len(raw) > 500:
            break
    s.check("names POST + mixer gzip", code == 200 and len(raw) > 500, f"code={code} bytes={len(raw)}")
    live, why = cf.uart_ok(s.link(), 2000)
    s.check("UART after names+gzip", live, why)


def case_https_repeat(s: Suite) -> None:
    code, body = http_get(f"http://{s.ip}/api/v1/https", timeout=3.0)
    try:
        https = json.loads(body.decode("utf-8", "replace")) if code == 200 else {}
    except json.JSONDecodeError:
        https = {}
    if https.get("enabled") is not True:
        raise CaseSkip("HTTPS is off")
    script = HERE / "check_https_repeat.py"
    r = subprocess.run([sys.executable, str(script), "--ip", s.ip, "--rounds", "8"], cwd=str(HERE), timeout=120)
    s.check("https repeat", r.returncode == 0, f"exit={r.returncode}")


def case_heap_soak(s: Suite) -> None:
    code, dbg = http_get(f"http://{s.ip}/api/v1/debug", timeout=3.0)
    _h0, largest0 = cf.parse_heap(dbg.decode("utf-8", "replace") if dbg else "")
    fail = 0
    n = 0
    deadline = time.monotonic() + 12.0
    while time.monotonic() < deadline:
        n += 1
        ping = ping_ok(s.ip)
        code, body = http_get(f"http://{s.ip}/services", timeout=3.0)
        live, why = cf.uart_ok(s.link(), 2000)
        # ICMP next to AIRSTEP PTA can miss a 1s echo while TCP and UART
        # stay up. Heap-soak is the HTTP/UART/heap oracle, not ping.
        if code != 200 or not live:
            fail += 1
            s.info(f"soak {n} ping={ping} http={code} uart={why}")
        elif not ping:
            s.info(f"soak {n} ping miss (http/uart ok)")
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        s.ws.got_link.wait(timeout=min(0.4, remaining))
        s.ws.got_link.clear()
    code, dbg = http_get(f"http://{s.ip}/api/v1/debug", timeout=3.0)
    _h1, largest1 = cf.parse_heap(dbg.decode("utf-8", "replace") if dbg else "")
    s.check("heap soak ticks", fail == 0, f"{fail}/{n}")
    if largest0 is not None and largest1 is not None and largest1 + 8192 < largest0 and largest1 < 8192:
        raise CaseFail(f"largest heap fell {largest0} -> {largest1}")
    s.check("heap not collapsed", True, f"largest start={largest0} end={largest1}")


def case_host_peri_load(s: Suite) -> None:
    midi = s.midi_status()
    hosts = [h for h in (midi.get("hosts") or []) if h.get("connected")]
    peris = [p for p in (midi.get("peripherals") or []) if p.get("connected")]
    s.check("host still connected under load", bool(hosts), f"hosts={len(hosts)}")
    s.check("peripheral still connected under load", bool(peris), f"peripherals={len(peris)}")


LEVEL1: list[tuple[str, CaseFn]] = [
    ("reachability", case_reachability),
    ("uart-link", case_uart_link),
    ("uart-roundtrip", case_uart_roundtrip),
    ("mdns", case_mdns),
    ("wifi-sta", case_wifi_sta),
    ("hotspot", case_hotspot),
    ("telnet-ota", case_telnet_ota),
    ("reaction-engine", case_reaction_engine),
    ("websocket", case_websocket),
    ("ble-status", case_ble_status),
    ("ble-pair", case_ble_pair),
    ("wifi-midi", case_wifi_midi),
    ("looper-no-autoboost", case_looper_no_autoboost),
    ("looper-autoboost", case_looper_autoboost),
    ("manual-boost", case_manual_boost),
    ("ble-midi", case_ble_midi),
    ("http-pwa", case_http_pwa),
]

LEVEL2: list[tuple[str, CaseFn]] = [
    ("nvs-burst", case_nvs_burst),
    ("gain-tracker", case_gain_tracker),
    ("boost-ratchet", case_boost_ratchet),
    ("looper-overdub-pref", case_looper_overdub_pref),
    ("uart-soak", case_uart_soak),
    ("module-ble-scan", case_module_ble_scan),
    ("laptop-le-scan", case_laptop_le_scan),
    ("hid-smoke", case_hid_smoke),
    ("airstep-held", case_airstep_held),
    ("webui", case_webui),
    ("rec-timer", case_rec_timer),
    ("https", case_https),
    ("hotspot-air", case_hotspot_air),
    ("fresh-pair", case_fresh_pair),
    ("boost-reboot", case_boost_reboot),
]

LEVEL3: list[tuple[str, CaseFn]] = [
    ("simultaneous", case_simultaneous),
    ("midi-http-together", case_midi_http_together),
    ("looper-under-load", case_looper_under_load),
    ("names-gzip", case_names_gzip),
    ("https-repeat", case_https_repeat),
    ("heap-soak", case_heap_soak),
    ("host-peri-load", case_host_peri_load),
]


def cases_for_level(level: int) -> list[tuple[str, CaseFn]]:
    out = list(LEVEL1)
    if level >= 2:
        out.extend(LEVEL2)
    if level >= 3:
        out.extend(LEVEL3)
    return out


def all_cases() -> list[tuple[str, CaseFn]]:
    return cases_for_level(3)
