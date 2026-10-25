"""E2E (slower, reboot test): the booster must undo its boost after a reboot.

Run after flashing or whenever the reboot path needs re-verification.
Takes ~60 s (module reboot + mixer link re-establish).
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
        return r.read()


def entity(name):
    for _ in range(4):
        try:
            for row in api("/api/v1/showbox/entities")["entities"]:
                if row["id"] == name:
                    return row["value"]
        except Exception:
            time.sleep(1.0)
    return None


def var(name, fallback=None):
    for v in api("/api/v1/actions/vars")["vars"]:
        if v["name"] == name:
            return v["value"]
    return fallback


def wait_http(seconds=60):
    end = time.time() + seconds
    while time.time() < end:
        try:
            entity("LOOPER_STATE")
            return True
        except Exception:
            time.sleep(1.0)
    return False


def check(label, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}: {label} {detail}")
    if not ok:
        FAILS.append(label)


def cc(number):
    api("/api/v1/midi/inject", {"status": 0xB0, "d1": number, "d2": 127})


BASE = 28.0
BOOSTED = BASE + 8.0

print("--- normalize")
api("/api/v1/showbox/entities/INPUT2_GAIN", {"value": BASE})
time.sleep(1.2)
api("/api/v1/actions/vars", {"name": "REMEMBERED_GAIN", "value": BASE})
api("/api/v1/actions/vars", {"name": "BOOST_ENABLED", "value": 0})
api("/api/v1/actions/vars", {"name": "AUTO_BOOST", "value": 1})
api("/api/v1/actions/vars", {"name": "BOOST_CHANNEL", "value": 2})
api("/api/v1/actions/vars", {"name": "BOOST_AMOUNT", "value": 2})
cc(104)
time.sleep(1.0)
check("baseline gain", abs(entity("INPUT2_GAIN") - BASE) < 0.05,
      f"gain={entity('INPUT2_GAIN')}")

print("--- boost on")
cc(105)
end = time.time() + 6
while time.time() < end and abs(entity("INPUT2_GAIN") - BOOSTED) > 0.05:
    time.sleep(0.3)
check("boosted", abs(entity("INPUT2_GAIN") - BOOSTED) < 0.05,
      f"gain={entity('INPUT2_GAIN')}")
check("BOOST_ENABLED=1", abs(var("BOOST_ENABLED", 0) - 1) < 0.05)
time.sleep(2.0)  # let the vars flash write settle before the reboot

print("--- reboot the module")
try:
    api("/api/v1/console", {"cmd": "reboot"})
except Exception:
    pass  # the module drops the connection as it restarts
check("module back up", wait_http(75))

print("--- wait for the entity dump (webui-ready)")
end = time.time() + 60
count = 0
while time.time() < end:
    try:
        count = len(api("/api/v1/showbox/entities")["entities"])
    except Exception:
        count = 0
    if count > 50:
        break
    time.sleep(1.0)
check("entity dump received", count > 50, f"{count} entities")
time.sleep(2)

print("--- wait for the boot reset rule")
end = time.time() + 20
gain = entity("INPUT2_GAIN")
while time.time() < end and abs(gain - BASE) > 0.05:
    time.sleep(1.0)
    gain = entity("INPUT2_GAIN")
check("gain unboosted after reboot", abs(gain - BASE) < 0.05, f"gain={gain}")
check("BOOST_ENABLED=0 after reboot", abs(var("BOOST_ENABLED", -1)) < 0.05,
      f"BOOST_ENABLED={var('BOOST_ENABLED')}")

print()
if FAILS:
    print("FAILURES:", ", ".join(FAILS))
    sys.exit(1)
print("ALL PASS")