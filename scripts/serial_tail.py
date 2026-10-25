"""Tail the module's USB serial log, tolerating re-enumeration."""
import sys
import time

import serial

PORT = "/dev/ttyACM0"


def main() -> None:
    while True:
        try:
            with serial.Serial(PORT, 115200, timeout=1) as ser:
                while True:
                    data = ser.read(4096)
                    if data:
                        sys.stdout.write(data.decode("utf-8", "replace"))
                        sys.stdout.flush()
        except Exception as exc:
            sys.stderr.write(f"[reconnect] {exc}\n")
            time.sleep(1.0)


if __name__ == "__main__":
    main()