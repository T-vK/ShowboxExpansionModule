Import("env")

# Cable in = USB JTAG. The OTA env is only for when that port is gone.
# `pio run -e esp32-s3-n16r8-ota -t upload` with 303a:1001 present is wrong.

import os
import subprocess
import sys

try:
    from SCons.Script import COMMAND_LINE_TARGETS
except ImportError:
    COMMAND_LINE_TARGETS = []


USB_JTAG_VIDPID = "303a:1001"
USB_JTAG_PORT = "/dev/ttyACM0"


def _usb_jtag_present():
    try:
        out = subprocess.run(
            ["lsusb"],
            capture_output=True,
            text=True,
            timeout=2,
            check=False,
        ).stdout or ""
        if USB_JTAG_VIDPID in out.lower():
            return True
    except (OSError, subprocess.TimeoutExpired):
        pass
    return os.path.exists(USB_JTAG_PORT)


def _refuse_ota_while_usb(source=None, target=None, env=None):
    pioenv = (env or globals().get("env")).get("PIOENV", "")
    if "ota" not in pioenv.lower():
        return
    if not _usb_jtag_present():
        return
    sys.stderr.write(
        "\nUSB JTAG is plugged in (%s %s).\n"
        "Do not OTA. Flash the USB env:\n"
        "  pio run -e esp32-s3-n16r8 -t upload --upload-port %s\n\n"
        % (USB_JTAG_VIDPID, USB_JTAG_PORT, USB_JTAG_PORT)
    )
    (env or globals().get("env")).Exit(1)


if "upload" in COMMAND_LINE_TARGETS:
    _refuse_ota_while_usb(env=env)

env.AddPreAction("upload", _refuse_ota_while_usb)
