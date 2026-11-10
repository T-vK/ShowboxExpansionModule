"""Host-side ShowboxMod SoftAP join helpers (nmcli)."""

from __future__ import annotations

import subprocess
import time

SOFTAP_IP = "192.168.4.1"
SOFTAP_SSID = "ShowboxMod"
SOFTAP_PASSWORD = "showboxmod"


def _softap_iface() -> str | None:
    try:
        out = subprocess.run(
            ["nmcli", "-t", "-f", "DEVICE,TYPE,STATE", "device", "status"],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    for ln in (out.stdout or "").splitlines():
        parts = ln.split(":")
        if len(parts) >= 3 and parts[1] == "wifi" and not parts[0].startswith("p2p-"):
            return parts[0]
    return None


def _join_showbox_softap(password: str = SOFTAP_PASSWORD) -> tuple[bool, str]:
    iface = _softap_iface()
    if not iface:
        return False, "no wifi iface"
    last = "nmcli failed"
    for cmd in (
        ["nmcli", "connection", "down", "ShowboxMod-suite"],
        ["nmcli", "connection", "delete", "ShowboxMod-suite"],
        ["nmcli", "device", "disconnect", iface],
    ):
        try:
            subprocess.run(cmd, capture_output=True, text=True, timeout=8, check=False)
        except (OSError, subprocess.TimeoutExpired):
            pass
    for attempt in range(5):
        try:
            subprocess.run(
                ["nmcli", "device", "wifi", "rescan", "ifname", iface],
                capture_output=True,
                text=True,
                timeout=8,
                check=False,
            )
            time.sleep(1.5 + attempt)
            listed = subprocess.run(
                ["nmcli", "-t", "-f", "SSID", "device", "wifi", "list", "ifname", iface],
                capture_output=True,
                text=True,
                timeout=8,
                check=False,
            )
            if SOFTAP_SSID not in (listed.stdout or ""):
                last = f"{SOFTAP_SSID} not in scan yet"
                continue
            out = subprocess.run(
                [
                    "nmcli",
                    "device",
                    "wifi",
                    "connect",
                    SOFTAP_SSID,
                    "password",
                    password,
                    "ifname",
                    iface,
                    "name",
                    "ShowboxMod-suite",
                ],
                capture_output=True,
                text=True,
                timeout=35,
                check=False,
            )
            if out.returncode == 0:
                return True, (out.stdout or "joined").strip()[:160]
            last = (out.stdout or out.stderr or "nmcli failed").strip()[:160]
        except subprocess.TimeoutExpired as exc:
            last = f"nmcli connect timeout attempt {attempt + 1}: {exc}"
            try:
                subprocess.run(
                    ["nmcli", "connection", "down", "ShowboxMod-suite"],
                    capture_output=True,
                    text=True,
                    timeout=5,
                    check=False,
                )
                subprocess.run(
                    ["nmcli", "device", "disconnect", iface],
                    capture_output=True,
                    text=True,
                    timeout=5,
                    check=False,
                )
            except (OSError, subprocess.TimeoutExpired):
                pass
        except OSError as exc:
            return False, str(exc)
    return False, last
