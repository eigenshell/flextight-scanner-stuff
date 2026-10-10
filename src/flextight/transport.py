"""SCSI transport layer for talking to the scanner.

Two implementations share a small command interface:

- :class:`SGIOTransport` issues real ``SG_IO`` ioctls against an open
  ``/dev/sgX`` file descriptor.  The v3 ``sg_io_hdr`` struct below is
  mirrored field-for-field from ``/usr/include/scsi/sg.h``; on x86-64 it
  is 88 bytes (the 32-bit layout is 60), and the kernel parses it by its
  own C layout, so any drift here silently corrupts every pointer and
  status field.

- :class:`FakeTransport` replays an explicit script of :class:`Step`
  expectations so all protocol code can be developed and tested without
  hardware.  Trace-derived loaders are deferred until the IOCTL payload
  layout is pinned by hardware captures.
"""

from __future__ import annotations

import ctypes
import fcntl
import os
from dataclasses import dataclass

SG_IO = 0x2285

SG_DXFER_NONE = 0
SG_DXFER_TO_DEV = -2
SG_DXFER_FROM_DEV = -3

_DIRECTIONS = {"from": SG_DXFER_FROM_DEV, "to": SG_DXFER_TO_DEV, "none": SG_DXFER_NONE}

_SENSE_LEN = 64
_TIMEOUT_MS = 20000  # 20 s -- scans are slow


class TransportError(Exception):
    """Raised for ioctl failures, SCSI status errors, and script violations."""


class _SgIoHdr(ctypes.Structure):
    """``struct sg_io_hdr`` (v3) from /usr/include/scsi/sg.h, x86-64 layout."""

    _fields_ = [
        ("interface_id", ctypes.c_int),
        ("dxfer_direction", ctypes.c_int),
        ("cmd_len", ctypes.c_ubyte),
        ("mx_sb_len", ctypes.c_ubyte),
        ("iovec_count", ctypes.c_ushort),
        ("dxfer_len", ctypes.c_uint),
        ("dxferp", ctypes.c_void_p),
        ("cmdp", ctypes.c_void_p),
        ("sbp", ctypes.c_void_p),
        ("timeout", ctypes.c_uint),
        ("flags", ctypes.c_uint),
        ("pack_id", ctypes.c_int),
        ("usr_ptr", ctypes.c_void_p),
        ("status", ctypes.c_ubyte),
        ("masked_status", ctypes.c_ubyte),
        ("msg_status", ctypes.c_ubyte),
        ("sb_len_wr", ctypes.c_ubyte),
        ("host_status", ctypes.c_ushort),
        ("driver_status", ctypes.c_ushort),
        ("resid", ctypes.c_int),
        ("duration", ctypes.c_uint),
        ("info", ctypes.c_uint),
    ]


def _norm_cdb(cdb: bytes | str) -> bytes:
    return bytes.fromhex(cdb) if isinstance(cdb, str) else cdb


class SGIOTransport:
    """Real SG_IO ioctl transport over an open /dev/sgX file descriptor."""

    def __init__(self, fd: int):
        self.fd = fd
        self._bufs: tuple = ()

    @classmethod
    def open(cls, path: str) -> "SGIOTransport":
        try:
            fd = os.open(path, os.O_RDWR)
        except OSError as e:
            raise TransportError(f"cannot open {path}: {e}") from e
        return cls(fd)

    def _build_header(self, cdb: bytes, dxfer_len: int, dxfer: str) -> _SgIoHdr:
        cdb_buf = ctypes.create_string_buffer(cdb.ljust(16, b"\0"), 16)
        data_buf = ctypes.create_string_buffer(max(dxfer_len, 1))
        sense_buf = ctypes.create_string_buffer(_SENSE_LEN)
        self._bufs = (cdb_buf, data_buf, sense_buf)

        hdr = _SgIoHdr()
        hdr.interface_id = ord("S")
        hdr.dxfer_direction = _DIRECTIONS[dxfer]
        hdr.cmd_len = len(cdb)
        hdr.mx_sb_len = _SENSE_LEN
        hdr.dxfer_len = dxfer_len
        hdr.dxferp = ctypes.addressof(data_buf)
        hdr.cmdp = ctypes.addressof(cdb_buf)
        hdr.sbp = ctypes.addressof(sense_buf)
        hdr.timeout = _TIMEOUT_MS
        hdr.flags = 0
        return hdr

    def _exec(self, cdb: bytes, data: bytes | None, dxfer: str, expected_len: int):
        hdr = self._build_header(cdb, len(data) if data is not None else expected_len, dxfer)
        if data is not None and dxfer == "to":
            self._bufs[1].raw = data
        buf = bytearray(ctypes.string_at(ctypes.byref(hdr), ctypes.sizeof(hdr)))
        fcntl.ioctl(self.fd, SG_IO, buf, True)  # mutate=True: kernel writes status back
        ctypes.memmove(ctypes.byref(hdr), bytes(buf), ctypes.sizeof(hdr))

        sense = self._bufs[2].raw
        if hdr.host_status or hdr.driver_status:
            raise TransportError(
                f"SG_IO host_status={hdr.host_status} driver_status={hdr.driver_status} "
                f"sense={sense.hex()}"
            )
        if hdr.status != 0:
            raise TransportError(f"SCSI status {hdr.status} sense={sense.hex()}")

        if dxfer == "from":
            return self._bufs[1].raw[:expected_len]
        return None

    def cmd_in(self, cdb: bytes | str, length: int) -> bytes:
        return self._exec(_norm_cdb(cdb), None, "from", length)

    def cmd_out(self, cdb: bytes | str, data: bytes):
        return self._exec(_norm_cdb(cdb), data, "to", len(data))

    def cmd_none(self, cdb: bytes | str):
        return self._exec(_norm_cdb(cdb), None, "none", 0)


@dataclass
class Step:
    """One expected command in a FakeTransport script."""

    cdb: bytes
    direction: str  # "in" | "out" | "none"
    response: bytes = b""
    status: int = 0  # SCSI status byte; 0 = good


class FakeTransport:
    """Replays an explicit script of expected commands; no hardware needed."""

    def __init__(self, steps: list[Step]):
        self._steps = list(steps)
        self._pos = 0

    @classmethod
    def from_script(cls, steps: list[Step]) -> "FakeTransport":
        return cls(steps)

    def _next(self, cdb_bytes: bytes) -> Step:
        if self._pos >= len(self._steps):
            raise TransportError(
                f"script exhausted at command {cdb_bytes.hex()}"
            )
        step = self._steps[self._pos]
        self._pos += 1
        if step.cdb != cdb_bytes:
            raise TransportError(
                f"script mismatch: expected {step.cdb.hex()}, got {cdb_bytes.hex()}"
            )
        if step.status != 0:
            raise TransportError(f"step failed with SCSI status {step.status}")
        return step

    def cmd_in(self, cdb: bytes | str, length: int) -> bytes:
        step = self._next(_norm_cdb(cdb))
        if step.direction != "in":
            raise TransportError(
                f"script mismatch: expected direction {step.direction}, got in"
            )
        resp = step.response
        if len(resp) < length:
            resp = resp + b"\0" * (length - len(resp))
        return resp[:length]

    def cmd_out(self, cdb: bytes | str, data: bytes):
        step = self._next(_norm_cdb(cdb))
        if step.direction != "out":
            raise TransportError(
                f"script mismatch: expected direction {step.direction}, got out"
            )
        return None

    def cmd_none(self, cdb: bytes | str):
        step = self._next(_norm_cdb(cdb))
        if step.direction != "none":
            raise TransportError(
                f"script mismatch: expected direction {step.direction}, got none"
            )
        return None
