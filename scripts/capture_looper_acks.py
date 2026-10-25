"""Burst-capture the wire around each looper press: press packet + ACKs.

The base ACKs commands (BE EF 01 01 00 <code> EF BE). If ignored presses
carry a different ACK code, the reason is right there.
"""
import json
import time
import urllib.request

IP = "192.168.3.55"


def post(path, payload=None, raw=False, ctype="application/json"):
    data = payload if raw else json.dumps(payload).encode()
    req = urllib.request.Request(f"http://{IP}{path}", data=data,
        headers={"Content-Type": ctype}, method="POST")
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.read()


def console():
    with urllib.request.urlopen(f"http://{IP}/api/v1/console", timeout=5) as r:
        return r.read().decode("utf-8", "replace")


def entity(name):
    with urllib.request.urlopen(f"http://{IP}/api/v1/showbox/entities", timeout=10) as r:
        for row in json.load(r)["entities"]:
            if row["id"] == name:
                return row["value"]
    return None


report = []
counts = {"ok": 0, "ignored": 0}
T0 = time.time()
count = 0
while time.time() - T0 < 50.0:
    cc = 102 if count % 4 != 3 else 104
    before = entity("LOOPER_STATE")
    # arm the burst capture
    post("/api/v1/midi/settings", b"uartPackets=1", raw=True,
         ctype="application/x-www-form-urlencoded")
    post("/api/v1/console", {"cmd": "clear"})
    t0 = time.time()
    post("/api/v1/midi/inject", {"status": 0xB0, "d1": cc, "d2": 127})
    time.sleep(1.2)
    wire = console()
    post("/api/v1/midi/settings", b"uartPackets=0", raw=True,
         ctype="application/x-www-form-urlencoded")
    after = entity("LOOPER_STATE")
    presses = [l for l in wire.splitlines() if "MIXER->BASE" in l and " 0x17" in l or "LOOPER_BUTTON" in l and "MIXER->BASE" in l]
    acks = [l for l in wire.splitlines() if "ACK" in l and "BASE->MIXER" in l]
    states = [l for l in wire.splitlines() if "LOOPER_STATE" in l]
    ok = after != before
    counts["ok" if ok else "ignored"] += 1
    report.append(f"cc{cc} {before}->{after} {'OK' if ok else 'IGNORED'}")
    report.append(f"  presses-on-wire: {len(presses)}")
    for p in presses:
        report.append(f"    {p}")
    report.append(f"  base acks after press: {len(acks)}")
    for a in acks[:6]:
        report.append(f"    {a}")
    report.append(f"  state announcements: {len(states)}")
    for s in states[:4]:
        report.append(f"    {s}")
    count += 1
    time.sleep(1.0)

with open("scripts/looper_ack_report.txt", "w") as fh:
    fh.write(f"commands: {count}  ok: {counts['ok']}  ignored: {counts['ignored']}\n\n")
    fh.write("\n".join(report))
print(f"commands={count} " + " ".join(f"{k}={v}" for k, v in counts.items()))
print("report: scripts/looper_ack_report.txt")