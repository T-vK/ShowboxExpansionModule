"""E2E: start an SD recording, watch recSec tick server-side, then stop.

Leaves a short take on the mixer's SD card (the mixer saves on stop).
"""
import asyncio
import json
import sys
import urllib.request

import websockets

IP = "192.168.3.55"


def post_sdcard() -> None:
    req = urllib.request.Request(
        f"http://{IP}/api/v1/showbox/action/sdcard",
        data=b"{}",
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        resp.read()


async def watch(seconds: float, want_rec: bool) -> bool:
    """Watch link messages for `seconds`; return True if the wanted state was seen."""
    async with websockets.connect(f"ws://{IP}/ws", max_size=None) as ws:
        loop = asyncio.get_event_loop()
        end = loop.time() + seconds
        saw_tick = False
        last = None
        while loop.time() < end:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=1.5)
            except asyncio.TimeoutError:
                continue
            msg = json.loads(raw)
            if msg.get("type") != "link":
                continue
            state = (msg.get("sdCard"), msg.get("recSec"), msg.get("recPre"))
            if state == last:
                continue
            last = state
            print(f"sdCard={state[0]} recSec={state[1]} recPre={state[2]}")
            if want_rec and state[0] == 2 and state[1] >= 3:
                saw_tick = True
            if not want_rec and state[0] == 1:
                return True
        return saw_tick


async def main() -> int:
    print("starting recording")
    post_sdcard()
    if not await watch(15, want_rec=True):
        print("FAIL: recSec did not tick while recording")
        return 1
    print("stopping recording")
    post_sdcard()
    if not await watch(15, want_rec=False):
        print("FAIL: recording did not return to READY")
        return 1
    print("PASS")
    return 0


sys.exit(asyncio.run(main()))