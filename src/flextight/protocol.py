# SPDX-License-Identifier: GPL-2.0-or-later
"""Protocol constants (see tools/poc for the device protocol as proven on hardware).

Enumeration uses fixed SCSI commands whose layouts are standard; vendor CDB
tables arrive with the spec.
"""

# TEST UNIT READY: 6-byte CDB, all zero.
CDB_TEST_UNIT_READY = bytes.fromhex("000000000000")

# Standard INQUIRY allocation length (enough for vendor/product/rev fields).
INQUIRY_ALLOC_LEN = 36


def CDB_INQUIRY(alloc_len: int = INQUIRY_ALLOC_LEN) -> bytes:
    """Build the 6-byte INQUIRY CDB (opcode 0x12).

    Byte 0: opcode 0x12.  Byte 1: EVPD/obsolete/reserved zero.  Bytes 3-4:
    allocation length, big-endian.  Byte 5: control, zero.
    """
    return bytes([0x12, 0x00, 0x00]) + alloc_len.to_bytes(2, "big") + b"\x00"


# INQUIRY data field offsets (standard layout).
INQ_VENDOR_OFF = 8
INQ_VENDOR_LEN = 8
INQ_MODEL_OFF = 16
INQ_MODEL_LEN = 16
INQ_REV_OFF = 32
INQ_REV_LEN = 4
