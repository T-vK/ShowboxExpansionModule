#!/usr/bin/env python3
"""Retired from the live suite.

POST /api/v1/midi/inject is a firmware debug hook that never touches BLE or
RTP. It is not a test. Use:

  python3 scripts/run.py --case ble-midi
  python3 scripts/run.py --case wifi-midi
"""

from __future__ import annotations

import sys


def main() -> int:
    print("test_midi_inject_fast.py is retired.")
    print("MIDI tests now send real BLE GATT / AppleMIDI RTP via scripts/run.py")
    print("  python3 scripts/run.py --case ble-midi")
    print("  python3 scripts/run.py --case wifi-midi")
    return 0


if __name__ == "__main__":
    sys.exit(main())
