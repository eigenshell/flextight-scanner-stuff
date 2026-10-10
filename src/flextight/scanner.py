"""Session state machine over a transport (SGIOTransport or FakeTransport)."""

from . import protocol


class Scanner:
    def __init__(self, transport):
        self.t = transport
        self.state = "new"

    def enumerate(self) -> dict:
        """TEST UNIT READY + INQUIRY. Returns {"vendor","model","rev"} strings."""
        self.t.cmd_none(protocol.CDB_TEST_UNIT_READY)
        inq = self.t.cmd_in(protocol.CDB_INQUIRY(), protocol.INQUIRY_ALLOC_LEN)

        def field(off: int, length: int) -> str:
            return inq[off : off + length].decode("ascii", "replace").strip()

        info = {
            "vendor": field(protocol.INQ_VENDOR_OFF, protocol.INQ_VENDOR_LEN),
            "model": field(protocol.INQ_MODEL_OFF, protocol.INQ_MODEL_LEN),
            "rev": field(protocol.INQ_REV_OFF, protocol.INQ_REV_LEN),
        }
        self.state = "enumerated"
        return info

    def close(self):
        close = getattr(self.t, "close", None)
        if callable(close):
            close()
