"""Send real BLE MIDI CCs through BlueZ GATT AcquireWrite.

PipeWire's bluez_midi node steals the MIDI characteristic (Write acquired),
so rtmidi links look live while packets never reach the ESP. Drop that
node, AcquireWrite, and keep the fd for the suite.

bluetoothd --noplugin=midi must stay set (do not revert that).
"""

from __future__ import annotations

import json
import os
import subprocess
import time

MIDI_CHAR_UUID = "7772e5db-3868-4112-a1a9-f2669d106bf3"


def _pw_destroy_node(address: str) -> None:
    compact = (address or "").replace(":", "_").upper()
    compact_l = compact.lower()
    try:
        dump = json.loads(subprocess.check_output(["pw-dump"], timeout=8))
    except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError):
        return
    for obj in dump:
        props = ((obj.get("info") or {}).get("props")) or {}
        name = str(props.get("node.name") or "").lower()
        ident = obj.get("id")
        if ident is None:
            continue
        if "midi-bridge" in name:
            continue
        # bluez_midi.server has no Showbox/MAC in the name but still steals
        # AcquireWrite the instant the LE link comes up (L3 streak4 gate:
        # Write acquired after flash / adapter cycle until this node dies).
        hit = (
            compact.lower() in name
            or compact_l in name
            or "bluez_midi.server" in name
            or (
                "bluez_midi" in name
                and ("showbox" in name or compact_l[-5:] in name)
            )
        )
        if not hit:
            continue
        subprocess.run(
            ["pw-cli", "destroy", str(ident)],
            capture_output=True,
            timeout=3,
            check=False,
        )


def _ble_midi_cc(cc: int, value: int = 127, channel: int = 1) -> bytes:
    ts = int(time.monotonic() * 1000) & 0x1FFF
    header = 0x80 | ((ts >> 7) & 0x3F)
    tslow = 0x80 | (ts & 0x7F)
    return bytes([header, tslow, 0xB0 | ((channel - 1) & 0x0F), cc & 0x7F, value & 0x7F])


class BleMidiOut:
    def __init__(self) -> None:
        self.address = ""
        self._port_name = ""
        self._char = None
        self._fd: int | None = None
        self._unix_fd = None
        # PipeWire bluez_midi can hold WriteAcquired forever; WriteValue still
        # delivers BLE MIDI CCs to the ESP (L1 ble-midi after SoftAP yield).
        self._write_value_only = False

    @property
    def connected(self) -> bool:
        if self._char is None:
            return False
        if self._write_value_only:
            return True
        return self._fd is not None and self._fd > 2

    def start(self, address: str, timeout: float = 25.0) -> None:
        if self.connected and self.address.lower() == address.lower():
            return
        self.close()
        import dbus

        self.address = address
        path = _wait_char_path(address, timeout)
        bus = dbus.SystemBus()
        obj = bus.get_object("org.bluez", path)
        char = dbus.Interface(obj, "org.bluez.GattCharacteristic1")
        try:
            char.StartNotify()
        except dbus.exceptions.DBusException:
            pass
        fd, unix_fd, last = _acquire_fd(char, address)
        self._char = char
        self._port_name = path
        if fd is not None and fd > 2:
            self._fd = fd
            self._unix_fd = unix_fd
            self._write_value_only = False
        else:
            # Write acquired by PipeWire/wireplumber — still send via WriteValue.
            err = str(last or "")
            if "Write acquired" not in err and "NotPermitted" not in err:
                self._char = None
                self._port_name = ""
                raise RuntimeError(f"BLE MIDI AcquireWrite failed on {path}: {last}")
            self._fd = None
            self._unix_fd = None
            self._write_value_only = True
        self.cc(20, 0)

    def cc(self, number: int, value: int = 127, channel: int = 1) -> None:
        if self._char is None:
            raise RuntimeError("BLE MIDI is not connected")
        pkt = _ble_midi_cc(number, value, channel)
        last = "BLE MIDI write failed"
        import dbus

        if not self._write_value_only:
            for attempt in range(3):
                _pw_destroy_node(self.address)
                if self._fd is None or self._fd <= 2:
                    try:
                        self._reacquire()
                    except Exception as exc:
                        last = str(exc)
                        continue
                try:
                    n = os.write(self._fd, pkt)
                    if n == len(pkt):
                        return
                    last = f"BLE MIDI short write {n}/{len(pkt)}"
                except OSError as exc:
                    last = str(exc)
                try:
                    self._reacquire()
                except Exception as exc:
                    last = str(exc)
                    if "Write acquired" in last or "NotPermitted" in last:
                        self._write_value_only = True
                        break
        payload = [dbus.Byte(b) for b in pkt]
        for _ in range(3):
            try:
                self._char.WriteValue(payload, {"type": "command"})
                return
            except dbus.exceptions.DBusException as exc:
                last = str(exc)
                time.sleep(0.05)
        raise RuntimeError(last)

    def _reacquire(self) -> None:
        if self._char is None:
            raise RuntimeError("BLE MIDI is not connected")
        self._drop_fd()
        fd, unix_fd, last = _acquire_fd(self._char, self.address)
        if fd is None or fd <= 2:
            err = str(last or "")
            if "Write acquired" in err or "NotPermitted" in err:
                self._write_value_only = True
                return
            raise RuntimeError("BLE MIDI AcquireWrite failed")
        self._fd = fd
        self._unix_fd = unix_fd
        self._write_value_only = False

    def _drop_fd(self) -> None:
        fd = self._fd
        self._fd = None
        self._unix_fd = None
        if fd is not None and fd > 2:
            try:
                os.close(fd)
            except OSError:
                pass

    def close(self) -> None:
        char = self._char
        self._drop_fd()
        self._write_value_only = False
        if char is not None:
            try:
                char.ReleaseWrite()
            except Exception:
                pass
        self._char = None
        self.address = ""
        self._port_name = ""


def _dev_path(address: str) -> str:
    return "/org/bluez/hci0/dev_" + (address or "").replace(":", "_").upper()


def _write_acquired(address: str) -> bool:
    import dbus

    try:
        bus = dbus.SystemBus()
        path = _wait_char_path(address, 2.0)
        props = dbus.Interface(
            bus.get_object("org.bluez", path), "org.freedesktop.DBus.Properties"
        )
        return bool(props.Get("org.bluez.GattCharacteristic1", "WriteAcquired"))
    except Exception:
        return False


def _bounce_acl_for_write(address: str) -> None:
    """Drop ACL so a stuck WriteAcquired (orphaned PipeWire FD) can clear."""
    import dbus

    _pw_destroy_node(address)
    bus = dbus.SystemBus()
    dev_path = _dev_path(address)
    try:
        dev = dbus.Interface(bus.get_object("org.bluez", dev_path), "org.bluez.Device1")
        dev.Disconnect()
    except Exception:
        pass
    end = time.monotonic() + 6.0
    while time.monotonic() < end:
        if not _write_acquired(address):
            break
        time.sleep(0.2)
    try:
        dev = dbus.Interface(bus.get_object("org.bluez", dev_path), "org.bluez.Device1")
        dev.Connect()
    except Exception:
        pass
    time.sleep(0.6)


def _acquire_fd(char, address: str):
    import dbus

    last = None
    bounced = False
    for attempt in range(12):
        _pw_destroy_node(address)
        try:
            unix_fd, _mtu = char.AcquireWrite({})
            fd = int(unix_fd.take())
            return fd, unix_fd, None
        except dbus.exceptions.DBusException as exc:
            last = exc
            err = str(exc)
            if (
                not bounced
                and attempt >= 3
                and ("Write acquired" in err or "NotPermitted" in err)
            ):
                bounced = True
                try:
                    _bounce_acl_for_write(address)
                    char_path = _wait_char_path(address, 12.0)
                    bus = dbus.SystemBus()
                    char = dbus.Interface(
                        bus.get_object("org.bluez", char_path),
                        "org.bluez.GattCharacteristic1",
                    )
                except Exception as bounce_exc:
                    last = bounce_exc
            time.sleep(0.08)
    return None, None, last


def _wait_char_path(address: str, timeout: float) -> str:
    import dbus

    bus = dbus.SystemBus()
    dev_path = _dev_path(address)
    deadline = time.monotonic() + timeout
    last = f"no MIDI characteristic on {address}"
    while time.monotonic() < deadline:
        try:
            props = dbus.Interface(
                bus.get_object("org.bluez", dev_path),
                "org.freedesktop.DBus.Properties",
            )
            if not bool(props.Get("org.bluez.Device1", "Connected")):
                last = f"{address} not connected"
                time.sleep(0.05)
                continue
        except dbus.exceptions.DBusException as exc:
            last = str(exc)
            time.sleep(0.05)
            continue
        om = dbus.Interface(bus.get_object("org.bluez", "/"), "org.freedesktop.DBus.ObjectManager")
        objs = om.GetManagedObjects()
        for path, ifaces in objs.items():
            char = ifaces.get("org.bluez.GattCharacteristic1")
            if char is None:
                continue
            uuid = str(char.get("UUID") or "").lower()
            # L1 22:49 CET: this object was on the bus for the whole 20s
            # wait while ServicesResolved stayed false (HID discovery still
            # walking under AIRSTEP). AcquireWrite only needs the char.
            # Waiting out the flag made the next connect hit BlueZ InProgress.
            if MIDI_CHAR_UUID in uuid and str(path).startswith(dev_path):
                return str(path)
        time.sleep(0.05)
    raise RuntimeError(last)
