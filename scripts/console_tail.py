"""Continuously poll the module console (X-Console-Next cursor) into a file."""
import sys
import time
import urllib.request

IP = "192.168.3.55"
OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/console.log"

since = 0
with open(OUT, "w") as fh:
    while True:
        try:
            req = urllib.request.Request(f"http://{IP}/api/v1/console?since={since}")
            with urllib.request.urlopen(req, timeout=5) as r:
                body = r.read().decode("utf-8", "replace")
                nxt = r.headers.get("X-Console-Next")
            fh.write(body)
            fh.flush()
            if nxt:
                since = int(nxt)
        except Exception:
            pass
        time.sleep(0.25)