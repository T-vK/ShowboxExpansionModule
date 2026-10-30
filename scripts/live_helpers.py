"""Shared helpers for live Showbox module tests."""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.request
from typing import Any, Optional

IP = os.environ.get("SHOWBOX_IP", "192.168.3.55")
HOST = os.environ.get("SHOWBOX_HOST", "showbox.local")


def api(path: str, payload: Any = None, params: str = "", timeout: float = 8.0) -> Any:
    url = f"http://{IP}{path}"
    if params:
        url += "?" + params
    if payload is None:
        with urllib.request.urlopen(url, timeout=timeout) as resp:
            return json.load(resp)
    req = urllib.request.Request(
        url,
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read()
        if not raw:
            return {}
        try:
            return json.loads(raw.decode("utf-8", "replace"))
        except json.JSONDecodeError:
            return {"_raw": raw.decode("utf-8", "replace")}


def http_status(path: str, payload: Any = None, timeout: float = 8.0) -> tuple[int, Any]:
    url = f"http://{IP}{path}"
    if payload is None:
        req = urllib.request.Request(url, method="GET")
    else:
        req = urllib.request.Request(
            url,
            data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            code = resp.getcode() or 0
    except urllib.error.HTTPError as err:
        raw = err.read() if err.fp else b""
        code = err.code
    except Exception:
        return 0, {}
    if not raw:
        return code, {}
    try:
        return code, json.loads(raw.decode("utf-8", "replace"))
    except json.JSONDecodeError:
        return code, {"_raw": raw.decode("utf-8", "replace")}


def entity(name: str) -> Optional[float]:
    try:
        with urllib.request.urlopen(f"http://{IP}/api/v1/showbox/entities/{name}", timeout=8) as resp:
            return float(resp.read().decode("utf-8", "replace").strip())
    except Exception:
        return None


def vars_list() -> list[dict[str, Any]]:
    return list(api("/api/v1/actions/vars").get("vars") or [])


def var(name: str, fallback: Any = None) -> Any:
    for row in vars_list():
        if row.get("name") == name:
            return row.get("value")
    return fallback


def set_var(name: str, value: float) -> Any:
    return api("/api/v1/actions/vars", {"name": name, "value": value})


def cc(number: int, value: int = 127, channel: int = 1) -> Any:
    return api("/api/v1/midi/inject", {"status": 0xB0 | (channel - 1), "d1": number, "d2": value})


def wait_gain(want: float, timeout: float = 6.0) -> Optional[float]:
    end = time.time() + timeout
    last = None
    while time.time() < end:
        last = entity("INPUT2_GAIN")
        if last is not None and abs(last - want) < 0.05:
            return last
        time.sleep(0.4)
    return last


def wait_var(name: str, want: float, timeout: float = 6.0) -> Any:
    end = time.time() + timeout
    last = None
    while time.time() < end:
        last = var(name)
        if last is not None and abs(float(last) - want) < 0.05:
            return last
        time.sleep(0.3)
    return last


class Checks:
    def __init__(self) -> None:
        self.fails: list[str] = []
        self.t0 = time.time()

    def check(self, label: str, ok: bool, detail: str = "") -> bool:
        stamp = time.time() - self.t0
        print(f"[{stamp:5.1f}s] {'PASS' if ok else 'FAIL'}: {label} {detail}".rstrip())
        if not ok:
            self.fails.append(label)
        return bool(ok)

    def finish(self) -> int:
        if self.fails:
            print("FAILURES:", ", ".join(self.fails))
            return 1
        print("ALL PASS")
        return 0


def exit_with(checks: Checks) -> None:
    sys.exit(checks.finish())
