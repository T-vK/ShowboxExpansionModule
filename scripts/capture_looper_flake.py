"""Classify looper-transition flakiness with definitive evidence.

For every CC-driven transition:
  1. record the command and watch the WS LOOPER_STATE stream for 1.5 s
  2. if the state did not change: force an entity re-sync (fresh dump)
     and re-read the state
       - changed after resync  -> announcement was lost (press worked)
       - still unchanged       -> the base ignored the press
Reports per-class counts + a per-event log.
"""
import json
import sys
import threading
import time
import urllib.request

import serial
import websockets
import asyncio

IP = "192.168.3.55"
PORT = "/dev/ttyACM0"
OUT = "scripts/looper_capture_report.txt"

ws_states = []
ws_lock = threading.Lock()


async def ws_reader(stop):
    try:
        async with websockets.connect(f"ws://{IP}/ws", max_size=None) as ws:
            while not stop.is_set():
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=0.5)
                except asyncio.TimeoutError:
                    continue
                msg = json.loads(raw)
                if msg.get("type") == "entity" and msg.get("id") == "LOOPER_STATE":
                    with ws_lock:
                        ws_states.append((time.time(), msg.get("value")))
    except Exception as exc:
        with ws_lock:
            ws_states.append((time.time(), f"!! ws: {exc}"))


def post(path, payload=None, raw=False, ctype="application/json"):
    data = payload if raw else json.dumps(payload).encode()
    req = urllib.request.Request(f"http://{IP}{path}", data=data,
        headers={"Content-Type": ctype}, method="POST")
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.read()


def entity(name):
    with urllib.request.urlopen(f"http://{IP}/api/v1/showbox/entities", timeout=10) as r:
        for row in json.load(r)["entities"]:
            if row["id"] == name:
                return row["value"]
    return None


def retry_post(path, payload=None, raw=False, ctype="application/json", tries=3):
    for _ in range(tries):
        try:
            return post(path, payload, raw, ctype)
        except Exception:
            time.sleep(1.0)
    return b""


def wait_state(want, timeout=1.5):
    end = time.time() + timeout
    while time.time() < end:
        if entity("LOOPER_STATE") == want:
            return True
        time.sleep(0.15)
    return False


stop = asyncio.Event()
loop = asyncio.new_event_loop()
threading.Thread(target=lambda: loop.run_until_complete(ws_reader(stop)), daemon=True).start()
time.sleep(1.0)

retry_post("/api/v1/midi/settings", b"uartPackets=0", raw=True,
           ctype="application/x-www-form-urlencoded")
retry_post("/api/v1/showbox/entities/INPUT2_GAIN", {"value": 28})
time.sleep(1.0)
retry_post("/api/v1/showbox/resync", {})
time.sleep(2.5)
retry_post("/api/v1/midi/inject", {"status": 0xB0, "d1": 104, "d2": 127})
time.sleep(2.5)

report = []
counts = {"ok": 0, "ann-lost": 0, "press-ignored": 0}
T0 = time.time()
count = 0
while time.time() - T0 < 45.0:
    cc = 102 if count % 4 != 3 else (103 if count % 8 == 3 else 104)
    before = entity("LOOPER_STATE")
    t0 = time.time()
    retry_post("/api/v1/midi/inject", {"status": 0xB0, "d1": cc, "d2": 127})
    changed = wait_state(-1, 1.5) is not None and entity("LOOPER_STATE") != before
    if entity("LOOPER_STATE") != before:
        counts["ok"] += 1
        report.append(f"ok   cc{cc} {before}->{entity('LOOPER_STATE')} in {(time.time() - t0) * 1000:.0f}ms")
    else:
        retry_post("/api/v1/showbox/resync", {})
        time.sleep(2.5)
        after = entity("LOOPER_STATE")
        if after != before:
            counts["ann-lost"] += 1
            report.append(f"ANN-LOST cc{cc} {before}->{after} (press worked, announcement lost)")
        else:
            counts["press-ignored"] += 1
            report.append(f"IGNORED cc{cc} state stuck at {before} (base did not act)")
    count += 1
    time.sleep(1.8)

stop.set()
with open(OUT, "w") as fh:
    fh.write(f"commands: {count}\n")
    for k, v in counts.items():
        fh.write(f"{k}: {v}\n")
    fh.write("\n" + "\n".join(report))
print(f"commands={count} " + " ".join(f"{k}={v}" for k, v in counts.items()))
print(f"report: {OUT}")