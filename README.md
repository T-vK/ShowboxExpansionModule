## Showbox Expansion Module

Gain more control over your Mackie Showbox. An ESP32-S3 sits between the Showbox base unit and the Breakaway Mixer, so you can drive presets, volume, EQ, effects, the looper, and more from footswitches, expression pedals, MIDI controllers, HID devices, or a browser on your phone or laptop.

### Features

- **Remote control** via tablet, phone, or laptop over Wi‑Fi or Ethernet, with as many devices in parallel as you want (`http://showbox.local`)
- **Full mixer control**: gains, faders, mute, EQ, compressors, FX sends/types, looper, snapshots, tuner, SD record, and more
- **Custom channel and snapshot names**
- **Full MIDI** via 5‑pin DIN, USB, Bluetooth, and Wi‑Fi/Ethernet (AppleMIDI / RTP‑MIDI)
- **USB/Bluetooth page turner**: use any pedal (TRS jack, MIDI, or wireless MIDI) to scroll on your tablet
- **Sophisticated web UI** with EQ graphs, compressor graphs, customizable views, etc.
- **Fully customizable actions** (any trigger, any action, any direction), for example:
  - Two-button looper
  - Boost pedal
  - FX change pedal
  - Snapshot load pedal
  - Sync a Behringer X‑Touch Compact with the Showbox
- **Footswitches and expression pedals**: up to 4 dual TRS jacks (tip + ring per jack)
- **DC jacks** to power your 9 V and 12 V pedals
- **USB ports** to power your USB devices
- **3D-printable case** ([GrabCAD model](https://grabcad.com/library/showboxexpansionmodule-v1-1))
- **Wiring diagrams** provided below
- **Front panel** with 3 customizable LED buttons
- **Updates** via browser [Web Installer](https://t-vk.github.io/ShowboxExpansionModule/) or OTA over the network

### Web interface

Open `http://showbox.local` for a live mixer, snapshots, tuner, looper, and system pages.

![Web UI mixer](./docs/webui-mixer.gif)

Useful settings pages in the SPA:

| Page | What it covers |
|------|----------------|
| **Network** | Join home Wi‑Fi, remembered networks, mixer hotspot |
| **USB** | Serial/JTAG vs MIDI host vs MIDI/HID gadget on the ESP USB‑C |
| **Bluetooth** | Pair MIDI/HID pedals and page turners |
| **System** | Services, OTA, diagnostics |
| **Actions** | Wire triggers to Showbox, MIDI, HID, and module behavior |

The **Actions** page is where you wire footswitches, MIDI, panel buttons, and Showbox state changes to whatever should happen next:

![Web UI actions](./docs/webui-actions.png)

### First connection (Wi‑Fi)

Out of the box the module can raise a **mixer hotspot**:

- SSID: `ShowboxMod`
- Password: `showboxmod`

Join that network from your phone or laptop, then open Chrome or Safari to `http://showbox.local` (or the hotspot IP shown in Network settings, often `192.168.4.1`).

In **Network**:

1. Scan and **Join** your home Wi‑Fi (SSID + password). The module remembers networks and can auto-reconnect.
2. Leave **Always-on hotspot** on if you still want phones to reach the captive portal while the module is also on home Wi‑Fi. Turn it off if you prefer more airtime for Bluetooth or cleaner `showbox.local` on the LAN.
3. **Auto hotspot** brings the mixer SSID back if home Wi‑Fi cannot be reached, so you are not locked out.

Optional **Ethernet** (W5500 on the reference design) uses DHCP with hostname `showbox`. When both Ethernet and Wi‑Fi have addresses, prefer the Ethernet IP for browsing; that is what `showbox.local` advertises.

#### If `showbox.local` does not open

The name is served with mDNS. On Android 12+, turn **Private DNS** off (or use a DNS that does not break local names). If a phone cached a bad lookup, toggle Wi‑Fi once, bookmark the numeric IP from Network settings, or add a router DNS override `showbox` → that IP.

### USB‑C modes

The ESP32‑S3 has **one** native USB‑C data port. It can be only one of these at a time (change in **USB**; MIDI/HID modes reboot):

| Mode | Use |
|------|-----|
| **Serial / JTAG** | Default. Flash and serial console over the ESP USB‑C. |
| **MIDI host** | Plug a class-compliant USB MIDI controller into the module (powered hub recommended). |
| **MIDI gadget** | Module appears as a USB MIDI device to a computer or tablet. |
| **HID gadget** | Module appears as a USB keyboard / mouse / pointer (page turner, etc.). |
| **MIDI + HID** | Composite gadget; MIDI and HID share the port. |

MIDI or HID gadget modes turn off native USB serial/JTAG on that port. Keep a **UART USB‑serial adapter** wired for console/flash recovery, or use OTA while the module is on the network. Switching back to Serial/JTAG restores direct USB flashing.

Pass-through USB‑C jacks on the reference case are for **powering** pedals/devices; they are not the ESP data port.

### Bluetooth MIDI and HID

Open **Bluetooth** in the web UI to scan, pair, connect, disconnect, and forget devices. Peers can carry MIDI, HID (page turner), or both, depending on the pedal. Pairing uses the usual Bluetooth slots on the module; forget a device there if you need to re-pair.

### Actions

Actions are *when → (optional if) → then* rules stored on the module and edited in the web UI.

**Triggers** can be:

- TRS footswitch gestures (press, long press, short/long release, double)
- Front-panel buttons
- MIDI (notes, CC, PC, and friends on any enabled port)
- Showbox entity / packet changes (e.g. looper state)
- Connection up/down events
- Variable changes

**Then** steps can:

- Change Showbox controls, run the looper, recall snapshots, toggle SD record, or open the tuner
- Apply a **gain boost** (manual or auto-with-looper)
- Send MIDI out any port
- Emit HID (keys, media keys, mouse, touch)
- Flip module radios / USB mode
- Set variables used by other rules

Groups of rules can be enabled or disabled together. Built-in groups ship ready for a TRS or MIDI looper pedal, a MIDI/TRS booster, and the front-panel buttons. Edit or replace them as you like.

### Firmware updates

Three supported paths:

1. **[Web Installer](https://t-vk.github.io/ShowboxExpansionModule/)** in Chrome or another Chromium browser (WebUSB). Put the ESP USB‑C in Serial/JTAG mode, connect it, and flash from the page.
2. **OTA** while the module is on your network: ArduinoOTA on UDP port `3232`. With PlatformIO, use env `esp32-s3-n16r8-ota` (`upload_port = showbox.local`). Network serial monitor can use `socket://showbox.local:23`.
3. **USB serial** with PlatformIO env `esp32-s3-n16r8` (default upload port `/dev/ttyACM0` on Linux). If the port is in MIDI/HID gadget mode, use the UART adapter or OTA instead.

### Getting started

Build a minimal ESP32 + two RJ45 breakouts, or the full reference design:

- [Parts List](./PartsList.md)
- [Schematics](https://raw.githubusercontent.com/T-vK/ShowboxExpansionModule/refs/heads/main/docs/schematics.svg)
- [Pin Mappings](#pin-mappings-for-esp32-s3)
- [3D Model](https://grabcad.com/library/showboxexpansionmodule-v1-1)
- [Firmware Web Installer](https://t-vk.github.io/ShowboxExpansionModule/)

#### Build with PlatformIO

```bash
pio run -e esp32-s3-n16r8 -t upload
pio run -e esp32-s3-n16r8-ota -t upload   # when networked; cable unplugged from Espressif USB JTAG
```

The default board is the ESP32‑S3 N16R8 DevKit. A larger NVS partition overlay is already selected in `platformio.ini` so Wi‑Fi, BLE, and action data fit.

### Hardware

Everything is off-the-shelf (AliExpress, eBay, etc.): ESP32-S3, RJ45 breakouts, optional MIDI DIN, W5500 Ethernet, TRS jacks, and a 3D-printable case. The reference design wires all supported features; a minimal build only needs the ESP and the two Showbox links.

Front-panel colors on the reference design: green ≈ Wi‑Fi, red ≈ USB, blue ≈ Bluetooth (behavior is customizable via Actions). White is a latching power button (not an ESP GPIO).

#### Schematics

[![Interceptor Schematics](./docs/schematics.svg)](https://raw.githubusercontent.com/T-vK/ShowboxExpansionModule/refs/heads/main/docs/schematics.svg)

#### Photos of the reference design

[<img src="./docs/expansion-module-slid-in.jpeg" alt="Expansion Module Slid In" width="200"/>](./docs/expansion-module-slid-in.jpeg) [<img src="./docs/expansion-module-top-open.jpeg" alt="Expansion Module Top Open" width="200"/>](./docs/expansion-module-top-open.jpeg)  
[<img src="./docs/expansion-module-front-open.jpeg" alt="Expansion Module Front Open" width="200"/>](./docs/expansion-module-front-open.jpeg) [<img src="./docs/expansion-module-open-wired.jpeg" alt="Expansion Module Open Wired" width="200"/>](./docs/expansion-module-open-wired.jpeg)

## Pin Mappings for ESP32-S3

Note: If you are using an ESP32 instead of an ESP32-S3, adjust the pin mappings accordingly.

### SPI Ethernet Module (Mini W5500)

```
µC Pin | Module Pin
-------------------
 6     | nReset
 7     | MISO
15     | nINT
16     | nSS
17     | SCLK
18     | MOSI
```

### TRS Jacks (For Footswitches / Expression Pedals)

```
µC Pin | Module Pin
-------------------
 8     | TRS 1 Tip
 2     | TRS 1 Ring
21     | TRS 2 Tip
 1     | TRS 2 Ring
47     | TRS 3 Tip
 4     | TRS 3 Ring
48     | TRS 4 Tip
 5     | TRS 4 Ring
```

### MIDI UART Module (Arduino MIDI Shield)

```
µC Pin | Module Pin
-------------------
 9     | TX
10     | RX
```

### Front-panel LED buttons

White is a latching power button on the reference design (not an ESP GPIO).
Red / green / blue are momentary buttons; their LEDs are simple on/off indicators
(one P9813 drives the three channels; not per-button RGB rings).

```
µC Pin | Module Pin
-------------------
42     | P9813 Data In
41     | P9813 Clock In
40     | Green Button (Wi‑Fi)
39     | Red Button (USB)
38     | Blue Button (Bluetooth)
```

### Mackie Showbox Base-Unit

```
µC Pin | Module Pin
-------------------
11     | TX
12     | RX
```

### Mackie Showbox Breakaway Mixer

```
µC Pin | Module Pin
-------------------
13     | RX
14     | TX
```
