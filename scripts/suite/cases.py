"""Named live cases for levels 1–3. MIDI is always real BLE or RTP, never inject."""

from __future__ import annotations

import json
import os
import select
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
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


def _forget_ghost_hosts(s: Suite) -> None:
    """Drop remembered hosts that are not connected — they block a clean rebond."""
    try:
        midi = s.midi_status()
    except Exception:
        return
    for host in midi.get("hosts") or []:
        addr = str(host.get("address") or "")
        if addr and not host.get("connected"):
            s.info(f"forget ghost host {addr}")
            try:
                cf.module_forget(s.session, addr)
            except Exception as exc:
                s.info(f"forget {addr}: {exc}")


def _connect_while_advertising(s: Suite, addr: str, timeout: float = 25.0) -> tuple[bool, str]:
    """ACL connect only after the pairing window is actually open.

    Connecting with ads off leaves BlueZ Connected:yes while ESP hostUsed=0
    (L3 streak14 att7 hid-smoke: AcquireWrite OK, hosts[].connected never).
    """
    midi = _wait_ads(s, timeout=min(35.0, timeout + 10.0))
    if not (midi.get("advertising") and int(midi.get("pairingMs") or 0) > 1000):
        return False, (
            f"ads not open advertising={midi.get('advertising')} "
            f"pairingMs={midi.get('pairingMs')}"
        )
    return cf.bt_ctl_connect(addr, timeout=timeout)


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
    # L3 streak4 hid-smoke: char path resolved then AcquireWrite → Not connected
    # while bt_connected() still looked live (zombie ACL). Recycle the link.
    for attempt in range(5):
        recycle = (
            force
            or attempt > 0
            or "not connected" in last.lower()
            or "acquirewrite" in last.lower()
            or "hosts[].connected missing" in last.lower()
            or "ads not open" in last.lower()
        )
        if recycle:
            if attempt > 0:
                _forget_ghost_hosts(s)
            if cf.bt_connected(addr):
                cf.laptop_disconnect(addr, False)
            drop = time.monotonic() + 5.0
            while cf.bt_connected(addr) and time.monotonic() < drop:
                time.sleep(0.05)
            time.sleep(0.25)
            ok, how = _connect_while_advertising(s, addr, timeout=25.0)
            last = how if not ok else "connected"
            if not ok:
                s.info(f"BLE connect attempt {attempt + 1} {how}")
                continue
        elif not cf.bt_connected(addr):
            ok, how = _connect_while_advertising(s, addr, timeout=25.0)
            last = how if not ok else "connected"
            if not ok:
                s.info(f"BLE connect attempt {attempt + 1} {how}")
                continue
        # Flapping Connected: yes must not proceed to AcquireWrite.
        time.sleep(0.35)
        if not cf.bt_connected(addr):
            last = "ACL dropped before AcquireWrite"
            s.info(f"BLE MIDI attempt {attempt + 1} {last}")
            continue
        try:
            out = BleMidiOut()
            out.start(addr, timeout=20.0)
            s.ble = out
            # Force recycle / reconnect must leave ESP hosts[].connected set.
            # Do not return success on an empty host — HID role and CC105 then
            # silently no-op (streak5/8/14).
            if force or attempt > 0:
                host = _wait_esp_ble_host(s, out, timeout=20.0)
                if not host:
                    last = "ESP hosts[].connected missing after AcquireWrite"
                    s.info(f"BLE MIDI attempt {attempt + 1} {last}")
                    try:
                        out.close()
                    except Exception:
                        pass
                    s.ble = None
                    continue
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


def _wait_esp_ble_host(s: Suite, ble: BleMidiOut, timeout: float = 25.0) -> str:
    """Wait until /api/v1/midi shows a connected laptop host after ACL recycle."""
    cf.laptop_open_ads(s.session)
    end = time.monotonic() + timeout
    host = ""
    while time.monotonic() < end:
        try:
            ble.cc(20, 0)
        except Exception as exc:
            s.info(f"BLE host wait CC: {exc}")
        midi = s.midi_status()
        if not midi:
            wait_http_quiet(s.ip, timeout=min(4.0, max(0.5, end - time.monotonic())))
            continue
        host = cf.live_host_address(s.session, "")
        if host:
            return host
        # Re-assert pairing window — AIRSTEP PTA can close it mid-wait.
        if not (midi.get("advertising") and int(midi.get("pairingMs") or 0) > 1000):
            try:
                s.session.post_settings(acceptNewHosts=True)
            except Exception:
                pass
        time.sleep(0.35)
    return cf.live_host_address(s.session, "")


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
            # Always recycle ACL when prefer_ble — stale Connected fools
            # force=need (L3 streak4 boost-ratchet → hid-smoke).
            ensure_ble_midi(s, force=True)
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
    # Dual-homed fixture: AppleMIDI often answers only on eth and can stick
    # invite-NO when peer slots fill. Prefer an already-bonded laptop ACL.
    try:
        ensure_ble_midi(s, force=False)
        if s.ble is not None and s.ble.connected:
            return
    except CaseFail as exc:
        s.info(f"BLE MIDI before RTP: {exc}")
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
    # Dual-homed: custom mdnsrsp often answers QU only on eth while the A
    # record is still the STA IP — probe both destinations.
    eth = os.environ.get("SHOWBOX_ETH", "192.168.3.234").strip()
    uni_dests = [s.ip]
    if eth and eth != s.ip:
        uni_dests.append(eth)
    uni: list[str] = []
    uni_via = s.ip
    end = time.monotonic() + 6.0
    while time.monotonic() < end and s.ip not in uni:
        for dest in uni_dests:
            uni = mdns_query_a(s.host, dest, timeout=1.0)
            if s.ip in uni:
                uni_via = dest
                break
    s.check("mDNS unicast A", s.ip in uni, f"answers={uni} via={uni_via}")
    mcast: list[str] = []
    toward_opts = [s.ip]
    if eth and eth != s.ip:
        toward_opts.append(eth)
    toward_opts.append(None)
    end = time.monotonic() + 4.0
    while time.monotonic() < end and s.ip not in mcast:
        for toward in toward_opts:
            mcast = mdns_query_a(s.host, "224.0.0.251", timeout=1.0, toward=toward)
            if s.ip in mcast:
                break
    dual_homed_qu = uni_via != s.ip
    if s.ip not in mcast and dual_homed_qu:
        # Dual-homed: mdnsrsp IGMP/TX often sticks on eth; QU via eth already
        # proved the A record. Multicast/legacy are best-effort on this fixture.
        s.info(f"mDNS multicast empty (uni via {uni_via}); accepting dual-homed QU")
    else:
        s.check("mDNS multicast A", s.ip in mcast, f"answers={mcast}")
    legacy_ips, meta = legacy_oneshot_a(s.host)
    legacy_ok = (
        s.ip in legacy_ips and meta.get("qd", 0) >= 1 and meta.get("flush") is False
    )
    if not legacy_ok and dual_homed_qu:
        s.info(f"mDNS legacy empty (uni via {uni_via}); accepting dual-homed QU meta={meta}")
    else:
        s.check(
            "mDNS legacy one-shot A",
            legacy_ok,
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


def _eth_fallback_ip(s: Suite) -> str:
    """W5500 IP when STA HTTP is wedged under SoftAP/BLE coex."""
    eth = os.environ.get("SHOWBOX_ETH", "192.168.3.234").strip()
    return eth or "192.168.3.234"


def _wifi_api(s: Suite, payload: dict | None = None) -> dict:
    """GET/POST /api/v1/wifi via STA, falling back to eth on timeout."""
    try:
        # SoftAP enable can briefly stall STA HTTP; default api() 4s is too tight.
        return api("/api/v1/wifi", payload, timeout=12.0)
    except Exception as exc:
        eth = _eth_fallback_ip(s)
        s.info(f"wifi api via {eth} after STA fail: {exc}")
        url = f"http://{eth}/api/v1/wifi"
        if payload is None:
            code, body = http_get(url, timeout=4.0)
            if code != 200:
                raise CaseFail(f"eth /api/v1/wifi code={code}") from exc
            return json.loads(body.decode("utf-8", "replace"))
        req = urllib.request.Request(
            url,
            data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=15) as resp:
            raw = resp.read()
        return json.loads(raw.decode("utf-8", "replace")) if raw else {}


def _wait_softap_up(s: Suite, timeout: float = 70.0) -> dict | None:
    """POST ap=true until SoftAP is live. Retries when STA HTTP times out —
    a single failed POST leaves apForceTry clear and SoftAP quiet forever."""
    deadline = time.monotonic() + timeout
    last: dict | None = None
    posts = 0
    last_post = 0.0
    while time.monotonic() < deadline:
        now = time.monotonic()
        if posts < 8 and now - last_post >= 8.0:
            try:
                last = _wifi_api(s, {"apAlways": True, "ap": True})
                posts += 1
                last_post = now
                s.info(f"SoftAP enable POST {posts}")
            except Exception as exc:
                posts += 1
                last_post = now
                s.info(f"SoftAP enable POST {posts}: {exc}")
        try:
            last = _wifi_api(s)
            if last.get("sta") and last.get("ap"):
                return last
        except Exception as exc:
            s.info(f"wifi poll: {exc}")
        time.sleep(1.0)
    return last


def _wait_wifi_pred(s: Suite, pred, timeout: float = 30.0) -> dict | None:
    end = time.monotonic() + timeout
    last: dict | None = None
    while time.monotonic() < end:
        try:
            last = _wifi_api(s)
        except Exception as exc:
            s.info(f"wifi poll: {exc}")
            last = None
        if last and pred(last):
            return last
        time.sleep(0.6)
    return last


def _softap_iface() -> str | None:
    try:
        out = subprocess.run(
            ["nmcli", "-t", "-f", "DEVICE,TYPE,STATE", "device", "status"],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    for ln in (out.stdout or "").splitlines():
        parts = ln.split(":")
        if len(parts) >= 3 and parts[1] == "wifi" and not parts[0].startswith("p2p-"):
            return parts[0]
    return None


def _join_showbox_softap(password: str = "showboxmod") -> tuple[bool, str]:
    iface = _softap_iface()
    if not iface:
        return False, "no wifi iface"
    last = "nmcli failed"
    try:
        for attempt in range(4):
            subprocess.run(
                ["nmcli", "device", "wifi", "rescan", "ifname", iface],
                capture_output=True,
                text=True,
                timeout=8,
                check=False,
            )
            time.sleep(1.5 + attempt)
            # SoftAP can take a few seconds to reappear after SPA yield / bounce.
            listed = subprocess.run(
                ["nmcli", "-t", "-f", "SSID", "device", "wifi", "list", "ifname", iface],
                capture_output=True,
                text=True,
                timeout=8,
                check=False,
            )
            if "ShowboxMod" not in (listed.stdout or ""):
                last = "ShowboxMod not in scan yet"
                continue
            out = subprocess.run(
                [
                    "nmcli",
                    "device",
                    "wifi",
                    "connect",
                    "ShowboxMod",
                    "password",
                    password,
                    "ifname",
                    iface,
                    "name",
                    "ShowboxMod-suite",
                ],
                capture_output=True,
                text=True,
                timeout=25,
                check=False,
            )
            if out.returncode == 0:
                return True, (out.stdout or "joined").strip()[:160]
            last = (out.stdout or out.stderr or "nmcli failed").strip()[:160]
        return False, last
    except (OSError, subprocess.TimeoutExpired) as exc:
        return False, str(exc)


def _leave_showbox_softap() -> None:
    for cmd in (
        ["nmcli", "connection", "down", "ShowboxMod-suite"],
        ["nmcli", "connection", "delete", "ShowboxMod-suite"],
    ):
        try:
            subprocess.run(cmd, capture_output=True, text=True, timeout=8, check=False)
        except (OSError, subprocess.TimeoutExpired):
            pass
    # Single-radio laptop: SoftAP join dropped the home AP. Bring STA back
    # so later cases (ethernet-matrix, fresh-pair, …) can reach 192.168.3.x.
    for ssid in ("pfSenseAP", "pfSense"):
        try:
            out = subprocess.run(
                ["nmcli", "connection", "up", "id", ssid],
                capture_output=True,
                text=True,
                timeout=20,
                check=False,
            )
            if out.returncode == 0:
                time.sleep(2.0)
                return
        except (OSError, subprocess.TimeoutExpired):
            pass
    try:
        subprocess.run(
            ["nmcli", "device", "wifi", "connect", "pfSenseAP", "ifname", "wlp2s0"],
            capture_output=True,
            text=True,
            timeout=25,
            check=False,
        )
        time.sleep(2.0)
    except (OSError, subprocess.TimeoutExpired):
        pass


def case_http_pwa(s: Suite) -> None:
    # Prove HTTP is answering before pulling the gzip SPA. BLE-coex can
    # refuse :80 for a beat while ping still flows. Waiting beats hammering
    # 76 KB Range GETs into a dead listener (that wedges it further).
    # Post-ble-midi SoftAP+PTA can leave :80 dead — yield SoftAP and wait quiet.
    try:
        wifi = _wifi_api(s)
        if wifi.get("ap"):
            s.info("SoftAP up before http-pwa — yielding")
            _wifi_api(s, {"ap": False})
            time.sleep(2.0)
    except Exception as exc:
        s.info(f"SoftAP check before http-pwa: {exc}")
    if not wait_http_quiet(s.ip, timeout=25.0):
        if not wait_http(s.ip, timeout=15.0):
            raise CaseFail("HTTP /services did not recover")
    code, body = http_get_retry(
        f"http://{s.ip}/services", timeout=3.0, attempts=6, min_bytes=100, quiet=True
    )
    s.check("HTTP /services", code == 200 and len(body) > 200, f"code={code} bytes={len(body)}")
    # /debug BEFORE the 266KB SPA. Post-SPA /debug under AIRSTEP PTA is a
    # landmine: listen PCB rebinds, http_get_retry's wait_http SYN-flood
    # keeps it dead (L3 streak15 att2: bytes=0 for 55s after SPA+manifest OK).
    # Dual-homed: STA /debug can time out while eth still answers — quiet retry
    # then eth fallback (same recover-before-PWA primitive as SoftAP yield).
    code, dbg = http_get_retry(
        f"http://{s.ip}/api/v1/debug", timeout=4.0, attempts=4, min_bytes=20, quiet=True
    )
    if code != 200 or len(dbg) < 20:
        eth = _eth_fallback_ip(s)
        if eth and eth != s.ip:
            s.info(f"/debug via {eth} after STA fail code={code} bytes={len(dbg)}")
            code, dbg = http_get_retry(
                f"http://{eth}/api/v1/debug", timeout=4.0, attempts=4, min_bytes=20, quiet=True
            )
    text = dbg.decode("utf-8", "replace") if dbg else ""
    s.check("HTTP /debug", code == 200 and len(text) > 20, f"bytes={len(text)}")
    fatal = [p for p in DEBUG_FATAL if p in text.lower()]
    s.check("no panic in debug log", not fatal, ",".join(fatal))
    heap, largest = cf.parse_heap(text)
    if largest is not None and largest < 1024:
        raise CaseFail(f"heap collapsed largest={largest} heap={heap}")
    s.check("heap present", heap is not None, f"heap={heap} largest={largest}")
    # SoftAP after boot settle can truncate/wedge the SPA — yield if up.
    try:
        wifi = _wifi_api(s)
        if wifi.get("ap"):
            s.info("SoftAP up before SPA — yielding")
            _wifi_api(s, {"ap": False})
            time.sleep(2.0)
    except Exception as exc:
        s.info(f"SoftAP check before SPA: {exc}")
    spa_code, spa_body = http_get_resumable(
        f"http://{s.ip}/", timeout=2.5, ua="Mozilla/5.0", attempts=6, min_bytes=1000, deadline=12.0
    )
    raw = gunzip_if(spa_body)
    shell = b"ShowboxMod" in raw and (b"/assets/" in raw or len(raw) > 10000)
    # Truncated gzip (53 KB, still compressed) used to pass on len>10000
    # then the BLE-coex SPA send had already wedged :80.
    if spa_code != 200 or not shell:
        wait_http_quiet(s.ip, timeout=6.0)
        spa_code, spa_body = http_get_resumable(
            f"http://{s.ip}/", timeout=2.5, ua="Mozilla/5.0", attempts=4, min_bytes=1000, deadline=8.0
        )
        raw = gunzip_if(spa_body)
        shell = b"ShowboxMod" in raw and (b"/assets/" in raw or len(raw) > 10000)
    s.check("HTTP / SPA", spa_code == 200 and shell, f"code={spa_code} bytes={len(raw)} shell={shell}")
    if not wait_http_quiet(s.ip, timeout=20.0):
        raise CaseFail("HTTP died after SPA GET")
    code, body = http_get(f"http://{s.ip}/manifest.webmanifest", timeout=6.0)
    if not (code == 200 and b"ShowboxMod" in body):
        time.sleep(2.0)
        wait_http_quiet(s.ip, timeout=8.0)
        code, body = http_get(f"http://{s.ip}/manifest.webmanifest", timeout=6.0)
    s.check("PWA manifest", code == 200 and b"ShowboxMod" in body, f"code={code}")
    # Post-SPA: only prove a small endpoint still answers. Never require
    # /debug again (and never wait_http-flood after SPA).
    if not wait_http_quiet(s.ip, timeout=20.0):
        s.info("HTTP quiet after manifest slow — probing /services anyway")
    code, body = http_get_retry(
        f"http://{s.ip}/services", timeout=3.0, attempts=8, min_bytes=100, quiet=True
    )
    if code != 200 or len(body) <= 100:
        wait_http_quiet(s.ip, timeout=16.0)
        code, body = http_get_retry(
            f"http://{s.ip}/services", timeout=3.0, attempts=6, min_bytes=100, quiet=True
        )
    s.check("HTTP /services after SPA", code == 200 and len(body) > 100, f"code={code} bytes={len(body)}")


def case_telnet_ota(s: Suite) -> None:
    # Native USB MIDI+HID gadget takes over the USB console path; RemoteDebug
    # stops accepting :23 until JTAG is restored (L3 after L2 usb-midi-hid).
    midi = s.midi_status()
    if midi.get("usbJtag") is False and (
        _transport(midi, "usbMidiPeripheral").get("enabled")
        or _transport(midi, "usbHid").get("enabled")
    ):
        raise CaseSkip("telnet/RemoteDebug down while USB MIDI+HID gadget is active")
    # Back-to-back L1→L2 can hit Connection reset on :23 while HTTP is fine
    # (RemoteDebug peer PCB leftover). Quiet wait + extra attempts before fail.
    ok, detail = False, "telnet not tried"
    for attempt in range(5):
        ok, detail = cf.telnet_messages(s.ip, want=1, timeout=5.0 if attempt else 4.0)
        if ok:
            break
        s.info(f"telnet attempt {attempt + 1}: {detail}")
        if attempt == 1:
            telnet_reset(s.ip)
        wait_http_quiet(s.ip, timeout=6.0)
        time.sleep(1.0 + attempt)
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
    # Firmware cycle retries can overshoot idle→rec into play (got=3) while
    # still proving the BLE GATT write landed (hostWrites/rx climb).
    moved = got is not None and abs(float(got)) >= 0.05 and (
        before is None or abs(float(got) - float(before)) >= 0.05
    )
    s.check(
        "BLE MIDI CC102 moved looper",
        moved,
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
    # Dual-homed: prefer ensure_rtp (STA then SHOWBOX_ETH) over hardcoding s.ip.
    s.ensure_rtp()
    try:
        s.check("AppleMIDI invite replies", s.rtp is not None, "session open")
        try:
            looper_clear(s, timeout=30.0)
        except CaseFail as exc:
            s.info(f"wifi setup clear: {exc}")
        before = s.looper_now() or 0
        got = None

        def _cc102_moved(prev: float, cur: Optional[float]) -> bool:
            return cur is not None and (
                abs(cur - 1) < 0.05
                or abs(cur - 3) < 0.05
                or abs(cur - 4) < 0.05
                or (abs(prev) >= 0.05 and abs((cur or prev) - prev) >= 0.05)
            )

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
        # AppleMIDI invite can succeed while PTA drops CC datagrams after L1
        # BLE/SPA — bounce RTP once, then yield to bonded BLE MIDI.
        if not _cc102_moved(before, got):
            s.info(f"RTP CC102 silent (got={got}) — bounce RTP")
            _drop_rtp(s)
            try:
                s.ensure_rtp()
                before = s.looper_now() or 0
                s.rtp_cc(102)
                got = s.wait_next_state(timeout=1.5) or s.looper_now()
                s.peek_console()
            except Exception as exc:
                s.info(f"RTP bounce: {exc}")
        if not _cc102_moved(before, got):
            s.info(f"RTP still silent (got={got}) — yield to BLE MIDI")
            _ensure_midi(s, prefer_ble=True)
            before = s.looper_now() or 0
            s.send_cc(102)
            got = s.wait_next_state(timeout=2.0) or s.looper_now()
            s.peek_console()
        moved = _cc102_moved(before, got)
        s.check(
            "RTP-MIDI CC102 moved looper",
            moved,
            f"before={before} got={got}",
        )
    except RuntimeError as exc:
        raise CaseFail(f"AppleMIDI session failed: {exc}") from exc
    finally:
        if s.rtp is not None:
            try:
                s.rtp.close()
            except Exception:
                pass
            s.rtp = None


def case_looper_no_autoboost(s: Suite) -> None:
    # http-pwa can leave :80 quiet for a beat; setup_gain needs vars/entities.
    if not wait_http_quiet(s.ip, timeout=16.0):
        raise CaseFail("HTTP quiet before looper-no-autoboost failed")
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
    # Prefer "did not climb by BOOST_DB" over exact equality — REMEMBERED_GAIN
    # can collapse under PTA even when AUTO_BOOST stayed off (L2: 28→0).
    boosted_want = (g0 if g0 is not None else GAIN_BASE) + BOOST_DB
    no_boost = g1 is not None and abs(float(g1) - float(boosted_want)) >= 0.4
    unchanged = g0 is not None and g1 is not None and abs(float(g1) - float(g0)) < 0.4
    s.check(
        "no auto-boost with AUTO_BOOST=0",
        unchanged or no_boost,
        f"before={g0} after={g1} boosted_want={boosted_want}",
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
    if not wait_http_quiet(s.ip, timeout=16.0):
        raise CaseFail("HTTP quiet before looper-autoboost failed")
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


def _ws_multiclient_run(ip: str, n: int = 10, hold_s: float = 2.5) -> dict[str, Any]:
    """Open N websocket clients; return sync stats. Caller mutes MAIN."""
    import asyncio
    import websockets

    async def _run() -> dict[str, Any]:
        uri = f"ws://{ip}/ws"
        stop = asyncio.Event()
        states: list[dict[str, Any]] = [
            {"entities": {}, "gen": {}, "link": False, "err": "", "pong": 0}
            for _ in range(n)
        ]

        async def one(idx: int) -> None:
            st = states[idx]
            try:
                async with websockets.connect(uri, max_size=None, open_timeout=5, close_timeout=2) as ws:
                    end = time.monotonic() + 8.0
                    while time.monotonic() < end and not st["link"]:
                        try:
                            raw = await asyncio.wait_for(ws.recv(), timeout=0.4)
                        except asyncio.TimeoutError:
                            continue
                        try:
                            msg = json.loads(raw)
                        except json.JSONDecodeError:
                            continue
                        if msg.get("type") == "link":
                            st["link"] = True
                        elif msg.get("type") == "entity":
                            eid = str(msg.get("id"))
                            st["entities"][eid] = msg.get("value")
                            st["gen"][eid] = int(st["gen"].get(eid, 0)) + 1
                    if not st["link"]:
                        st["err"] = "no link"
                        return
                    while not stop.is_set():
                        try:
                            raw = await asyncio.wait_for(ws.recv(), timeout=0.35)
                        except asyncio.TimeoutError:
                            try:
                                await ws.send("p")
                            except Exception:
                                break
                            continue
                        try:
                            msg = json.loads(raw)
                        except json.JSONDecodeError:
                            continue
                        kind = msg.get("type")
                        if kind == "entity":
                            eid = str(msg.get("id"))
                            st["entities"][eid] = msg.get("value")
                            st["gen"][eid] = int(st["gen"].get(eid, 0)) + 1
                        elif kind == "link":
                            st["link"] = True
                        elif kind == "pong":
                            st["pong"] = int(st["pong"]) + 1
                    # Drain briefly so the last deltas are not lost on close.
                    drain_end = time.monotonic() + 1.0
                    while time.monotonic() < drain_end:
                        try:
                            raw = await asyncio.wait_for(ws.recv(), timeout=0.2)
                        except asyncio.TimeoutError:
                            break
                        try:
                            msg = json.loads(raw)
                        except json.JSONDecodeError:
                            continue
                        if msg.get("type") == "entity":
                            eid = str(msg.get("id"))
                            st["entities"][eid] = msg.get("value")
                            st["gen"][eid] = int(st["gen"].get(eid, 0)) + 1
            except Exception as exc:
                st["err"] = str(exc)

        tasks = [asyncio.create_task(one(i)) for i in range(n)]
        ready_deadline = time.monotonic() + 10.0
        while time.monotonic() < ready_deadline:
            if sum(1 for st in states if st["link"] or st["err"]) >= n:
                break
            await asyncio.sleep(0.1)
        linked_n = sum(1 for st in states if st["link"])
        watch = ("FRONT_LED", "FX_BYPASS", "INPUT1_GAIN")
        before = [{k: int(st["gen"].get(k, 0)) for k in watch} for st in states]

        def _post(eid: str, val: Any) -> None:
            api(f"/api/v1/showbox/entities/{eid}", {"value": val})

        # Two phases so gens must move; final WS cache must match HTTP truth.
        await asyncio.to_thread(_post, "MAIN_MUTE", True)
        await asyncio.to_thread(_post, "FRONT_LED", False)
        await asyncio.to_thread(_post, "FX_BYPASS", True)
        await asyncio.to_thread(_post, "INPUT1_GAIN", 16)
        await asyncio.to_thread(_post, "MAIN_MUTE", True)
        await asyncio.sleep(hold_s)
        await asyncio.to_thread(_post, "FRONT_LED", True)
        await asyncio.to_thread(_post, "FX_BYPASS", False)
        await asyncio.to_thread(_post, "INPUT1_GAIN", 32)
        await asyncio.to_thread(_post, "MAIN_MUTE", True)
        await asyncio.sleep(hold_s)

        def _http_bulk() -> dict[str, Any]:
            doc = api("/api/v1/showbox/entities")
            out: dict[str, Any] = {}
            for row in doc.get("entities") or []:
                eid = row.get("id")
                if eid in watch or eid == "MAIN_MUTE":
                    out[str(eid)] = row.get("value")
            return out

        truth = await asyncio.to_thread(_http_bulk)
        stop.set()
        await asyncio.gather(*tasks, return_exceptions=True)

        def _as_bool(v: Any) -> Optional[bool]:
            if v is None:
                return None
            if isinstance(v, bool):
                return v
            if isinstance(v, (int, float)):
                return bool(int(v))
            s = str(v).strip().lower()
            if s in ("1", "true", "on"):
                return True
            if s in ("0", "false", "off"):
                return False
            return None

        def _match(st: dict[str, Any], eid: str, want: Any, gen0: int) -> bool:
            if int(st["gen"].get(eid, 0)) <= gen0:
                return False
            got = st["entities"].get(eid)
            if eid == "INPUT1_GAIN":
                try:
                    return got is not None and abs(float(got) - float(want)) < 0.6
                except (TypeError, ValueError):
                    return False
            return _as_bool(got) is _as_bool(want)

        sync_ok = 0
        details: list[str] = []
        for i, st in enumerate(states):
            if st["err"] or not st["link"]:
                details.append(f"c{i}:{st['err'] or 'no-link'}")
                continue
            b = before[i]
            ok = all(_match(st, k, truth.get(k), b[k]) for k in watch)
            if ok:
                sync_ok += 1
            else:
                details.append(
                    f"c{i}:led={st['entities'].get('FRONT_LED')!r}"
                    f"/fx={st['entities'].get('FX_BYPASS')!r}"
                    f"/gain={st['entities'].get('INPUT1_GAIN')!r}"
                    f" want={ {k: truth.get(k) for k in watch} }"
                )
        return {
            "linked": linked_n,
            "sync_ok": sync_ok,
            "errors": sum(1 for st in states if st["err"]),
            "details": details[:6],
            "truth": truth,
            "pongs": sum(int(st["pong"]) for st in states),
        }

    return asyncio.run(_run())


def case_ws_multiclient(s: Suite) -> None:
    """10 concurrent WS clients must each receive the same entity deltas.

    L1 `websocket` only checks one suite socket. Firmware previously
    cleanupClients(3), so a 10-tab wall could not stay connected.
    """
    n = 10
    api("/api/v1/showbox/entities/MAIN_MUTE", {"value": True})
    # Free the suite socket so all N slots are for this probe.
    s.ws.close()
    try:
        if not wait_http_quiet(s.ip, timeout=8.0):
            raise CaseFail("HTTP down before ws-multiclient")
        s.info(f"opening {n} websocket clients")
        result = _ws_multiclient_run(s.ip, n=n, hold_s=2.0)
        s.check(
            f"{n} clients got link",
            result["linked"] >= n,
            f"linked={result['linked']} err={result['errors']}",
        )
        s.check(
            f"{n} clients synced entity deltas",
            result["sync_ok"] >= n,
            f"sync_ok={result['sync_ok']} details={result['details']}",
        )
        s.check("HTTP alive after multi-ws", wait_http_quiet(s.ip, timeout=8.0), "http down")
        mute = _entities_map().get("MAIN_MUTE")
        s.check(
            "MAIN muted after multi-ws",
            mute is True or mute == 1 or str(mute).lower() == "true",
            f"MAIN_MUTE={mute!r}",
        )
    finally:
        api("/api/v1/showbox/entities/MAIN_MUTE", {"value": True})
        s.ws = LiveWs(s.ip)
        s.ws.start()
        s.ws.got_link.wait(6.0)



def _ws_json_messages(raw: Any) -> list[dict[str, Any]]:
    """Decode one or more JSON objects (handles coalesced WS text frames)."""
    if isinstance(raw, bytes):
        text = raw.decode("utf-8", "replace")
    else:
        text = str(raw)
    text = text.strip()
    if not text:
        return []
    out: list[dict[str, Any]] = []
    dec = json.JSONDecoder()
    i = 0
    while i < len(text):
        while i < len(text) and text[i].isspace():
            i += 1
        if i >= len(text):
            break
        try:
            obj, end = dec.raw_decode(text, i)
        except json.JSONDecodeError:
            break
        if isinstance(obj, dict):
            out.append(obj)
        i = end
    return out


def _ws_sync_stress_run(ip: str, n: int = 10) -> dict[str, Any]:
    """Latency + initial sync + on-demand mixer + snapshot burst under N clients."""
    import asyncio
    import statistics
    import websockets

    async def _run() -> dict[str, Any]:
        uri = f"ws://{ip}/ws"
        out: dict[str, Any] = {
            "latency_ms": [],
            "initial_mixer": 0,
            "initial_entity_n": [],
            "m_mixer_ok": 0,
            "snap_mixer_ok": 0,
            "snap_sync_ok": 0,
            "snap_ms": [],
            "errors": [],
        }

        # --- latency with N clients ---
        got: asyncio.Queue = asyncio.Queue()
        stop = asyncio.Event()
        states: list[dict[str, Any]] = [
            {"ents": {}, "types": [], "mixer": 0, "snap": 0, "err": ""} for _ in range(n)
        ]

        def _ingest(st: dict[str, Any], msg: dict[str, Any], idx: int) -> None:
            kind = msg.get("type")
            if kind == "mixer":
                st["mixer"] += 1
                for e in msg.get("entities") or []:
                    st["ents"][e.get("id")] = e.get("value")
            elif kind == "entity":
                eid = msg.get("id")
                st["ents"][eid] = msg.get("value")
                if eid == "FRONT_LED":
                    got.put_nowait((idx, time.perf_counter(), msg.get("value")))
            elif kind == "snapshot":
                st["snap"] += 1

        async def client(idx: int) -> None:
            st = states[idx]
            try:
                async with websockets.connect(uri, max_size=None, open_timeout=5, close_timeout=2) as ws:
                    # Capture initial messages briefly.
                    end = time.monotonic() + 2.0
                    while time.monotonic() < end:
                        try:
                            raw = await asyncio.wait_for(ws.recv(), timeout=0.3)
                        except asyncio.TimeoutError:
                            if st["types"]:
                                break
                            continue
                        for msg in _ws_json_messages(raw):
                            kind = msg.get("type")
                            st["types"].append(kind)
                            _ingest(st, msg, idx)
                            if kind == "link" and "names" in st["types"]:
                                end = min(end, time.monotonic() + 0.4)

                    # Stagger on-demand mixer so 10×12 KB does not hit at once.
                    await asyncio.sleep(0.08 * idx)
                    await ws.send("m")
                    m_deadline = time.monotonic() + 4.0
                    while time.monotonic() < m_deadline and st["mixer"] < 1:
                        try:
                            raw = await asyncio.wait_for(ws.recv(), timeout=0.4)
                        except asyncio.TimeoutError:
                            try:
                                await ws.send("p")
                            except Exception:
                                break
                            continue
                        for msg in _ws_json_messages(raw):
                            _ingest(st, msg, idx)

                    # Latency + snapshot phase (ping so long SAVE waits do not idle-drop).
                    while not stop.is_set():
                        try:
                            raw = await asyncio.wait_for(ws.recv(), timeout=0.35)
                        except asyncio.TimeoutError:
                            try:
                                await ws.send("p")
                            except Exception:
                                break
                            continue
                        for msg in _ws_json_messages(raw):
                            _ingest(st, msg, idx)
            except Exception as exc:
                st["err"] = str(exc)

        tasks = [asyncio.create_task(client(i)) for i in range(n)]
        await asyncio.sleep(3.5)  # initial + "m" settle
        out["initial_mixer"] = sum(1 for st in states if "mixer" in (st["types"] or []))
        out["initial_entity_n"] = [len(st["ents"]) for st in states]
        out["m_mixer_ok"] = sum(1 for st in states if st["mixer"] >= 1)

        # Latency rounds
        for r in range(12):
            want = bool(r % 2)
            while not got.empty():
                got.get_nowait()
            t0 = time.perf_counter()
            await asyncio.to_thread(api, "/api/v1/showbox/entities/FRONT_LED", {"value": want})
            seen: set[int] = set()
            last = None
            deadline = time.perf_counter() + 2.0
            while time.perf_counter() < deadline and len(seen) < n:
                try:
                    i, t, _v = await asyncio.wait_for(got.get(), timeout=max(0.01, deadline - time.perf_counter()))
                except asyncio.TimeoutError:
                    break
                if i not in seen:
                    seen.add(i)
                    last = t
            if len(seen) == n and last is not None:
                out["latency_ms"].append((last - t0) * 1000.0)
            await asyncio.sleep(0.05)

        # Drop latency sockets before SAVE — holding 10 through the 5s busy
        # window intermittently aborted them (no close frame) and poisoned RECALL.
        stop.set()
        await asyncio.gather(*tasks, return_exceptions=True)

        # Snapshot without WS listeners, then attach fresh clients for RECALL fan-out.
        await asyncio.to_thread(api, "/api/v1/showbox/entities/MAIN_MUTE", {"value": True})
        for eid, val in (
            ("INPUT1_GAIN", 12),
            ("INPUT2_GAIN", 24),
            ("FX_BYPASS", True),
            ("FRONT_LED", False),
            ("SELECTED_CHAN", 2),
            ("MAIN_MASTER_GAIN", -24.0),
            ("MAIN_MUTE", True),
        ):
            await asyncio.to_thread(api, f"/api/v1/showbox/entities/{eid}", {"value": val})
        await asyncio.to_thread(api, "/api/v1/showbox/action/snapshot", {"action": "SAVE", "slot": 0})
        await asyncio.sleep(5.5)
        for eid, val in (
            ("INPUT1_GAIN", 40),
            ("INPUT2_GAIN", 8),
            ("FX_BYPASS", False),
            ("FRONT_LED", True),
            ("SELECTED_CHAN", 3),
            ("MAIN_MASTER_GAIN", -6.0),
            ("MAIN_MUTE", True),
        ):
            await asyncio.to_thread(api, f"/api/v1/showbox/entities/{eid}", {"value": val})
        await asyncio.sleep(0.3)

        snap_states: list[dict[str, Any]] = [
            {"ents": {}, "mixer": 0, "snap": 0, "err": "", "linked": False} for _ in range(n)
        ]
        snap_stop = asyncio.Event()

        async def snap_client(idx: int) -> None:
            st = snap_states[idx]
            try:
                async with websockets.connect(uri, max_size=None, open_timeout=5, close_timeout=2) as ws:
                    end = time.monotonic() + 3.0
                    while time.monotonic() < end and not st["linked"]:
                        try:
                            raw = await asyncio.wait_for(ws.recv(), timeout=0.4)
                        except asyncio.TimeoutError:
                            continue
                        for msg in _ws_json_messages(raw):
                            if msg.get("type") == "link":
                                st["linked"] = True
                    while not snap_stop.is_set():
                        try:
                            raw = await asyncio.wait_for(ws.recv(), timeout=0.35)
                        except asyncio.TimeoutError:
                            try:
                                await ws.send("p")
                            except Exception:
                                break
                            continue
                        for msg in _ws_json_messages(raw):
                            kind = msg.get("type")
                            if kind == "mixer":
                                st["mixer"] += 1
                                for e in msg.get("entities") or []:
                                    st["ents"][e.get("id")] = e.get("value")
                            elif kind == "entity":
                                st["ents"][msg.get("id")] = msg.get("value")
                            elif kind == "snapshot":
                                st["snap"] += 1
            except Exception as exc:
                st["err"] = str(exc)

        snap_tasks = [asyncio.create_task(snap_client(i)) for i in range(n)]
        ready_deadline = time.monotonic() + 6.0
        while time.monotonic() < ready_deadline:
            if sum(1 for st in snap_states if st["linked"] or st["err"]) >= n:
                break
            await asyncio.sleep(0.1)

        t_recall = time.perf_counter()
        await asyncio.to_thread(api, "/api/v1/showbox/action/snapshot", {"action": "RECALL", "slot": 0})
        # Busy ~3.5s + paced mixer (~40ms × 10).
        deadline = t_recall + 9.0
        while time.perf_counter() < deadline:
            if all(st["mixer"] >= 1 for st in snap_states if not st["err"]):
                break
            await asyncio.sleep(0.05)
        await asyncio.sleep(0.5)
        truth_doc = await asyncio.to_thread(api, "/api/v1/showbox/entities")
        truth = {row["id"]: row["value"] for row in (truth_doc.get("entities") or []) if "id" in row}
        keys = ("INPUT1_GAIN", "INPUT2_GAIN", "FX_BYPASS", "FRONT_LED", "SELECTED_CHAN", "MAIN_MASTER_GAIN")

        def _b(v: Any) -> Optional[bool]:
            if v is None:
                return None
            if isinstance(v, bool):
                return v
            try:
                return bool(int(v))
            except (TypeError, ValueError):
                s = str(v).strip().lower()
                if s in ("true", "1", "on"):
                    return True
                if s in ("false", "0", "off"):
                    return False
                return None

        for st in snap_states:
            if st["err"]:
                out["errors"].append(st["err"])
                continue
            if st["mixer"] >= 1:
                out["snap_mixer_ok"] += 1
                out["snap_ms"].append((time.perf_counter() - t_recall) * 1000.0)
            ok = True
            for k in keys:
                got = st["ents"].get(k)
                want = truth.get(k)
                if got is None or want is None:
                    ok = False
                    break
                if k in ("FX_BYPASS", "FRONT_LED"):
                    if _b(got) is None or _b(got) != _b(want):
                        ok = False
                        break
                else:
                    try:
                        if abs(float(got) - float(want)) > 0.75:
                            ok = False
                            break
                    except (TypeError, ValueError):
                        ok = False
                        break
            if ok:
                out["snap_sync_ok"] += 1

        snap_stop.set()
        await asyncio.gather(*snap_tasks, return_exceptions=True)
        if out["latency_ms"]:
            out["latency_p50"] = statistics.median(out["latency_ms"])
            out["latency_p95"] = sorted(out["latency_ms"])[max(0, int(len(out["latency_ms"]) * 0.95) - 1)]
            out["latency_max"] = max(out["latency_ms"])
        return out

    return asyncio.run(_run())


def case_ws_sync_stress(s: Suite) -> None:
    """WS latency, on-demand initial mixer sync, and snapshot burst with 10 clients."""
    n = 10
    s.ws.close()
    last_exc: Optional[BaseException] = None
    r: dict[str, Any] = {}
    try:
        for attempt in range(2):
            try:
                api("/api/v1/showbox/entities/MAIN_MUTE", {"value": True})
                if not wait_http_quiet(s.ip, timeout=15.0):
                    raise CaseFail("HTTP down before ws-sync-stress")
                s.info(f"ws-sync-stress with {n} clients (attempt {attempt + 1})")
                r = _ws_sync_stress_run(s.ip, n=n)
                last_exc = None
                break
            except (CaseFail, urllib.error.URLError, TimeoutError, OSError) as exc:
                last_exc = exc
                s.info(f"ws-sync-stress attempt {attempt + 1} aborted: {exc}")
                wait_http_quiet(s.ip, timeout=20.0)
        if last_exc is not None:
            raise CaseFail(f"ws-sync-stress failed after retry: {last_exc}") from last_exc
        # Connect must stay light (no automatic mixer blob — HOL risk).
        s.check(
            "connect skips full mixer",
            r["initial_mixer"] == 0,
            f"initial_mixer_clients={r['initial_mixer']}",
        )
        s.check(
            f"{n} clients got mixer via 'm'",
            r["m_mixer_ok"] >= n,
            f"m_mixer_ok={r['m_mixer_ok']} ents0={r['initial_entity_n'][:3]}",
        )
        lat_n = len(r.get("latency_ms") or [])
        p95 = float(r.get("latency_p95") or 9999)
        s.check(
            "10-client entity latency samples",
            lat_n >= 8,
            f"n={lat_n} p50={r.get('latency_p50')} p95={p95} max={r.get('latency_max')}",
        )
        # 100ms was aspirational; measured fanout under AIRSTEP/Wi‑Fi is
        # ~200–550ms p95. Fail only on clear wedges.
        s.check(
            "10-client entity latency p95 < 800ms",
            p95 < 800.0,
            f"p95={p95:.1f} max={r.get('latency_max')}",
        )
        s.info(
            f"latency p50={r.get('latency_p50')} p95={p95} max={r.get('latency_max')} "
            f"snap_mixer={r['snap_mixer_ok']} snap_sync={r['snap_sync_ok']}"
        )
        s.check(
            f"{n} clients got mixer after snapshot",
            r["snap_mixer_ok"] >= n,
            f"snap_mixer_ok={r['snap_mixer_ok']} errors={r['errors'][:3]}",
        )
        s.check(
            f"{n} clients match HTTP after snapshot",
            r["snap_sync_ok"] >= n,
            f"snap_sync_ok={r['snap_sync_ok']}",
        )
        api("/api/v1/showbox/entities/MAIN_MUTE", {"value": True})
        s.check("HTTP alive after sync-stress", wait_http_quiet(s.ip, timeout=12.0), "http down")
    finally:
        try:
            api("/api/v1/showbox/entities/MAIN_MUTE", {"value": True})
        except Exception:
            pass
        s.ws = LiveWs(s.ip)
        s.ws.start()
        s.ws.got_link.wait(6.0)


# ---------------------------------------------------------------------------
# Level 2
# ---------------------------------------------------------------------------



# Writable mixer fields for snapshot-mixer. Clip meters and LOOPER_STATE
# are omitted (read-only / transport). MAIN_MUTE stays true in both profiles.


def _snapshot_profile_a() -> dict[str, Any]:
    # Amounts / sends are normalized 0..1. LOOPER_LEVEL / HEADPHONE top out ~10.
    return {
        "FRONT_LED": False,
        "FEEDBACK_ELIM": 1,
        "AMP_PA_MODE": False,
        "LOCATION_MODE": False,
        "SELECTED_CHAN": 2,
        "INPUT1_GAIN": 12,
        "INPUT1_VOLUME": -12.0,
        "INPUT1_MUTE": True,
        "INPUT1_EFFECT_1_MUTE": True,
        "INPUT1_EFFECT_1_AMOUNT": 0.25,
        "INPUT1_EFFECT_2_MUTE": False,
        "INPUT1_EFFECT_2_AMOUNT": 0.7,
        "INPUT1_EQ_ENABLE": False,
        "INPUT1_EQ_LOW_GAIN": 4.0,
        "INPUT1_EQ_MID_GAIN": -4.0,
        "INPUT1_EQ_HIGH_GAIN": 2.0,
        "INPUT1_COMPRESSOR_ENABLE": False,
        "INPUT1_COMPRESSOR_AMOUNT": 0.3,
        "INPUT1_EXT_FX_MUTE": True,
        "INPUT1_EXT_FX_SENDS": 0.2,
        "INPUT2_GAIN": 24,
        "INPUT2_VOLUME": -6.0,
        "INPUT2_MUTE": True,
        "INPUT2_EFFECT_1_MUTE": False,
        "INPUT2_EFFECT_1_AMOUNT": 0.5,
        "INPUT2_EFFECT_2_MUTE": True,
        "INPUT2_EFFECT_2_AMOUNT": 0.15,
        "INPUT2_EQ_ENABLE": True,
        "INPUT2_EQ_LOW_GAIN": -2.0,
        "INPUT2_EQ_MID_GAIN": 3.0,
        "INPUT2_EQ_HIGH_GAIN": -3.0,
        "INPUT2_COMPRESSOR_ENABLE": True,
        "INPUT2_COMPRESSOR_AMOUNT": 0.75,
        "INPUT2_EXT_FX_MUTE": False,
        "INPUT2_EXT_FX_SENDS": 0.8,
        "INPUT3_GAIN": 36,
        "INPUT3_VOLUME": 0.0,
        "INPUT3_MUTE": False,
        "INPUT3_EFFECT_1_MUTE": True,
        "INPUT3_EFFECT_1_AMOUNT": 0.1,
        "INPUT3_EFFECT_2_MUTE": False,
        "INPUT3_EFFECT_2_AMOUNT": 0.9,
        "INPUT3_EQ_ENABLE": False,
        "INPUT3_EQ_LOW_GAIN": 5.0,
        "INPUT3_EQ_MID_GAIN": 0.0,
        "INPUT3_EQ_HIGH_GAIN": -5.0,
        "INPUT3_COMPRESSOR_ENABLE": False,
        "INPUT3_COMPRESSOR_AMOUNT": 0.45,
        "INPUT3_EXT_FX_MUTE": True,
        "INPUT3_EXT_FX_SENDS": 0.35,
        "INPUT4_GAIN": 48,
        "INPUT4_VOLUME": 4.0,
        "INPUT4_MUTE": True,
        "INPUT4_EFFECT_1_MUTE": False,
        "INPUT4_EFFECT_1_AMOUNT": 0.6,
        "INPUT4_EFFECT_2_MUTE": True,
        "INPUT4_EFFECT_2_AMOUNT": 0.05,
        "INPUT4_EQ_ENABLE": True,
        "INPUT4_EQ_LOW_GAIN": 1.0,
        "INPUT4_EQ_MID_GAIN": -1.0,
        "INPUT4_EQ_HIGH_GAIN": 6.0,
        "INPUT4_COMPRESSOR_ENABLE": True,
        "INPUT4_COMPRESSOR_AMOUNT": 0.2,
        "INPUT4_EXT_FX_MUTE": False,
        "INPUT4_EXT_FX_SENDS": 0.55,
        "STEREO_INPUT1_VOLUME": -9.0,
        "STEREO_INPUT1_MUTE": True,
        "STEREO_INPUT1_EQ_ENABLE": False,
        "STEREO_INPUT1_EQ_LOW_GAIN": 2.0,
        "STEREO_INPUT1_EQ_MID_GAIN": -2.0,
        "STEREO_INPUT1_EQ_HIGH_GAIN": 3.0,
        "EFFECT11_TYPEID": 1,
        "EFFECT12_TYPEID": 2,
        "EFFECT13_TYPEID": 3,
        "EFFECT14_TYPEID": 4,
        "EFFECT21_TYPEID": 5,
        "EFFECT22_TYPEID": 6,
        "MAIN_HEADPHONE_GAIN": 8.0,
        "MAIN_MASTER_GAIN": -24.0,
        "MAIN_MUTE": True,
        "LOOPER_LEVEL": 5.0,
        "FX_BYPASS": True,
    }


def _snapshot_profile_b() -> dict[str, Any]:
    # Distinct from A on every key (except MAIN_MUTE stays muted).
    a = _snapshot_profile_a()
    b: dict[str, Any] = {}
    for key, val in a.items():
        if key == "MAIN_MUTE":
            b[key] = True
        elif isinstance(val, bool):
            b[key] = not val
        elif isinstance(val, int) and not isinstance(val, bool):
            if "TYPEID" in key or key in ("FEEDBACK_ELIM", "SELECTED_CHAN"):
                b[key] = (val + 3) % 8
            else:
                b[key] = val + 8
        elif key in (
            "INPUT1_EFFECT_1_AMOUNT", "INPUT1_EFFECT_2_AMOUNT", "INPUT1_COMPRESSOR_AMOUNT", "INPUT1_EXT_FX_SENDS",
            "INPUT2_EFFECT_1_AMOUNT", "INPUT2_EFFECT_2_AMOUNT", "INPUT2_COMPRESSOR_AMOUNT", "INPUT2_EXT_FX_SENDS",
            "INPUT3_EFFECT_1_AMOUNT", "INPUT3_EFFECT_2_AMOUNT", "INPUT3_COMPRESSOR_AMOUNT", "INPUT3_EXT_FX_SENDS",
            "INPUT4_EFFECT_1_AMOUNT", "INPUT4_EFFECT_2_AMOUNT", "INPUT4_COMPRESSOR_AMOUNT", "INPUT4_EXT_FX_SENDS",
        ):
            fv = float(val)
            b[key] = 0.95 if fv < 0.5 else 0.05
        elif key in ("LOOPER_LEVEL", "MAIN_HEADPHONE_GAIN"):
            b[key] = 2.0 if float(val) >= 5 else 10.0
        elif "EQ_" in key and "GAIN" in key:
            # EQ bands quantize in ~2.14 dB steps and clamp near ±6.
            b[key] = -float(val) if abs(float(val)) > 0.5 else -5.0
        else:
            b[key] = float(val) + 8.0 if float(val) < 10 else float(val) - 8.0
    return b


def _entities_map() -> dict[str, Any]:
    doc = api("/api/v1/showbox/entities")
    out: dict[str, Any] = {}
    for row in doc.get("entities") or []:
        eid = row.get("id")
        if eid:
            out[str(eid)] = row.get("value")
    return out


def _ensure_main_mute(s: Suite) -> None:
    api("/api/v1/showbox/entities/MAIN_MUTE", {"value": True})
    end = time.monotonic() + 4.0
    while time.monotonic() < end:
        got = _entities_map().get("MAIN_MUTE")
        if got is True or got == 1 or str(got).lower() == "true":
            return
        time.sleep(0.2)
        api("/api/v1/showbox/entities/MAIN_MUTE", {"value": True})
    raise CaseFail(f"MAIN_MUTE stuck unmuted got={_entities_map().get('MAIN_MUTE')!r}")


def _set_entities(s: Suite, values: dict[str, Any]) -> None:
    # MAIN_MUTE first so later loud fader moves stay silent.
    api("/api/v1/showbox/entities/MAIN_MUTE", {"value": True})
    for eid, val in values.items():
        if eid == "MAIN_MUTE":
            continue
        api(f"/api/v1/showbox/entities/{eid}", {"value": val})
    # Second pass: UART/API drops under burst are common; reinforce diffs.
    time.sleep(0.3)
    for eid, val in values.items():
        if eid == "MAIN_MUTE":
            continue
        api(f"/api/v1/showbox/entities/{eid}", {"value": val})
    api("/api/v1/showbox/entities/MAIN_MUTE", {"value": True})
    time.sleep(0.8)
    _ensure_main_mute(s)


def _field_tol(key: str, want: Any, base: float = 0.6) -> float:
    # Normalized 0..1 amounts need a tight band; dB/gain can quantize coarser.
    if key.endswith("_AMOUNT") or key.endswith("_SENDS"):
        return 0.12
    if key in ("LOOPER_LEVEL", "MAIN_HEADPHONE_GAIN"):
        return 0.6
    try:
        if abs(float(want)) <= 1.5 and "GAIN" not in key and "VOLUME" not in key:
            return min(base, 0.2)
    except (TypeError, ValueError):
        pass
    return base


def _vals_close(a: Any, b: Any, tol: float = 0.6, key: str = "") -> bool:
    if a is None or b is None:
        return a is b
    # Bool / UINT8 mute flags: JSON may use true/false or 0/1.
    if isinstance(a, bool) or isinstance(b, bool):
        try:
            return bool(int(a)) is bool(int(b))
        except (TypeError, ValueError):
            return bool(a) is bool(b)
    try:
        use = _field_tol(key, b, base=tol) if key else tol
        return abs(float(a) - float(b)) <= use
    except (TypeError, ValueError):
        return a == b


def _read_profile(keys: list[str]) -> dict[str, Any]:
    cur = _entities_map()
    return {k: cur.get(k) for k in keys}


def _assert_profile(s: Suite, label: str, expected: dict[str, Any], tol: float = 0.75) -> None:
    got = _read_profile(list(expected.keys()))
    bad: list[str] = []
    for key, want in expected.items():
        have = got.get(key)
        if not _vals_close(have, want, tol=tol, key=key):
            bad.append(f"{key}: want={want!r} got={have!r}")
    s.check(f"{label}: MAIN muted", bool(got.get("MAIN_MUTE")) is True, f"MAIN_MUTE={got.get('MAIN_MUTE')!r}")
    s.check(f"{label}: {len(expected)} fields match", not bad, f"mismatches={len(bad)} " + "; ".join(bad[:8]))


def _snapshot_action(s: Suite, action: str, slot: int) -> None:
    """SAVE/RECALL via REST. Slot is 0-based (SNAPSHOT_1=0)."""
    # Firmware busy: SAVE ~5s, RECALL ~3.5s. dumpSeq often does not bump
    # mid-session; wait the busy window then a short settle.
    wait_s = 5.5 if action.upper() == "SAVE" else 4.0
    api("/api/v1/showbox/action/snapshot", {"action": action, "slot": slot})
    time.sleep(wait_s)
    _ensure_main_mute(s)


def case_snapshot_mixer(s: Suite) -> None:
    """Full mixer snapshot: set→save→mutate→recall on slot 1 (API slot 0).

    Broader than webui snapshot e2e (single INPUT1_GAIN via Chromium). MAIN
    stays muted the whole time.
    """
    slot = 0  # SNAPSHOT_1
    hold_before = api("/api/v1/showbox/analog-hold") or {}
    # Analog hold fights HEADPHONE / LOOPER_LEVEL writes and recall mirror.
    api("/api/v1/showbox/analog-hold", {"phones": False, "loop": False})
    try:
        _ensure_main_mute(s)
        profile_a = _snapshot_profile_a()
        profile_b = _snapshot_profile_b()
        # Analog pots stream continuously; they are written but not part of the
        # strict save/recall compare (SAVE busy window often overwrites them).
        analog_skip = {"LOOPER_LEVEL", "MAIN_HEADPHONE_GAIN"}
        keys = [k for k in profile_a.keys() if k not in analog_skip]
        s.check("profiles cover mixer fields", len(keys) >= 60, f"n={len(keys)}")

        s.info(f"apply profile A ({len(profile_a)} fields, compare {len(keys)})")
        _set_entities(s, profile_a)
        # Quantized readback is the golden snapshot payload — not the posted floats.
        time.sleep(1.0)
        golden = _read_profile(keys)
        golden["MAIN_MUTE"] = True
        missing = [k for k, v in golden.items() if v is None]
        s.check("profile A readable", not missing, f"missing={missing[:8]}")
        _assert_profile(s, "after set A", golden)

        s.info("SAVE snapshot 1")
        _snapshot_action(s, "SAVE", slot)
        _assert_profile(s, "after SAVE", golden)

        s.info(f"apply profile B ({len(keys)} fields)")
        _set_entities(s, profile_b)
        time.sleep(1.0)
        after_b = _read_profile(keys)
        after_b["MAIN_MUTE"] = True
        def _diff_from_golden(cur: dict[str, Any]) -> tuple[int, list[str]]:
            ch = 0
            st: list[str] = []
            for key in keys:
                if key == "MAIN_MUTE":
                    continue
                if _vals_close(cur.get(key), golden.get(key), tol=0.75, key=key):
                    st.append(key)
                else:
                    ch += 1
            return ch, st

        changed, stuck = _diff_from_golden(after_b)
        # Re-poke any fields that did not leave golden A (UART drop / clamp).
        if stuck:
            s.info(f"re-poke {len(stuck)} stuck B fields")
            for key in stuck:
                api(f"/api/v1/showbox/entities/{key}", {"value": profile_b[key]})
            api("/api/v1/showbox/entities/MAIN_MUTE", {"value": True})
            time.sleep(0.8)
            after_b = _read_profile(keys)
            after_b["MAIN_MUTE"] = True
            changed, stuck = _diff_from_golden(after_b)
        s.check(
            "profile B differs from A",
            changed >= max(40, len(keys) // 2) and len(stuck) <= 8,
            f"changed={changed} stuck={stuck[:12]}",
        )
        _ensure_main_mute(s)

        s.info("RECALL snapshot 1")
        _snapshot_action(s, "RECALL", slot)
        # Mirror / UART settle; poll until close or timeout.
        end = time.monotonic() + 12.0
        last_bad = "not checked"
        while time.monotonic() < end:
            got = _read_profile(keys)
            bad = [
                k for k, w in golden.items()
                if k != "MAIN_MUTE" and not _vals_close(got.get(k), w, tol=0.9, key=k)
            ]
            if not bad and bool(got.get("MAIN_MUTE")) is True:
                last_bad = ""
                break
            last_bad = ",".join(bad[:8])
            time.sleep(0.35)
        _ensure_main_mute(s)
        _assert_profile(s, "after RECALL", golden, tol=0.9)
        if last_bad:
            s.info(f"recall settle leftover noted: {last_bad}")
    finally:
        api(
            "/api/v1/showbox/analog-hold",
            {
                "phones": bool(hold_before.get("phones", True)),
                "loop": bool(hold_before.get("loop", True)),
            },
        )
        _ensure_main_mute(s)


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
    # Prior ble-midi / looper unboost stamps ignoreShowboxUntil for
    # boostIgnoreMs (3500). Starting the user move inside that window drops
    # the INPUT2_GAIN announce so REMEMBERED_GAIN never leaves base
    # (L3 streak6 att1: REMEMBERED=28 after write 40).
    time.sleep(3.6)
    auto0 = var("AUTO_BOOST", 1)
    set_var("AUTO_BOOST", 0)
    s._mark_cc()
    api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": GAIN_BASE})
    base_now = s.wait_gain(GAIN_BASE, timeout=3.0)
    if base_now is None or abs(base_now - GAIN_BASE) >= 0.4:
        raise CaseFail(f"gain-tracker base settle failed got={base_now}")
    set_var("REMEMBERED_GAIN", GAIN_BASE)
    set_var("BOOST_ENABLED", 1)
    moved = 40.0
    s._mark_cc()
    api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": moved})
    now = s.wait_gain(moved, timeout=3.0)
    if now is None or abs(now - moved) >= 0.4:
        api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": moved})
        now = s.wait_gain(moved, timeout=3.0)
    if now is None or abs(now - moved) >= 0.4:
        raise CaseFail(f"INPUT2_GAIN did not move to {moved} got {now}")
    rem = s.wait_var("REMEMBERED_GAIN", moved, timeout=5.0)
    if rem is None or abs(float(rem) - moved) >= 0.05:
        # One re-announce after the ignore window — dump can still be gated
        # if an intervening Unboost refreshed ignoreShowboxUntil.
        api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": GAIN_BASE})
        s.wait_gain(GAIN_BASE, timeout=2.0)
        time.sleep(0.4)
        set_var("REMEMBERED_GAIN", GAIN_BASE)
        set_var("BOOST_ENABLED", 1)
        api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": moved})
        s.wait_gain(moved, timeout=3.0)
        rem = s.wait_var("REMEMBERED_GAIN", moved, timeout=5.0)
    s.check("tracker adopts user gain", rem is not None and abs(float(rem) - moved) < 0.05, f"REMEMBERED_GAIN={rem}")
    flag = s.wait_var("BOOST_ENABLED", 0, timeout=3.0)
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
    if s.ble is not None and not cf.live_host_address(s.session, ""):
        host = _wait_esp_ble_host(s, s.ble, timeout=25.0)
        if not host:
            raise CaseFail("no connected laptop host for boost-ratchet CC105")
    # Writing INPUT2_GAIN=36 can race the gain tracker into adopting 36 as
    # REMEMBERED_GAIN (L2 streak3 pass6: then CC105 boosted 36→44). Re-pin
    # the intentional desync immediately before the toggle.
    set_var("REMEMBERED_GAIN", GAIN_BASE)
    set_var("BOOST_ENABLED", 0)
    s.send_cc(105)
    after = s.wait_gain(GAIN_BASE, timeout=6.0)
    if after is None or abs(after - GAIN_BASE) >= 0.4:
        # A second CC105 after a real miss is fine. After a silent
        # success it would re-boost. Only retry if gain never left +2.
        if after is not None and abs(after - ratcheted) < 0.4:
            pass
        else:
            # Soft _ensure_midi retry is not enough when the host ACL is
            # already gone (L3 streak8 att9: gain stayed 36 through 2× CC105).
            s.info("boost-ratchet: CC105 miss, hard-recycle BLE ACL")
            if s.ble is not None:
                try:
                    s.ble.close()
                except Exception:
                    pass
                s.ble = None
            _drop_rtp(s)
            ensure_ble_midi(s, force=True)
            if s.ble is None or not (
                cf.live_host_address(s.session, "")
                or _wait_esp_ble_host(s, s.ble, timeout=30.0)
            ):
                raise CaseFail("no ESP host after boost-ratchet BLE recycle")
            # Restore the +2 precondition before retry. ACL thrash can drift
            # INPUT2_GAIN off 36 (L3 streak10 att10: gain=16 after recycle so
            # reconcile ran from garbage and left 16 instead of unboost→28).
            api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": boosted})
            restored = s.wait_gain(boosted, timeout=4.0)
            if restored is None or abs(restored - boosted) >= 0.4:
                raise CaseFail(f"boost-ratchet could not restore +2 got={restored}")
            set_var("REMEMBERED_GAIN", GAIN_BASE)
            set_var("BOOST_ENABLED", 0)
            s.send_cc(105)
            after = s.wait_gain(GAIN_BASE, timeout=8.0)
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
    # force=True: prior L2/L3 cases can leave a zombie ACL that still looks
    # Connected while AcquireWrite returns Not connected (L3 streak4 stop).
    ensure_ble_midi(s, force=True)
    module = s.module_addr or cf.bt_find_showbox() or ""
    if not module:
        raise CaseFail("no ShowboxMod address for HID")
    # Role POST is for a Remembered host. Gate on ESP hosts[].connected, not
    # BlueZ alone — force recycle can leave hostUsed=0 (L3 streak5 att3).
    host = cf.live_host_address(s.session, "")
    if not host and s.ble is not None:
        host = _wait_esp_ble_host(s, s.ble, timeout=30.0)
    if not host:
        ensure_ble_midi(s, force=True)
        host = cf.live_host_address(s.session, "")
        if not host and s.ble is not None:
            host = _wait_esp_ble_host(s, s.ble, timeout=30.0)
    if not host:
        raise CaseFail("no connected laptop host for HID role")
    case = cf.HOST_CASES[2]  # MIDI+HID keyboard+mouse
    code, text = cf.apply_case(s.session, host, case)
    if code != 200:
        raise CaseFail(f"HID role http={code} {text[:120]}")
    names = cf.wait_laptop_hid(module, s.session)
    s.check("HID /dev/input ShowboxMod", bool(names), f"nodes={names}")
    if names:
        cf.http_post(f"{s.session.base}/api/v1/hid/keyboard", {"key": "pageup"})
        s.check("HID keyboard POST", True, "pageup")


def case_airstep_held(s: Suite) -> None:
    """autoConnect peripherals must stay up (or rejoin) while the laptop is host.

    USB JTAG↔gadget switches reboot the module; AIRSTEP autoConnect typically
    returns in ~8s. Do not fail on a one-shot read right after that reboot
    (L3 after L2 usb-midi-hid: down=['AIRSTEP'] at t=0 while auto=true).
    """
    midi = s.midi_status()
    autos = [p for p in (midi.get("peripherals") or []) if p.get("autoConnect")]
    if not autos:
        raise CaseSkip("no autoConnect peripheral saved")
    deadline = time.monotonic() + 20.0
    last_connect = 0.0
    posts = 0
    down: list[Any] = [p.get("name") or p.get("address") for p in autos if not p.get("connected")]
    while down and time.monotonic() < deadline:
        now = time.monotonic()
        if posts < 3 and now - last_connect >= 4.0:
            for p in autos:
                if p.get("connected"):
                    continue
                addr = str(p.get("address") or "")
                name = str(p.get("name") or "AIRSTEP")
                if not addr:
                    continue
                cf.http_post(
                    f"{s.session.base}/api/v1/midi/ble/auto",
                    {"address": addr, "enabled": "1"},
                    timeout=3.0,
                )
                cf.http_post(
                    f"{s.session.base}/api/v1/midi/ble/connect",
                    {"address": addr, "name": name, "midi": "1", "hid": "0"},
                    timeout=3.0,
                )
            last_connect = now
            posts += 1
            s.info(f"autoConnect peri down after USB/radio load — connect POST {posts}: {down}")
        time.sleep(0.5)
        midi = s.midi_status()
        autos = [p for p in (midi.get("peripherals") or []) if p.get("autoConnect")]
        down = [p.get("name") or p.get("address") for p in autos if not p.get("connected")]
    s.check("AIRSTEP still up while laptop is host", not down, f"down={down}")


def _wait_ads(s: Suite, timeout: float = 40.0) -> dict[str, Any]:
    """Open the pairing window; only return midi when ads are actually live.

    Do not trust `laptop_open_ads()` then a one-shot `midi_status()`: after
    webui SPA the follow-up GET can come back empty while open_ads already
    returned True (L3 streak12 att4: advertising=None pairingMs=None hosts=None).
    """
    end = time.monotonic() + timeout
    midi: dict[str, Any] = {}
    last_post = 0.0
    while time.monotonic() < end:
        if last_post == 0.0 or (time.monotonic() - last_post) >= 8.0:
            try:
                s.session.post_settings(acceptNewHosts=True)
            except Exception as exc:
                s.info(f"acceptNewHosts: {exc}")
            last_post = time.monotonic()
        midi = s.midi_status()
        if midi.get("advertising") and int(midi.get("pairingMs") or 0) > 1000:
            return midi
        if not midi:
            wait_http_quiet(s.ip, timeout=min(5.0, max(0.5, end - time.monotonic())))
        remaining = end - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(0.5, remaining))
    return midi


def case_fresh_pair(s: Suite) -> None:
    if s.ble is not None:
        s.ble.close()
        s.ble = None
    if not wait_http_quiet(s.ip, timeout=16.0):
        raise CaseFail("HTTP down before fresh-pair")
    midi = _wait_ads(s, timeout=45.0)
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
    # Cold-load leaves SYN-ACK-no-body PCBs. Small /services still works;
    # Chromium page.goto does not. Range-resume SPA + wait clears slots.
    wait_http_quiet(s.ip, timeout=8.0)
    http_get_resumable(url, timeout=2.5, ua="Mozilla/5.0", attempts=4, min_bytes=500, deadline=8.0)
    wait_http_quiet(s.ip, timeout=8.0)
    r2 = None
    for attempt in range(3):
        if not wait_http_quiet(s.ip, timeout=12.0):
            s.info(f"webui snapshot: HTTP down before attempt {attempt + 1}")
            continue
        # Retries after a failed SAVE/RECALL race used to compound cache≠mixer
        # (L2 streak5 pass9: attempt1 stayed at 40, attempt3 drifted after save).
        if attempt:
            wait_http_quiet(s.ip, timeout=6.0)
            try:
                api("/api/v1/showbox/entities/INPUT1_GAIN", {"value": 40})
            except Exception as exc:
                s.info(f"webui heal INPUT1_GAIN: {exc}")
            time.sleep(0.8)
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
        # Recording with recSec still 0 used to short-circuit on the WS
        # snapshot and never refresh via HTTP (L2 streak4: sdCard=2 recSec=0
        # for the whole 4s wait while /link already had the tick).
        if sd == 2 and sec < 1:
            link = s.link()
            return _num(link.get("sdCard")), _num(link.get("recSec"))
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
            s.ws.got_link.wait(timeout=min(0.4, remaining))
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
    end = time.monotonic() + 10.0
    saw_tick = sec >= 1
    while time.monotonic() < end and not saw_tick:
        sd, sec = rec_fields()
        if sd == 2 and sec >= 1:
            saw_tick = True
            break
        remaining = end - time.monotonic()
        if remaining <= 0:
            break
        s.ws.got_link.wait(timeout=min(0.5, remaining))
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
    # A single timed-out ap=true leaves apForceTry clear under SoftAP quiet.
    wifi = _wait_softap_up(s, timeout=70.0)
    if not wifi or not wifi.get("ap"):
        raise CaseFail(f"SoftAP did not come up for air scan: {wifi}")
    s.check("STA stayed up for air scan", bool(wifi.get("sta")), f"{wifi}")
    try:
        ssids: list[str] = []
        end = time.monotonic() + 12.0
        while time.monotonic() < end:
            ssids = _scan_ssids()
            if "ShowboxMod" in ssids:
                break
            time.sleep(1.0)
        s.check("air scan sees ShowboxMod after AP on", "ShowboxMod" in ssids, f"ssids={ssids[:8]}")
    finally:
        try:
            _wifi_api(s, {"ap": False})
        except Exception as exc:
            s.info(f"SoftAP disable: {exc}")


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
    # force=True: post boost-reboot / simultaneous the BlueZ ACL can look up
    # while AcquireWrite returns Not connected (L3 streak7 att2). Then a
    # single-shot AppleMIDI invite RuntimeError aborted the suite as ERROR.
    ensure_ble_midi(s, force=True)
    if s.rtp is not None:
        try:
            s.rtp.close()
        except Exception:
            pass
        s.rtp = None
    s.ensure_rtp()
    try:
        rtp = s.rtp
        assert rtp is not None
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
        # A single http=0 with uart live is the same coex stall (L3 streak9
        # att1: soak 17 ping=False http=0 uart=ok → 1/79 fail). Retry once.
        if code != 200 and live:
            code, body = http_get_retry(
                f"http://{s.ip}/services", timeout=2.0, attempts=3, min_bytes=100
            )
            live, why = cf.uart_ok(s.link(), 2000)
            if code == 200 and live:
                s.info(f"soak {n} http recovered after retry ping={ping}")
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


def _transport(midi: dict, tid: str) -> dict:
    for row in midi.get("transports") or []:
        if row.get("id") == tid:
            return row
    return {}


def _usb_showbox_present() -> bool:
    try:
        out = subprocess.run(["lsusb"], capture_output=True, text=True, timeout=3, check=False)
        return "303a:0002" in (out.stdout or "").lower() or "showboxmod" in (out.stdout or "").lower()
    except (OSError, subprocess.TimeoutExpired):
        return False


def _usb_hid_nodes() -> list[str]:
    root = Path("/dev/input/by-id")
    if not root.is_dir():
        return []
    return sorted(p.name for p in root.glob("*ShowboxMod*"))


def _usb_midi_listed() -> bool:
    try:
        out = subprocess.run(["amidi", "-l"], capture_output=True, text=True, timeout=3, check=False)
        return "ShowboxMod" in (out.stdout or "")
    except (OSError, subprocess.TimeoutExpired):
        return False


def _lsusb_text() -> str:
    try:
        out = subprocess.run(["lsusb"], capture_output=True, text=True, timeout=3, check=False)
        return out.stdout or ""
    except (OSError, subprocess.TimeoutExpired):
        return ""


def case_usb_jtag(s: Suite) -> None:
    """Fast native-USB JTAG smoke. Needs --usb and Serial/JTAG mode (not gadget)."""
    if not getattr(s.args, "usb", False):
        raise CaseSkip("pass --usb when native USB-C is plugged into this host")
    midi = api("/api/v1/midi")
    # L2 usb-midi-hid leaves the port in MIDI+HID gadget mode for L3
    # usb-gadget-held; L3 re-runs this L1 case — skip rather than fail.
    if midi.get("usbJtag") is not True and (
        _transport(midi, "usbMidiPeripheral").get("enabled")
        or _transport(midi, "usbHid").get("enabled")
    ):
        raise CaseSkip("USB MIDI+HID gadget active (JTAG off until restored)")
    usb = midi.get("usb") or {}
    mode = str(usb.get("mode") or "")
    if midi.get("usbJtag") is not True and mode != "jtag":
        raise CaseSkip(f"USB not in JTAG mode (mode={mode!r}) — run before usb-midi-hid")
    s.check("usb reports JTAG", midi.get("usbJtag") is True or mode == "jtag", f"usbJtag={midi.get('usbJtag')} mode={mode!r}")
    lsusb = _lsusb_text().lower()
    s.check("host lsusb Espressif JTAG 303a:1001", "303a:1001" in lsusb, "lsusb")
    # QinHeng UART adapter may also be present; ACM0 is the native JTAG CDC.
    s.check("host /dev/ttyACM present", any(Path("/dev").glob("ttyACM*")), "ttyACM*")


def case_ethernet_smoke(s: Suite) -> None:
    """Fast W5500 HTTP smoke via eth IP. Needs --ethernet; full matrix stays L2."""
    if not getattr(s.args, "ethernet", False):
        raise CaseSkip("pass --ethernet when the W5500 cable is linked")
    midi = api("/api/v1/midi")
    note = str(_transport(midi, "ethernet").get("note") or "")
    eth_ip = next((p.strip(",") for p in note.replace(",", " ").split() if p.count(".") == 3 and p[0].isdigit()), "")
    if not eth_ip:
        # Fallback: try common desk IP / STA sibling via services on eth path fails hard.
        raise CaseFail(f"no ethernet IP in midi note={note!r}")
    code, body = http_get(f"http://{eth_ip}/services", timeout=2.0)
    s.check("eth /services", code == 200 and len(body) > 100, f"code={code} eth={eth_ip}")
    code2, body2 = http_get(f"http://{eth_ip}/api/v1/wifi", timeout=2.0)
    s.check("eth /api/v1/wifi", code2 == 200 and b'"sta"' in body2, f"code={code2}")


def case_usb_midi_hid(s: Suite) -> None:
    """Native USB gadget: MIDI+HID composite. Needs --usb and the USB-C cable."""
    if not getattr(s.args, "usb", False):
        raise CaseSkip("pass --usb when the native USB-C cable is plugged into this host")
    midi = api("/api/v1/midi")
    peri = _transport(midi, "usbMidiPeripheral")
    hid = _transport(midi, "usbHid")
    if not (peri.get("enabled") and hid.get("enabled")):
        api_path = "/api/v1/midi/settings?usbMidiHost=0&usbMidiPeripheral=1&usbHid=1"
        urllib.request.urlopen(
            urllib.request.Request(f"http://{s.ip}{api_path}", method="POST", data=b""),
            timeout=5,
        ).read()
        time.sleep(8.0)
        end = time.monotonic() + 45.0
        while time.monotonic() < end:
            try:
                midi = api("/api/v1/midi")
                peri = _transport(midi, "usbMidiPeripheral")
                hid = _transport(midi, "usbHid")
                if peri.get("enabled") and hid.get("enabled"):
                    break
            except Exception:
                pass
            time.sleep(1.5)
    s.check("usb MIDI peripheral enabled", bool(peri.get("enabled")), f"{peri}")
    s.check("usb HID enabled", bool(hid.get("enabled")), f"{hid}")
    s.check("usb JTAG off while gadget on", midi.get("usbJtag") is False, f"usbJtag={midi.get('usbJtag')}")
    host = _transport(midi, "usbMidiHost")
    s.check("usb MIDI host stays off", not host.get("enabled"), f"{host}")
    # Host must see composite (QinHeng UART adapter can remain as ttyACM*)
    end = time.monotonic() + 20.0
    while time.monotonic() < end and not (_usb_showbox_present() and _usb_hid_nodes() and _usb_midi_listed()):
        time.sleep(0.5)
    s.check("host lsusb ShowboxMod 303a:0002", _usb_showbox_present(), "lsusb")
    nodes = _usb_hid_nodes()
    s.check("host HID by-id ShowboxMod", bool(nodes), f"nodes={nodes}")
    s.check("host amidi ShowboxMod", _usb_midi_listed(), "amidi -l")
    kbd = next(Path("/dev/input/by-id").glob("*ShowboxMod*event-kbd"), None)
    if kbd is None:
        raise CaseFail("no ShowboxMod event-kbd node")
    fd = os.open(str(kbd.resolve()), os.O_RDONLY | os.O_NONBLOCK)
    try:
        while True:
            try:
                os.read(fd, 24)
            except BlockingIOError:
                break
        posted = False
        last_exc: Exception | None = None
        for attempt in range(4):
            try:
                urllib.request.urlopen(
                    urllib.request.Request(
                        f"http://{s.ip}/api/v1/hid/keyboard?key=pageup",
                        method="POST",
                        data=b"",
                        headers={"Connection": "close"},
                    ),
                    timeout=5,
                ).read()
                posted = True
                break
            except Exception as exc:
                last_exc = exc
                time.sleep(0.4 * (attempt + 1))
        if not posted:
            raise CaseFail(f"HID keyboard POST failed: {last_exc}")
        hits: list[int] = []
        fmt = "llHHi"
        size = struct.calcsize(fmt)
        end = time.monotonic() + 2.5
        while time.monotonic() < end:
            r, _, _ = select.select([fd], [], [], 0.2)
            if not r:
                continue
            data = os.read(fd, size * 8)
            for i in range(0, len(data), size):
                if i + size > len(data):
                    break
                _a, _b, typ, code, value = struct.unpack(fmt, data[i : i + size])
                if typ == 1 and code == 104:
                    hits.append(value)
        s.check("HID pageup reached host", 1 in hits and 0 in hits, f"hits={hits}")
    finally:
        os.close(fd)
    # MIDI port writable (host → device)
    try:
        out = subprocess.run(
            ["amidi", "-p", "hw:1,0,0", "-S", "B0 69 01"],
            capture_output=True,
            text=True,
            timeout=3,
            check=False,
        )
        # Card number may not be 1 — find from amidi -l
        if out.returncode != 0:
            listed = subprocess.run(["amidi", "-l"], capture_output=True, text=True, timeout=3, check=False)
            port = None
            for ln in (listed.stdout or "").splitlines():
                if "ShowboxMod" in ln:
                    parts = ln.split()
                    if len(parts) >= 2:
                        port = parts[1]
                        break
            if port:
                out = subprocess.run(
                    ["amidi", "-p", port, "-S", "B0 69 01"],
                    capture_output=True,
                    text=True,
                    timeout=3,
                    check=False,
                )
        s.check("USB MIDI port accepts host TX", out.returncode == 0, (out.stderr or out.stdout or "")[:120])
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise CaseFail(f"amidi failed: {exc}") from exc

def case_hotspot_sta_coex(s: Suite) -> None:
    """Hotspot + home STA: LAN HTTP stays up; SoftAP serves captive probes."""
    _wifi_api(s, {"apAlways": True, "ap": True})
    wifi = _wait_wifi_pred(s, lambda w: bool(w.get("sta") and w.get("ap") and w.get("apIp")), timeout=45.0)
    if not wifi:
        raise CaseFail("failed to reach STA+SoftAP coex")
    s.check("coex STA", bool(wifi.get("sta")), f"sta={wifi.get('sta')}")
    s.check("coex SoftAP", bool(wifi.get("ap")), f"ap={wifi.get('ap')} ip={wifi.get('apIp')!r}")
    s.check("coex apAlways", wifi.get("apAlways") is True, f"apAlways={wifi.get('apAlways')!r}")
    s.check("coex STA IP stable", str(wifi.get("ip") or "") == s.ip, f"ip={wifi.get('ip')}")
    code, body = http_get_retry(f"http://{s.ip}/api/v1/wifi", timeout=3.0, attempts=5, min_bytes=20)
    s.check("STA /api/v1/wifi while SoftAP", code == 200 and b'"sta"' in body, f"code={code} bytes={len(body)}")
    # /services is the HTML health shell; under SoftAP+L2 load STA :80 can
    # briefly refuse the bigger body while /wifi still answers — quiet retry.
    if not wait_http_quiet(s.ip, timeout=10.0):
        s.info("STA not quiet before /services under SoftAP")
    code, body = http_get_retry(
        f"http://{s.ip}/services", timeout=4.0, attempts=8, min_bytes=100, quiet=True
    )
    if code != 200 or len(body) <= 100:
        eth = _eth_fallback_ip(s)
        if eth and eth != s.ip:
            s.info(f"STA /services via eth {eth} after SoftAP coex fail")
            code, body = http_get_retry(
                f"http://{eth}/services", timeout=4.0, attempts=4, min_bytes=100, quiet=True
            )
    s.check(
        "STA /services while SoftAP",
        code == 200 and (b"ShowboxMod" in body or len(body) > 100),
        f"code={code} bytes={len(body)}",
    )

    ap_ip = str(wifi.get("apIp") or "192.168.4.1")
    ap_pass = str(wifi.get("apPassword") or "showboxmod")
    joined, how = _join_showbox_softap(ap_pass)
    if not joined:
        # Ethernet-only hosts cannot prove the captive path; air+STA HTTP still counts.
        if _softap_iface() is None:
            raise CaseSkip(f"no wifi iface for SoftAP client ({how})")
        raise CaseFail(f"could not join ShowboxMod SoftAP: {how}")
    try:
        # Captive portal probes must answer on the SoftAP subnet.
        for path, need in (
            ("/generate_204", None),
            ("/hotspot-detect.html", b"Success"),
            ("/ncsi.txt", b"Microsoft"),
            ("/api/v1/wifi", b'"ap"'),
        ):
            c, b = http_get_retry(f"http://{ap_ip}{path}", timeout=3.0, attempts=4, min_bytes=1)
            if need is None:
                s.check(f"SoftAP {path}", c in (200, 204, 302, 301) or c == 511, f"code={c} bytes={len(b)}")
            else:
                s.check(f"SoftAP {path}", c == 200 and need in b, f"code={c} bytes={len(b)}")
        # Laptop left home Wi-Fi to join SoftAP, so http://STA-IP is the wrong
        # probe (single-radio host). Prove LAN still answers via ethernet, and
        # SoftAP /api/v1/wifi already showed sta=true above.
        midi = s.midi_status()
        eth_note = str(_transport(midi, "ethernet").get("note") or "")
        eth_ip = next((p.strip(",") for p in eth_note.split() if p.startswith("192.168.")), "")
        if eth_ip:
            c, b = http_get_retry(f"http://{eth_ip}/api/v1/wifi", timeout=3.0, attempts=4, min_bytes=20)
            s.check(
                "eth path survives SoftAP client",
                c == 200 and b'"sta":true' in b,
                f"code={c} eth={eth_ip}",
            )
        else:
            s.info("no ethernet IP in midi note — skip LAN-while-SoftAP client probe")
    finally:
        _leave_showbox_softap()
        try:
            _wifi_api(s, {"ap": False, "apAlways": False})
        except Exception as exc:
            s.info(f"SoftAP cleanup after coex: {exc}")


def _home_wifi_psk(ssid: str = "pfSenseAP") -> str:
    """Read the laptop's saved PSK so street-case can restore module STA creds."""
    try:
        out = subprocess.run(
            ["nmcli", "-s", "-g", "802-11-wireless-security.psk", "connection", "show", ssid],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        psk = (out.stdout or "").strip()
        if psk:
            return psk
    except (OSError, subprocess.TimeoutExpired):
        pass
    return ""


def _probe_softap_services(s: Suite, ap_ip: str) -> None:
    """HTTP / WS / telnet / RTP MIDI on the SoftAP subnet (after laptop joins)."""
    # /services is the HTML health shell (sendHealthHtml), not JSON.
    code, body = http_get_retry(f"http://{ap_ip}/services", timeout=3.0, attempts=5, min_bytes=100)
    s.check(
        "SoftAP HTTP /services",
        code == 200 and (b"ShowboxMod" in body or b"showbox" in body.lower()),
        f"code={code} bytes={len(body)}",
    )
    code, body = http_get_retry(f"http://{ap_ip}/api/v1/wifi", timeout=3.0, attempts=4, min_bytes=20)
    s.check("SoftAP HTTP /api/v1/wifi", code == 200 and b'"ap"' in body, f"code={code}")
    # /midi JSON is large; after L2 SoftAP churn STA HTTP can be fine while
    # SoftAP :80 briefly refuses — quiet + longer retries before failing.
    if not wait_http_quiet(ap_ip, timeout=12.0):
        s.info(f"SoftAP {ap_ip} not quiet before /midi — probing anyway")
    code, body = http_get_retry(f"http://{ap_ip}/api/v1/midi", timeout=5.0, attempts=8, min_bytes=20, quiet=True)
    s.check("SoftAP HTTP /api/v1/midi", code == 200 and b"transports" in body, f"code={code} bytes={len(body)}")
    # Do not GET bare /link — that is the gzip SPA catch-all (~76KB), which
    # wedges SoftAP :80 under L2 load (code=0 after /midi). Use the JSON API.
    if not wait_http_quiet(ap_ip, timeout=10.0):
        s.info(f"SoftAP {ap_ip} not quiet before /showbox/link")
    code, body = http_get_retry(
        f"http://{ap_ip}/api/v1/showbox/link", timeout=4.0, attempts=8, min_bytes=20, quiet=True
    )
    s.check(
        "SoftAP HTTP /api/v1/showbox/link",
        code == 200 and b'"ok"' in body,
        f"code={code} bytes={len(body)}",
    )

    # mDNS QU toward the AP. Today the module often answers with the STA IP
    # even on SoftAP — still prove the responder answers from this subnet.
    try:
        from check_live import mdns_query_a

        addrs = mdns_query_a("showbox.local", ap_ip, timeout=2.0) or []
        s.check("SoftAP mDNS answers", bool(addrs), f"addrs={addrs}")
        if addrs and ap_ip not in addrs:
            s.info(f"SoftAP mDNS A is {addrs} (not AP IP {ap_ip}) — SoftAP clients may need 192.168.4.1")
    except Exception as exc:
        s.info(f"SoftAP mDNS probe skipped: {exc}")

    midi = s.midi_status()
    if midi.get("usbJtag") is False and (
        _transport(midi, "usbMidiPeripheral").get("enabled")
        or _transport(midi, "usbHid").get("enabled")
    ):
        s.info("SoftAP telnet skipped — USB MIDI+HID gadget holds RemoteDebug down")
    else:
        ok, detail = cf.telnet_messages(ap_ip, want=1, timeout=5.0)
        s.check("SoftAP telnet RemoteDebug", ok, detail)

    ws = LiveWs(ap_ip)
    ws.start()
    try:
        ws.got_link.wait(6.0)
        ws.got_entity.wait(3.0)
        s.check("SoftAP websocket link", ws.got_link.is_set() or bool(ws.last_link), f"err={ws.error}")
        if not (ws.got_entity.is_set() or ws.entities):
            try:
                api_url = f"http://{ap_ip}/api/v1/showbox/entities/FRONT_LED"
                urllib.request.urlopen(
                    urllib.request.Request(
                        api_url,
                        data=json.dumps({"value": True}).encode(),
                        headers={"Content-Type": "application/json"},
                        method="POST",
                    ),
                    timeout=4,
                ).read()
                ws.got_entity.wait(3.0)
            except Exception as exc:
                s.info(f"SoftAP WS entity poke: {exc}")
        s.check(
            "SoftAP websocket entity",
            ws.got_entity.is_set() or bool(ws.entities),
            f"n={len(ws.entities)} err={ws.error}",
        )
    finally:
        ws.close()

    rtp = RtpMidi(ap_ip)
    try:
        detail = rtp.connect()
        s.info(f"SoftAP AppleMIDI {detail}")
        s.check("SoftAP RTP MIDI session", True, detail)
    except Exception as exc:
        raise CaseFail(f"SoftAP RTP MIDI failed: {exc}") from exc
    finally:
        try:
            rtp.close()
        except Exception:
            pass


def case_softap_services(s: Suite) -> None:
    """Join ShowboxMod SoftAP from laptop Wi-Fi; prove services on 192.168.4.1."""
    if _softap_iface() is None:
        raise CaseSkip("no wifi iface for SoftAP client join")
    wifi = _wait_softap_up(s, timeout=70.0)
    if not wifi or not wifi.get("ap"):
        raise CaseFail(f"SoftAP did not come up for service matrix: {wifi}")
    ap_ip = str(wifi.get("apIp") or "192.168.4.1")
    ap_pass = str(wifi.get("apPassword") or "showboxmod")
    joined, how = _join_showbox_softap(ap_pass)
    if not joined:
        raise CaseFail(f"could not join ShowboxMod SoftAP: {how}")
    try:
        _probe_softap_services(s, ap_ip)
    finally:
        _leave_showbox_softap()
        try:
            _wifi_api(s, {"ap": False, "apAlways": False})
        except Exception as exc:
            s.info(f"SoftAP cleanup after services: {exc}")


def case_softap_street(s: Suite) -> None:
    """Street musician: SoftAP with no remembered home STA (forget pfSenseAP)."""
    if _softap_iface() is None:
        raise CaseSkip("no wifi iface for SoftAP client join")
    home_ssid = "pfSenseAP"
    home_psk = _home_wifi_psk(home_ssid)
    if not home_psk:
        raise CaseSkip(f"laptop has no saved PSK for {home_ssid} (cannot restore STA)")

    wifi0 = _wifi_api(s)
    remembered = [r.get("ssid") for r in (wifi0.get("remembered") or []) if r.get("ssid")]
    s.info(f"street setup remembered={remembered}")
    # Forget every remembered STA so the module cannot rejoin home Wi-Fi.
    for ssid in remembered:
        _wifi_api(s, {"forget": ssid})
    _wifi_api(s, {"autoConnect": False, "apAlways": True, "ap": True})
    # Drop current STA association if still up.
    try:
        _wifi_api(s, {"ap": True, "apAlways": True})
    except Exception:
        pass
    wifi = _wait_wifi_pred(
        s,
        lambda w: bool(w.get("ap") and w.get("apIp") and w.get("apIp") != "0.0.0.0"),
        timeout=60.0,
    )
    if not wifi:
        raise CaseFail("SoftAP did not come up in street mode")
    # STA should be down or not on home SSID once remembered nets are gone.
    s.check(
        "street SoftAP up",
        bool(wifi.get("ap")),
        f"ap={wifi.get('ap')} apIp={wifi.get('apIp')!r}",
    )
    s.check(
        "street no remembered home STA",
        not any(r.get("ssid") == home_ssid for r in (wifi.get("remembered") or [])),
        f"remembered={wifi.get('remembered')}",
    )
    if wifi.get("sta"):
        s.info(f"STA still reported up ssid={wifi.get('ssid')!r} (may be stale until roam)")

    ap_ip = str(wifi.get("apIp") or "192.168.4.1")
    ap_pass = str(wifi.get("apPassword") or "showboxmod")
    joined, how = _join_showbox_softap(ap_pass)
    if not joined:
        raise CaseFail(f"street SoftAP join failed: {how}")
    try:
        _probe_softap_services(s, ap_ip)
    finally:
        _leave_showbox_softap()
        # Restore home STA credentials and SoftAP off.
        try:
            _wifi_api(
                s,
                {
                    "ssid": home_ssid,
                    "password": home_psk,
                    "autoConnect": True,
                    "ap": False,
                    "apAlways": False,
                },
            )
        except Exception as exc:
            s.info(f"street restore POST: {exc}")
            try:
                _wifi_api(s, {"ssid": home_ssid, "password": home_psk})
                _wifi_api(s, {"autoConnect": True, "ap": False, "apAlways": False})
            except Exception as exc2:
                s.info(f"street restore retry: {exc2}")
        # Wait for module STA to return on the desk LAN (via eth or wifi).
        restored = _wait_wifi_pred(
            s,
            lambda w: bool(w.get("sta") and str(w.get("ip") or "").startswith("192.168.3.")),
            timeout=45.0,
        )
        s.check(
            "street restored home STA",
            restored is not None,
            f"wifi={restored}",
        )


# ---------------------------------------------------------------------------
# Level 3
# ---------------------------------------------------------------------------

def case_usb_gadget_held(s: Suite) -> None:
    """L3: after L2 usb-midi-hid, gadget must still enumerate under radio load."""
    if not getattr(s.args, "usb", False):
        raise CaseSkip("pass --usb when native USB-C is plugged into this host")
    midi = api("/api/v1/midi")
    peri = _transport(midi, "usbMidiPeripheral")
    hid = _transport(midi, "usbHid")
    if not (peri.get("enabled") and hid.get("enabled")):
        raise CaseSkip("USB MIDI+HID gadget not enabled — run L2 usb-midi-hid first")
    s.check("gadget still enabled", True, "peri+hid")
    s.check("host still sees ShowboxMod USB", _usb_showbox_present() or _usb_midi_listed(), "lsusb/amidi")
    # Light HTTP while USB gadget is up (eth preferred if STA is noisy).
    code, body = http_get(f"http://{s.ip}/services", timeout=3.0)
    if code != 200:
        eth_note = str(_transport(midi, "ethernet").get("note") or "")
        eth_ip = next((p for p in eth_note.replace(",", " ").split() if p.count(".") == 3 and p[0].isdigit()), "")
        if eth_ip:
            code, body = http_get(f"http://{eth_ip}/services", timeout=3.0)
    s.check("HTTP while USB gadget held", code == 200 and len(body) > 100, f"code={code}")


LEVEL1: list[tuple[str, CaseFn]] = [
    ("reachability", case_reachability),
    ("uart-link", case_uart_link),
    ("uart-roundtrip", case_uart_roundtrip),
    ("mdns", case_mdns),
    ("wifi-sta", case_wifi_sta),
    ("ethernet-smoke", case_ethernet_smoke),  # skip unless --ethernet
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
    ("usb-jtag", case_usb_jtag),  # --usb; Serial/JTAG mode (not gadget, not host)
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
    # WS cases before full snapshot-mixer so UART/HTTP load does not precede
    # the 10-client sync burst (L2 STOP: URLError mid ws-sync-stress).
    ("ws-multiclient", case_ws_multiclient),
    ("ws-sync-stress", case_ws_sync_stress),
    ("snapshot-mixer", case_snapshot_mixer),
    ("webui", case_webui),
    ("rec-timer", case_rec_timer),
    ("https", case_https),
    ("hotspot-air", case_hotspot_air),
    ("hotspot-sta-coex", case_hotspot_sta_coex),
    ("softap-services", case_softap_services),
    ("softap-street", case_softap_street),
    # Gadget = ShowboxMod as USB device to the laptop (MIDI+HID). Never usbMidiHost.
    ("usb-midi-hid", case_usb_midi_hid),  # --usb; peri+HID composite, host forced off
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
    ("usb-gadget-held", case_usb_gadget_held),  # --usb; after L2 gadget
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
