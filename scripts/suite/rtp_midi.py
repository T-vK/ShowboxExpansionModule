"""Minimal AppleMIDI / RTP-MIDI session: invite, clock-sync, send a CC, bye."""

from __future__ import annotations

import random
import socket
import struct
import time
from typing import Optional


class RtpMidi:
    def __init__(self, ip: str, control_port: int = 5004) -> None:
        self.ip = ip
        self.control_port = control_port
        self.data_port = control_port + 1
        self.token = random.randint(1, 0xFFFFFFFE)
        self.ssrc = random.randint(1, 0xFFFFFFFE)
        self.peer_ssrc = 0
        self.seq = 1
        self.ctrl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.data = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.ctrl.bind(("", 0))
        self.data.bind(("", 0))
        self.ctrl.settimeout(2.0)
        self.data.settimeout(2.0)
        self._open = False

    def connect(self) -> str:
        ok = self._invite(self.ctrl, self.control_port)
        if ok not in ("OK",):
            raise RuntimeError(f"AppleMIDI control invite got {ok or 'timeout'}")
        ok = self._invite(self.data, self.data_port)
        if ok not in ("OK",):
            raise RuntimeError(f"AppleMIDI data invite got {ok or 'timeout'}")
        self._clock_sync()
        self._open = True
        return f"ssrc={self.peer_ssrc:#x}"

    def _invite(self, sock: socket.socket, dest_port: int) -> str:
        pkt = struct.pack("!HHIII", 0xFFFF, 0x494E, 2, self.token, self.ssrc) + b"showbox-suite\x00"
        sock.sendto(pkt, (self.ip, dest_port))
        try:
            data, _addr = sock.recvfrom(512)
        except socket.timeout:
            return ""
        if len(data) < 16 or data[0] != 0xFF or data[1] != 0xFF:
            return f"junk:{data[:8]!r}"
        cmd = data[2:4].decode("ascii", "replace")
        if cmd == "OK" and len(data) >= 16:
            self.peer_ssrc = struct.unpack("!I", data[12:16])[0]
        return cmd

    def _clock_sync(self) -> None:
        now = int(time.time() * 10000) & 0xFFFFFFFFFFFFFFFF
        pkt = struct.pack("!HHIBBBBQQQ", 0xFFFF, 0x434B, self.ssrc, 0, 0, 0, 0, now, 0, 0)
        self.data.sendto(pkt, (self.ip, self.data_port))
        try:
            data, _addr = self.data.recvfrom(512)
        except socket.timeout:
            return
        if len(data) < 36 or data[2:4] != b"CK":
            return
        count, t1 = data[8], struct.unpack("!Q", data[12:20])[0]
        if count != 1:
            return
        now2 = int(time.time() * 10000) & 0xFFFFFFFFFFFFFFFF
        reply = struct.pack("!HHIBBBBQQQ", 0xFFFF, 0x434B, self.ssrc, 2, 0, 0, 0, t1, now2, 0)
        self.data.sendto(reply, (self.ip, self.data_port))

    def cc(self, number: int, value: int = 127, channel: int = 1) -> None:
        if not self._open:
            raise RuntimeError("AppleMIDI session is not open")
        midi = bytes([0xB0 | (channel - 1), number & 0x7F, value & 0x7F])
        # RTP: V=2, M=0, PT=97 (AppleMIDI). M=1 is ignored by this stack.
        header = struct.pack("!BBHII", 0x80, 0x61, self.seq & 0xFFFF, self.seq * 100, self.ssrc)
        self.seq = (self.seq + 1) & 0xFFFF
        cmd_hdr = len(midi) & 0x0F
        self.data.sendto(header + bytes([cmd_hdr]) + midi, (self.ip, self.data_port))

    def close(self) -> None:
        if self._open:
            bye = struct.pack("!HHIII", 0xFFFF, 0x4259, 2, self.token, self.ssrc)
            try:
                self.ctrl.sendto(bye, (self.ip, self.control_port))
                self.data.sendto(bye, (self.ip, self.data_port))
            except OSError:
                pass
        self._open = False
        self.ctrl.close()
        self.data.close()
