"""Send BLE MIDI through the PipeWire graph.

WirePlumber's bluez-midi monitor is the one process that AcquireWrites the
characteristic. This opens an ALSA sequencer port, waits until that monitor
has published bluez_midi.<address> and holds the write, links the port to
that node's input, and sends a normal MIDI CC. The local bluez_midi.server
node is a different device and is never the target.

bluetoothd --noplugin=midi must stay set so bluetoothd's MIDI plugin does
not take the write before WirePlumber.
"""

from __future__ import annotations

import ctypes
import json
import os
import subprocess
import time
import urllib.request

_CLIENT = "showbox-suite"
_SEQ_OPEN_DUPLEX = 3
_PORT_CAP_READ = 1
_PORT_CAP_SUBS_READ = 32
_PORT_TYPE_MIDI = 2
_EVENT_CONTROLLER = 10
_QUEUE_DIRECT = 253
_ADDR_SUBSCRIBERS = 254
_ADDR_UNKNOWN = 253


class _SeqEvent(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_ubyte),
        ("flags", ctypes.c_ubyte),
        ("tag", ctypes.c_ubyte),
        ("queue", ctypes.c_ubyte),
        ("time", ctypes.c_uint32 * 2),
        ("source_client", ctypes.c_ubyte),
        ("source_port", ctypes.c_ubyte),
        ("dest_client", ctypes.c_ubyte),
        ("dest_port", ctypes.c_ubyte),
        ("channel", ctypes.c_ubyte),
        ("unused", ctypes.c_ubyte * 3),
        ("param", ctypes.c_uint),
        ("value", ctypes.c_int),
    ]


def _seq():
    lib = ctypes.CDLL("libasound.so.2")
    lib.snd_seq_open.argtypes = [
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_int,
    ]
    lib.snd_seq_open.restype = ctypes.c_int
    lib.snd_seq_set_client_name.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.snd_seq_set_client_name.restype = ctypes.c_int
    lib.snd_seq_create_simple_port.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_uint,
        ctypes.c_uint,
    ]
    lib.snd_seq_create_simple_port.restype = ctypes.c_int
    lib.snd_seq_client_id.argtypes = [ctypes.c_void_p]
    lib.snd_seq_client_id.restype = ctypes.c_int
    lib.snd_seq_event_output_direct.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.snd_seq_event_output_direct.restype = ctypes.c_int
    lib.snd_seq_close.argtypes = [ctypes.c_void_p]
    lib.snd_seq_close.restype = ctypes.c_int
    return lib


def _node_name(address: str) -> str:
    return "bluez_midi." + (address or "").replace(":", "_").lower()


def _graph() -> tuple[list[dict], list[dict]]:
    try:
        dump = json.loads(subprocess.check_output(["pw-dump"], timeout=8))
    except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError) as exc:
        raise RuntimeError(f"pw-dump failed: {exc}") from exc
    nodes: list[dict] = []
    ports: list[dict] = []
    for obj in dump:
        props = dict(((obj.get("info") or {}).get("props")) or {})
        props["_id"] = obj.get("id")
        kind = str(obj.get("type") or "")
        if kind.endswith("Node"):
            nodes.append(props)
        elif kind.endswith("Port"):
            ports.append(props)
    return nodes, ports


def _client_node(nodes: list[dict], address: str) -> dict | None:
    want = _node_name(address)
    for node in nodes:
        name = str(node.get("node.name") or "")
        role = str(node.get("api.bluez5.role") or "")
        if role == "server" or name.lower() == "bluez_midi.server":
            continue
        if name.lower() == want:
            return node
    return None


def _input_port(ports: list[dict], node: dict) -> dict | None:
    for port in ports:
        if port.get("node.id") == node.get("_id") and port.get("port.direction") == "in":
            return port
    return None


def _suite_port(ports: list[dict]) -> dict | None:
    prefix = _CLIENT + ":"
    for port in ports:
        name = str(port.get("port.name") or "")
        if name.startswith(prefix) and port.get("port.direction") == "out":
            return port
    return None


def _ble_rx() -> int | None:
    """GATT write count. The MIDI status cache refreshes about every 400 ms."""
    ip = os.environ.get("SHOWBOX_IP", "192.168.4.1").strip() or "192.168.4.1"
    try:
        with urllib.request.urlopen(f"http://{ip}/api/v1/midi", timeout=1.0) as resp:
            raw = resp.read()
        if raw[:2] == b"\x1f\x8b":
            import gzip
            raw = gzip.decompress(raw)
        data = json.loads(raw.decode("utf-8", "replace"))
    except (OSError, json.JSONDecodeError, TimeoutError):
        return None
    if "bleMidiRx" not in data:
        return None
    try:
        return int(data["bleMidiRx"])
    except (TypeError, ValueError):
        return None


def release_client_node(address: str) -> None:
    """Drop PipeWire's client node before the ACL goes down.

    The node is what holds AcquireWrite. Disconnecting while that node
    still exists leaves WriteAcquired set, and the next connect's node
    accepts a link that never writes. Wait until this node is gone.
    """
    try:
        nodes, _ports = _graph()
    except RuntimeError:
        return
    node = _client_node(nodes, address)
    if node is None or node.get("_id") is None:
        return
    gone_id = node.get("_id")
    subprocess.run(
        ["pw-cli", "destroy", str(gone_id)],
        capture_output=True,
        timeout=3,
        check=False,
    )
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        try:
            nodes, _ports = _graph()
        except RuntimeError:
            return
        current = _client_node(nodes, address)
        if current is None or current.get("_id") != gone_id:
            return
        time.sleep(0.05)


class BleMidiOut:
    def __init__(self) -> None:
        self.address = ""
        self._port_name = ""
        self._lib = None
        self._seq = None
        self._port = -1
        self._client = -1
        self._link: tuple[int, int] | None = None

    @property
    def connected(self) -> bool:
        return self._seq is not None and self._port >= 0 and self._link is not None

    def start(self, address: str, timeout: float = 8.0) -> None:
        if self.connected and self.address.lower() == address.lower():
            return
        self.close()
        self._open_seq()
        try:
            self._wait_link(address, timeout)
            self.address = address
            # CC 20 is not a looper or boost rule. PipeWire drops the event
            # that wakes a suspended bluez-midi node, and a musical CC sent
            # in that same burst is the one that disappears. Return only
            # after the module has counted a probe.
            self._prove_write()
        except Exception:
            self.close()
            raise

    def cc(self, number: int, value: int = 127, channel: int = 1) -> None:
        if not self.connected:
            raise RuntimeError("BLE MIDI is not connected")
        self._emit(number, value, channel)

    def _emit(self, number: int, value: int, channel: int = 1) -> None:
        ev = _SeqEvent()
        ev.type = _EVENT_CONTROLLER
        ev.queue = _QUEUE_DIRECT
        ev.source_client = self._client
        ev.source_port = self._port
        ev.dest_client = _ADDR_SUBSCRIBERS
        ev.dest_port = _ADDR_UNKNOWN
        ev.channel = (channel - 1) & 0x0F
        ev.param = number & 0x7F
        ev.value = value & 0x7F
        wrote = self._lib.snd_seq_event_output_direct(self._seq, ctypes.byref(ev))
        if wrote < 0:
            raise RuntimeError(f"BLE MIDI short write {wrote}")

    def _prove_write(self) -> None:
        before = _ble_rx()
        # One slow GET is not a missing counter.
        arm = time.monotonic() + 0.4
        while before is None and time.monotonic() < arm:
            time.sleep(0.05)
            before = _ble_rx()
        if before is None:
            raise RuntimeError("BLE MIDI probe needs a live bleMidiRx on /midi")
        self._emit(20, 0)
        # An idle bluez-midi node drops the write that wakes it, and the
        # module's MIDI JSON lags that write by a few hundred ms. A second
        # probe at 120 ms was still inside the wake. Space them out.
        deadline = time.monotonic() + 3.5
        resent = 0
        next_at = time.monotonic() + 0.6
        while time.monotonic() < deadline:
            now = _ble_rx()
            if now is not None and now > before:
                return
            if resent < 2 and time.monotonic() >= next_at:
                self._emit(20, 0)
                resent += 1
                next_at = time.monotonic() + 0.9
            time.sleep(0.05)
        raise RuntimeError(
            f"BLE MIDI probe was not received (rx {before} -> {_ble_rx()})"
        )

    def close(self) -> None:
        link = self._link
        self._link = None
        if link is not None:
            subprocess.run(
                ["pw-link", "-d", str(link[0]), str(link[1])],
                capture_output=True,
                timeout=3,
                check=False,
            )
        seq = self._seq
        lib = self._lib
        self._seq = None
        self._port = -1
        self._client = -1
        self.address = ""
        self._port_name = ""
        if seq is not None and lib is not None:
            lib.snd_seq_close(seq)

    def _open_seq(self) -> None:
        lib = _seq()
        handle = ctypes.c_void_p()
        rc = lib.snd_seq_open(ctypes.byref(handle), b"default", _SEQ_OPEN_DUPLEX, 0)
        if rc != 0 or not handle.value:
            raise RuntimeError(f"snd_seq_open failed ({rc})")
        if lib.snd_seq_set_client_name(handle, _CLIENT.encode()) != 0:
            lib.snd_seq_close(handle)
            raise RuntimeError("snd_seq_set_client_name failed")
        port = lib.snd_seq_create_simple_port(
            handle,
            b"out",
            _PORT_CAP_READ | _PORT_CAP_SUBS_READ,
            _PORT_TYPE_MIDI,
        )
        if port < 0:
            lib.snd_seq_close(handle)
            raise RuntimeError(f"snd_seq_create_simple_port failed ({port})")
        self._lib = lib
        self._seq = handle
        self._port = int(port)
        self._client = int(lib.snd_seq_client_id(handle))

    def _wait_link(self, address: str, timeout: float) -> None:
        # PipeWire emits the client node only after BlueZ ServicesResolved.
        # A quiet reconnect finishes in about 5 s. With AIRSTEP up, the HID
        # walk has been measured out to about 11 s. The wait ends when the
        # node appears.
        want = _node_name(address)
        deadline = time.monotonic() + timeout
        last = f"PipeWire node {want} did not appear"
        while time.monotonic() < deadline:
            nodes, ports = _graph()
            node = _client_node(nodes, address)
            src = _suite_port(ports)
            if node is None:
                last = f"PipeWire node {want} did not appear"
            elif src is None:
                last = "showbox-suite MIDI port is not on the Midi-Bridge"
            else:
                dst = _input_port(ports, node)
                if dst is None:
                    last = f"{want} has no input port"
                else:
                    self._connect(int(src["_id"]), int(dst["_id"]), str(node.get("node.name") or want))
                    return
            time.sleep(0.05)
        raise RuntimeError(last)

    def _connect(self, src: int, dst: int, name: str) -> None:
        proc = subprocess.run(
            ["pw-link", str(src), str(dst)],
            capture_output=True,
            text=True,
            timeout=3,
            check=False,
        )
        if proc.returncode != 0 and "exists" not in (proc.stderr or "").lower():
            detail = (proc.stderr or proc.stdout or "").strip()
            raise RuntimeError(f"pw-link {src} {dst} failed: {detail}")
        self._link = (src, dst)
        self._port_name = name
