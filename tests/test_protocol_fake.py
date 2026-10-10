"""Scanner enumeration over FakeTransport: TDD pins for protocol.py/scanner.py."""

import pytest

from flextight import protocol
from flextight.scanner import Scanner
from flextight.transport import FakeTransport, Step, TransportError


def _inquiry_data(vendor=b"IMACON  ", model=b"FlexTight II    ", rev=b"1.00"):
    """Build a 36-byte standard INQUIRY response.

    Byte 0 is 0x06 (peripheral qualifier 0, device type 6 = scanner); the
    enum path never reads it, this just documents what the hardware reports.
    """
    assert len(vendor) == 8, "vendor field is 8 bytes, space-padded"
    assert len(model) == 16, "model field is 16 bytes, space-padded"
    assert len(rev) == 4, "revision field is 4 bytes"
    data = bytearray(protocol.INQUIRY_ALLOC_LEN)
    data[0] = 0x06
    data[protocol.INQ_VENDOR_OFF : protocol.INQ_VENDOR_OFF + 8] = vendor
    data[protocol.INQ_MODEL_OFF : protocol.INQ_MODEL_OFF + 16] = model
    data[protocol.INQ_REV_OFF : protocol.INQ_REV_OFF + 4] = rev
    return bytes(data)


def _enum_script(**inq_kwargs):
    return [
        Step(protocol.CDB_TEST_UNIT_READY, "none"),
        Step(protocol.CDB_INQUIRY(), "in", response=_inquiry_data(**inq_kwargs)),
    ]


def test_enumerate_against_fake():
    s = Scanner(FakeTransport.from_script(_enum_script()))
    info = s.enumerate()
    assert info["vendor"] == "IMACON"
    assert "FlexTight" in info["model"]
    assert info["rev"] == "1.00"
    assert s.state == "enumerated"


def test_enumerate_consumes_script():
    s = Scanner(FakeTransport.from_script(_enum_script()))
    s.enumerate()
    with pytest.raises(TransportError, match="exhausted"):
        s.enumerate()


def test_enumerate_model_acceptance():
    """Acceptance must not be hardwired to one model string."""
    s = Scanner(
        FakeTransport.from_script(_enum_script(model=b"Precision II    "))
    )
    info = s.enumerate()
    assert info["vendor"] == "IMACON"
    assert info["model"] == "Precision II"


class TestProtocolConstants:
    def test_cdb_inquiry_default(self):
        assert protocol.CDB_INQUIRY() == bytes.fromhex("120000002400")

    def test_cdb_inquiry_custom_len(self):
        assert protocol.CDB_INQUIRY(96) == bytes.fromhex("120000006000")
        assert protocol.CDB_INQUIRY(255) == bytes.fromhex("12000000ff00")
