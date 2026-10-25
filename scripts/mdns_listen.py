"""Listen on the mDNS multicast group and log every packet with a source addr."""
import socket
import struct
import sys
import time

out = open(sys.argv[1] if len(sys.argv) > 1 else "/tmp/mdns_listen.log", "w", buffering=1)

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 5353))
mreq = socket.inet_aton("224.0.0.251") + socket.inet_aton("0.0.0.0")
s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
s.settimeout(1.0)

start = time.time()
while time.time() - start < 180:
    try:
        data, addr = s.recvfrom(4096)
    except socket.timeout:
        continue
    ts = round(time.time() - start, 2)
    name = ""
    try:
        i = 12
        labels = []
        while data[i] != 0:
            ln = data[i]
            labels.append(data[i+1:i+1+ln].decode("utf-8", "replace"))
            i += 1 + ln
        name = ".".join(labels)
    except Exception:
        pass
    out.write(f"{ts:8.2f} from={addr[0]}:{addr[1]} len={len(data)} q={name[:60]}\n")
out.write("done\n")