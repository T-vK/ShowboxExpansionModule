#!/usr/bin/env python3
"""Repeated https://showbox.local/ checks while BLE and mixer UART stay live."""

from __future__ import annotations

import argparse
import json
import socket
import ssl
import subprocess
import sys
import time
import urllib.request

from test_reminders import print_run_reminders


IP_DEFAULT = "192.168.3.55"
HOST = "showbox.local"
ROUNDS = 20


def ping_ok(ip: str) -> bool:
    return subprocess.call(
        ["ping", "-c", "1", "-W", "1", ip],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    ) == 0


def ping_ok_retry(ip: str, tries: int = 3) -> bool:
    for _ in range(tries):
        if ping_ok(ip):
            return True
        time.sleep(0.4)
    return False


def http_get(url: str, timeout: float = 8.0) -> tuple[int, bytes]:
    req = urllib.request.Request(url, headers={"Host": HOST, "Connection": "close"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.getcode() or 0, resp.read()
    except Exception:
        return 0, b""


def https_get(ip: str, path: str, timeout: float = 45.0) -> tuple[str, int, bytes]:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    raw = socket.create_connection((ip, 443), timeout=timeout)
    raw.settimeout(timeout)
    try:
        tls = ctx.wrap_socket(raw, server_hostname=HOST)
        ver = tls.version() or ""
        tls.sendall(
            f"GET {path} HTTP/1.0\r\nHost: {HOST}\r\nConnection: close\r\n\r\n".encode()
        )
        chunks: list[bytes] = []
        header_blob = b""
        header_done = False
        content_len: int | None = None
        body_len = 0
        try:
            while True:
                buf = tls.recv(4096)
                if not buf:
                    break
                chunks.append(buf)
                if not header_done:
                    header_blob += buf
                    sep = header_blob.find(b"\r\n\r\n")
                    if sep >= 0:
                        header_done = True
                        for line in header_blob[:sep].split(b"\r\n"):
                            if line.lower().startswith(b"content-length:"):
                                try:
                                    content_len = int(line.split(b":", 1)[1].strip())
                                except ValueError:
                                    content_len = None
                        body_len = len(header_blob) - (sep + 4)
                else:
                    body_len += len(buf)
                # JTAG: 11b often delivers the TLS body and then drops
                # close_notify/FIN. HTTP clients stop at Content-Length.
                if header_done and content_len is not None and body_len >= content_len:
                    break
        except Exception as exc:
            payload = b"".join(chunks)
            raise RuntimeError(f"{exc} got={len(payload)}") from exc
        tls.close()
        payload = b"".join(chunks)
        status = 0
        if payload.startswith(b"HTTP/"):
            try:
                status = int(payload.split(b" ", 2)[1])
            except (IndexError, ValueError):
                status = 0
        return ver, status, payload
    finally:
        try:
            raw.close()
        except OSError:
            pass


def json_from_payload(payload: bytes) -> dict:
    if not payload:
        return {}
    start = payload.find(b"{")
    if start < 0:
        return {}
    try:
        return json.loads(payload[start:].decode("utf-8", "replace"))
    except json.JSONDecodeError:
        return {}


def json_get(ip: str, path: str) -> dict:
    # Health polls must not open TLS. JTAG: wait_ready's 8 s HTTPS
    # handshakes died at send=792 (SSL EOF) and then TCP SYN to :443
    # and :80 stopped being answered while ICMP still worked.
    code, body = http_get(f"http://{ip}{path}", timeout=2.0)
    if code == 200:
        parsed = json_from_payload(body)
        if parsed:
            return parsed
    return {}


def wait_ready(ip: str, timeout: float) -> None:
    t0 = time.time()
    last = ""
    sta_at = 0.0
    while time.time() - t0 < timeout:
        if not ping_ok(ip):
            last = "ping"
            time.sleep(1)
            continue
        if sta_at == 0.0:
            sta_at = time.time()
            # An HTTP GET while HTTPS is still binding (listening=false,
            # hasCa=false) wedged AsyncTCP: later /api polls timed out
            # for 60 s while ICMP stayed up.
            time.sleep(5)
            continue
        https = json_get(ip, "/api/v1/https")
        link = json_get(ip, "/api/v1/showbox/link")
        listening = bool(https.get("listening"))
        mixer = bool(link.get("ok"))
        last = (
            f"https={https.get('listening')} state={https.get('state')} "
            f"err={https.get('error')!r} link={link.get('ok')} rxM={link.get('rxMix')}"
        )
        if listening and mixer:
            print(
                f"sta ready in {time.time() - t0:.1f}s listening=1 mixer=1 "
                f"https={json.dumps(https)[:160]}",
                flush=True,
            )
            return
        time.sleep(1)
    raise SystemExit(f"not ready after {timeout:.0f}s last={last}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ip", default=IP_DEFAULT)
    parser.add_argument("--rounds", type=int, default=ROUNDS)
    parser.add_argument("--wait", type=float, default=90.0)
    args = parser.parse_args()

    print_run_reminders()
    print(f"wait ready {args.ip} showbox.local https / x{args.rounds}", flush=True)
    status = {}
    t_status = time.time()
    while time.time() - t_status < min(30.0, args.wait):
        if ping_ok(args.ip):
            status = json_get(args.ip, "/api/v1/https")
            if status:
                break
        time.sleep(1)
    if status.get("enabled") is not True:
        print("SKIP HTTPS is off (optional). Enable it before check_https_repeat.", flush=True)
        return 0
    wait_ready(args.ip, args.wait)
    time.sleep(1.5)

    def snapshot() -> tuple[dict, dict]:
        return json_get(args.ip, "/api/v1/https"), json_get(args.ip, "/api/v1/showbox/link")

    def fetch_page(tag: str) -> int:
        t0 = time.time()
        if not ping_ok_retry(args.ip):
            print(f"FAIL {tag} ping dead", flush=True)
            return 1
        try:
            ver, status, payload = https_get(args.ip, "/")
        except Exception as exc:
            print(f"FAIL {tag} https://{HOST}/ {exc}", flush=True)
            https, _ = snapshot()
            print(f"https after fail {json.dumps(https)[:220]}", flush=True)
            return 1
        dt = time.time() - t0
        html = (
            b"<html" in payload.lower()
            or b"<!doctype" in payload.lower()
            or b"showbox" in payload.lower()
            or b"content-encoding: gzip" in payload.lower()
        )
        if status != 200 or len(payload) < 1000 or not html:
            print(
                f"FAIL {tag} https://{HOST}/ {ver} http={status} "
                f"size={len(payload)} time={dt:.2f} head={payload[:80]!r}",
                flush=True,
            )
            https, _ = snapshot()
            print(f"https after short {json.dumps(https)[:220]}", flush=True)
            return 1
        https, link = {}, {}
        for _ in range(6):
            https, link = snapshot()
            if https.get("listening") and link.get("ok"):
                break
            time.sleep(0.5)
        mixer = bool(link.get("ok"))
        listening = bool(https.get("listening"))
        ble = bool(https.get("ble"))
        print(
            f"PASS {tag} {ver} {status} {len(payload)}B {dt:.2f}s "
            f"mixer={int(mixer)} listen={int(listening)} ble={int(ble)} "
            f"hsOk={int(bool(https.get('hsOk')))} rxM={link.get('rxMix')}",
            flush=True,
        )
        if not mixer:
            print(f"FAIL {tag} mixer link dropped", flush=True)
            return 1
        if not listening:
            print(f"FAIL {tag} https listener dropped err={https.get('error')!r}", flush=True)
            return 1
        return 0

    https, _ = snapshot()
    print(
        f"before warmup listening={int(bool(https.get('listening')))} "
        f"hsOk={int(bool(https.get('hsOk')))} ble={int(bool(https.get('ble')))}",
        flush=True,
    )
    time.sleep(0.4)
    if fetch_page("warmup") != 0:
        return 1

    t_ble = time.time()
    ble = False
    while time.time() - t_ble < 45:
        # GAP connect holds PTA. JTAG/host: ICMP and HTTP die for several
        # seconds after the first page releases the BLE hold. Do not treat
        # that dip as STA death, and do not issue HTTP while ping is down.
        alive = ping_ok_retry(args.ip, tries=4)
        if not alive:
            print(f"wait ble ping-miss t={time.time() - t_ble:.1f}", flush=True)
            if time.time() - t_ble > 35:
                print("FAIL ping dead while waiting for bluetooth", flush=True)
                return 1
            time.sleep(1)
            continue
        https, link = snapshot()
        ble = bool(https.get("ble"))
        print(
            f"wait ble={int(ble)} hsOk={int(bool(https.get('hsOk')))} "
            f"mixer={int(bool(link.get('ok')))} err={https.get('error')!r}",
            flush=True,
        )
        if ble and link.get("ok"):
            break
        time.sleep(1)
    if not ble:
        print("FAIL bluetooth/AIRSTEP did not connect after first https://showbox.local/", flush=True)
        return 1

    for i in range(1, args.rounds + 1):
        if fetch_page(f"{i}/{args.rounds}") != 0:
            return 1
        https, _ = snapshot()
        if not https.get("ble"):
            print(f"FAIL {i} bluetooth dropped during https://showbox.local/", flush=True)
            return 1
        time.sleep(0.4)
    print(f"OK {args.rounds}/{args.rounds} https://{HOST}/ mixer+https+ble stayed up", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
