"""Parser for the spti_logger JSONL trace format.

Record contract (tools/spti_logger/fmt.h): one compact JSON object per
line with fields h, code, in_len, in_head, ok, out_len, out_head. The
same file may also contain foreign-schema control lines such as
{"tag":"hook_loaded",...} or {"tag":"install_failed",...}; parsers must
skip (but may surface) them.

CRITICAL: the layout of the IOCTL_SCSISCAN_CMD payload — i.e. where
inside the input buffer the CDB bytes sit — is not yet known. No code in
this module may assume a layout; see guess_cdb() for the single
quarantined heuristic.
"""

from __future__ import annotations

import json
from dataclasses import dataclass

KNOWN_OPCODES: frozenset[int] = frozenset(
    {
        0x00,  # TEST_UNIT_READY
        0x12,  # INQUIRY
        0x3B,  # WRITE_BUFFER
        0x3C,  # READ_BUFFER
        0xD0,  # Boot
        0xD1,  # IShutDown
        0xD4,  # LightSet
        0xD8,  # FlashWrite
        0xDB,  # ScanStop
        0xDF,  # ScannerReady
        0xE0,  # MotorMove
        0xE1,  # MotorGet
        0xE2,  # SetFilter
        0xE3,  # SetScanMode
    }
)

OPCODE_NAMES: dict[int, str] = {
    0x00: "TEST_UNIT_READY",
    0x12: "INQUIRY",
    0x3B: "WRITE_BUFFER",
    0x3C: "READ_BUFFER",
    0xD0: "Boot",
    0xD1: "IShutDown",
    0xD4: "LightSet",
    0xD8: "FlashWrite",
    0xDB: "ScanStop",
    0xDF: "ScannerReady",
    0xE0: "MotorMove",
    0xE1: "MotorGet",
    0xE2: "SetFilter",
    0xE3: "SetScanMode",
}


class TraceParseError(Exception):
    """Raised on malformed JSONL; message includes the 1-based line number."""


@dataclass
class TraceRecord:
    h: int
    code: int
    in_len: int
    in_head: bytes
    ok: int
    out_len: int
    out_head: bytes
    control: str | None = None  # None for ioctl records; tag for control lines


def _parse_ioctl(obj: dict, lineno: int) -> TraceRecord:
    try:
        return TraceRecord(
            h=int(obj["h"], 16),
            code=int(obj["code"], 16),
            in_len=int(obj["in_len"]),
            in_head=bytes.fromhex(obj.get("in_head") or ""),
            ok=int(obj["ok"]),
            out_len=int(obj["out_len"]),
            out_head=bytes.fromhex(obj.get("out_head") or ""),
        )
    except (KeyError, TypeError, ValueError) as e:
        raise TraceParseError(f"line {lineno}: malformed ioctl record: {e}") from e


def load(path: str) -> list[TraceRecord]:
    """Parse a JSONL trace file into a list of TraceRecords.

    Blank lines are skipped. A line whose object has a "code" field is an
    ioctl record; any other JSON object is a control record (control set
    to its "tag", or "unknown" if absent). Malformed JSON or wrong field
    types raise TraceParseError with the 1-based line number.
    """
    records: list[TraceRecord] = []
    with open(path, "r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as e:
                raise TraceParseError(f"line {lineno}: invalid JSON: {e}") from e
            if not isinstance(obj, dict):
                raise TraceParseError(f"line {lineno}: expected JSON object")
            if "code" in obj:
                records.append(_parse_ioctl(obj, lineno))
            else:
                records.append(
                    TraceRecord(
                        h=0,
                        code=0,
                        in_len=0,
                        in_head=b"",
                        ok=0,
                        out_len=0,
                        out_head=b"",
                        control=str(obj.get("tag", "unknown")),
                    )
                )
    return records


def sequence(records: list[TraceRecord]) -> list[TraceRecord]:
    """Ioctl records only, in file order."""
    return [r for r in records if r.control is None]


def group_by_code(records: list[TraceRecord]) -> dict[int, list[TraceRecord]]:
    """Ioctl records keyed by IOCTL code; control records excluded."""
    groups: dict[int, list[TraceRecord]] = {}
    for r in records:
        if r.control is not None:
            continue
        groups.setdefault(r.code, []).append(r)
    return groups


def guess_cdb(record: TraceRecord, max_offset: int = 32) -> tuple[int, int] | None:
    """Guess the CDB opcode offset inside the IOCTL input buffer.

    PLACEHOLDER HEURISTIC, quarantined in this function: the layout of the
    IOCTL_SCSISCAN_CMD payload (where inside in_head the CDB bytes sit) is
    not yet known — it will be pinned by real hardware traces
    (docs/recon/transport.md open question a). Until then this scans
    record.in_head[0:max_offset] for the first byte that is a known SCSI
    opcode and returns (offset, opcode), or None if no candidate exists.
    Results are hypotheses, not facts; do not build decoding on top of
    them.
    """
    for offset, b in enumerate(record.in_head[0:max_offset]):
        if b in KNOWN_OPCODES:
            return (offset, b)
    return None
