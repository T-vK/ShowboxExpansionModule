## Showbox Expansion Module

Gain more control over your Mackie Showbox. An ESP32-S3 sits between the Showbox base unit and the Breakaway Mixer, so you can drive presets, volume, EQ, effects, the looper, and more from footswitches, expression pedals, MIDI controllers, HID devices, or a browser on your phone or laptop.

### Features

- **Full mixer control** — read and change essentially everything the Breakaway Mixer exposes (gains, faders, mute, EQ, compressors, FX sends/types, looper, snapshots, tuner, SD record, and more)
- **Web UI (PWA)** — live mixer board, snapshots, tuner, looper, actions editor, and connection settings over Wi‑Fi or Ethernet at `http://showbox.local`
- **Actions engine** — map any trigger to any reaction in the browser (no firmware rebuild). Built-ins cover TRS/MIDI looper control, gain booster, and the front-panel buttons
- **Footswitches & expression pedals** — up to 4 dual TRS jacks (tip + ring per jack)
- **MIDI everywhere** — 5‑pin DIN, Bluetooth LE (host + peripheral), USB device + host, AppleMIDI / RTP‑MIDI over Wi‑Fi, and Ethernet MIDI (W5500)
- **Bluetooth & USB HID** — keyboard, mouse, absolute pointer, and touch for page-turning and similar uses
- **Behringer X‑Touch Compact** — MIDI surface mirroring for hands-on Showbox control
- **Radios & networking** — Wi‑Fi station/AP, captive portal setup, mDNS, optional Ethernet
- **Front panel** — RGB status rings plus blue / green / red buttons for Bluetooth, Wi‑Fi, and USB mode
- **Updates** — browser [Web Installer](https://t-vk.github.io/ShowboxExpansionModule/), OTA over the network

### Web interface

Open the module on your LAN (typically `http://showbox.local`) for a live mixer, snapshots, tuner, looper, and system pages.

![Web UI mixer](./docs/webui-mixer.gif)

The **Actions** page is where you wire footswitches, MIDI, panel buttons, and Showbox state changes to whatever should happen next:

![Web UI actions](./docs/webui-actions.png)

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

Groups of rules can be enabled or disabled together. Built-in groups ship ready for a TRS or MIDI looper pedal, a MIDI/TRS booster, and the front-panel buttons — edit or replace them as you like.

### Getting started

Build a minimal ESP32 + two RJ45 breakouts, or the full reference design:

- [Parts List](./PartsList.md)
- [Schematics](https://raw.githubusercontent.com/T-vK/ShowboxExpansionModule/refs/heads/main/docs/schematics.svg)
- [Pin Mappings](#pin-mappings-for-esp32-s3)
- [3D Model](https://grabcad.com/library/showboxexpansionmodule-v1-1)
- [Firmware Web Installer](https://t-vk.github.io/ShowboxExpansionModule/)

Firmware can also be built and flashed with PlatformIO.

### Hardware

Everything is off-the-shelf (AliExpress, eBay, etc.): ESP32-S3, RJ45 breakouts, optional MIDI DIN, W5500 Ethernet, TRS jacks, and a 3D-printable case. The reference design wires all supported features; a minimal build only needs the ESP and the two Showbox links.

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

### RGB LED Controller (P9813)

```
µC Pin | Module Pin
-------------------
42     | Data In
41     | Clock In
```

### Buttons

```
µC Pin | Module Pin
-------------------
40     | Green Button
39     | Red Button
38     | Blue Button
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
