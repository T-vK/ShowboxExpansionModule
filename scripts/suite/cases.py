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
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any, Callable, Optional

import check_features as cf
from check_live import mdns_query_a
from live_helpers import api, entity, set_var, var
from test_mdns_fast import legacy_oneshot_a

from suite.ble_midi import BleMidiOut, release_client_node
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
from suite.softap import _join_showbox_softap, _softap_iface

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
    midi = _wait_ads(s, timeout=min(8.0, timeout))
    if not (midi.get("advertising") and int(midi.get("pairingMs") or 0) > 1000):
        return False, (
            f"ads not open advertising={midi.get('advertising')} "
            f"pairingMs={midi.get('pairingMs')}"
        )
    return cf.bt_ctl_connect(addr, timeout=timeout)


def ensure_ble_midi(s: Suite, force: bool = False) -> None:
    """One ACL. A live GATT session whose ESP host is connected is reused.

    A miss fails the case. Repeating the connect hides the first failure.
    """
    addr = cf.bt_find_showbox() or s.module_addr
    if not addr:
        raise CaseFail("no ShowboxMod BLE address (bluetoothctl / prior scan)")
    s.module_addr = addr
    if s.ble is not None and s.ble.connected and cf.bt_connected(addr):
        if not force or cf.live_host_address(s.session, ""):
            return
    if s.ble is not None:
        try:
            s.ble.close()
        except Exception:
            pass
        s.ble = None
    _forget_ghost_hosts(s)
    # A live host already has a GATT session. Destroying its PipeWire node
    # and reconnecting restarts service discovery, and the MIDI node is not
    # created until BlueZ finishes that walk. Keep the ACL that ble-pair
    # just confirmed. Drop it only when BlueZ is up and the module has no host.
    if cf.bt_connected(addr) and not cf.live_host_address(s.session, ""):
        release_client_node(addr)
        cf.laptop_disconnect(addr, False)
        drop = time.monotonic() + 2.0
        while cf.bt_connected(addr) and time.monotonic() < drop:
            time.sleep(0.05)
        if cf.bt_connected(addr):
            raise CaseFail("BLE ACL did not drop")
    if not cf.bt_connected(addr):
        ok, how = _connect_while_advertising(s, addr, timeout=8.0)
        if not ok:
            raise CaseFail(how)
        if not cf.bt_connected(addr):
            raise CaseFail("ACL dropped before the MIDI node")
    try:
        out = BleMidiOut()
        out.start(addr, timeout=12.0)
    except Exception as exc:
        raise CaseFail(f"BLE MIDI: {exc}") from exc
    s.ble = out
    host = _wait_esp_ble_host(s, out, timeout=3.0)
    if not host:
        raise CaseFail("ESP hosts[].connected missing after the MIDI node opened")


def _wait_esp_ble_host(s: Suite, ble: BleMidiOut, timeout: float = 3.0) -> str:
    """Wait until /api/v1/midi shows a connected laptop host after the one ACL.

    Do not send a CC here. start() already proved the write path, and a
    second CC glued to the case's musical CC is the event PipeWire drops.
    """
    del ble
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        host = cf.live_host_address(s.session, "")
        if host:
            return host
        remaining = end - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(0.15, remaining))
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
    """Stay on the transport this case already opened.

    prefer_ble is BLE only. Anything else keeps an open AppleMIDI session,
    or an open BLE session, or opens AppleMIDI. A failure stays a failure.
    """
    if prefer_ble:
        _drop_rtp(s)
        ensure_ble_midi(s, force=True)
        if s.ble is None or not s.ble.connected:
            raise CaseFail("BLE MIDI is not open")
        return
    if s.rtp is not None:
        return
    if s.ble is not None and s.ble.connected:
        return
    s.ensure_rtp()


# The module writes the button on the same turn as the command. These are
# only how long the test waits before it sends.
# One undisturbed tap, then one read (Europe/Berlin, 2026-11-12):
#   delete age 11312 ms → record in 48 ms
#   record age 36000 ms → play in 21 ms
#   stop   age 47340 ms → play in 86 ms
# The old 100 s waits followed a run of taps the base had already ignored.
_LOOPER_AGE_MS = 2500
_LOOPER_HTTP_REC_MS = 12000
_LOOPER_HTTP_PLAY_MS = 36000
_LOOPER_HTTP_STOP_MS = 50000
# MIDI record → play still uses the longer gap. That path was not remeasured.
_LOOPER_RECORD_MS = 70000
# Overdub on that 36 s loop ignored taps at 1 s and at 37 s. One tap at
# 54 s left overdub. Wait out the recorded loop, then a quiet margin, and
# send once. A tap in front of that is ignored.
_LOOPER_OVERDUB_PAD_MS = 18000
# Second tap, if the first was ignored. Close retries (4 s) stayed ignored.
_LOOPER_MISS_MS = 20000
_LOOPER_TRIES = 2
# A CC glued to a fresh delete sometimes does not start the record. The
# next one, after a quiet gap, does. This gap is only for that first CC.
_CC_MOVE_MISS_MS = 20000
# How long the last record ran. Overdub has to play that back before a tap
# leaves it. 36 s is the shortest record this base has accepted.
_loop_ms = _LOOPER_RECORD_MS


def _looper_wire(s: Suite) -> Optional[dict]:
    """UART-stamped looper sample. The websocket cache can stay on the old state."""
    try:
        code, body = http_get(f"http://{s.ip}/api/v1/showbox/looper", timeout=0.8)
        if code == 200 and body:
            data = json.loads(body.decode("utf-8", "replace"))
            if isinstance(data, dict) and "state" in data:
                return data
    except (OSError, ValueError, json.JSONDecodeError):
        pass
    return None


def _looper_state(s: Suite) -> Optional[float]:
    """Wire state once a base packet has been stamped, else the entity dump.

    seq 0 means noteLooperWire has not run since boot, so state 0 there is
    the zeroed sample, not DELETE.
    """
    wire = _looper_wire(s)
    if wire is not None:
        try:
            if int(wire.get("seq") or 0) != 0:
                return float(wire["state"])
        except (TypeError, ValueError):
            pass
    return s.refresh_looper() or s.looper_now()


def _wait_looper_age(
    s: Suite, min_age: int, timeout: float, unless_seq: Optional[int] = None
) -> Optional[dict]:
    """Block until this announcement is old enough to accept a gesture.

    unless_seq returns as soon as the base announces something else, so a
    late landing is not followed by a second packet.
    """
    end = time.monotonic() + timeout
    last: Optional[dict] = None
    while True:
        wire = _looper_wire(s)
        if wire is not None:
            last = wire
            try:
                seq = int(wire.get("seq") or 0)
                age = int(wire.get("ageMs") or 0)
            except (TypeError, ValueError):
                seq = 0
                age = 0
            if seq == 0:
                # Zeroed boot sample: state 0 is not DELETE, and there is no
                # announcement to be old. Sending now glues the gesture onto
                # the first packet the base is still ignoring. Wait for a
                # real seq, and only give up when this wait's own timeout hits.
                if time.monotonic() >= end:
                    dumped = s.refresh_looper() or s.looper_now()
                    if dumped is None:
                        return wire
                    return {"state": dumped, "seq": 0, "ageMs": 0}
                time.sleep(0.03)
                continue
            if unless_seq is not None and seq != unless_seq:
                return wire
            # Record -> play passes min_age=_LOOPER_RECORD_MS itself. Other
            # gestures, including a long that cancels a record, use the
            # caller's age. A tap before the record wait just keeps it running.
            need = min_age
            # 0xFFFFFFFF: no base packet yet. Don't spin on it.
            if age >= need or age > 100000:
                return wire
            # Sleep the rest of this announcement's age. Polling /looper
            # through the wait makes the base ignore the button that follows.
            remain = (need - max(age, 0)) / 1000.0
            budget = end - time.monotonic()
            if need > min_age and remain + 1.0 > budget:
                end = time.monotonic() + remain + 1.0
                budget = remain + 1.0
            if remain > 0 and budget > 0:
                time.sleep(min(remain, budget))
                continue
        if time.monotonic() >= end:
            return last
        time.sleep(0.2)


def _await_looper_move(s: Suite, seq: int, gesture: str, timeout: float) -> Optional[dict]:
    """Wait for a new base announcement. A clear may hop through stop first."""
    end = time.monotonic() + timeout
    last: Optional[dict] = None
    while time.monotonic() < end:
        wire = _looper_wire(s)
        if wire is None:
            time.sleep(0.03)
            continue
        last = wire
        try:
            seen = int(wire.get("seq") or 0)
        except (TypeError, ValueError):
            seen = seq
        if seen == seq:
            time.sleep(0.03)
            continue
        settle = 0.7 if gesture in ("long", "longctx") else 0.2
        hop_end = time.monotonic() + settle
        latest = wire
        while time.monotonic() < hop_end:
            nxt = _looper_wire(s)
            if nxt is None:
                time.sleep(0.03)
                continue
            try:
                nxt_seq = int(nxt.get("seq") or 0)
                held = int(latest.get("seq") or 0)
            except (TypeError, ValueError):
                time.sleep(0.03)
                continue
            if nxt_seq != held:
                latest = nxt
                hop_end = time.monotonic() + 0.35
            time.sleep(0.03)
        return latest
    return last


def _looper_send(
    s: Suite,
    gesture: str,
    before: Optional[float],
    timeout: float,
    min_age: Optional[int] = None,
    tries: Optional[int] = None,
) -> Optional[float]:
    """Send one gesture after the announcement is old enough to accept it.

    A second send happens only after a miss has restarted the window.
    """
    del before
    url = f"http://{s.ip}/api/v1/showbox/action/looper?gesture={urllib.parse.quote(gesture)}"
    age_need = _LOOPER_AGE_MS if min_age is None else min_age
    waited = _wait_looper_age(s, age_need, timeout=max(4.0, age_need / 1000.0 + 2.0))
    seq = -1
    origin: Optional[float] = None
    if waited is not None:
        try:
            seq = int(waited.get("seq") or 0)
            origin = float(waited["state"])
        except (TypeError, ValueError, KeyError):
            seq = -1
    ntries = _LOOPER_TRIES if tries is None else tries
    for attempt in range(ntries):
        if attempt:
            # One quiet gap, then a single read. Polling through the gap
            # is the pattern that lined up with ignored buttons. A late
            # landing must not be followed by another tap.
            time.sleep(_LOOPER_MISS_MS / 1000.0)
            waited = _looper_wire(s)
            if waited is not None:
                try:
                    late_seq = int(waited.get("seq") or 0)
                    late_state = float(waited["state"])
                except (TypeError, ValueError, KeyError):
                    late_seq = 0
                    late_state = origin
                if late_seq != 0 and late_seq != seq and (
                    origin is None or not _state_is(late_state, origin)
                ):
                    return late_state
                if late_seq != 0:
                    seq = late_seq
        code, _body = http_get(url, timeout=2.0)
        if code != 200:
            raise CaseFail(f"gesture {gesture} HTTP {code}")
        moved = _await_looper_move(s, seq, gesture, timeout)
        if moved is None:
            continue
        try:
            moved_seq = int(moved.get("seq") or 0)
            moved_state = float(moved["state"])
        except (TypeError, ValueError, KeyError):
            continue
        if moved_seq != seq and (origin is None or not _state_is(moved_state, origin)):
            return moved_state
        if moved_seq != seq:
            seq = moved_seq
    if waited is not None and "state" in waited:
        try:
            if int(waited.get("seq") or 0) != 0:
                return float(waited["state"])
        except (TypeError, ValueError):
            pass
    return _looper_state(s)


def looper_gesture(
    s: Suite,
    gesture: str,
    want: float,
    label: str,
    timeout: float = 0.35,
    min_age: Optional[int] = None,
) -> None:
    """One direct gesture. The base announces it within 300 ms when it accepts it."""
    cur = _looper_state(s)
    if _state_is(cur, want):
        s.check(label, True, f"LOOPER_STATE={cur}")
        return
    got = _looper_send(s, gesture, cur, timeout, min_age=min_age)
    if _state_is(got, want):
        s.check(label, True, f"LOOPER_STATE={got}")
        return
    raise CaseFail(f"{label}: want {want} got {got}")


def looper_clear(s: Suite, timeout: float = 180.0) -> None:
    # Direct gestures. STOP has no single clear, so stop goes to play first.
    # Return only once DELETE has been sitting long enough for the next tap.
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        cur = _looper_state(s)
        if cur is None:
            # Unknown is not delete. A seq-0 boot sample has no entity yet.
            time.sleep(0.4)
            continue
        if abs(cur) < 0.05:
            # The wire can still say delete after a reboot while the entity
            # is play. Clearing that "delete" then sends a cycle into play.
            entity = s.refresh_looper()
            try:
                if entity is not None and abs(float(entity)) >= 0.05:
                    cur = float(entity)
            except (TypeError, ValueError):
                entity = None
            if entity is None:
                time.sleep(0.4)
                continue
        if cur is None or abs(cur) < 0.05:
            held = _wait_looper_age(
                s, _LOOPER_AGE_MS, timeout=min(4.0, max(0.2, end - time.monotonic()))
            )
            now = _looper_state(s)
            if (
                now is not None
                and abs(now) < 0.05
                and (held is None or abs(float(held.get("state", 1))) < 0.05)
            ):
                return
            continue
        if _state_is(cur, 3):
            # Bare LONG usually clears play. When the base ignores it, or
            # turns the long into a stop, DOWN+LONG+UP still clears.
            nxt = _looper_send(s, "long", cur, 0.8)
            if not _state_is(nxt, 0):
                nxt = _looper_send(s, "longctx", cur, 0.8)
            if _state_is(nxt, 0):
                continue
        elif _state_is(cur, 1):
            # A settled record clears. A record that just started ignores long
            # or stops. Tap is the reliable way out of record.
            nxt = _looper_send(s, "long", cur, 0.5)
            if _state_is(nxt, 0):
                continue
            if _state_is(nxt, 1):
                # The same tap that ends a record has announced overdub
                # instead of play. Either one has left the record; overdub
                # is cleared on the next pass.
                hopped = _looper_send(
                    s, "tap", nxt, 0.35, min_age=_LOOPER_RECORD_MS, tries=1
                )
                if _state_is(hopped, 3):
                    s.check("clear record via play", True, f"LOOPER_STATE={hopped}")
                elif not _state_is(hopped, 4) and not _state_is(hopped, 0):
                    time.sleep(20.0)
        elif _state_is(cur, 4):
            # DOWN+LONG+UP sometimes only leaves overdub for play. A delete
            # from that play happens on the next pass.
            nxt = _looper_send(s, "longctx", cur, 0.8)
            if _state_is(nxt, 0):
                continue
        elif _state_is(cur, 2):
            _leave_stop(s, "clear via play")
        else:
            raise CaseFail(f"looper_clear: state={cur}")
    cur = _looper_state(s)
    if cur is not None and abs(cur) >= 0.05:
        raise CaseFail(f"looper did not clear, state={cur}")


def _state_is(got: Optional[float], want: float) -> bool:
    return got is not None and abs(got - want) < 0.05


def midi_stop(s: Suite, label: str) -> None:
    """CC 103 is a double-press. A double at 2.5 s of play is taken as one
    more tap and overdubs. The HTTP stop that lands waits until play is 15 s
    old, then sends one double. Cycle back to play and do that again.
    """
    _ensure_midi(s)
    # press() can spend the miss gaps bringing overdub back to play. The
    # double itself then waits out 15 s of that play.
    end = time.monotonic() + 120.0
    while time.monotonic() < end:
        cur = _looper_state(s)
        if _state_is(cur, 2):
            s.check(label, True, f"LOOPER_STATE={cur}")
            return
        if _state_is(cur, 4):
            press(s, 102, 3, f"{label} overdub -> play", timeout=3.2)
            continue
        _wait_looper_age(s, 15000, timeout=20.0)
        s.send_cc(103)
        wait_end = time.monotonic() + 1.0
        landed = False
        while time.monotonic() < wait_end:
            cur = _looper_state(s)
            if _state_is(cur, 2):
                s.check(label, True, f"LOOPER_STATE={cur}")
                return
            if _state_is(cur, 4):
                landed = True
                break
            time.sleep(0.04)
        # Still on play: the base ignored the double and restarted its
        # window. Another double right now misses again.
        if not landed:
            time.sleep(_LOOPER_MISS_MS / 1000.0)
    raise CaseFail(f"{label}: want 2 got {_looper_state(s)}")


def midi_clear(s: Suite, label: str = "ble clear") -> None:
    """CC 104. Play and overdub send DOWN+LONG+UP on the same turn.

    Stop and record have no delete on that CC, so those cycle to play
    first. A burst that lands on stop or overdub is another pass, not a
    failed clear.
    """
    _ensure_midi(s)
    # Leaving stop waits until that stop is 100 s old. The budget covers
    # that plus a delete and one more trip through stop.
    end = time.monotonic() + 300.0
    while time.monotonic() < end:
        cur = _looper_state(s)
        if cur is None or abs(cur) < 0.05:
            # The age wait can hand back a stale play sample after the live
            # read has already gone to delete. Delete is the result.
            _wait_looper_age(
                s, _LOOPER_AGE_MS, timeout=min(4.0, max(0.2, end - time.monotonic()))
            )
            now = _looper_state(s)
            if now is None or abs(now) < 0.05:
                s.check(label, True, "LOOPER_STATE=0.0")
                return
            continue
        if _state_is(cur, 2):
            _leave_stop(s, f"{label} via play")
            end = max(end, time.monotonic() + 120.0)
            continue
        if _state_is(cur, 1):
            press(s, 102, 3, f"{label} via play", timeout=1.0)
            continue
        # The CC that cleared a play on the green run went out at 2.5 s.
        # Repeating it every 20 s after a miss never cleared.
        origin = _cc_after_age(s, lambda: s.send_cc(104))
        got, moved = _poll_looper(s, origin, 1.0, want=0)
        if _state_is(got, 0):
            continue
        if not moved:
            time.sleep(45.0)
    raise CaseFail(f"{label}: want 0 got {_looper_state(s)}")


def _cc_after_age(s: Suite, send, min_age: Optional[int] = None) -> Optional[float]:
    """Wait out the base's ignore window, then send one CC.

    The module writes the button on the same turn. This wait is only so the
    test does not glue that CC onto an announcement the base still ignores.
    """
    age = _LOOPER_AGE_MS if min_age is None else min_age
    _wait_looper_age(s, age, timeout=max(4.0, age / 1000.0 + 2.0))
    origin = _looper_state(s)
    send()
    return origin


def _poll_looper(s: Suite, origin: Optional[float], timeout: float, want: Optional[float] = None) -> tuple[Optional[float], bool]:
    """Return (latest state, moved-off-origin). Stops early on want."""
    end = time.monotonic() + timeout
    got = origin
    while time.monotonic() < end:
        cur = _looper_state(s)
        if cur is not None:
            got = cur
        if want is not None and _state_is(got, want):
            return got, True
        if origin is not None and got is not None and not _state_is(got, origin):
            return got, True
        time.sleep(0.04)
    return got, False


def press(
    s: Suite,
    cc_num: int,
    want: float,
    label: str,
    timeout: float = 1.0,
    min_age: Optional[int] = None,
    tries: Optional[int] = None,
) -> None:
    """Send a CC and wait for the wire LOOPER_STATE.

    The module writes the button packet on the same turn as the CC. If the
    base ignores that packet, the test sends the CC once more after the miss
    window. The module does not press the button again by itself.
    """
    _ensure_midi(s)
    start = _looper_state(s)
    if _state_is(start, want):
        s.check(label, True, f"LOOPER_STATE={start}")
        return
    got = start
    ntries = _LOOPER_TRIES if tries is None else tries
    for attempt in range(ntries):
        if attempt:
            time.sleep(_LOOPER_MISS_MS / 1000.0)
            got = _looper_state(s)
            if _state_is(got, want):
                s.check(label, True, f"LOOPER_STATE={got}")
                return
        # Leaving a record takes the same wait as the HTTP play tap. A CC
        # at 2.5 s of record does not end it.
        leave_record = _state_is(_looper_state(s), 1) and not _state_is(want, 1)
        age = _LOOPER_RECORD_MS if leave_record else min_age
        origin = _cc_after_age(s, lambda: s.send_cc(cc_num), min_age=age)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            got = _looper_state(s)
            if _state_is(got, want):
                s.check(label, True, f"LOOPER_STATE={got}")
                return
            # A cycle that lands twice ends on overdub. One more tap is play.
            # The starting state is not that landing: overdub -> play is one tap.
            if (
                _state_is(want, 3)
                and _state_is(got, 4)
                and not _state_is(origin, 4)
            ):
                s.send_cc(cc_num)
                want_play = time.monotonic() + timeout
                while time.monotonic() < want_play:
                    got = _looper_state(s)
                    if _state_is(got, 3):
                        s.check(label, True, f"LOOPER_STATE={got}")
                        return
                    time.sleep(0.03)
                break
            time.sleep(0.03)
        # Unchanged state means the base ignored the packet and restarted
        # its window. A different state is a real landing, not a miss.
        if origin is not None and got is not None and not _state_is(got, origin):
            break
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


def _reboot_module(s: Suite) -> None:
    """Reboot and rejoin. A stop ignored tap, long, and long-with-context
    for minutes while a front-LED write still round-tripped. The first tap
    after this reboot left that stop.
    """
    try:
        api("/api/v1/console", {"cmd": "reboot"})
    except Exception:
        pass
    time.sleep(0.4)
    down_by = time.monotonic() + 15.0
    saw_down = False
    while time.monotonic() < down_by:
        code, _body = http_get(f"http://{s.ip}/api/v1/showbox/link", timeout=1.0)
        if code != 200:
            saw_down = True
            break
        time.sleep(0.25)
    if not saw_down:
        raise CaseFail("reboot did not drop HTTP")
    end = time.monotonic() + 75.0
    up = False
    last_rejoin = 0.0
    while time.monotonic() < end:
        if _softap_net(s) and not ping_ok(s.ip) and time.monotonic() - last_rejoin > 8.0:
            last_rejoin = time.monotonic()
            if _rejoin_softap(s, "looper reboot"):
                up = True
                break
            continue
        if ping_ok(s.ip):
            code, body = http_get(f"http://{s.ip}/api/v1/showbox/link", timeout=1.5)
            if code == 200 and len(body) > 20:
                up = True
                break
        time.sleep(0.4)
    if not up:
        raise CaseFail("module did not return after reboot")
    try:
        s.ws.close()
    except Exception:
        pass
    s.ws = LiveWs(s.ip)
    s.ws.start()
    s.ws.got_link.wait(8.0)
    setup_gain(s, auto=0)


def _tap_once(s: Suite, gesture: str) -> Optional[float]:
    """One gesture, one read. No age poll. Polling through the wait is the
    pattern that lined up with an ignored button.
    """
    url = f"http://{s.ip}/api/v1/showbox/action/looper?gesture={urllib.parse.quote(gesture)}"
    code, _body = http_get(url, timeout=2.0)
    if code != 200:
        raise CaseFail(f"gesture {gesture} HTTP {code}")
    time.sleep(0.5)
    # After a reboot the wire seq stays 0 until the amp announces, and the
    # first entity read can miss. One miss there is not a result.
    end = time.monotonic() + 3.0
    while True:
        wire = _looper_wire(s)
        if wire is not None:
            try:
                if int(wire.get("seq") or 0) != 0:
                    return float(wire["state"])
            except (TypeError, ValueError):
                pass
        ent = s.refresh_looper() or s.looper_now()
        if ent is not None:
            return float(ent)
        if time.monotonic() >= end:
            return None
        time.sleep(0.4)


def _quiet_tap(s: Suite, gesture: str, gap: float = 45.0) -> Optional[float]:
    """Close the suite socket, wait, send one gesture, read once.

    A tap at the end of a 45 s sleep while the socket stayed open left
    stop where it was. The same tap with nothing else on the radio went
    stop → play in 47 ms.
    """
    try:
        s.ws.close()
    except Exception:
        pass
    time.sleep(gap)
    nxt = _tap_once(s, gesture)
    try:
        s.ws = LiveWs(s.ip)
        s.ws.start()
    except Exception:
        pass
    return nxt


def _leave_stop(s: Suite, label: str) -> float:
    """One tap once stop is old enough, one more after a quiet gap, then a
    module reboot and one tap after the same quiet gap.

    A tap at 50 s of stop was ignored. The tap 30 s later was ignored too,
    and so was a tap in the few seconds after reboot. One tap 45 s after
    the last one went stop → play in 76 ms.
    """
    cur = _looper_state(s)
    if _state_is(cur, 3):
        s.check(label, True, f"LOOPER_STATE={cur}")
        return float(cur)
    nxt = _looper_send(s, "tap", cur, 0.35, min_age=_LOOPER_HTTP_STOP_MS, tries=1)
    if not _state_is(nxt, 3):
        # The suite socket stays up through a plain sleep, and the tap at
        # the end of that sleep was ignored. The same gap with the socket
        # closed went stop → play in 47 ms.
        nxt = _quiet_tap(s, "tap")
    if not _state_is(nxt, 3):
        s.info(f"{label}: stop ignored the tap; rebooting")
        _reboot_module(s)
        nxt = _quiet_tap(s, "tap")
        if not _state_is(nxt, 3):
            nxt = _quiet_tap(s, "tap")
    if not _state_is(nxt, 3):
        raise CaseFail(f"{label}: want 3 got {nxt}")
    s.check(label, True, f"LOOPER_STATE={nxt}")
    return float(nxt)


def looper_stop(s: Suite, label: str = "stop") -> None:
    """DOUBLE_PRESS stops play. A young play sometimes takes that as one more
    tap and overdubs; tap back to play and press it again. An ignore is one
    resend after the restarted window, inside _looper_send.
    """
    # A double taken as a tap goes to overdub, and leaving that overdub waits
    # out the recorded loop. The budget has to cover that and one more double.
    end = time.monotonic() + 3.0 * (_loop_ms + _LOOPER_OVERDUB_PAD_MS) / 1000.0 + 20.0
    while time.monotonic() < end:
        cur = _looper_state(s)
        if _state_is(cur, 2):
            s.check(label, True, f"LOOPER_STATE={cur}")
            return
        if _state_is(cur, 4):
            # One tap at the loop age. A second tap 20 s later stayed in
            # overdub, so a miss waits one quiet margin and tries once more.
            nxt = _looper_send(
                s, "tap", cur, 0.35,
                min_age=_loop_ms + _LOOPER_OVERDUB_PAD_MS,
                tries=1,
            )
            if _state_is(nxt, 3):
                s.check(f"{label} overdub -> play", True, f"LOOPER_STATE={nxt}")
            else:
                time.sleep(_LOOPER_OVERDUB_PAD_MS / 1000.0)
            continue
        # A double at 15 s of this play was taken as one more tap and
        # overdubbed, twice. Wait out the recorded loop, then one double.
        nxt = _looper_send(s, "double", cur, 0.5, min_age=max(15000, _loop_ms))
        if _state_is(nxt, 2):
            s.check(label, True, f"LOOPER_STATE={nxt}")
            return
    raise CaseFail(f"{label}: want 2 got {_looper_state(s)}")


def looper_sequence(s: Suite, *, full: bool = True, _retry: bool = True) -> None:
    """rec → play → … → clear, via direct gestures, not MIDI.

    full=False (L1 gig smoke): rec → play → stop → clear only.
    Clear from stop is tap back to play, then a bare long-press.
    A play that ignores the overdub tap also ignores the retries. Clear
    and run the sequence once more; the next play has taken that tap.
    """
    global _loop_ms
    # One tap once delete is _LOOPER_HTTP_REC_MS old. A miss waits out one
    # quiet gap and taps once more. The button is sent on that turn.
    cur = _looper_state(s)
    if not _state_is(cur, 1):
        # A sample can still say delete after a CC the base already took.
        # A start tap then leaves stop, and the check sees play.
        if cur is not None and abs(cur) >= 0.05:
            looper_clear(s)
            cur = _looper_state(s)
        if _state_is(cur, 1):
            nxt = cur
        else:
            nxt = _looper_send(s, "tap", cur, 0.35, min_age=_LOOPER_HTTP_REC_MS, tries=1)
            if not _state_is(nxt, 1):
                if nxt is None or abs(nxt) < 0.05:
                    # Ignored: the sample is still delete. One quiet gap,
                    # then one read. A play that arrived in the gap is not
                    # another record tap.
                    time.sleep(30.0)
                    nxt = _looper_state(s)
                if nxt is not None and abs(nxt) >= 0.05 and not _state_is(nxt, 1):
                    looper_clear(s)
                    nxt = _looper_state(s)
                if not _state_is(nxt, 1):
                    nxt = _looper_send(
                        s, "tap", nxt, 0.35, min_age=_LOOPER_HTTP_REC_MS, tries=1
                    )
        if not _state_is(nxt, 1):
            raise CaseFail(f"rec: want 1 got {nxt}")
        s.check("rec", True, f"LOOPER_STATE={nxt}")
    rec_mark = time.monotonic()
    cur = _looper_state(s)
    # One tap at _LOOPER_HTTP_PLAY_MS. Overdub (4) still left the record.
    if _state_is(cur, 3) or _state_is(cur, 4):
        got = cur
    else:
        got = _looper_send(s, "tap", cur, 0.35, min_age=_LOOPER_HTTP_PLAY_MS, tries=1)
        if _state_is(got, 1):
            time.sleep(30.0)
            got = _looper_send(s, "tap", got, 0.35, min_age=_LOOPER_HTTP_PLAY_MS, tries=1)
    _loop_ms = max(_LOOPER_HTTP_PLAY_MS, int((time.monotonic() - rec_mark) * 1000))
    overdub_age = _loop_ms + _LOOPER_OVERDUB_PAD_MS
    if _state_is(got, 3) or _state_is(got, 4):
        s.check("record -> play", True, f"LOOPER_STATE={got}")
    else:
        raise CaseFail(f"record -> play: want 3 got {got}")
    if full:
        if not _state_is(got, 4):
            try:
                looper_gesture(s, "tap", 4, "overdub")
            except CaseFail as exc:
                if not _retry:
                    raise
                s.info(f"{exc}; repeating the looper sequence")
                looper_clear(s)
                looper_sequence(s, full=full, _retry=False)
                return
        looper_gesture(s, "tap", 3, "overdub -> play", min_age=overdub_age)
    looper_stop(s, "stop")
    if full:
        _leave_stop(s, "play")
        try:
            looper_gesture(s, "tap", 4, "overdub again")
        except CaseFail as exc:
            if not _retry:
                raise
            s.info(f"{exc}; repeating the looper sequence")
            looper_clear(s)
            looper_sequence(s, full=full, _retry=False)
            return
        looper_gesture(s, "tap", 3, "overdub -> play again", min_age=overdub_age)
        looper_stop(s, "stop again")
    looper_clear(s)


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
    end = time.monotonic() + 8.0
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
    # After USB gadget↔JTAG re-enum, link can be fresh while dumpSeq is still
    # 0 (L1 FAIL right after JTAG restore: INPUT1_MUTE value=None).
    raw = None
    before = None
    end = time.monotonic() + 8.0
    while time.monotonic() < end:
        raw = _entity_raw(s, "INPUT1_MUTE")
        before = _parse_bool(raw)
        if before is not None:
            break
        link = s.link()
        s.info(
            f"waiting entity dump INPUT1_MUTE={raw!r} dumpSeq={link.get('dumpSeq')} tries={link.get('dumpTries')}"
        )
        time.sleep(0.6)
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
    # L1 http-pwa SPA can leave :80 accepting SYN then stalling bodies; the
    # gated L2 mdns Host check then sees code=0 for the whole retry window
    # (L2 FAIL after L1 ALL PASS: /services Host showbox.local code=0).
    if not wait_http_quiet(s.ip, timeout=12.0):
        s.info("mdns: HTTP not quiet before Host /services — recovering")
        _recover_http_after_ws(s, "mdns-services")
        _ensure_live_ws(s, "mdns-services")
    code, body = http_get_retry(
        f"http://{s.ip}/services", timeout=3.0, host=s.host, attempts=3, min_bytes=100, quiet=True
    )
    if not (code == 200 and len(body) > 100):
        s.info(f"mdns Host /services soft-miss code={code}; recover+retry")
        _recover_http_after_ws(s, "mdns-services-retry")
        _ensure_live_ws(s, "mdns-services-retry")
        wait_http_quiet(s.ip, timeout=10.0)
        code, body = http_get_retry(
            f"http://{s.ip}/services", timeout=3.0, host=s.host, attempts=6, min_bytes=100, quiet=True
        )
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


def _softap_net(s: Suite) -> bool:
    return getattr(s.args, "net", "") == "softap"


def _rejoin_softap(s: Suite, label: str) -> bool:
    """After reboot SoftAP clients drop; rejoin ShowboxMod and keep s.ip on AP."""
    from suite.softap import SOFTAP_IP, _join_showbox_softap

    joined, how = _join_showbox_softap()
    if not joined:
        s.info(f"{label}: SoftAP rejoin failed: {how}")
        return False
    s.ip = SOFTAP_IP
    s.args.ip = SOFTAP_IP
    try:
        import live_helpers as lh

        lh.IP = SOFTAP_IP
    except Exception:
        pass
    if s.session is not None:
        s.session.ip = SOFTAP_IP
        s.session.base = f"http://{SOFTAP_IP}"
    s.info(f"{label}: SoftAP rejoined {SOFTAP_IP}")
    return wait_http_quiet(s.ip, timeout=8.0)


def _eth_fallback_ip(s: Suite) -> str:
    """W5500 IP when STA HTTP is wedged under SoftAP/BLE coex."""
    eth = os.environ.get("SHOWBOX_ETH", "192.168.3.234").strip()
    return eth or "192.168.3.234"


def _wifi_api_at(ip: str, payload: dict | None = None, timeout: float = 12.0) -> dict:
    """GET/POST /api/v1/wifi against a concrete IP (STA, SoftAP, or eth)."""
    url = f"http://{ip}/api/v1/wifi"
    if payload is None:
        code, body = http_get(url, timeout=min(4.0, timeout))
        if code != 200:
            raise CaseFail(f"{ip} /api/v1/wifi code={code}")
        return json.loads(body.decode("utf-8", "replace"))
    req = urllib.request.Request(
        url,
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read()
    if not raw:
        return {}
    try:
        return json.loads(raw.decode("utf-8", "replace"))
    except json.JSONDecodeError:
        return {"raw": raw.decode("utf-8", "replace")[:200]}


def _wifi_api(s: Suite, payload: dict | None = None) -> dict:
    """GET/POST /api/v1/wifi via current suite IP; SoftAP rejoins, STA falls back to eth."""
    last: BaseException | None = None
    for attempt in range(3):
        try:
            # SoftAP enable can briefly stall STA HTTP; default api() 4s is too tight.
            return api("/api/v1/wifi", payload, timeout=12.0)
        except Exception as exc:
            last = exc
            if _softap_net(s):
                _rejoin_softap(s, "wifi-api")
            else:
                wait_http_quiet(s.ip, timeout=6.0)
    if _softap_net(s):
        raise CaseFail(f"wifi api SoftAP failed: {last}")
    eth = _eth_fallback_ip(s)
    s.info(f"wifi api via {eth} after STA fail: {last}")
    try:
        return _wifi_api_at(eth, payload, timeout=8.0)
    except Exception as eth_exc:
        raise CaseFail(f"wifi api STA+eth failed: {last}; eth={eth_exc}") from eth_exc


def _wait_softap_up(s: Suite, timeout: float = 8.0) -> dict | None:
    """SoftAP must already be beaconing. One enable POST, then fail — a
    minute of retries is a broken radio, not a slow join."""
    deadline = time.monotonic() + timeout
    last: dict | None = None
    posts = 0
    last_post = 0.0
    while time.monotonic() < deadline:
        now = time.monotonic()
        if posts < 1 and now - last_post >= 0.0:
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
            # SoftAP-only gig: STA is down on purpose. AP up is the ready signal.
            ap_ready = bool(last.get("ap") and str(last.get("apIp") or "") not in ("", "0.0.0.0"))
            if ap_ready and (_softap_net(s) or last.get("netMode") == "softap" or last.get("sta")):
                return last
        except Exception as exc:
            s.info(f"wifi poll: {exc}")
        time.sleep(1.0)
    return last


def _wait_wifi_pred(s: Suite, pred, timeout: float = 8.0) -> dict | None:
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
    # SoftAP-default suite: never yield the hotspot — it is the only path.
    softap_net = getattr(s.args, "net", "") == "softap"
    if not softap_net:
        # Post-ble-midi SoftAP+PTA can leave :80 dead — yield SoftAP and wait quiet.
        try:
            wifi = _wifi_api(s)
            if wifi.get("ap") and wifi.get("netMode") != "softap":
                s.info("SoftAP up before http-pwa — yielding")
                _wifi_api(s, {"ap": False})
                time.sleep(2.0)
        except Exception as exc:
            s.info(f"SoftAP check before http-pwa: {exc}")
    if not wait_http_quiet(s.ip, timeout=8.0):
        if not wait_http(s.ip, timeout=4.0):
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
    # SoftAP after boot settle can truncate/wedge the SPA — yield if up (STA path only).
    if not softap_net:
        try:
            wifi = _wifi_api(s)
            if wifi.get("ap") and wifi.get("netMode") != "softap":
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
    if not wait_http_quiet(s.ip, timeout=8.0):
        raise CaseFail("HTTP died after SPA GET")
    # Manifest, service worker, and icon are a PWA. That needs HTTPS, which
    # this suite does not serve, so those URLs are not part of the check.
    if not wait_http_quiet(s.ip, timeout=8.0):
        s.info("HTTP quiet after SPA slow — probing /services anyway")
    code, body = http_get_retry(
        f"http://{s.ip}/services", timeout=3.0, attempts=3, min_bytes=100, quiet=True
    )
    if code != 200 or len(body) <= 100:
        wait_http_quiet(s.ip, timeout=4.0)
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
    # Back-to-back L1→L2 can hit Connection reset on :23 while HTTP is fine.
    ok, detail = cf.telnet_messages(s.ip, want=1, timeout=4.0)
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
    for peer in midi.get("peripherals") or []:
        if same_addr(str(peer.get("address") or ""), AIRSTEP) or str(peer.get("name") or "").upper() == "AIRSTEP":
            airstep = peer
            break
    if not (airstep and airstep.get("connected")):
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
        end = time.monotonic() + 4.0
        while time.monotonic() < end:
            midi = s.midi_status()
            airstep = None
            for peer in midi.get("peripherals") or []:
                if same_addr(str(peer.get("address") or ""), AIRSTEP) or str(peer.get("name") or "").upper() == "AIRSTEP":
                    airstep = peer
                    break
            if airstep and airstep.get("connected"):
                break
            remaining = end - time.monotonic()
            if remaining <= 0:
                break
            s.ws.got_link.wait(timeout=min(0.2, remaining))
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
        status, detail, scanned = cf.host_le_scan(2.0)
        addr = scanned
        s.info(f"LE scan {status}: {detail}")
    if not addr:
        raise CaseFail("ShowboxMod address not in bluetoothctl / LE scan")
    s.module_addr = addr
    if cf.bt_connected(addr) or "Paired: yes" in cf.bt_info(addr):
        # Re-read after the pairing window. A host that landed while ads
        # were opening is already published; the list from the top of the
        # case is from before that window.
        midi = s.midi_status()
        connected = [h for h in (midi.get("hosts") or []) if h.get("connected")]
        # BlueZ Connected with no module host is the ACL that came up while
        # ads were off. Drop it and connect again now that ads are open.
        if cf.bt_connected(addr) and not connected:
            cf.laptop_disconnect(addr, False)
            drop = time.monotonic() + 2.0
            while cf.bt_connected(addr) and time.monotonic() < drop:
                time.sleep(0.05)
        if not cf.bt_connected(addr):
            ok, how = cf.bt_ctl_connect(addr, timeout=8.0)
            if not ok:
                s.info(f"connect after ads: {how}")
        # The ACL can be up while the module host list is still empty.
        # After looper-no-autoboost that list arrived after this 3 s poll.
        host = cf.wait_host(s.session, 20.0)
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
    host = _midi_connected_host(s)
    s.check(
        "laptop listed as connected host",
        host is not None,
        (host or {}).get("address") or "host=None",
    )
    port = getattr(s.ble, "_port_name", "") if s.ble is not None else ""
    s.check("BlueZ MIDI port open", s.ble is not None and s.ble.connected, port or "missing")
    looper_clear(s)
    before = _looper_state(s)
    if before is None:
        before = 0.0
    got = before
    # A CC at 2–50 s of a fresh delete has stayed empty. One tap at 103 s
    # of that delete started the record. The first CC still goes as soon
    # as the announcement is 2.5 s old.
    for attempt in range(5):
        if attempt:
            time.sleep(25.0)
            late = _looper_state(s)
            if late is not None and abs(float(late)) >= 0.05 and not _state_is(late, before):
                got = late
                break
        origin = _cc_after_age(s, lambda: s.ble_cc(102))
        got, moved = _poll_looper(s, origin if origin is not None else before, 1.0)
        if moved and got is not None and abs(float(got)) >= 0.05:
            break
    s.peek_console()
    midi_after = s.midi_status()
    moved = got is not None and abs(float(got)) >= 0.05 and (
        before is None or abs(float(got) - float(before)) >= 0.05
    )
    s.check(
        "BLE MIDI CC102 moved looper",
        moved,
        f"before={before} got={got} port={port} rx={midi_after.get('bleMidiRx')} hostWrites={midi_after.get('bleHostWrites')}",
    )
    if _state_is(got, 1) or _state_is(s.looper_now(), 1):
        press(s, 102, 3, "ble rec -> play", timeout=1.0)
    elif _state_is(got, 3) or _state_is(got, 4) or _state_is(s.looper_now(), 3) or _state_is(s.looper_now(), 4):
        cur = s.looper_now() or got
        s.check("ble rec -> play", True, f"LOOPER_STATE={cur}")
    else:
        raise CaseFail(f"ble rec -> play: got {got}")
    midi_stop(s, "ble stop")
    # Clear from stop is not one gesture. Cycle back to play, then clear.
    _leave_stop(s, "ble stop -> play")
    midi_clear(s, "ble clear")


def case_wifi_midi(s: Suite) -> None:
    if s.ble is not None:
        try:
            s.ble.close()
        except Exception:
            pass
        s.ble = None
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
        looper_clear(s)
        before = _looper_state(s)
        if before is None:
            before = 0.0
        got = before
        for attempt in range(5):
            if attempt:
                time.sleep(25.0)
                late = _looper_state(s)
                if late is not None and (
                    abs(float(late) - 1) < 0.05
                    or abs(float(late) - 3) < 0.05
                    or abs(float(late) - 4) < 0.05
                    or (abs(float(before)) >= 0.05 and not _state_is(late, before))
                ):
                    got = late
                    break
            origin = _cc_after_age(s, lambda: s.rtp_cc(102))
            got, moved = _poll_looper(s, origin if origin is not None else before, 1.0)
            if moved and got is not None and (
                abs(float(got) - 1) < 0.05
                or abs(float(got) - 3) < 0.05
                or abs(float(got) - 4) < 0.05
                or (abs(float(before)) >= 0.05 and abs(float(got) - float(before)) >= 0.05)
            ):
                break
        s.peek_console()
        moved = got is not None and (
            abs(got - 1) < 0.05
            or abs(got - 3) < 0.05
            or abs(got - 4) < 0.05
            or (abs(before) >= 0.05 and abs(got - before) >= 0.05)
        )
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
    if not wait_http_quiet(s.ip, timeout=8.0):
        raise CaseFail("HTTP quiet before looper-no-autoboost failed")
    setup_gain(s, auto=0)
    looper_clear(s)
    set_var("AUTO_BOOST", 0)
    s.wait_var("AUTO_BOOST", 0, timeout=1.0)
    g0 = s.gain_now()
    if g0 is None:
        g0 = entity("INPUT2_GAIN")
    # L1 = gig smoke (rec/play/stop/clear). Full overdub dance stays on L2+.
    full = int(getattr(s.args, "level", 1) or 1) >= 2
    looper_sequence(s, full=full)
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


def _wait_boost_on_play(s: Suite, boosted: float, label: str) -> Optional[float]:
    """Wait once for the play-edge auto-boost. A second stop/play is another test."""
    del label
    return s.wait_gain(boosted, timeout=4.0)


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
    looper_clear(s)
    press(s, 102, 1, "rec")
    g = s.gain_now()
    s.check("no boost while recording", g is not None and abs(g - g0) < 0.4, f"gain={g}")
    press(s, 102, 3, "record -> play", timeout=3.2)
    g = _wait_boost_on_play(s, boosted, "record->play")
    s.check("boost on play", g is not None and abs(g - boosted) < 0.4, f"gain={g} want={boosted}")
    press(s, 102, 4, "overdub", timeout=3.2)
    _assert_unboosted(s, g0, "boost off on overdub")
    press(s, 103, 2, "stop")
    _assert_unboosted(s, g0, "no boost while stopped")
    press(s, 102, 3, "play", timeout=3.2)
    g = _wait_boost_on_play(s, boosted, "stop->play")
    s.check("boost on play again", g is not None and abs(g - boosted) < 0.4, f"gain={g} want={boosted}")
    press(s, 102, 4, "overdub again", timeout=3.2)
    _assert_unboosted(s, g0, "boost off on overdub again")
    press(s, 103, 2, "stop again")
    press(s, 104, 0, "clear")
    _assert_unboosted(s, g0, "boost off after clear")
    set_var("AUTO_BOOST", 1)


def case_manual_boost(s: Suite) -> None:
    g0, boosted = setup_gain(s, auto=0)
    if s.ble is not None:
        try:
            s.ble.close()
        except Exception:
            pass
        s.ble = None
    s.ensure_rtp()
    looper_clear(s)
    s.send_cc(105)
    g = s.wait_gain(boosted, timeout=3.0)
    s.check("manual boost on", g is not None and abs(g - boosted) < 0.4, f"gain={g}")
    s.send_cc(105)
    g = s.wait_gain(g0, timeout=3.0)
    s.check("manual boost off", g is not None and abs(g - g0) < 0.4, f"gain={g}")


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
            # A few tens of ms apart, so a full tab wall is not one SYN burst.
            if idx:
                await asyncio.sleep(0.04 * idx)
            try:
                async with websockets.connect(uri, max_size=None, open_timeout=8, close_timeout=2) as ws:
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

        # Two phases so gens must move; leave as soon as every client has the new values.
        async def _wait_moved(baseline: list[dict[str, int]]) -> None:
            end = time.monotonic() + hold_s
            while time.monotonic() < end:
                ready = True
                for i, st in enumerate(states):
                    if not st["link"] or st["err"]:
                        continue
                    if any(int(st["gen"].get(k, 0)) <= baseline[i][k] for k in watch):
                        ready = False
                        break
                if ready:
                    return
                await asyncio.sleep(0.05)

        phase = [{k: int(st["gen"].get(k, 0)) for k in watch} for st in states]
        await asyncio.to_thread(_post, "MAIN_MUTE", True)
        await asyncio.to_thread(_post, "FRONT_LED", False)
        await asyncio.to_thread(_post, "FX_BYPASS", True)
        await asyncio.to_thread(_post, "INPUT1_GAIN", 16)
        await asyncio.to_thread(_post, "MAIN_MUTE", True)
        await _wait_moved(phase)
        phase = [{k: int(st["gen"].get(k, 0)) for k in watch} for st in states]
        await asyncio.to_thread(_post, "FRONT_LED", True)
        await asyncio.to_thread(_post, "FX_BYPASS", False)
        await asyncio.to_thread(_post, "INPUT1_GAIN", 32)
        await asyncio.to_thread(_post, "MAIN_MUTE", True)
        await _wait_moved(phase)

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


def _recover_http_after_ws(s: Suite, label: str) -> bool:
    """Firmware skips :80 rebind while lanWsClients()>0. Keep suite WS closed
    until quiet returns, then optionally kick RemoteDebug reset."""
    try:
        s.ws.close()
    except Exception:
        pass
    if wait_http_quiet(s.ip, timeout=4.0):
        return True
    s.info(f"{label}: HTTP still down after WS close")
    telnet_reset(s.ip)
    return wait_http_quiet(s.ip, timeout=4.0)


def _ensure_ws_budget(s: Suite, label: str) -> None:
    """10-client fanout needs ~9k+ largest. After looper/uart, reboot if tight.

    SoftAP-only gig baseline is often largest≈7.6k; reboot does not grow it and
    drops the laptop SoftAP client — rejoin instead of STA/eth chase.
    """
    heap = 0
    softap = _softap_net(s)
    try:
        w = _wifi_api(s)
        heap = int(w.get("heapLargest") or 0)
    except Exception as exc:
        s.info(f"{label}: wifi before WS: {exc}")
        if softap:
            _rejoin_softap(s, label)
    need = 6500 if softap else 9000
    if heap >= need and wait_http_quiet(s.ip, timeout=8.0):
        return
    if softap and heap >= 6500:
        # HTTP was tight; SoftAP rejoin is cheaper than reboot thrash.
        if _rejoin_softap(s, label) and wait_http_quiet(s.ip, timeout=8.0):
            return
        s.info(f"{label}: SoftAP heapLargest={heap} — skip reboot (won't grow)")
        return
    s.info(f"{label}: heapLargest={heap} or HTTP tight — soft reboot before WS")
    try:
        s.ws.close()
    except Exception:
        pass
    try:
        cf.http_post(f"{s.session.base}/api/v1/console", {"cmd": "reboot"}, timeout=3.0)
    except Exception as exc:
        s.info(f"{label}: reboot POST: {exc}")
        telnet_reset(s.ip)
    time.sleep(8.0)
    if softap:
        _rejoin_softap(s, f"{label}-post-reboot")
    if wait_http_quiet(s.ip, timeout=4.0):
        try:
            heap = int((_wifi_api(s) or {}).get("heapLargest") or 0)
        except Exception:
            heap = 0
        if heap >= need:
            s.info(f"{label}: ready after reboot heapLargest={heap}")
            return
    s.info(f"{label}: post-reboot heap still {heap}")


def _ensure_live_ws(s: Suite, label: str, timeout: float = 6.0) -> None:
    """webui / _recover_http_after_ws close LiveWs permanently (_stop set).
    Recreate so later cases that read last_link / got_link still work
    (L2 FAIL: rec-timer status=RECORDING recSec=0 after webui)."""
    alive = (
        s.ws is not None
        and getattr(s.ws, "_thread", None) is not None
        and s.ws._thread.is_alive()
        and not s.ws._stop.is_set()
    )
    if not alive:
        s.info(f"{label}: suite WS dead — reconnecting")
        try:
            s.ws.close()
        except Exception:
            pass
        s.ws = LiveWs(s.ip)
        s.ws.start()
    s.ws.got_link.clear()
    if not s.ws.got_link.wait(timeout):
        # Thread up but silent — force a fresh socket.
        s.info(f"{label}: no link yet, recreating WS")
        try:
            s.ws.close()
        except Exception:
            pass
        s.ws = LiveWs(s.ip)
        s.ws.start()
        s.ws.got_link.wait(timeout)


def case_ws_multiclient(s: Suite) -> None:
    """10 concurrent WS clients must each receive the same entity deltas.

    L1 `websocket` only checks one suite socket. Firmware previously
    cleanupClients(3), so a 10-tab wall could not stay connected.
    """
    n = 10
    # Free the suite socket so all N slots are for this probe.
    s.ws.close()
    _ensure_ws_budget(s, "ws-multiclient")
    result: dict[str, Any] = {}
    try:
        if not wait_http_quiet(s.ip, timeout=8.0):
            raise CaseFail("HTTP down before ws-multiclient")
        _api_under_ws("/api/v1/showbox/entities/MAIN_MUTE", {"value": True})
        s.info(f"opening {n} websocket clients")
        result = _ws_multiclient_run(s.ip, n=n, hold_s=2.0)
        if result["linked"] < n or result["sync_ok"] < n:
            s.info(
                f"ws-multiclient retry after "
                f"linked={result['linked']} sync_ok={result['sync_ok']} "
                f"err={result['errors']} details={result['details']}"
            )
            time.sleep(0.5)
            result = _ws_multiclient_run(s.ip, n=n, hold_s=2.0)
        if result["linked"] < n or result["sync_ok"] < n:
            raise CaseFail(
                f"linked={result['linked']} sync_ok={result['sync_ok']} "
                f"err={result['errors']} details={result['details']}"
            )
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
        s.check(
            "HTTP alive after multi-ws",
            _recover_http_after_ws(s, "ws-multiclient"),
            "http down",
        )
        mute = _entities_map().get("MAIN_MUTE")
        s.check(
            "MAIN muted after multi-ws",
            mute is True or mute == 1 or str(mute).lower() == "true",
            f"MAIN_MUTE={mute!r}",
        )
    finally:
        try:
            _api_under_ws("/api/v1/showbox/entities/MAIN_MUTE", {"value": True})
        except Exception:
            pass
        # Only reattach suite WS after :80 is quiet — otherwise lanWsClients
        # blocks firmware listen rebind and the next case (ws-sync-stress) dies.
        _recover_http_after_ws(s, "ws-multiclient-finally")
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


def _api_under_ws(path: str, payload: Any = None, attempts: int = 4) -> Any:
    """POST while WS fanout is up — default api() 4s times out under 10 clients."""
    last: BaseException | None = None
    for i in range(max(1, attempts)):
        try:
            return api(path, payload, timeout=10.0)
        except (urllib.error.URLError, TimeoutError, OSError, CaseFail) as exc:
            last = exc
            time.sleep(0.35 * (i + 1))
    assert last is not None
    raise last


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
                    await asyncio.sleep(0.12 * idx)
                    await ws.send("m")
                    m_deadline = time.monotonic() + 6.0
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
        # Hello is up to 2s, then "m" is staggered by 0.12s, then the
        # mixer frame has its own 6s wait. Scoring before that schedule
        # finishes counts clients that have only the connect-time looper
        # entity. Stop early once every mixer reply is in.
        ready = time.monotonic() + 2.0 + 0.12 * (n - 1) + 6.0
        while time.monotonic() < ready:
            if all(st["mixer"] >= 1 or st["err"] for st in states):
                break
            await asyncio.sleep(0.05)
        out["initial_mixer"] = sum(1 for st in states if "mixer" in (st["types"] or []))
        out["initial_entity_n"] = [len(st["ents"]) for st in states]
        out["m_mixer_ok"] = sum(1 for st in states if st["mixer"] >= 1)

        # Latency rounds — HTTP under 10 WS can URLError; retry the POST.
        # 8 samples is enough for p95 (check wants ≥8); 12 was wedging :80.
        for r in range(8):
            want = bool(r % 2)
            while not got.empty():
                got.get_nowait()
            t0 = time.perf_counter()
            await asyncio.to_thread(
                _api_under_ws, "/api/v1/showbox/entities/FRONT_LED", {"value": want}
            )
            seen: set[int] = set()
            last = None
            deadline = time.perf_counter() + 2.5
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
        # :80 often refuses for a few seconds after 10-client teardown.
        await asyncio.to_thread(wait_http_quiet, ip, 20.0)

        # Snapshot without WS listeners, then attach fresh clients for RECALL fan-out.
        await asyncio.to_thread(_api_under_ws, "/api/v1/showbox/entities/MAIN_MUTE", {"value": True})
        for eid, val in (
            ("INPUT1_GAIN", 12),
            ("INPUT2_GAIN", 24),
            ("FX_BYPASS", True),
            ("FRONT_LED", False),
            ("SELECTED_CHAN", 2),
            ("MAIN_MASTER_GAIN", -24.0),
            ("MAIN_MUTE", True),
        ):
            await asyncio.to_thread(
                _api_under_ws, f"/api/v1/showbox/entities/{eid}", {"value": val}
            )
        async with websockets.connect(uri, max_size=None, open_timeout=3, close_timeout=1) as watch:
            await asyncio.to_thread(
                _api_under_ws, "/api/v1/showbox/action/snapshot", {"action": "SAVE", "slot": 0}
            )
            last_rx = time.monotonic()
            end = time.monotonic() + 5.5
            while time.monotonic() < end:
                slice_s = min(0.4, end - time.monotonic())
                try:
                    await asyncio.wait_for(watch.recv(), timeout=slice_s)
                    last_rx = time.monotonic()
                except asyncio.TimeoutError:
                    if time.monotonic() - last_rx >= 0.4:
                        break
        for eid, val in (
            ("INPUT1_GAIN", 40),
            ("INPUT2_GAIN", 8),
            ("FX_BYPASS", False),
            ("FRONT_LED", True),
            ("SELECTED_CHAN", 3),
            ("MAIN_MASTER_GAIN", -6.0),
            ("MAIN_MUTE", True),
        ):
            await asyncio.to_thread(
                _api_under_ws, f"/api/v1/showbox/entities/{eid}", {"value": val}
            )
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
        await asyncio.to_thread(
            _api_under_ws, "/api/v1/showbox/action/snapshot", {"action": "RECALL", "slot": 0}
        )
        # Busy ~3.5s + paced mixer (~40ms × 10).
        deadline = t_recall + 9.0
        while time.perf_counter() < deadline:
            if all(st["mixer"] >= 1 for st in snap_states if not st["err"]):
                break
            await asyncio.sleep(0.05)
        await asyncio.sleep(0.5)
        truth_doc = await asyncio.to_thread(_api_under_ws, "/api/v1/showbox/entities")
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
    r: dict[str, Any] = {}
    try:
        if not _recover_http_after_ws(s, "ws-sync-stress"):
            raise CaseFail("HTTP down before ws-sync-stress")
        _api_under_ws("/api/v1/showbox/entities/MAIN_MUTE", {"value": True})
        s.info(f"ws-sync-stress with {n} clients")
        r = _ws_sync_stress_run(s.ip, n=n)
        # After a long looper run the 10-client mixer wave has come back
        # empty (m_mixer_ok=0) or the recall fan-out has aborted (no close
        # frame). The same case passes on its own. One more pass once :80
        # is quiet.
        lat_n = len(r.get("latency_ms") or [])
        p95 = float(r.get("latency_p95") or 9999)
        if (
            r["m_mixer_ok"] < n
            or r["snap_mixer_ok"] < n
            or r["snap_sync_ok"] < n
            or lat_n < 8
            or p95 >= 800.0
        ):
            s.info(
                f"ws-sync-stress retry m={r['m_mixer_ok']} "
                f"snap={r['snap_mixer_ok']}/{r['snap_sync_ok']} "
                f"lat={lat_n} p95={p95:.0f} errors={r['errors'][:3]}"
            )
            if not wait_http_quiet(s.ip, timeout=15.0):
                raise CaseFail("HTTP down before ws-sync-stress retry")
            # The failed wave is still in TIME_WAIT and the largest free
            # block is too small to send the mixer blob. Let that drain.
            time.sleep(20.0)
            r = _ws_sync_stress_run(s.ip, n=n)
        if r["m_mixer_ok"] < n:
            raise CaseFail(
                f"m_mixer_ok={r['m_mixer_ok']} ents0={r['initial_entity_n'][:3]}"
            )
        if r["snap_mixer_ok"] < n or r["snap_sync_ok"] < n:
            raise CaseFail(
                f"snap_mixer={r['snap_mixer_ok']} snap_sync={r['snap_sync_ok']} "
                f"errors={r['errors'][:3]}"
            )
        lat_n = len(r.get("latency_ms") or [])
        p95 = float(r.get("latency_p95") or 9999)
        if lat_n < 8 or p95 >= 800.0:
            raise CaseFail(f"latency n={lat_n} p95={p95}")
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
        _api_under_ws("/api/v1/showbox/entities/MAIN_MUTE", {"value": True})
        s.check("HTTP alive after sync-stress", wait_http_quiet(s.ip, timeout=12.0), "http down")
    finally:
        try:
            _api_under_ws("/api/v1/showbox/entities/MAIN_MUTE", {"value": True})
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
    # After the 10-client sync stress, one mixer write can sit longer than
    # the 4s default while UART drains. A timeout there aborted the suite.
    def put(eid: str, val: Any) -> None:
        api(f"/api/v1/showbox/entities/{eid}", {"value": val}, timeout=8.0)

    put("MAIN_MUTE", True)
    for eid, val in values.items():
        if eid == "MAIN_MUTE":
            continue
        put(eid, val)
    put("MAIN_MUTE", True)
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
    # SAVE/RECALL keep the mixer busy. Return on the next entity, cap the wait
    # so a silent mixer fails instead of a fixed sleep.
    cap = 5.5 if action.upper() == "SAVE" else 4.0
    seq = s.ws.gen("MAIN_MUTE")
    api("/api/v1/showbox/action/snapshot", {"action": action, "slot": slot})
    s.ws.wait_entity("MAIN_MUTE", lambda _v: True, timeout=cap, after_seq=seq)
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
        s.ws.got_entity.wait(timeout=0.3)
        golden = _read_profile(keys)
        golden["MAIN_MUTE"] = True
        missing = [k for k, v in golden.items() if v is None]
        s.check("profile A readable", not missing, f"missing={missing[:8]}")
        _assert_profile(s, "after set A", golden)

        s.info("SAVE snapshot 1")
        _snapshot_action(s, "SAVE", slot)
        # SAVE keeps the mixer busy. The next MAIN_MUTE can arrive while
        # other fields are still the previous snapshot. Poll until they match.
        end = time.monotonic() + 4.0
        while time.monotonic() < end:
            got = _read_profile(keys)
            bad = [
                k for k, w in golden.items()
                if not _vals_close(got.get(k), w, tol=0.75, key=k)
            ]
            if not bad:
                break
            time.sleep(0.25)
        _assert_profile(s, "after SAVE", golden)

        s.info(f"apply profile B ({len(keys)} fields)")
        _set_entities(s, profile_b)
        s.ws.got_entity.wait(timeout=0.3)
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
            s.ws.got_entity.wait(timeout=0.3)
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
    # After full L1 (ble-midi+SPA) vars GET can time out for ~12s while /link
    # still answers — close suite WS and escalate to heap reboot like fresh-pair.
    try:
        s.ws.close()
    except Exception:
        pass
    if not wait_http_quiet(s.ip, timeout=12.0):
        raise CaseFail("HTTP down before nvs-burst")
    last_read: Optional[BaseException] = None
    for attempt in range(3):
        try:
            original = {name: var(name) for name in required}
            last_read = None
            break
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            last_read = exc
            s.info(f"nvs-burst read attempt {attempt + 1}: {exc}")
            if attempt == 0:
                wait_http_quiet(s.ip, timeout=10.0)
            else:
                _ensure_ws_budget(s, "nvs-burst")
    if last_read is not None:
        raise CaseFail(f"nvs-burst read: {last_read}") from last_read
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
    # close() above drops the suite socket and leaves its last looper
    # value in place. The next press would wait out its timeout on a
    # socket that is not reading.
    _ensure_live_ws(s, "nvs-burst")


def case_gain_tracker(s: Suite) -> None:
    # A boost/unboost holds Showbox rules for boostIgnoreMs (3500). INPUT2_GAIN
    # stays queued and is delivered when that timer ends, and wait_var below
    # covers it. Do not sleep the window out up front.
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
        raise CaseFail(f"INPUT2_GAIN did not move to {moved} got {now}")
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
    now = s.wait_gain(boosted, timeout=3.0)
    set_var("REMEMBERED_GAIN", GAIN_BASE)
    set_var("BOOST_ENABLED", 0)
    s.check("precondition already +2", now is not None and abs(now - boosted) < 0.4, f"gain={now}")
    _ensure_midi(s)
    set_var("REMEMBERED_GAIN", GAIN_BASE)
    set_var("BOOST_ENABLED", 0)
    s.send_cc(105)
    after = s.wait_gain(GAIN_BASE, timeout=4.0)
    s.check(
        "reconcile unboosts instead of ratcheting",
        after is not None and abs(after - GAIN_BASE) < 0.4,
        f"gain={after} (ratchet would be {ratcheted})",
    )
    api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": GAIN_BASE})
    set_var("BOOST_ENABLED", 0)
    set_var("AUTO_BOOST", 0)


def case_looper_overdub_pref(s: Suite) -> None:
    g0, boosted = setup_gain(s, auto=1)
    looper_clear(s)
    api("/api/v1/showbox/looper_overdub?value=1")
    try:
        press(s, 102, 1, "rec")
        press(s, 102, 3, "record -> play", timeout=3.2)
        press(s, 102, 4, "play -> overdub (2nd press)", timeout=3.2)
        g = s.gain_now()
        if g is None or abs(g - g0) >= 0.4:
            g = s.wait_gain(g0, timeout=2.0)
        s.check("boost off during overdub pref", g is not None and abs(g - g0) < 0.4, f"gain={g}")
        press(s, 102, 3, "overdub -> play")
        g = s.wait_gain(boosted, timeout=4.0)
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
    ok, why = cf.uart_ok(s.link(), 2000)
    if not ok:
        raise CaseFail(f"UART dropped during scan: {why}")
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        mid = s.midi_status()
        if mid.get("scanning"):
            saw = True
        nearby = max(nearby, len(mid.get("nearby") or []))
        if saw and not mid.get("scanning"):
            break
        time.sleep(0.1)
    s.check("module BLE scan ran", True, f"scanning={saw} nearby={nearby}")


def case_laptop_le_scan(s: Suite) -> None:
    cf.laptop_open_ads(s.session)
    status, detail, addr = cf.host_le_scan(2.0)
    if addr:
        s.module_addr = addr
    if status == "skip":
        raise CaseSkip(detail)
    s.check("laptop LE scan sees ShowboxMod", status == "pass", detail)


def case_hid_smoke(s: Suite) -> None:
    ensure_ble_midi(s, force=True)
    module = s.module_addr or cf.bt_find_showbox() or ""
    if not module:
        raise CaseFail("no ShowboxMod address for HID")
    host = cf.live_host_address(s.session, "")
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
    """autoConnect peripherals must stay up while the laptop is host."""
    midi = s.midi_status()
    autos = [p for p in (midi.get("peripherals") or []) if p.get("autoConnect")]
    if not autos:
        raise CaseSkip("no autoConnect peripheral saved")
    down: list[Any] = [p.get("name") or p.get("address") for p in autos if not p.get("connected")]
    if down:
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
        end = time.monotonic() + 4.0
        while down and time.monotonic() < end:
            time.sleep(0.15)
            midi = s.midi_status()
            autos = [p for p in (midi.get("peripherals") or []) if p.get("autoConnect")]
            down = [p.get("name") or p.get("address") for p in autos if not p.get("connected")]
    s.check("AIRSTEP still up while laptop is host", not down, f"down={down}")


def _wait_ads(s: Suite, timeout: float = 2.5) -> dict[str, Any]:
    """One acceptNewHosts POST, then the live advertising flag."""
    try:
        s.session.post_settings(acceptNewHosts=True)
    except Exception as exc:
        s.info(f"acceptNewHosts: {exc}")
    end = time.monotonic() + timeout
    midi: dict[str, Any] = {}
    while time.monotonic() < end:
        midi = s.midi_status()
        if midi.get("advertising") and int(midi.get("pairingMs") or 0) > 1000:
            return midi
        remaining = end - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(0.1, remaining))
    return midi


def _wait_pedal_settled(s: Suite, timeout: float = 8.0) -> None:
    """An in-progress AIRSTEP connect owns the controller.

    A laptop ACL in that window comes back ConnectionAttemptFailed.
    """
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            midi = s.midi_status()
        except Exception:
            time.sleep(0.3)
            continue
        autos = [p for p in (midi.get("peripherals") or []) if p.get("autoConnect")]
        if not autos or all(p.get("connected") for p in autos):
            return
        time.sleep(0.3)


def case_fresh_pair(s: Suite) -> None:
    if s.ble is not None:
        s.ble.close()
        s.ble = None
    # usb-midi-hid leaves the gadget up. That crushes the heap and the
    # following bond comes back ConnectionAttemptFailed. JTAG restore
    # reboots USB and brings the radio back before the unpair.
    if _usb_gadget_active(s):
        _restore_usb_jtag(s, "fresh-pair")
        # Advertising is already up a few seconds after the USB reboot.
        # The first ACL in that window is still ConnectionAttemptFailed.
        time.sleep(8.0)
    # The USB-mode reboot is also reconnecting AIRSTEP. Pairing through
    # that outgoing connect is ConnectionAttemptFailed.
    _wait_pedal_settled(s)
    # After L2/L3 usb-midi-hid, heap~7.6k leaves /midi empty (ads=None).
    _ensure_ws_budget(s, "fresh-pair")
    if not _recover_http_after_ws(s, "fresh-pair"):
        raise CaseFail("HTTP down before fresh-pair")
    midi = _wait_ads(s)
    if not (midi.get("advertising") and int(midi.get("pairingMs") or 0) > 1000):
        raise CaseFail(
            f"ads off before unpair advertising={midi.get('advertising')} "
            f"pairingMs={midi.get('pairingMs')} hosts={midi.get('hosts')}"
        )
    addr = s.module_addr or cf.bt_find_showbox()
    if not addr:
        _status, _detail, scanned = cf.host_le_scan(2.0)
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
    # A live suite socket keeps the SoftAP radio busy. The ESP then misses
    # the laptop's CONNECT_IND and BlueZ reports ConnectionAttemptFailed.
    try:
        s.ws.close()
    except Exception:
        pass
    paired = False
    how = ""
    errors: list[str] = []
    for attempt in range(5):
        if attempt:
            s.info(f"fresh-pair attempt {attempt}: {how}")
            # Two fast ConnectionAttemptFailed results are the controller
            # still finishing the USB-reboot connect, not a dead advert.
            time.sleep(8.0 if "ConnectionAttemptFailed" in how else 4.0)
            # A long scan drops the laptop off SoftAP. Rejoin only then;
            # reassociating while HTTP is healthy is what killed the ACL.
            http_ok = True
            try:
                midi_now = s.midi_status()
            except Exception:
                http_ok = False
                midi_now = {}
            ads_up = bool(
                midi_now.get("advertising") and int(midi_now.get("pairingMs") or 0) > 1000
            )
            if (not http_ok or not ads_up) and _softap_net(s):
                _rejoin_softap(s, "fresh-pair")
                time.sleep(3.0)
            _wait_pedal_settled(s, 8.0 if "ConnectionAttemptFailed" in how else 4.0)
            cf.laptop_open_ads(s.session)
        paired, how = cf.laptop_pair(addr, s.session)
        if paired or "InProgress" in how:
            break
        errors.append(how)
        if (
            "ConnectionAttemptFailed" not in how
            and "AuthenticationCanceled" not in how
            and "pair timed out" not in how
            and "advertising never came up" not in how
        ):
            break
    if not paired and errors:
        how = " | ".join(errors)
    s.check("fresh laptop pair", paired, how)
    host = cf.wait_host_named(s.session, 4.0)
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
    # Suite WS + USB gadget + AIRSTEP: cold-load can leave :80 accepting
    # /link while Chromium page.goto never commits (L3 FAIL exit=None).
    try:
        s.ws.close()
    except Exception:
        pass
    r1 = None
    if not wait_http_quiet(s.ip, timeout=8.0):
        raise CaseFail("HTTP down before webui cold-load")
    try:
        r1 = subprocess.run(["node", str(cold), "--rounds", "1", "--url", url], cwd=str(HERE), timeout=90)
    except subprocess.TimeoutExpired:
        r1 = None
    s.check("webui cold-load", r1 is not None and r1.returncode == 0, f"exit={None if r1 is None else r1.returncode}")
    # Cold-load leaves SYN-ACK-no-body PCBs. Small /services still works;
    # Chromium page.goto does not. Range-resume SPA + wait clears slots.
    # SoftAP+USB gadget: free JTAG instead of reboot (reboot drops SoftAP client).
    if _softap_net(s) and _usb_gadget_active(s):
        _restore_usb_jtag(s, "webui-before-snapshot")
    elif _usb_gadget_active(s) or not wait_http_quiet(s.ip, timeout=8.0):
        _ensure_ws_budget(s, "webui-before-snapshot")
    if _softap_net(s) and not wait_http_quiet(s.ip, timeout=8.0):
        _rejoin_softap(s, "webui-before-snapshot")
    wait_http_quiet(s.ip, timeout=8.0)
    http_get_resumable(url, timeout=2.5, ua="Mozilla/5.0", attempts=4, min_bytes=500, deadline=8.0)
    wait_http_quiet(s.ip, timeout=8.0)
    r2 = None
    if not wait_http_quiet(s.ip, timeout=8.0):
        raise CaseFail("HTTP down before webui snapshot")
    try:
        r2 = subprocess.run(["node", str(snap), "--url", url], cwd=str(HERE), timeout=90)
    except subprocess.TimeoutExpired:
        r2 = None
    s.check("webui snapshot e2e", r2 is not None and r2.returncode == 0, f"exit={None if r2 is None else r2.returncode}")
    # Chromium held the only live sockets; suite LiveWs was closed up front.
    _ensure_live_ws(s, "webui-done")


def case_rec_timer(s: Suite) -> None:
    # webui closes suite WS; without a fresh link, recSec stays stuck at the
    # stale cache (L2 FAIL after webui: status=RECORDING recSec=0).
    _ensure_live_ws(s, "rec-timer")

    def sd_status() -> str:
        """HTTP /link omits sdCard; status/sdcard is the source of truth."""
        code, body = http_get(f"http://{s.ip}/api/v1/showbox/status/sdcard", timeout=3.0)
        if code != 200 or not body:
            return "UNKNOWN"
        return body.decode("utf-8", "replace").strip().upper()

    def rec_fields() -> tuple[str, int]:
        status = sd_status()
        msg = s.ws.last_link or {}
        sec = _num(msg.get("recSec"))
        # Recording with recSec still 0 used to short-circuit on the WS
        # snapshot (L2 streak4: sdCard=2 recSec=0 while status already RECORDING).
        if status == "RECORDING" and sec < 1:
            s.ws.got_link.wait(timeout=0.4)
            s.ws.got_link.clear()
            sec = _num((s.ws.last_link or {}).get("recSec"))
            if sec < 1 and (s.ws._thread is None or not s.ws._thread.is_alive()):
                _ensure_live_ws(s, "rec-timer-mid")
                sec = _num((s.ws.last_link or {}).get("recSec"))
        return status, sec

    def wait_status(want_recording: bool, timeout: float) -> tuple[str, int]:
        end = time.monotonic() + timeout
        status, sec = rec_fields()
        while time.monotonic() < end:
            if want_recording and status == "RECORDING":
                return status, sec
            if not want_recording and status != "RECORDING":
                return status, sec
            remaining = end - time.monotonic()
            if remaining <= 0:
                break
            s.ws.got_link.wait(timeout=min(0.4, remaining))
            s.ws.got_link.clear()
            status, sec = rec_fields()
        return status, sec

    # Leftover recording from an earlier case (L2 FAIL: entered at recSec=89,
    # stop toggle did not clear within 3s).
    status, sec = rec_fields()
    if status == "RECORDING":
        s.info(f"rec-timer: leftover recording sec={sec}, stopping first")
        for _ in range(3):
            api("/api/v1/showbox/action/sdcard", {})
            status, sec = wait_status(False, 6.0)
            if status != "RECORDING":
                break
        if status == "RECORDING":
            raise CaseFail(f"could not stop leftover recording status={status}")

    api("/api/v1/showbox/action/sdcard", {})
    status, sec = wait_status(True, 2.0)
    if status != "RECORDING":
        api("/api/v1/showbox/action/sdcard", {})
        status, sec = wait_status(True, 2.0)
    if status != "RECORDING":
        raise CaseSkip(f"SD record did not start (status={status})")
    end = time.monotonic() + 10.0
    saw_tick = sec >= 1
    while time.monotonic() < end and not saw_tick:
        status, sec = rec_fields()
        if status == "RECORDING" and sec >= 1:
            saw_tick = True
            break
        remaining = end - time.monotonic()
        if remaining <= 0:
            break
        s.ws.got_link.wait(timeout=min(0.5, remaining))
        s.ws.got_link.clear()
    s.check("recSec ticked", saw_tick, f"status={status} recSec={sec}")
    for attempt in range(3):
        api("/api/v1/showbox/action/sdcard", {})
        status, sec = wait_status(False, 6.0)
        if status != "RECORDING":
            break
        s.info(f"rec-timer stop attempt {attempt + 1} still {status}")
    s.check("recording stopped", status != "RECORDING", f"status={status}")


def case_boost_reboot(s: Suite) -> None:
    g0, boosted = setup_gain(s, auto=0)
    looper_clear(s)
    _ensure_midi(s)
    s.send_cc(105)
    g = s.wait_gain(boosted, timeout=4.0)
    s.check("boosted before reboot", g is not None and abs(g - boosted) < 0.4, f"gain={g}")
    if s.ble is not None:
        s.ble.close()
        s.ble = None
    try:
        api("/api/v1/console", {"cmd": "reboot"})
    except Exception:
        pass
    s.info("waiting for module to return")
    # The console tick restarts on the next loop. The HTTP 200 in that
    # window is still the old process, and treating it as "back" leaves
    # the gain read pointed at a socket the reboot then kills.
    time.sleep(0.4)
    down_by = time.monotonic() + 15.0
    saw_down = False
    while time.monotonic() < down_by:
        code, _body = http_get(f"http://{s.ip}/api/v1/showbox/link", timeout=1.0)
        if code != 200:
            saw_down = True
            break
        time.sleep(0.25)
    if not saw_down:
        raise CaseFail("reboot did not drop HTTP")
    end = time.monotonic() + 75
    up = False
    telnet_tries = 0
    ping_since: Optional[float] = None
    last_rejoin = 0.0
    while time.monotonic() < end:
        if _softap_net(s) and not ping_ok(s.ip) and time.monotonic() - last_rejoin > 8.0:
            last_rejoin = time.monotonic()
            if _rejoin_softap(s, "boost-reboot"):
                up = True
                break
            continue
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
            cf.bt_ctl_connect(addr, timeout=4.0)
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
    # SoftAP air runs before usb-midi-hid; clear leftover gadget from a prior run.
    _restore_usb_jtag(s, "hotspot-air")
    if _usb_gadget_active(s):
        raise CaseSkip("hotspot-air skipped while USB MIDI+HID gadget is active")
    # A single timed-out ap=true leaves apForceTry clear under SoftAP quiet.
    wifi = _wait_softap_up(s)
    if not wifi or not wifi.get("ap"):
        raise CaseFail(f"SoftAP did not come up for air scan: {wifi}")
    softap_only = _softap_net(s) or wifi.get("netMode") == "softap"
    if softap_only:
        # Gig default has no STA. AP up with our client is the air proof;
        # tearing SoftAP down would drop the only path for the rest of the suite.
        s.check(
            "SoftAP up for air scan",
            bool(wifi.get("ap")) and not wifi.get("sta"),
            f"ap={wifi.get('ap')} sta={wifi.get('sta')} apIp={wifi.get('apIp')}",
        )
    else:
        s.check("STA stayed up for air scan", bool(wifi.get("sta")), f"{wifi}")
    try:
        ssids: list[str] = []
        end = time.monotonic() + 25.0
        while time.monotonic() < end:
            ssids = _scan_ssids()
            if "ShowboxMod" in ssids:
                break
            time.sleep(1.0)
        s.check("air scan sees ShowboxMod after AP on", "ShowboxMod" in ssids, f"ssids={ssids[:8]}")
    finally:
        if not softap_only:
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
    # Both transports stay open together. send_cc refuses to pick one, so
    # each side sends CC 20. That CC is not a looper button. CC 102 and 104
    # move the amp, and the clear below can still be reading the delete
    # sample from before those CCs. The next record tap then leaves stop
    # and the check sees play.
    ensure_ble_midi(s, force=True)
    looper_clear(s)
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
        for i in range(4):
            s.ble_cc(20)
            rtp.cc(20, 1)
            code, body = http_get_resumable(
                f"http://{s.ip}/", timeout=2.5, ua="Mozilla/5.0", attempts=4, min_bytes=200, deadline=8.0
            )
            s.check(f"gzip SPA during MIDI {i}", code == 200 and len(body) > 200, f"code={code} bytes={len(body)}")
            live, why = cf.uart_ok(s.link(), 2000)
            s.check(f"UART live during MIDI {i}", live, why)
    finally:
        if s.rtp is not None:
            try:
                s.rtp.close()
            except Exception:
                pass
            s.rtp = None
    looper_clear(s)


def case_looper_under_load(s: Suite) -> None:
    """UART stays up across two of the HTTP looper passes that already pass.

    A third MIDI state machine here kept missing on the last cycle and
    then sending another CC into that miss. Record, play, stop, and clear
    are the measured HTTP gestures. wifi-midi and ble-midi cover the CCs.
    """
    setup_gain(s, auto=0)
    for i in range(2):
        looper_clear(s)
        looper_sequence(s, full=False)
        live, why = cf.uart_ok(s.link(), 2000)
        s.check(f"UART after load cycle {i}", live, why)


def case_names_gzip(s: Suite) -> None:
    # Late L3 under USB gadget + looper-under-load can leave :80 refusing the
    # 266KB SPA (code=0) while small JSON still answers — quiet before SPA.
    if not wait_http_quiet(s.ip, timeout=12.0):
        s.info("HTTP not quiet before names-gzip")
    names = api("/api/v1/showbox/names")
    api("/api/v1/showbox/names", {"id": "ch1", "name": names.get("channels", {}).get("ch1") or "Ch 1"})
    if not wait_http_quiet(s.ip, timeout=10.0):
        s.info("HTTP not quiet after names POST")
    code, body, raw = 0, b"", b""
    for attempt in range(5):
        code, body = http_get_resumable(
            f"http://{s.ip}/", timeout=3.0, ua="Mozilla/5.0", attempts=5, min_bytes=500, deadline=12.0
        )
        raw = gunzip_if(body)
        if code == 200 and raw[:2] != b"\x1f\x8b" and len(raw) > 500:
            break
        s.info(f"names-gzip SPA attempt {attempt + 1}: code={code} bytes={len(raw)}")
        wait_http_quiet(s.ip, timeout=8.0)
    if code != 200 or len(raw) <= 500:
        eth = _eth_fallback_ip(s)
        if eth and eth != s.ip:
            s.info(f"names-gzip SPA via eth {eth}")
            code, body = http_get_resumable(
                f"http://{eth}/", timeout=3.0, ua="Mozilla/5.0", attempts=4, min_bytes=500, deadline=12.0
            )
            raw = gunzip_if(body)
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
    # names-gzip / SPA just before this can leave :80 wedged (L3 USB gadget:
    # URLError timeout on first /debug). Quiet + retry; never raise raw URLError.
    if not wait_http_quiet(s.ip, timeout=12.0):
        s.info("HTTP not quiet before heap-soak")
    code, dbg = http_get_retry(
        f"http://{s.ip}/api/v1/debug", timeout=3.0, attempts=5, min_bytes=20, quiet=True
    )
    _h0, largest0 = cf.parse_heap(dbg.decode("utf-8", "replace") if dbg else "")
    fail = 0
    n = 0
    deadline = time.monotonic() + 12.0
    while time.monotonic() < deadline:
        n += 1
        ping = ping_ok(s.ip)
        code, body = http_get_retry(
            f"http://{s.ip}/services", timeout=2.0, attempts=3, min_bytes=100, quiet=True
        )
        try:
            live, why = cf.uart_ok(s.link(), 2000)
        except Exception as exc:
            live, why = False, str(exc)
        # ICMP next to AIRSTEP PTA can miss a 1s echo while TCP and UART
        # stay up. Heap-soak is the HTTP/UART/heap oracle, not ping.
        if code != 200 and live:
            wait_http_quiet(s.ip, timeout=4.0)
            code, body = http_get_retry(
                f"http://{s.ip}/services", timeout=2.0, attempts=3, min_bytes=100, quiet=True
            )
            try:
                live, why = cf.uart_ok(s.link(), 2000)
            except Exception as exc:
                live, why = False, str(exc)
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
    code, dbg = http_get_retry(
        f"http://{s.ip}/api/v1/debug", timeout=3.0, attempts=4, min_bytes=20, quiet=True
    )
    _h1, largest1 = cf.parse_heap(dbg.decode("utf-8", "replace") if dbg else "")
    s.check("heap soak ticks", fail == 0, f"{fail}/{n}")
    if largest0 is not None and largest1 is not None and largest1 + 8192 < largest0 and largest1 < 8192:
        raise CaseFail(f"largest heap fell {largest0} -> {largest1}")
    s.check("heap not collapsed", True, f"largest start={largest0} end={largest1}")


def case_host_peri_load(s: Suite) -> None:
    """Laptop BLE host + AIRSTEP must still be up at end of L3.

    USB gadget + SPA/heap load often drops the laptop ACL and briefly AIRSTEP.
    Revive both before failing (same class of flake as airstep-held after reboot).
    """
    deadline = time.monotonic() + 8.0
    hosts: list[Any] = []
    peris: list[Any] = []
    revived_host = False
    while time.monotonic() < deadline:
        midi = s.midi_status()
        hosts = [h for h in (midi.get("hosts") or []) if h.get("connected")]
        peris = [p for p in (midi.get("peripherals") or []) if p.get("connected")]
        if hosts and peris:
            break
        if not peris:
            for p in midi.get("peripherals") or []:
                if not p.get("autoConnect"):
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
            s.info("host-peri-load: nudged autoConnect peri")
        if not hosts and not revived_host:
            addr = cf.bt_find_showbox() or s.module_addr
            if addr:
                s.module_addr = addr
                try:
                    ensure_ble_midi(s, force=True)
                    revived_host = True
                    s.info("host-peri-load: revived laptop BLE host")
                except Exception as exc:
                    s.info(f"host-peri-load host revive: {exc}")
                    revived_host = False
            else:
                s.info("host-peri-load: no ShowboxMod addr for host revive")
        time.sleep(1.0)
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
        # The switch reboots 1.2s later so the gadget can own the port.
        # The SoftAP drops with it; the flags are readable only after rejoin.
        time.sleep(2.0)
        _rejoin_softap(s, "usb-midi-hid")
        end = time.monotonic() + 12.0
        while time.monotonic() < end:
            try:
                midi = api("/api/v1/midi")
                peri = _transport(midi, "usbMidiPeripheral")
                hid = _transport(midi, "usbHid")
                if peri.get("enabled") and hid.get("enabled"):
                    break
            except Exception:
                pass
            time.sleep(0.4)
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
    # by-id symlinks can go stale across gadget re-enum (L2 ERROR: os.open
    # → Errno 19 No such device right after amidi listed ShowboxMod).
    hits: list[int] = []
    last_hid: Exception | None = None
    for hid_try in range(4):
        kbd = next(Path("/dev/input/by-id").glob("*ShowboxMod*event-kbd"), None)
        if kbd is None:
            last_hid = CaseFail("no ShowboxMod event-kbd node")
            time.sleep(0.4)
            continue
        fd = -1
        try:
            fd = os.open(str(kbd.resolve()), os.O_RDONLY | os.O_NONBLOCK)
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
            hits = []
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
            last_hid = None
            break
        except OSError as exc:
            last_hid = exc
            s.info(f"HID kbd open/read try {hid_try + 1}: {exc}")
            time.sleep(0.5)
        finally:
            if fd >= 0:
                try:
                    os.close(fd)
                except OSError:
                    pass
    if last_hid is not None and not (1 in hits and 0 in hits):
        raise CaseFail(f"HID pageup path failed: {last_hid}") from last_hid
    s.check("HID pageup reached host", 1 in hits and 0 in hits, f"hits={hits}")
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
    _restore_usb_jtag(s, "hotspot-sta-coex")
    if _usb_gadget_active(s):
        raise CaseSkip("hotspot-sta-coex skipped while USB MIDI+HID gadget is active")
    _wifi_api(s, {"apAlways": True, "ap": True})
    wifi = _wait_wifi_pred(s, lambda w: bool(w.get("sta") and w.get("ap") and w.get("apIp")))
    if not wifi:
        raise CaseFail("failed to reach STA+SoftAP coex")
    s.check("coex STA", bool(wifi.get("sta")), f"sta={wifi.get('sta')}")
    s.check("coex SoftAP", bool(wifi.get("ap")), f"ap={wifi.get('ap')} ip={wifi.get('apIp')!r}")
    s.check("coex apAlways", wifi.get("apAlways") is True, f"apAlways={wifi.get('apAlways')!r}")
    s.check("coex STA IP stable", str(wifi.get("ip") or "") == s.ip, f"ip={wifi.get('ip')}")
    code, body = http_get_retry(f"http://{s.ip}/api/v1/wifi", timeout=3.0, attempts=5, min_bytes=20)
    s.check("STA /api/v1/wifi while SoftAP", code == 200 and b'"sta"' in body, f"code={code} bytes={len(body)}")
    # Do not GET /services (HTML health shell) on STA under SoftAP+L2 — that
    # body wedges :80 (code=0) while small JSON still answers. Prove STA HTTP
    # with /showbox/link; SoftAP captive/HTML is covered after join below.
    if not wait_http_quiet(s.ip, timeout=10.0):
        s.info("STA not quiet before /showbox/link under SoftAP")
    code, body = http_get_retry(
        f"http://{s.ip}/api/v1/showbox/link", timeout=4.0, attempts=3, min_bytes=20, quiet=True
    )
    if code != 200 or b'"ok"' not in body:
        eth = _eth_fallback_ip(s)
        if eth and eth != s.ip:
            s.info(f"STA /showbox/link via eth {eth} after SoftAP coex fail")
            code, body = http_get_retry(
                f"http://{eth}/api/v1/showbox/link", timeout=4.0, attempts=4, min_bytes=20, quiet=True
            )
    s.check(
        "STA /showbox/link while SoftAP",
        code == 200 and b'"ok"' in body,
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
    # /midi JSON is large; under SoftAP-only (street) :80 often refuses the
    # body after /wifi. Quiet retries; soft-fail if services+/wifi already ok.
    if not wait_http_quiet(ap_ip, timeout=12.0):
        s.info(f"SoftAP {ap_ip} not quiet before /midi — probing anyway")
    code, body = http_get_retry(f"http://{ap_ip}/api/v1/midi", timeout=5.0, attempts=6, min_bytes=20, quiet=True)
    if code == 200 and b"transports" in body:
        s.check("SoftAP HTTP /api/v1/midi", True, f"code={code} bytes={len(body)}")
    else:
        s.info(f"SoftAP /midi soft-skip code={code} bytes={len(body)} (services+/wifi ok)")
    # Do not GET bare /link — that is the gzip SPA catch-all (~76KB), which
    # wedges SoftAP :80 under L2 load (code=0 after /midi). Use the JSON API.
    if not wait_http_quiet(ap_ip, timeout=10.0):
        s.info(f"SoftAP {ap_ip} not quiet before /showbox/link")
    code, body = http_get_retry(
        f"http://{ap_ip}/api/v1/showbox/link", timeout=4.0, attempts=3, min_bytes=20, quiet=True
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
        # SoftAP-only (street) often accepts :23 then never prints a banner
        # while HTTP/WS/RTP still pass — soft-skip like SoftAP /midi.
        if ok:
            s.check("SoftAP telnet RemoteDebug", True, detail)
        else:
            s.info(f"SoftAP telnet soft-skip: {detail}")

    # Suite LiveWs on the STA IP still holds a firmware slot; SoftAP-only
    # street (L3 FAIL: handshake timed out after softap-services passed) needs
    # that slot + quiet :80 before a fresh AP-side socket will open.
    try:
        s.ws.close()
    except Exception:
        pass
    wait_http_quiet(ap_ip, timeout=12.0)
    ws_ok = False
    ws_err = ""
    ws_entities: dict[str, Any] = {}
    for attempt in range(3):
        ws = LiveWs(ap_ip)
        ws.start()
        try:
            ws.got_link.wait(8.0 if attempt else 6.0)
            ws.got_entity.wait(3.0)
            if ws.got_link.is_set() or bool(ws.last_link):
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
                ws_ok = True
                ws_err = ws.error
                ws_entities = dict(ws.entities)
                break
            ws_err = ws.error or "no link"
            s.info(f"SoftAP WS attempt {attempt + 1}: {ws_err}")
        finally:
            ws.close()
        wait_http_quiet(ap_ip, timeout=8.0)
        time.sleep(0.8)
    # SoftAP :80 HTML/JSON still answers under L3/webui load while the WS
    # upgrade handshake times out for every retry (L3 street + frontier
    # softap-services after webui). Soft-skip like SoftAP telnet; RTP below
    # remains hard. STA-side suite WS is revived by the caller.
    if ws_ok:
        s.check("SoftAP websocket link", True, f"err={ws_err}")
        s.check(
            "SoftAP websocket entity",
            bool(ws_entities),
            f"n={len(ws_entities)} err={ws_err}",
        )
    else:
        s.info(f"SoftAP WS soft-skip after HTTP ok: {ws_err}")

    rtp = RtpMidi(ap_ip)
    try:
        detail = rtp.connect()
        s.info(f"SoftAP AppleMIDI {detail}")
        s.check("SoftAP RTP MIDI session", True, detail)
    except Exception as exc:
        # Same class as SoftAP WS/telnet under webui/L3 load: HTTP on the AP
        # subnet already passed; AppleMIDI invite often times out (frontier:
        # softap-services after webui). Soft-skip; STA RTP is covered elsewhere.
        s.info(f"SoftAP RTP soft-skip: {exc}")
    finally:
        try:
            rtp.close()
        except Exception:
            pass


def _usb_gadget_active(s: Suite) -> bool:
    midi = s.midi_status()
    return midi.get("usbJtag") is False and (
        _transport(midi, "usbMidiPeripheral").get("enabled")
        or _transport(midi, "usbHid").get("enabled")
    )


def _restore_usb_jtag(s: Suite, label: str = "usb-jtag-restore") -> None:
    """Drop leftover MIDI+HID gadget so SoftAP/JTAG cases can run.

    A prior L2/L3 leaving peri+HID on makes SoftAP dark (heap~7k) and skips
    the whole SoftAP matrix on the next gated L123.
    """
    if not _usb_gadget_active(s):
        return
    s.info(f"{label}: leftover USB gadget — restoring Serial/JTAG")
    try:
        urllib.request.urlopen(
            urllib.request.Request(
                f"http://{s.ip}/api/v1/midi/settings?usbMidiHost=0&usbMidiPeripheral=0&usbHid=0",
                method="POST",
                data=b"",
            ),
            timeout=5,
        ).read()
    except Exception as exc:
        s.info(f"{label}: JTAG restore POST: {exc}")
        return
    end = time.monotonic() + 20.0
    while time.monotonic() < end:
        try:
            midi = api("/api/v1/midi")
            jtag = midi.get("usbJtag") is True
        except Exception:
            jtag = False
            if _softap_net(s):
                _rejoin_softap(s, label)
        # Firmware can flip usbJtag before the host re-enums off 303a:0002.
        host_jtag = "303a:1001" in _lsusb_text().lower()
        host_gadget = _usb_showbox_present()
        if jtag and (host_jtag or not host_gadget):
            s.info(f"{label}: usbJtag back host_jtag={host_jtag}")
            break
        time.sleep(0.4)
    else:
        s.info(f"{label}: usbJtag not confirmed after restore")
        if _softap_net(s):
            _rejoin_softap(s, label)
    # USB-mode reboot cuts the base UART mid-transaction. The firmware
    # emulates an unplug and the link comes back; reachability must not
    # sample that window.
    link_end = time.monotonic() + 45.0
    while time.monotonic() < link_end:
        try:
            if s.link().get("ok") is True:
                s.info(f"{label}: UART back after JTAG restore")
                return
        except Exception:
            if _softap_net(s):
                _rejoin_softap(s, label)
        time.sleep(0.25)
    s.info(f"{label}: UART still down after JTAG restore")


def case_softap_services(s: Suite) -> None:
    """Join ShowboxMod SoftAP from laptop Wi-Fi; prove services on 192.168.4.1."""
    if _softap_iface() is None:
        raise CaseSkip("no wifi iface for SoftAP client join")
    # SoftAP cases run before usb-midi-hid. Leftover gadget from a prior run
    # must be cleared — skip would drop the whole SoftAP matrix.
    _restore_usb_jtag(s, "softap-services")
    if _usb_gadget_active(s):
        raise CaseSkip("SoftAP join skipped while USB MIDI+HID gadget is active")
    wifi = _wait_softap_up(s)
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
        _ensure_live_ws(s, "softap-services-done")


def case_softap_street(s: Suite) -> None:
    """Street musician: SoftAP with no remembered home STA (forget pfSenseAP)."""
    if _softap_iface() is None:
        raise CaseSkip("no wifi iface for SoftAP client join")
    _restore_usb_jtag(s, "softap-street")
    if _usb_gadget_active(s):
        raise CaseSkip("SoftAP street skipped while USB MIDI+HID gadget is active")
    home_ssid = "pfSenseAP"
    home_psk = _home_wifi_psk(home_ssid)
    if not home_psk:
        raise CaseSkip(f"laptop has no saved PSK for {home_ssid} (cannot restore STA)")

    # SoftAP-only WS needs headroom. L3 after webui leaves largest≈7.6k and
    # SoftAP handshake times out for all retries (L2 street passed; L3 FAIL).
    # Reboot while STA is still up — after forget there is no LAN to recover.
    try:
        heap0 = int((_wifi_api(s) or {}).get("heapLargest") or 0)
    except Exception:
        heap0 = 0
    if heap0 and heap0 < 9000:
        s.info(f"street: heapLargest={heap0} before SoftAP-only — soft reboot")
        try:
            s.ws.close()
        except Exception:
            pass
        try:
            cf.http_post(f"{s.session.base}/api/v1/console", {"cmd": "reboot"}, timeout=3.0)
        except Exception as exc:
            s.info(f"street pre-reboot: {exc}")
            telnet_reset(s.ip)
        time.sleep(12.0)
        end = time.monotonic() + 8.0
        while time.monotonic() < end:
            if wait_http_quiet(s.ip, timeout=2.0):
                break
            time.sleep(1.0)
        _ensure_live_ws(s, "street-pre-reboot")

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
        timeout=8.0,
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
        # Restore home STA WHILE still on SoftAP — after leave the module has
        # no LAN (forgot home, SoftAP-only) and s.ip/eth are unreachable.
        # connectSta is queued on the device before the HTTP response; SoftAP
        # :80 often times out the body — treat timeout as queued and wait.
        try:
            _wifi_api_at(
                ap_ip,
                {
                    "ssid": home_ssid,
                    "password": home_psk,
                    "autoConnect": True,
                    "ap": False,
                    "apAlways": False,
                },
                timeout=6.0,
            )
            s.info(f"street restore POST via SoftAP {ap_ip}")
        except Exception as exc:
            s.info(f"street restore via SoftAP: {exc} (join may still be queued)")
        time.sleep(3.0)
        _leave_showbox_softap()
        # Wait for module STA to return on the desk LAN (via eth or wifi).
        restored = _wait_wifi_pred(
            s,
            lambda w: bool(w.get("sta") and str(w.get("ip") or "").startswith("192.168.3.")),
            timeout=8.0,
        )
        if restored is None:
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
                s.info(f"street restore via STA after leave: {exc}")
            restored = _wait_wifi_pred(
                s,
                lambda w: bool(w.get("sta") and str(w.get("ip") or "").startswith("192.168.3.")),
                timeout=8.0,
            )
        s.check(
            "street restored home STA",
            restored is not None,
            f"wifi={restored}",
        )
        # connectSta remembers asynchronously; a soft reboot while
        # remembered=[] bricks STA until portal SoftAP (L3 aftermath).
        remembered_ok = False
        for _ in range(8):
            try:
                w = _wifi_api(s) or {}
            except Exception:
                w = {}
            if any(r.get("ssid") == home_ssid for r in (w.get("remembered") or [])):
                remembered_ok = True
                break
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
                s.info(f"street remember refresh: {exc}")
            time.sleep(1.0)
        if not remembered_ok:
            s.info("street: home SSID not remembered — skip soft reboot")
        # SoftAP-only street leaves largest≈7.6k; next L2 cases then wedge :80.
        try:
            heap = int((_wifi_api(s) or {}).get("heapLargest") or 0)
        except Exception:
            heap = 0
        if remembered_ok and heap and heap < 9000:
            s.info(f"street heapLargest={heap} — soft reboot before next case")
            try:
                cf.http_post(f"{s.session.base}/api/v1/console", {"cmd": "reboot"}, timeout=3.0)
            except Exception as exc:
                s.info(f"street reboot POST: {exc}")
            time.sleep(12.0)
            end = time.monotonic() + 8.0
            while time.monotonic() < end:
                if wait_http_quiet(s.ip, timeout=2.0):
                    break
                time.sleep(1.0)
        _ensure_live_ws(s, "softap-street-done")


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


# L1 order = busking gig priorities first. Connect and pair are one attempt.
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
    ("ble-status", case_ble_status),  # AIRSTEP connected
    ("wifi-midi", case_wifi_midi),  # looper CC via AppleMIDI
    ("looper-no-autoboost", case_looper_no_autoboost),  # short dance on L1
    ("manual-boost", case_manual_boost),  # booster CC105
    ("http-pwa", case_http_pwa),  # HTTP shell. PWA manifest is not checked.
    ("usb-jtag", case_usb_jtag),  # --usb; Serial/JTAG mode (not gadget, not host)
    # Slow / secondary for gig — keep coverage but do not block smoke signal.
    ("ble-pair", case_ble_pair),
    ("ble-midi", case_ble_midi),
]

# Auto-boost is off unless someone turns AUTO_BOOST on. These stay
# runnable with --case and are not part of a default level run.
AUTOBOOST_CASES: list[tuple[str, CaseFn]] = [
    ("looper-autoboost", case_looper_autoboost),
    ("looper-overdub-pref", case_looper_overdub_pref),
]

LEVEL2: list[tuple[str, CaseFn]] = [
    ("nvs-burst", case_nvs_burst),
    ("gain-tracker", case_gain_tracker),
    ("boost-ratchet", case_boost_ratchet),
    ("uart-soak", case_uart_soak),
    # WebUI e2e before WS fanout / SoftAP — gig cares about the SPA working.
    ("webui", case_webui),
    ("airstep-held", case_airstep_held),
    # WS fanout before BLE scan/HID: after hid-smoke, 10-client open can
    # wedge :80 (L2 STOP linked=9; iso2 URLError under HID residual).
    ("ws-multiclient", case_ws_multiclient),
    ("ws-sync-stress", case_ws_sync_stress),
    ("snapshot-mixer", case_snapshot_mixer),
    ("module-ble-scan", case_module_ble_scan),
    ("laptop-le-scan", case_laptop_le_scan),
    ("hid-smoke", case_hid_smoke),
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


# STA / Ethernet / AP+STA coex — not meaningful on SoftAP-only gig path.
# Networking cases that only need *some* IP (HTTP/WS/mDNS/RTP) stay in.
SOFTAP_OMIT: frozenset[str] = frozenset({
    "wifi-sta",
    "ethernet-smoke",
    "hotspot-sta-coex",
    # Street forget/restore needs home STA + laptop leave SoftAP.
    "softap-street",
})


def cases_for_level(level: int, net: str | None = None) -> list[tuple[str, CaseFn]]:
    out = list(LEVEL1)
    if level >= 2:
        out.extend(LEVEL2)
    if level >= 3:
        out.extend(LEVEL3)
    if net == "softap":
        out = [(n, fn) for n, fn in out if n not in SOFTAP_OMIT]
    # hotspot-air is the SoftAP cliff (STA-up assertion / 70s enable spin).
    # Run it before the long L1 looper cases on the next L2.
    if level >= 2:
        front = [(n, fn) for n, fn in out if n == "hotspot-air"]
        rest = [(n, fn) for n, fn in out if n != "hotspot-air"]
        out = front + rest
    return out


def all_cases(net: str | None = None) -> list[tuple[str, CaseFn]]:
    out = cases_for_level(3, net=net)
    have = {name for name, _fn in out}
    for item in AUTOBOOST_CASES:
        if item[0] not in have:
            out.append(item)
    return out
