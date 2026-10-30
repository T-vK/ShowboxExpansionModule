"""E2E: looper state machine + boost behaviour via MIDI CC injection.

The base occasionally ignores a looper press (same as the physical
button) — the transitions retry once, which is the realistic recovery.
The boost flag/gain reconciliation prevents the +2 ratchet.

Two phases:
  1. Looper transitions only (auto-boost disarmed) — the full state machine
  2. Boost behaviour (auto-boost armed) — on/off around transitions
  3. Manual boost (CC105 toggle) + gain tracker
"""
import json
import sys
import time
import urllib.request

IP = "192.168.3.55"
FAILS = []
T0 = time.time()


def api(path, payload=None, params=""):
    url = f"http://{IP}{path}"
    if params:
        url += "?" + params
    if payload is None:
        with urllib.request.urlopen(url, timeout=10) as r:
            return json.load(r)
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.load(r)


def entity(name):
    try:
        with urllib.request.urlopen(f"http://{IP}/api/v1/showbox/entities/{name}", timeout=10) as r:
            return float(r.read().decode("utf-8", "replace").strip())
    except Exception:
        return None


def var(name, fallback=None):
    for v in api("/api/v1/actions/vars")["vars"]:
        if v["name"] == name:
            return v["value"]
    return fallback


def set_var(name, value):
    api("/api/v1/actions/vars", {"name": name, "value": value})


def cc(number, value=127, channel=1):
    return api("/api/v1/midi/inject", {"status": 0xB0 | (channel - 1), "d1": number, "d2": value})


def wait_state(want, timeout=8.0):
    time.sleep(min(timeout, 2.0))
    end = time.time() + timeout
    while time.time() < end:
        if entity("LOOPER_STATE") == want:
            return True
        time.sleep(1.5)
    return False


def wait_gain(want, timeout=8.0):
    time.sleep(1.5)
    end = time.time() + timeout
    while time.time() < end:
        g = entity("INPUT2_GAIN")
        if g is not None and abs(g - want) < 0.05:
            return g
        time.sleep(1.5)
    return entity("INPUT2_GAIN")


def transition(cc_num, want, label, timeout=8.0):
    """Send a CC; re-press once if the base ignores it."""
    for attempt in range(2):
        cc(cc_num)
        if wait_state(want, timeout):
            check(label, True)
            return True
        if attempt == 0:
            print(f"  [{time.time() - T0:6.1f}s] {label}: missed, re-pressing")
            time.sleep(2.0)
    check(label, False, f"state stuck at {entity('LOOPER_STATE')}")
    return False


def boost_snapshot():
    return (
        f"gain={entity('INPUT2_GAIN')} state={entity('LOOPER_STATE')} "
        f"BOOST_ENABLED={var('BOOST_ENABLED')} REMEMBERED_GAIN={var('REMEMBERED_GAIN')} "
        f"AUTO_BOOST={var('AUTO_BOOST')}"
    )


def check(label, ok, detail=""):
    extra = detail
    if not ok and "boost" in label.lower():
        extra = (detail + " " + boost_snapshot()).strip()
    print(f"[{time.time() - T0:6.1f}s] {'PASS' if ok else 'FAIL'}: {label} {extra}")
    if not ok:
        FAILS.append(label)


def wait_ready(seconds=90):
    end = time.time() + seconds
    while time.time() < end:
        try:
            if len(api("/api/v1/showbox/entities")["entities"]) > 50:
                return True
        except Exception:
            pass
        time.sleep(2.0)
    return False


def hard_reset():
    for _ in range(4):
        api("/api/v1/showbox/resync", {})
        time.sleep(2.5)
        cc(104)
        if wait_state(0, 5):
            return
    print("WARN: looper not empty after reset")


# ==========================================================
print("waiting for module ready...")
if not wait_ready():
    print("FAIL: module not ready")
    sys.exit(1)

api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": 28})
time.sleep(1.5)
set_var("REMEMBERED_GAIN", 28)
set_var("BOOST_ENABLED", 0)
set_var("AUTO_BOOST", 0)
set_var("BOOST_CHANNEL", 2)
set_var("BOOST_AMOUNT", 2)
api("/api/v1/showbox/looper_overdub?value=0")
hard_reset()
time.sleep(2.0)

GAIN0 = entity("INPUT2_GAIN")
BOOSTED = GAIN0 + 8.0
STATE0 = entity("LOOPER_STATE")
print(f"reset: gain={GAIN0} state={STATE0:.0f}")
check("clean start", STATE0 == 0)

# ==========================================================
# PHASE 1: full looper state machine (auto-boost disarmed)
# ==========================================================
print("\n=== PHASE 1: looper state machine (auto-boost off) ===")

transition(102, 1, "idle -> record")
time.sleep(2.0)
transition(102, 3, "record -> play")
time.sleep(2.0)
transition(102, 4, "play -> overdub")
time.sleep(2.0)
transition(102, 3, "overdub -> play")
time.sleep(2.0)
transition(103, 2, "play -> stop")
time.sleep(2.0)
transition(104, 0, "stop -> delete")
time.sleep(2.0)

# ==========================================================
# PHASE 2: boost behaviour (auto-boost armed)
# ==========================================================
print("\n=== PHASE 2: boost behaviour (auto-boost armed) ===")
set_var("AUTO_BOOST", 1)
time.sleep(0.5)

transition(102, 1, "record starts")
time.sleep(2.5)
g = entity("INPUT2_GAIN")
check("no boost while recording", g is not None and abs(g - GAIN0) < 0.05, f"gain={g}")

transition(102, 3, "record -> play")
g = wait_gain(BOOSTED)
check("boost on play", g is not None and abs(g - BOOSTED) < 0.05, f"gain={g} (want {BOOSTED})")

transition(104, 0, "delete")
g = wait_gain(GAIN0)
check("boost undone after delete", g is not None and abs(g - GAIN0) < 0.05, f"gain={g}")

transition(102, 1, "record starts (2nd)")
time.sleep(2.5)
transition(102, 3, "record -> play (2nd)")
g = wait_gain(BOOSTED)
check("boost on play (2nd)", g is not None and abs(g - BOOSTED) < 0.05, f"gain={g}")

transition(102, 4, "play -> overdub (2nd)")
g = wait_gain(GAIN0)
check("boost undone on overdub (2nd)", g is not None and abs(g - GAIN0) < 0.05, f"gain={g}")

transition(103, 2, "stop")
g = entity("INPUT2_GAIN")
check("no boost while stopped", g is not None and abs(g - GAIN0) < 0.05, f"gain={g}")

transition(104, 0, "delete (clean)")
time.sleep(2.0)

# ==========================================================
# PHASE 3: overdub preference (two-press workflow)
# ==========================================================
print("\n=== PHASE 3: overdub preference ===")
api("/api/v1/showbox/looper_overdub?value=1")
time.sleep(0.5)

transition(102, 1, "record starts")
time.sleep(2.5)
transition(102, 3, "record -> play (1st press)")
time.sleep(2.5)
transition(102, 4, "play -> overdub (2nd press)")
time.sleep(2.5)
g = entity("INPUT2_GAIN")
check("boost off during overdub (state != play)", g is not None and abs(g - GAIN0) < 0.05, f"gain={g}")

transition(102, 3, "overdub -> play")
g = wait_gain(BOOSTED)
check("boost back on once playing (state == play)", g is not None and abs(g - BOOSTED) < 0.05, f"gain={g}")

api("/api/v1/showbox/looper_overdub?value=0")
time.sleep(0.5)

# ==========================================================
# PHASE 4: manual boost (CC105 toggle) + gain tracker
# ==========================================================
print("\n=== PHASE 4: manual boost + tracker ===")
# Reset the gain AND the flag: the previous phase may have left the gain
# boosted with the flag desynced. A clean slate for the manual tests.
api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": 28})
time.sleep(1.5)
GAIN0 = entity("INPUT2_GAIN")
BOOSTED = GAIN0 + 8.0
set_var("REMEMBERED_GAIN", GAIN0)
set_var("BOOST_ENABLED", 0)
set_var("AUTO_BOOST", 0)
time.sleep(0.5)
hard_reset()

cc(105)  # fire the manual boost toggle
g = wait_gain(BOOSTED)
check("manual boost on (+2 steps)", g is not None and abs(g - BOOSTED) < 0.05, f"gain={g}")

cc(105)
g = wait_gain(GAIN0)
check("manual boost off (toggle back)", g is not None and abs(g - GAIN0) < 0.05, f"gain={g}")

print("--- user moves gain during boost: tracker adopts + clears flag")
cc(105)
wait_gain(BOOSTED)
moved = GAIN0 + 12.0
api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": moved})
end = time.time() + 6
rem = None
while time.time() < end:
    rem = var("REMEMBERED_GAIN")
    if rem is not None and abs(rem - moved) < 0.05:
        break
    time.sleep(0.5)
check("tracker adopts the new gain", rem is not None and abs(rem - moved) < 0.05,
      f"REMEMBERED_GAIN={rem} (moved to {moved})")
check("BOOST_ENABLED cleared by user gain move", abs(var("BOOST_ENABLED", -1)) < 0.05,
      f"BOOST_ENABLED={var('BOOST_ENABLED')}")
api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": GAIN0})
time.sleep(1.5)

print("--- cleanup")
g = entity("INPUT2_GAIN")
if g is not None and abs(g - GAIN0) > 0.05:
    api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": GAIN0})
set_var("AUTO_BOOST", 1)
hard_reset()

print()
if FAILS:
    print("FAILURES:", ", ".join(FAILS))
    sys.exit(1)
print("ALL PASS")