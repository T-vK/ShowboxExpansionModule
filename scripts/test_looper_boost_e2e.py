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


def entity(name):
    for row in api("/api/v1/showbox/entities")["entities"]:
        if row["id"] == name:
            return row["value"]
    return None


def wait_state(want, timeout=8.0):
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


def check(label, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}: {label} {detail}")
    if not ok:
        FAILS.append(label)


GAIN0 = entity("INPUT2_GAIN")
BOOSTED = GAIN0 + 8.0
print(f"baseline: INPUT2_GAIN={GAIN0} LOOPER_STATE={entity('LOOPER_STATE')}")
api("/api/v1/showbox/looper_overdub?value=0")

print("--- clean slate: delete any loop, ensure autoboost armed")
cc(104)
wait_state(0)


def ensure_armed():
    """CC107 toggles; find the armed state empirically (max two tries)."""
    for _ in range(2):
        cc(102)
        if not wait_state(1):
            continue
        time.sleep(1.0)
        cc(102)  # -> play
        if wait_state(3):
            g = wait_gain(BOOSTED, 4)
            cc(104)
            wait_state(0)
            wait_gain(GAIN0, 4)
            if abs(g - BOOSTED) < 0.05:
                return True
        else:
            cc(104)
            wait_state(0)
        cc(107)
        time.sleep(0.5)
    return False


check("autoboost armed (after at most one CC107)", ensure_armed())

print("--- CC102: record; NO boost while recording")
cc(102)
check("recording starts", wait_state(1))
time.sleep(1.5)
g = entity("INPUT2_GAIN")
check("no boost while recording", abs(g - GAIN0) < 0.05, f"gain={g}")

print("--- CC102: cycle -> play; boost +2 steps, exactly once")
cc(102)
check("play after record", wait_state(3))
g = wait_gain(BOOSTED)
check("boost on play", abs(g - BOOSTED) < 0.05, f"gain={g} (want {BOOSTED})")

print("--- CC103: stop; boost undone")
cc(103)
check("stop", wait_state(2))
g = wait_gain(GAIN0)
check("boost undone on stop", abs(g - GAIN0) < 0.05, f"gain={g}")

print("--- CC102 from stop: resumes play; boost again")
cc(102)
check("resume play from stop", wait_state(3))
g = wait_gain(BOOSTED)
check("boost on resumed play", abs(g - BOOSTED) < 0.05, f"gain={g}")

print("--- CC102: play -> overdub; boost undone")
cc(102)
check("overdub after play", wait_state(4))
g = wait_gain(GAIN0)
check("boost undone on overdub", abs(g - GAIN0) < 0.05, f"gain={g}")

print("--- repeated overdub cycles: no stacking")
for want in (3, 4, 3, 4):
    cc(102)
    wait_state(want)
    time.sleep(0.8)
g = entity("INPUT2_GAIN")
check("gain stable across overdub cycles", abs(g - GAIN0) < 0.05, f"gain={g}")

print("--- CC104: delete; boost undone")
cc(102)
wait_state(3)
cc(104)
check("delete", wait_state(0))
g = wait_gain(GAIN0)
check("boost undone after delete", abs(g - GAIN0) < 0.05, f"gain={g}")

print("--- CC107 disarm: no boost on play")
cc(107)
time.sleep(0.5)
cc(102)
check("recording starts", wait_state(1))
cc(102)
check("play after record", wait_state(3))
time.sleep(1.5)
g = entity("INPUT2_GAIN")
check("no boost while disarmed", abs(g - GAIN0) < 0.05, f"gain={g}")
cc(107)  # re-arm
time.sleep(0.5)

print("--- overdub after record preference")
api("/api/v1/showbox/looper_overdub?value=1")
time.sleep(0.3)
cc(104)
wait_state(0)
cc(102)
check("recording starts", wait_state(1))
cc(102)
check("overdub after record (2 taps)", wait_state(4))
cc(102)
check("play after overdub", wait_state(3))
api("/api/v1/showbox/looper_overdub?value=0")

print("--- manual boost CC105/CC106")
cc(105)
g = wait_gain(BOOSTED)
check("manual boost on (+2 steps)", abs(g - BOOSTED) < 0.05, f"gain={g}")
cc(106)
g = wait_gain(GAIN0)
check("manual boost off", abs(g - GAIN0) < 0.05, f"gain={g}")

print("--- cleanup")
g = entity("INPUT2_GAIN")
if abs(g - GAIN0) > 0.05:
    api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": GAIN0})
    print(f"restored INPUT2_GAIN -> {GAIN0}")
cc(104)

print()
if FAILS:
    print("FAILURES:", ", ".join(FAILS))
    sys.exit(1)
print("ALL PASS")