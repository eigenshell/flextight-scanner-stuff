# SPDX-License-Identifier: GPL-2.0-or-later
"""Parser for the spti_logger JSONL trace format.

Record contract (tools/spti_logger/fmt.h): one compact JSON object per
line with fields h, code, in_len, in_head, ok, out_len, out_head. The
same file may also contain foreign-schema control lines such as
{"tag":"hook_loaded",...} or {"tag":"install_failed",...}; parsers must
skip (but may surface) them.

Head-truncation contract: in_head/out_head carry at most the first 64
bytes of each transfer (FT_MAX_HEAD in tools/spti_logger/fmt.h), while
in_len/out_len report the FULL transfer length. The two must never be
equated: len(in_head) <= in_len always, and a short in_head is
truncation, not the whole buffer.

Schema mixing: load() returns ioctl records and control lines in one
list. Iterate via sequence() for ioctl records; raw load() output
includes control lines (control tag set, all other fields zeroed — note
the zeroed `code` counts as a phantom opcode 0x00 in naive Counter
one-liners over the raw list).

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
    # First <=64 bytes of the input buffer only (FT_MAX_HEAD); in_len is
    # the FULL transfer length — never equate len(in_head) with in_len.
    in_head: bytes
    ok: int
    out_len: int
    # First <=64 bytes of the output buffer only (FT_MAX_HEAD); out_len is
    # the FULL transfer length — never equate len(out_head) with out_len.
    out_head: bytes
    control: str | None = None  # None for ioctl records; tag for control lines


def _as_int(obj: dict, key: str, lineno: int) -> int:
    """Strict int coercion: bools and floats are rejected, not truncated.

    The failure mode at stake: a logger regression emitting float lengths
    would silently corrupt the evidence corpus if int(14.9) truncated.
    """
    v = obj[key]
    if isinstance(v, bool) or not isinstance(v, int):
        raise TraceParseError(
            f"line {lineno}: field {key!r} must be an int, got {type(v).__name__}"
        )
    return v


def _as_head(obj: dict, key: str, lineno: int) -> bytes:
    """Absent head field -> b"" (valid per contract); non-str -> error.

    Distinguishes "key absent" from "key present with wrong type":
    in_head:0 / in_head:false / in_head:[] must not silently become b"".
    """
    if key not in obj:
        return b""
    v = obj[key]
    if not isinstance(v, str):
        raise TraceParseError(
            f"line {lineno}: field {key!r} must be a hex string, got {type(v).__name__}"
        )
    return bytes.fromhex(v) if v else b""


def _parse_ioctl(obj: dict, lineno: int) -> TraceRecord:
    try:
        h = int(obj["h"], 16)
        code = int(obj["code"], 16)
    except (KeyError, TypeError, ValueError) as e:
        raise TraceParseError(f"line {lineno}: malformed ioctl record: {e}") from e
    return TraceRecord(
        h=h,
        code=code,
        in_len=_as_int(obj, "in_len", lineno),
        in_head=_as_head(obj, "in_head", lineno),
        ok=_as_int(obj, "ok", lineno),
        out_len=_as_int(obj, "out_len", lineno),
        out_head=_as_head(obj, "out_head", lineno),
    )


def load(path: str) -> list[TraceRecord]:
    """Parse a JSONL trace file into a list of TraceRecords.

    Blank lines are skipped. Classification is positive: an object with a
    "tag" key is a control record; an object with both "h" and "code" is
    an ioctl record; any other JSON object is treated as a foreign-schema
    control record (control set to its "tag", or "unknown" if absent) —
    the README contract requires skipping foreign lines rather than
    aborting, so a control line that happens to contain a "code" key no
    longer misclassifies as ioctl and kills the whole file. Malformed JSON
    or wrong field types raise TraceParseError with the 1-based line
    number.
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
            if "tag" in obj:
                records.append(_control_record(obj))
            elif "h" in obj and "code" in obj:
                records.append(_parse_ioctl(obj, lineno))
            else:
                records.append(_control_record(obj))
    return records


def _control_record(obj: dict) -> TraceRecord:
    return TraceRecord(
        h=0,
        code=0,
        in_len=0,
        in_head=b"",
        ok=0,
        out_len=0,
        out_head=b"",
        control=str(obj.get("tag", "unknown")),
    )


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
    (an open transport question). Until then this scans
    record.in_head[0:max_offset] for the first byte that is a known SCSI
    opcode and returns (offset, opcode), or None if no candidate exists.
    Results are hypotheses, not facts; do not build decoding on top of
    them.
    """
    for offset, b in enumerate(record.in_head[0:max_offset]):
        if b in KNOWN_OPCODES:
            return (offset, b)
    return None
