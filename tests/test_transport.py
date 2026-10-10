"""Tests for the SG_IO transport and the script-replay fake."""

import ctypes

import pytest

from flextight.transport import (
    FakeTransport,
    SGIOTransport,
    Step,
    TransportError,
)


class TestSgIoHdrLayout:
    """Regression pins against /usr/include/scsi/sg.h (struct sg_io_hdr v3).

    On x86-64 this struct is 88 bytes: three 8-byte pointers force 8-byte
    alignment, so the C tail from ``status`` onward sits at offset 64.
    The 32-bit layout would be 60 bytes -- if sizeof ever comes back 60,
    the packed buffer below would misalign every field from ``cmdp``
    onward relative to what the kernel parses.
    """

    def test_sizeof_is_88_on_64bit(self):
        assert ctypes.sizeof(ctypes.c_void_p) == 8, (
            "these offsets are x86-64 specific; do not run on a 32-bit interpreter"
        )
        from flextight.transport import _SgIoHdr

        # 32-bit would be 60; 88 = 64-bit layout (see class docstring).
        assert ctypes.sizeof(_SgIoHdr) == 88

    def test_field_order_and_offsets(self):
        from flextight.transport import _SgIoHdr

        names = [name for name, _ in _SgIoHdr._fields_]
        assert names[0] == "interface_id"
        assert _SgIoHdr.dxfer_len.offset == 12
        assert _SgIoHdr.cmdp.offset == 24
        assert _SgIoHdr.status.offset == 64


class TestBuildHeader:
    def test_build_header_inquiry(self):
        t = SGIOTransport(fd=-1)  # never issues an ioctl
        h = t._build_header(bytes.fromhex("120000002400"), 36, "from")
        assert h.interface_id == ord("S")
        assert h.dxfer_direction == -3  # SG_DXFER_FROM_DEV
        assert h.dxfer_len == 36
        assert h.cmd_len == 6
        assert h.mx_sb_len == 64
        assert h.timeout == 20000
        assert h.flags == 0
        assert h.sbp
        cdb_buf, data_buf, sense_buf = t._bufs
        assert len(cdb_buf) == 16
        assert bytes(cdb_buf[:6]) == bytes.fromhex("120000002400")
        assert bytes(cdb_buf[6:]) == b"\0" * 10
        assert len(data_buf) == 36
        assert len(sense_buf) == 64
        assert h.cmdp == ctypes.addressof(cdb_buf)

    def test_build_header_directions(self):
        t = SGIOTransport(fd=-1)
        assert t._build_header(b"\0" * 6, 0, "to").dxfer_direction == -2
        assert t._build_header(b"\0" * 6, 0, "none").dxfer_direction == 0


class TestFakeTransport:
    def test_fake_replays_script(self):
        vendor = b"IMATION FLEXTIGHTII  1.00"
        steps = [
            Step(bytes.fromhex("00000000000000"), "none"),
            Step(bytes.fromhex("120000002400"), "in", response=vendor),
        ]
        t = FakeTransport.from_script(steps)
        assert t.cmd_none("00000000000000") is None
        resp = t.cmd_in(bytes.fromhex("120000002400"), 36)
        assert resp[: len(vendor)] == vendor

    def test_fake_exhaustion(self):
        t = FakeTransport([Step(b"\0" * 6, "none")])
        t.cmd_none(b"\0" * 6)
        with pytest.raises(TransportError, match="exhausted"):
            t.cmd_none(b"\0" * 6)

    def test_fake_mismatch(self):
        t = FakeTransport([Step(bytes.fromhex("120000002400"), "in", response=b"x")])
        with pytest.raises(TransportError) as excinfo:
            t.cmd_in("00000000000000", 1)
        msg = str(excinfo.value)
        assert "120000002400" in msg
        assert "00000000000000" in msg

    def test_fake_status_error(self):
        t = FakeTransport([Step(b"\0" * 6, "none", status=2)])
        with pytest.raises(TransportError, match="2"):
            t.cmd_none(b"\0" * 6)

    def test_fake_response_sizing(self):
        t = FakeTransport(
            [
                Step(b"\x01", "in", response=b"abc"),
                Step(b"\x02", "in", response=b"abcdef"),
            ]
        )
        padded = t.cmd_in(b"\x01", 6)
        assert padded == b"abc\0\0\0"
        truncated = t.cmd_in(b"\x02", 3)
        assert truncated == b"abc"
