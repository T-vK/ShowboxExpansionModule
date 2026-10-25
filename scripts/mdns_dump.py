"""Capture a few mDNS packets and hex-dump them."""
import socket
import struct
import sys
import time

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 5353))
mreq = socket.inet_aton("224.0.0.251") + socket.inet_aton("0.0.0.0")
s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
s.settimeout(10)

def parse_name(data, i):
    labels = []
    while True:
        ln = data[i]
        if ln == 0:
            i += 1
            break
        if ln & 0xC0 == 0xC0:
            labels.append("<ptr>")
            i += 2
            break
        labels.append(data[i+1:i+1+ln].decode("utf-8", "replace"))
        i += 1 + ln
    return ".".join(labels), i

seen = 0
while seen < 3:
    data, addr = s.recvfrom(4096)
    seen += 1
    print(f"--- packet {seen} from {addr[0]}:{addr[1]} len={len(data)} t={round(time.time()%100,2)}")
    print(data.hex(" "))
    tid, flags, qd, an, ns, ar = struct.unpack(">HHHHHH", data[:12])
    print(f"tid={tid:#06x} flags={flags:#06x} qd={qd} an={an} ns={ns} ar={ar}")
    i = 12
    for _ in range(qd):
        name, i = parse_name(data, i)
        typ, cls = struct.unpack(">HH", data[i:i+4]); i += 4
        print(f"  Q: {name} type={typ} class={cls:#06x}")
    for _ in range(an + ns + ar):
        name, i = parse_name(data, i)
        typ, cls, ttl, rdl = struct.unpack(">HHIH", data[i:i+10]); i += 10
        rdata = data[i:i+rdl]; i += rdl
        extra = ""
        if typ == 1 and rdl == 4:
            extra = f" A={socket.inet_ntoa(rdata)}"
        print(f"  R: {name} type={typ} class={cls:#06x} ttl={ttl} len={rdl}{extra}")