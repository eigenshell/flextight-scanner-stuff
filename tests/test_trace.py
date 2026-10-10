"""Tests for flextight.trace — JSONL parser for SPTI ioctl captures.

All fixtures are synthetic; no real trace files exist yet (hardware
capture is a later gated task).
"""

import pytest

from flextight.trace import (
    KNOWN_OPCODES,
    OPCODE_NAMES,
    TraceParseError,
    TraceRecord,
    group_by_code,
    guess_cdb,
    load,
    sequence,
)


def write_trace(tmp_path, lines):
    """Write fixture lines (list of str) to a JSONL file, return its path."""
    p = tmp_path / "trace.jsonl"
    p.write_text("\n".join(lines) + "\n")
    return str(p)


def test_load_ioctl_record_end_to_end(tmp_path):
    path = write_trace(
        tmp_path,
        [
            '{"h":"0x1c","code":"0x2a002c","in_len":14,'
            '"in_head":"12000000600000","ok":1,"out_len":96,'
            '"out_head":"494d41434f4e"}'
        ],
    )
    recs = load(path)
    assert len(recs) == 1
    r = recs[0]
    assert isinstance(r, TraceRecord)
    assert r.h == 0x1C
    assert r.code == 0x2A002C
    assert r.in_len == 14
    assert r.in_head == bytes.fromhex("12000000600000")
    assert r.ok == 1
    assert r.out_len == 96
    assert r.out_head == bytes.fromhex("494d41434f4e")
    assert r.out_head.startswith(b"IMACON")
    assert r.control is None


def test_load_tolerates_missing_head_fields(tmp_path):
    path = write_trace(
        tmp_path,
        ['{"h":"0x1c","code":"0x2a002c","in_len":0,"ok":0,"out_len":0}'],
    )
    r = load(path)[0]
    assert r.in_head == b""
    assert r.out_head == b""


def test_hook_loaded_control_line(tmp_path):
    path = write_trace(
        tmp_path,
        [
            '{"tag":"hook_loaded","pid":1234}',
            '{"h":"0x1c","code":"0x2a002c","in_len":0,"ok":1,"out_len":0}',
        ],
    )
    recs = load(path)
    assert len(recs) == 2
    ctrl = recs[0]
    assert ctrl.control == "hook_loaded"
    assert sequence(recs) == [recs[1]]
    assert group_by_code(recs) == {0x2A002C: [recs[1]]}


def test_control_line_without_tag_is_unknown(tmp_path):
    path = write_trace(tmp_path, ['{"foo":1}'])
    assert load(path)[0].control == "unknown"


def test_install_failed_control_line(tmp_path):
    path = write_trace(tmp_path, ['{"tag":"install_failed","reason":"x"}'])
    assert load(path)[0].control == "install_failed"


def test_blank_lines_skipped(tmp_path):
    path = write_trace(
        tmp_path,
        [
            "",
            '{"h":"0x1c","code":"0x2a002c","in_len":0,"ok":1,"out_len":0}',
            "   ",
            "",
        ],
    )
    assert len(load(path)) == 1


def test_malformed_json_reports_line_number(tmp_path):
    path = write_trace(
        tmp_path,
        [
            '{"h":"0x1c","code":"0x2a002c","in_len":0,"ok":1,"out_len":0}',
            "this is not json",
        ],
    )
    with pytest.raises(TraceParseError) as ei:
        load(path)
    assert "line 2" in str(ei.value)


def test_wrong_types_raise(tmp_path):
    path = write_trace(tmp_path, ['{"h":"0x1c","code":"0x2a002c","in_len":"x","ok":1,"out_len":0}'])
    with pytest.raises(TraceParseError):
        load(path)


def test_bad_hex_raises(tmp_path):
    path = write_trace(
        tmp_path,
        ['{"h":"zz","code":"0x2a002c","in_len":0,"ok":1,"out_len":0}'],
    )
    with pytest.raises(TraceParseError):
        load(path)


def test_sequence_preserves_file_order_of_ioctl_records(tmp_path):
    path = write_trace(
        tmp_path,
        [
            '{"h":"0x1","code":"0x2a002c","in_len":0,"ok":1,"out_len":0}',
            '{"tag":"hook_loaded"}',
            '{"h":"0x1","code":"0x2a0030","in_len":0,"ok":1,"out_len":0}',
            '{"h":"0x1","code":"0x2a002c","in_len":0,"ok":0,"out_len":0}',
        ],
    )
    recs = load(path)
    seq = sequence(recs)
    assert [r.code for r in seq] == [0x2A002C, 0x2A0030, 0x2A002C]
    assert all(r.control is None for r in seq)


def test_group_by_code_groups_and_separates(tmp_path):
    path = write_trace(
        tmp_path,
        [
            '{"h":"0x1","code":"0x2a002c","in_len":0,"ok":1,"out_len":0}',
            '{"h":"0x1","code":"0x2a0030","in_len":0,"ok":1,"out_len":0}',
            '{"h":"0x1","code":"0x2a002c","in_len":0,"ok":0,"out_len":0}',
        ],
    )
    groups = group_by_code(load(path))
    assert set(groups) == {0x2A002C, 0x2A0030}
    assert len(groups[0x2A002C]) == 2
    assert len(groups[0x2A0030]) == 1


def _ioctl_record(in_head: str) -> TraceRecord:
    return TraceRecord(
        h=0x1C,
        code=0x2A002C,
        in_len=len(in_head) // 2,
        in_head=bytes.fromhex(in_head),
        ok=1,
        out_len=0,
        out_head=b"",
    )


def test_guess_cdb_at_offset_zero():
    assert guess_cdb(_ioctl_record("12000000600000")) == (0, 0x12)


def test_guess_cdb_at_offset_eight():
    rec = _ioctl_record("deadbeefdeadbeef12000000600000")
    assert guess_cdb(rec) == (8, 0x12)


def test_guess_cdb_no_known_opcode():
    assert guess_cdb(_ioctl_record("deadbeef")) is None


def test_guess_cdb_empty_in_head():
    assert guess_cdb(_ioctl_record("")) is None


def test_guess_cdb_respects_max_offset():
    # 0x12 sits at offset 16; with max_offset=8 it must not be found.
    # 0xDE is not a known opcode, so the junk prefix yields no false hit.
    rec = _ioctl_record("de" * 16 + "12000000600000")
    assert guess_cdb(rec, max_offset=8) is None
    assert guess_cdb(rec, max_offset=32) == (16, 0x12)


def test_opcode_names_covers_all_known_opcodes():
    for op in KNOWN_OPCODES:
        assert op in OPCODE_NAMES
        assert OPCODE_NAMES[op]


def test_opcode_names_expected_entries():
    assert OPCODE_NAMES[0x00] == "TEST_UNIT_READY"
    assert OPCODE_NAMES[0x12] == "INQUIRY"
    assert OPCODE_NAMES[0xD0] == "Boot"
