#!/usr/bin/env python3
"""Fast: Wi-Fi status JSON has SSID, IP, hotspot name, and client list.

Read-only. Do not toggle the AP from this test — that previously broke
UART interception and advertised a cryptic ESP_xxxx SSID.
"""

from __future__ import annotations

from live_helpers import IP, Checks, api, exit_with


def main() -> None:
    c = Checks()
    wifi = api("/api/v1/wifi")
    c.check("STA associated", bool(wifi.get("sta")), f"sta={wifi.get('sta')}")
    ssid = str(wifi.get("ssid") or "")
    c.check("STA SSID present", len(ssid) > 0, f"ssid={ssid!r}")
    ip = str(wifi.get("ip") or "")
    c.check("STA IP matches", ip == IP, f"ip={ip}")
    c.check("hotspot SSID is ShowboxMod", wifi.get("apSsid") == "ShowboxMod",
            f"apSsid={wifi.get('apSsid')!r}")
    c.check("hotspot password present", bool(wifi.get("apPassword")),
            "set" if wifi.get("apPassword") else "missing")
    ap_ip = str(wifi.get("apIp") or "")
    if wifi.get("ap"):
        c.check("AP IP is 192.168.4.1", ap_ip == "192.168.4.1", f"apIp={ap_ip}")
        clients = wifi.get("apClients")
        c.check("apClients is a list when AP is up", isinstance(clients, list) or clients is None,
                f"apClients={clients}")
    else:
        c.check("AP reported off or unnumbered", ap_ip in ("0.0.0.0", "", "192.168.4.1"),
                f"ap={wifi.get('ap')} apIp={ap_ip}")
    remembered = wifi.get("remembered")
    c.check("remembered networks list", isinstance(remembered, list), f"type={type(remembered)}")
    c.check("radio on", bool(wifi.get("radio")), f"radio={wifi.get('radio')}")
    exit_with(c)


if __name__ == "__main__":
    main()
