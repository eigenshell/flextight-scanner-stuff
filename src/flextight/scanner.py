"""Session state machine over a transport (SGIOTransport or FakeTransport)."""

from . import protocol
from .transport import TransportError


class Scanner:
    def __init__(self, transport):
        self.transport = transport
        self.state = "new"

    def enumerate(self) -> dict:
        """TEST UNIT READY + INQUIRY.

        Returns {"vendor","model","rev","pdt"} strings/ints; ``pdt`` is the
        peripheral device type from INQUIRY byte 0 (0x06 = scanner).

        Raises TransportError if called when state != "new" -- no
        re-enumeration: on real hardware the unit stays ready, so the only
        observable effect of a second call would be divergent fake behavior.
        """
        if self.state != "new":
            raise TransportError("scanner already enumerated")
        self.transport.cmd_none(protocol.CDB_TEST_UNIT_READY)
        inq = self.transport.cmd_in(protocol.CDB_INQUIRY(), protocol.INQUIRY_ALLOC_LEN)

        def field(off: int, length: int) -> str:
            return inq[off : off + length].decode("ascii", "replace").strip()

        info = {
            "vendor": field(protocol.INQ_VENDOR_OFF, protocol.INQ_VENDOR_LEN),
            "model": field(protocol.INQ_MODEL_OFF, protocol.INQ_MODEL_LEN),
            "rev": field(protocol.INQ_REV_OFF, protocol.INQ_REV_LEN),
            "pdt": inq[0] & 0x1F,
        }
        self.state = "enumerated"
        return info

    def close(self):
        close = getattr(self.transport, "close", None)
        if callable(close):
            close()
