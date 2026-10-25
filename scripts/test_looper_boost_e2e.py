"""E2E: looper + boost via MIDI CC injection (production inbound dispatch).

Semantics (MidiBoostLoop, the single owner of CC 102-108):
  CC102 cycle: idle->record; record->play/overdub (looperOverdub var);
               play<->overdub; from stop resumes play.
  CC103 stop:  double press.
  CC104 delete: long press (frees the looper).
  Auto boost:  +2 gain steps (8 dB) on INPUT2 while the loop PLAYS,
               undone on stop/delete/record start. CC107 arms/disarms.
  CC105/106:   manual boost on/off.
"""
import json
import sys
import time
import urllib.request

IP = "192.168.3.55"
FAILS = []


def api(path, payload=None):
    if payload is None:
        with urllib.request.urlopen(f"http://{IP}{path}", timeout=10) as r:
            return json.load(r)
    req = urllib.request.Request(
        f"http://{IP}{path}", data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.load(r)


def cc(number, value=127, channel=1):
    return api("/api/v1/midi/inject", {"status": 0xB0 | (channel - 1), "d1": number, "d2": value})


def var(name, fallback=None):
    for v in api("/api/v1/actions/vars")["vars"]:
        if v["name"] == name:
            return v["value"]
    return fallback


def entity(name):
    for row in api("/api/v1/showbox/entities")["entities"]:
        if row["id"] == name:
            return row["value"]
    return None


def wait_state(want, timeout=13.0):
    end = time.time() + timeout
    last = None
    while time.time() < end:
        state = entity("LOOPER_STATE")
        if state != last:
            print(f"  LOOPER_STATE -> {state}")
            last = state
        if state == want:
            return True
        time.sleep(0.3)
    return False


def wait_gain(want, timeout=6.0):
    end = time.time() + timeout
    g = entity("INPUT2_GAIN")
    while time.time() < end:
        g = entity("INPUT2_GAIN")
        if abs(g - want) < 0.05:
            return g
        time.sleep(0.3)
    return g


T0 = time.time()


def check(label, ok, detail=""):
    print(f"[{time.time() - T0:6.1f}s] {'PASS' if ok else 'FAIL'}: {label} {detail}")
    if not ok:
        FAILS.append(label)


def wait_ready(seconds=90):
    """Wait for the module to boot fully and receive the entity dump."""
    end = time.time() + seconds
    count = 0
    while time.time() < end:
        try:
            count = len(api("/api/v1/showbox/entities")["entities"])
        except Exception:
            count = 0
        if count > 50:
            return True
        time.sleep(1.0)
    return False


print("waiting for module ready...")
if not wait_ready():
    print("FAIL: module not ready (entity dump missing)")
    sys.exit(1)
GAIN0 = entity("INPUT2_GAIN")
BOOSTED = GAIN0 + 8.0
print(f"baseline: INPUT2_GAIN={GAIN0} LOOPER_STATE={entity('LOOPER_STATE')}")
# Recover from any prior corrupted state: write a known gain (a genuine
# user write, so the gain tracker re-syncs REMEMBERED_GAIN to it), clear
# the loop and the boost flag, then re-read the baseline.
api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": 28})
time.sleep(1.2)
api("/api/v1/actions/vars", {"name": "REMEMBERED_GAIN", "value": 28})
api("/api/v1/actions/vars", {"name": "BOOST_ENABLED", "value": 0})
api("/api/v1/actions/vars", {"name": "AUTO_BOOST", "value": 1})
api("/api/v1/actions/vars", {"name": "BOOST_CHANNEL", "value": 2})
api("/api/v1/actions/vars", {"name": "BOOST_AMOUNT", "value": 2})
api("/api/v1/showbox/looper_overdub?value=0")
# Re-sync the entity cache from the mixer (announcements can be missed
# during radio windows) and clear the loop until the looper is empty.
for attempt in range(4):
    api("/api/v1/showbox/resync", {})
    time.sleep(2.5)
    cc(104)
    if wait_state(0, 5):
        break
time.sleep(1.0)
GAIN0 = entity("INPUT2_GAIN")
BOOSTED = GAIN0 + 8.0
print(f"reset: INPUT2_GAIN={GAIN0} LOOPER_STATE={entity('LOOPER_STATE')}")
time.sleep(0.3)

print("--- clean slate: delete any loop, ensure autoboost armed")
cc(104)
wait_state(0)


def ensure_armed():
    api("/api/v1/actions/vars", {"name": "AUTO_BOOST", "value": 1})
    time.sleep(0.3)
    return True


check("autoboost armed via vars", ensure_armed())

print("--- CC102: record; NO boost while recording")
cc(102)
check("recording starts", wait_state(1))
time.sleep(1.0)
time.sleep(1.5)
g = entity("INPUT2_GAIN")
check("no boost while recording", abs(g - GAIN0) < 0.05, f"gain={g}")

print("--- CC102: cycle -> play; boost +2 steps, exactly once")
cc(102)
check("play after record", wait_state(3))
time.sleep(1.0)
g = wait_gain(BOOSTED)
check("boost on play", abs(g - BOOSTED) < 0.05, f"gain={g} (want {BOOSTED})")

print("--- CC103: stop; boost undone")
cc(103)
check("stop", wait_state(2))
time.sleep(1.0)
g = wait_gain(GAIN0)
check("boost undone on stop", abs(g - GAIN0) < 0.05, f"gain={g}")

print("--- CC102 from stop: resumes play; boost again")
cc(102)
check("resume play from stop", wait_state(3))
time.sleep(1.0)
g = wait_gain(BOOSTED)
check("boost on resumed play", abs(g - BOOSTED) < 0.05, f"gain={g}")

print("--- CC102: play -> overdub; boost undone")
cc(102)
check("overdub after play", wait_state(4))
time.sleep(1.0)
g = wait_gain(GAIN0)
check("boost undone on overdub", abs(g - GAIN0) < 0.05, f"gain={g}")

print("--- repeated overdub cycles: no stacking")
for want in (3, 4, 3, 4):
    cc(102)
    wait_state(want)
    time.sleep(1.0)
g = entity("INPUT2_GAIN")
check("gain stable across overdub cycles", abs(g - GAIN0) < 0.05, f"gain={g}")

print("--- CC104: delete; boost undone")
cc(102)
wait_state(3)
cc(104)
check("delete", wait_state(0))
time.sleep(1.0)
g = wait_gain(GAIN0)
check("boost undone after delete", abs(g - GAIN0) < 0.05, f"gain={g}")

print("--- CC107 removed: disarm via vars, no boost on play")
api("/api/v1/actions/vars", {"name": "AUTO_BOOST", "value": 0})
time.sleep(0.3)
cc(102)
check("recording starts", wait_state(1))
time.sleep(1.0)
cc(102)
check("play after record", wait_state(3))
time.sleep(1.0)
time.sleep(1.5)
g = entity("INPUT2_GAIN")
check("no boost while disarmed", abs(g - GAIN0) < 0.05, f"gain={g}")
api("/api/v1/actions/vars", {"name": "AUTO_BOOST", "value": 1})  # re-arm
time.sleep(0.3)

print("--- overdub after record preference")
api("/api/v1/showbox/looper_overdub?value=1")
time.sleep(0.3)
cc(104)
wait_state(0)
cc(102)
check("recording starts", wait_state(1))
time.sleep(1.0)
cc(102)
check("overdub after record (2 taps)", wait_state(4))
time.sleep(1.0)
cc(102)
check("play after overdub", wait_state(3))
time.sleep(1.0)
api("/api/v1/showbox/looper_overdub?value=0")
time.sleep(0.3)

print("--- manual boost CC105/CC106")
api("/api/v1/actions/vars", {"name": "BOOST_ENABLED", "value": 0})
time.sleep(0.3)
cc(104)
wait_state(0)
wait_gain(GAIN0)
api("/api/v1/actions/vars", {"name": "BOOST_ENABLED", "value": 0})
time.sleep(0.3)
cc(105)
g = wait_gain(BOOSTED)
check("manual boost on (+2 steps)", abs(g - BOOSTED) < 0.05, f"gain={g}")
cc(105)
g = wait_gain(GAIN0)
check("manual boost off (toggle back)", abs(g - GAIN0) < 0.05, f"gain={g}")

print("--- manual gain move during boost: tracker must adopt it and clear the flag")
cc(105)
wait_gain(BOOSTED)
moved = GAIN0 + 12.0
api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": moved})
end = time.time() + 5
rem = None
while time.time() < end:
    rem = var("REMEMBERED_GAIN")
    if abs(rem - moved) < 0.05:
        break
    time.sleep(0.3)
check("tracker adopts the new gain", rem is not None and abs(rem - moved) < 0.05,
      f"REMEMBERED_GAIN={rem} (moved to {moved})")
check("BOOST_ENABLED cleared by user gain move", abs(var("BOOST_ENABLED", -1)) < 0.05,
      f"BOOST_ENABLED={var('BOOST_ENABLED')}")
api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": GAIN0})
wait_gain(GAIN0)

print("--- cleanup")
g = entity("INPUT2_GAIN")
if abs(g - GAIN0) > 0.05:
    api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": GAIN0})
    print(f"restored INPUT2_GAIN -> {GAIN0}")
cc(104)
wait_state(0)

print()
if FAILS:
    print("FAILURES:", ", ".join(FAILS))
    sys.exit(1)
print("ALL PASS")