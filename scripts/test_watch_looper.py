"""Watch WS entity messages for LOOPER_STATE / INPUT2_GAIN while a CC fires."""
import asyncio
import json
import urllib.request

import websockets

IP = "192.168.3.55"


def post(path, payload):
    req = urllib.request.Request(
        f"http://{IP}{path}", data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.read()


async def main():
    stop = asyncio.Event()
    async with websockets.connect(f"ws://{IP}/ws", max_size=None) as ws:
        async def reader():
            while not stop.is_set():
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=1.0)
                except asyncio.TimeoutError:
                    continue
                m = json.loads(raw)
                if m.get("type") == "entity" and m.get("id") in ("LOOPER_STATE", "INPUT2_GAIN"):
                    print("WS", m.get("id"), "=", m.get("value"))
                if m.get("type") == "link":
                    pass
        task = asyncio.create_task(reader())
        await asyncio.sleep(1)
        print(">>> CC104 delete")
        post("/api/v1/midi/inject", {"status": 0xB0, "d1": 104, "d2": 127})
        await asyncio.sleep(2)
        print(">>> CC102 record")
        post("/api/v1/midi/inject", {"status": 0xB0, "d1": 102, "d2": 127})
        await asyncio.sleep(6)
        print(">>> CC104 delete")
        post("/api/v1/midi/inject", {"status": 0xB0, "d1": 104, "d2": 127})
        await asyncio.sleep(4)
        stop.set()
        task.cancel()

asyncio.run(main())