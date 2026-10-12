# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests for flextight.fff — .fff (3F) writer with LibRaw round-trip.

Wire-format constants (TIFF magic, tag numbers, field types, Imacon
metadata block offsets) are pinned here as literals, per project rule:
tests must not import the constants they verify. Format authority: TIFF 6.0
for the generic TIFF structure, LibRaw source for the FFF-specific
requirements.
"""

import io
import struct

import numpy as np
import pytest
import rawpy

from flextight.fff import write_fff

W, H = 64, 48  # round-trip dimensions (LibRaw refuses raws < 22x22)


def gradient_pixels(w=W, h=H):
    """Deterministic 16-bit RGB gradient as a numpy uint16 array (h, w, 3)."""
    r = np.arange(w, dtype=np.uint16)[None, :] * 1000
    g = np.arange(h, dtype=np.uint16)[:, None] * 1000
    pix = np.empty((h, w, 3), dtype=np.uint16)
    pix[:, :, 0] = r
    pix[:, :, 1] = g
    pix[:, :, 2] = (r + g) // 2
    return pix


def write_to_bytes(pix, w, h, **kwargs):
    buf = io.BytesIO()
    write_fff(buf, pix, w, h, **kwargs)
    return buf.getvalue()


def parse_ifd(blob):
    """Parse the first IFD; return (entry_count, [(tag, type, count, value)])."""
    assert blob[:4] == b"II*\x00"  # pinned: little-endian TIFF magic + v42
    (first_ifd,) = struct.unpack_from("<I", blob, 4)
    assert first_ifd == 8  # pinned: IFD immediately follows the header
    (count,) = struct.unpack_from("<H", blob, first_ifd)
    entries = []
    for i in range(count):
        entries.append(struct.unpack_from("<HHII", blob, first_ifd + 2 + 12 * i))
    (next_ifd,) = struct.unpack_from("<I", blob, first_ifd + 2 + 12 * count)
    assert next_ifd == 0
    return count, entries


# ---------------------------------------------------------------------------
# Byte-structure tests (pinned literals, parsed with struct)
# ---------------------------------------------------------------------------


def test_header_magic_and_ifd_offset_pinned():
    blob = write_to_bytes(gradient_pixels().tobytes(), W, H)
    assert blob[0:4] == b"II*\x00"
    assert struct.unpack_from("<H", blob, 2) == (42,)  # TIFF version, pinned
    assert struct.unpack_from("<I", blob, 4) == (8,)


def test_ifd_entries_exact_tags_and_types():
    _, entries = parse_ifd(write_to_bytes(gradient_pixels().tobytes(), W, H))
    by_tag = {e[0]: e for e in entries}
    # Six entries, pinned tag ids.
    assert sorted(by_tag) == [256, 257, 273, 279, 46274, 46275]
    # Field types: LONG=4 for standard tags, UNDEFINED=7 for Imacon tags.
    assert by_tag[256] == (256, 4, 1, W)
    assert by_tag[257] == (257, 4, 1, H)
    assert by_tag[273][0:3] == (273, 4, 1)  # StripOffsets
    assert by_tag[279] == (279, 4, 1, W * H * 3 * 2)  # StripByteCounts
    # Imacon raw payload: count is the full pixel byte count.
    assert by_tag[46275][0:2] == (46275, 7)
    assert by_tag[46275][2] == W * H * 6
    # Imacon metadata block: 244 bytes.
    assert by_tag[46274][0:3] == (46274, 7, 244)


def test_imacon_raw_tag_precedes_meta_tag():
    # LibRaw parses 46274's dimension branch against ima_len set by 46275;
    # the descending order is a deliberate, researched deviation from the
    # TIFF ascending-tag convention.
    _, entries = parse_ifd(write_to_bytes(gradient_pixels().tobytes(), W, H))
    order = [e[0] for e in entries]
    assert order.index(46275) < order.index(46274)


def test_strip_offsets_point_at_pixel_data():
    pix = gradient_pixels()
    blob = write_to_bytes(pix.tobytes(), W, H)
    _, entries = parse_ifd(blob)
    by_tag = {e[0]: e for e in entries}
    strip_off = by_tag[273][3]
    assert by_tag[46275][3] == strip_off  # same payload, both tags
    payload = blob[strip_off : strip_off + W * H * 6]
    assert payload == pix.tobytes()  # lossless: exact bytes


def test_imacon_meta_block_layout():
    blob = write_to_bytes(gradient_pixels().tobytes(), W, H)
    _, entries = parse_ifd(blob)
    by_tag = {e[0]: e for e in entries}
    meta_off = by_tag[46274][3]
    # Pinned offsets from LibRaw tiff.cpp 0xb4c2 parsing.
    assert struct.unpack_from("<I", blob, meta_off + 40) == (W,)
    assert struct.unpack_from("<I", blob, meta_off + 44) == (H,)
    for off in (48, 52, 56, 60):  # margins zero
        assert struct.unpack_from("<I", blob, meta_off + off) == (0,)
    cam_mul = struct.unpack_from("<fff", blob, meta_off + 116)
    assert cam_mul == (1.0, 1.0, 1.0)
    assert struct.unpack_from("<H", blob, meta_off + 242) == (0,)  # no rotation


def test_make_model_ascii_entries():
    blob = write_to_bytes(
        gradient_pixels().tobytes(),
        W,
        H,
        metadata={"make": "Imacon", "model": "Flextight Precision II"},
    )
    _, entries = parse_ifd(blob)
    by_tag = {e[0]: e for e in entries}
    make_off = by_tag[271][3]
    assert blob[make_off : make_off + 7] == b"Imacon\x00"
    model_off = by_tag[272][3]
    assert blob[model_off : model_off + 23] == b"Flextight Precision II\x00"


def test_rotation_stored_in_meta_block():
    # LibRaw: flip = (get2() >> 7) * 90 -> rotation/90 stored at bit 7 and up.
    for rotation, expected in ((0, 0), (90, 1 << 7), (180, 2 << 7), (270, 3 << 7)):
        blob = write_to_bytes(
            gradient_pixels().tobytes(), W, H, metadata={"rotation": rotation}
        )
        _, entries = parse_ifd(blob)
        meta_off = {e[0]: e for e in entries}[46274][3]
        assert struct.unpack_from("<H", blob, meta_off + 242) == (expected,)


def test_make_only_and_model_only_metadata():
    _, entries = parse_ifd(
        write_to_bytes(
            gradient_pixels().tobytes(), W, H, metadata={"make": "Imacon"}
        )
    )
    tags = [e[0] for e in entries]
    assert 271 in tags
    assert 272 not in tags
    _, entries = parse_ifd(
        write_to_bytes(
            gradient_pixels().tobytes(),
            W,
            H,
            metadata={"model": "Flextight Precision II"},
        )
    )
    tags = [e[0] for e in entries]
    assert 272 in tags
    assert 271 not in tags


# ---------------------------------------------------------------------------
# LibRaw/rawpy round-trip
# ---------------------------------------------------------------------------


def test_rawpy_roundtrip_gradient():
    pix = gradient_pixels()
    buf = io.BytesIO()
    write_fff(buf, pix.tobytes(), W, H)
    buf.seek(0)
    with rawpy.imread(buf) as raw:
        assert raw.sizes.raw_width == W
        assert raw.sizes.raw_height == H
        assert raw.sizes.width == W
        assert raw.sizes.height == H
        assert raw.sizes.flip == 0
        assert raw.raw_type == rawpy.RawType.Stack
        # Lossless (compression=none): decoded image equals input exactly.
        np.testing.assert_array_equal(raw.raw_image[:, :, :3], pix)
        # postprocess must run without error (its color conversion is a
        # fixed camera profile matrix, so exact equality is not expected).
        out = raw.postprocess(gamma=(1, 1), no_auto_bright=True, output_bps=16)
        assert out.shape == (H, W, 3)


def test_rawpy_roundtrip_random_odd_dimensions():
    w, h = 63, 47  # odd, non-multiple-of-4
    rng = np.random.default_rng(1234)
    pix = rng.integers(0, 65536, size=(h, w, 3), dtype=np.uint16)
    buf = io.BytesIO()
    write_fff(buf, pix, w, h)  # numpy (h, w, 3) array input
    buf.seek(0)
    with rawpy.imread(buf) as raw:
        assert raw.sizes.raw_width == w
        assert raw.sizes.raw_height == h
        np.testing.assert_array_equal(raw.raw_image[:, :, :3], pix)


def test_rawpy_roundtrip_from_path(tmp_path):
    pix = gradient_pixels()
    path = tmp_path / "scan.fff"
    write_fff(path, pix.tobytes(), W, H)  # os.PathLike dispatch
    with rawpy.imread(str(path)) as raw:
        np.testing.assert_array_equal(raw.raw_image[:, :, :3], pix)


def test_libraw_minimum_dimension_conformance():
    # Documented LibRaw limit (identify.cpp): raw_width/raw_height must be
    # >= 22 (and <= 64000). The writer still produces a well-formed file;
    # LibRaw refuses it.
    w = h = 16
    pix = np.zeros((h, w, 3), dtype=np.uint16)
    buf = io.BytesIO()
    write_fff(buf, pix, w, h)
    buf.seek(0)
    with pytest.raises(rawpy.LibRawFileUnsupportedError):
        rawpy.imread(buf)


# ---------------------------------------------------------------------------
# Edge cases
# ---------------------------------------------------------------------------


def test_one_pixel_image_structure():
    pix = np.array([[[1, 2, 3]]], dtype=np.uint16)
    blob = write_to_bytes(pix.tobytes(), 1, 1)
    _, entries = parse_ifd(blob)
    by_tag = {e[0]: e for e in entries}
    assert by_tag[256] == (256, 4, 1, 1)
    assert by_tag[257] == (257, 4, 1, 1)
    assert by_tag[46275][2] == 6  # 1*1*3 samples * 2 bytes
    strip_off = by_tag[273][3]
    assert blob[strip_off : strip_off + 6] == b"\x01\x00\x02\x00\x03\x00"


def test_odd_and_nonmultiple_dimensions_structure():
    for w, h in [(1, 7), (13, 1), (23, 17)]:
        pix = np.zeros((h, w, 3), dtype=np.uint16)
        blob = write_to_bytes(pix.tobytes(), w, h)
        _, entries = parse_ifd(blob)
        by_tag = {e[0]: e for e in entries}
        assert by_tag[256] == (256, 4, 1, w)
        assert by_tag[257] == (257, 4, 1, h)
        assert by_tag[279][3] == w * h * 6


def test_rejects_wrong_pixel_buffer_length():
    with pytest.raises(ValueError):
        write_fff(_sink(), b"\x00" * (W * H * 6 - 2), W, H)
    with pytest.raises(ValueError):
        write_fff(_sink(), b"\x00" * (W * H * 6 + 6), W, H)
    with pytest.raises(ValueError):
        write_fff(_sink(), np.zeros(W * H * 3 - 1, dtype=np.uint16), W, H)


def test_numpy_shape_validation():
    flat = gradient_pixels().reshape(-1)  # accepted: (width*height*3,)
    buf = io.BytesIO()
    write_fff(buf, flat, W, H)
    assert len(buf.getvalue()) > 0
    with pytest.raises(ValueError):
        # wrong 2-D shape (transposed dims)
        write_fff(_sink(), np.zeros((W, H, 3), dtype=np.uint16), W, H)
    with pytest.raises(ValueError):
        # 2-D but wrong channel count
        write_fff(_sink(), np.zeros((H, W, 4), dtype=np.uint16), W, H)
    with pytest.raises(ValueError):
        # unsupported rank
        write_fff(_sink(), np.zeros((3, H, W, 1), dtype=np.uint16), W, H)


def test_rejects_bad_dimensions_and_rotation():
    with pytest.raises(ValueError):
        write_fff(_sink(), b"", 0, H)
    with pytest.raises(ValueError):
        write_fff(_sink(), b"", W, -1)
    with pytest.raises(ValueError):
        write_fff(
            _sink(), np.zeros((H, W, 3), dtype=np.uint16), W, H,
            metadata={"rotation": 45},
        )


def _sink():
    return io.BytesIO()
