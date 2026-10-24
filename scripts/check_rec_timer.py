"""Watch the LiveSocket link message for recSec/recPre over a short window."""
import argparse
import asyncio
import json

import websockets


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ip", default="192.168.3.55")
    parser.add_argument("--seconds", type=float, default=10.0)
    args = parser.parse_args()

    uri = f"ws://{args.ip}/ws"
    async with websockets.connect(uri, max_size=None) as ws:
        seen = 0
        try:
            while seen < 3:
                raw = await asyncio.wait_for(ws.recv(), timeout=args.seconds)
                msg = json.loads(raw)
                if msg.get("type") == "link":
                    seen += 1
                    print({k: msg.get(k) for k in ("sdCard", "sdHeard", "recSec", "recPre")})
        except asyncio.TimeoutError:
            pass
        if seen == 0:
            print("no link message received")
            raise SystemExit(1)


asyncio.run(main())