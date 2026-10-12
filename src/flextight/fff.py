# SPDX-License-Identifier: GPL-2.0-or-later
"""Writer for Flextight .fff (FlexColor "3F") files.

The .fff format is an extended TIFF. Layout and required tags were
researched against the LibRaw source (the format authority open-source
tooling uses) and verified by round-tripping through rawpy; see
In short, LibRaw rejects
plain 16-bit RGB TIFFs and accepts a file as FFF only when the IFD carries
Imacon private tags 46275 (raw RGB payload) and 46274 (metadata block), in
that order, plus a StripOffsets entry the generic TIFF post-selection
overwrites data_offset with.
"""

from __future__ import annotations

import os
import struct
from typing import Any, BinaryIO, Union

# TIFF 6.0 field type codes (pinned here; tests assert these exact values).
TYPE_ASCII = 2
TYPE_LONG = 4
TYPE_UNDEFINED = 7

# Standard TIFF tags we emit.
TAG_IMAGE_WIDTH = 256
TAG_IMAGE_LENGTH = 257
TAG_MAKE = 271
TAG_MODEL = 272
TAG_STRIP_OFFSETS = 273
TAG_STRIP_BYTE_COUNTS = 279

# Imacon/Hasselblad private tags (Flextight FFF), per LibRaw tiff.cpp:
# 46275 marks the file as Imacon FFF and its *value* is the raw pixel
# payload; 46274 is a 244-byte metadata block (dims, margins, cam_mul,
# rotation). 46275 MUST be parsed before 46274 (LibRaw reads 46274's dims
# branch with the ima_len state set by 46275), so IFD entries are emitted
# with these two in descending order — deliberate deviation from the TIFF
# ascending-tag convention, required by the format authority.
TAG_IMACON_RAW = 46275
TAG_IMACON_META = 46274

# Layout of the 46274 metadata block, byte offsets from block start, as
# parsed by LibRaw (tiff.cpp, 0xb4c2 case). Offsets not listed are unread.
META_BLOCK_LEN = 244
META_RAW_WIDTH_OFF = 40  # u32
META_RAW_HEIGHT_OFF = 44  # u32
# u32 margins at 48 (left), 52 (right), 56 (top), 60 (bottom); kept zero:
# a non-zero margin breaks LibRaw's width*height*6 == ima_len match and the
# file falls off the RGB decode path.
META_CAM_MUL_OFF = 116  # 3 x f32 camera white balance
META_ROTATION_OFF = 242  # u16; LibRaw computes flip = (get2() >> 7) * 90

_TIFF_HEADER = b"II*\x00"  # little-endian TIFF magic + version 42
_FIRST_IFD_OFFSET = 8
_IFD_ENTRY_LEN = 12


def _as_le_bytes(pixels: Any, width: int, height: int) -> bytes:
    """Normalise *pixels* to little-endian uint16 bytes of the image size."""
    if isinstance(pixels, (bytes, bytearray, memoryview)):
        raw = bytes(pixels)
    else:
        try:
            import numpy as np
        except ImportError as exc:  # pragma: no cover - numpy ships with rawpy
            raise TypeError(
                "pixels must be bytes-like or a numpy uint16 array"
            ) from exc
        arr = np.asarray(pixels)
        if arr.dtype != np.uint16:
            raise TypeError(f"pixels must be uint16, got dtype {arr.dtype}")
        if arr.ndim == 1:
            pass  # flat buffer of width*height*3 values
        elif arr.ndim == 3 and arr.shape == (height, width, 3):
            pass
        else:
            raise ValueError(
                "pixels array must have shape (width*height*3,) or "
                f"({height}, {width}, 3); got shape {arr.shape}"
            )
        raw = arr.astype("<u2", copy=False).tobytes(order="C")
    expected_len = width * height * 3 * 2
    if len(raw) != expected_len:
        raise ValueError(
            f"pixel buffer is {len(raw)} bytes, expected "
            f"{expected_len} ({expected_len // 6} RGB pixels)"
        )
    return raw


def _ifd_entry(tag: int, type_: int, count: int, value: int) -> bytes:
    """Pack one 12-byte little-endian IFD entry."""
    return struct.pack("<HHII", tag, type_, count, value)


def _ascii_entry(tag: int, text: str, offset: int) -> bytes:
    """Pack an ASCII IFD entry; the NUL-terminated string lives at *offset*."""
    data = text.encode("ascii") + b"\x00"
    return _ifd_entry(tag, TYPE_ASCII, len(data), offset), data


def _metadata_block(
    width: int, height: int, rotation: int
) -> bytes:
    """Build the 244-byte Imacon metadata block (tag 46274 value)."""
    block = bytearray(META_BLOCK_LEN)
    struct.pack_into(
        "<IIIIII",
        block,
        META_RAW_WIDTH_OFF,
        width,
        height,
        0,
        0,
        0,
        0,
    )
    struct.pack_into("<fff", block, META_CAM_MUL_OFF, 1.0, 1.0, 1.0)
    struct.pack_into("<H", block, META_ROTATION_OFF, (rotation // 90) << 7)
    return bytes(block)


def write_fff(
    path_or_buffer: Union[str, bytes, "os.PathLike[str]", BinaryIO],
    pixels: Any,
    width: int,
    height: int,
    *,
    metadata: dict | None = None,
) -> None:
    """Write a Flextight .fff file readable by LibRaw/rawpy.

    *pixels* is 16-bit interleaved RGB data: either a bytes-like object of
    exactly ``width*height*3*2`` bytes, or a numpy uint16 array shaped
    ``(width*height*3,)`` or ``(height, width, 3)``. The image is stored
    uncompressed (lossless).

    *metadata* is an extension point for future FlexColor-private fields.
    Currently recognised keys (all optional):

    - ``"make"`` (str): TIFF Make (tag 271), e.g. ``"Imacon"``.
    - ``"model"`` (str): TIFF Model (tag 272).
    - ``"rotation"`` (int): 0/90/180/270, stored in the Imacon metadata
      block. Note LibRaw swaps width/height for 90/270 when decoding, so
      only 0 round-trips with unchanged dimensions.

    Film format is not representable: no tag number for it could be
    confirmed from FlexColor.dll strings or the LibRaw source, so it is
    deliberately not emitted.
    """
    if width < 1 or height < 1:
        raise ValueError(f"width/height must be >= 1, got {width}x{height}")
    metadata = metadata or {}
    rotation = int(metadata.get("rotation", 0))
    if rotation not in (0, 90, 180, 270):
        raise ValueError(f"rotation must be one of 0/90/180/270, got {rotation}")

    data = _as_le_bytes(pixels, width, height)
    meta_block = _metadata_block(width, height, rotation)

    # Assemble IFD entries. Standard tags ascending; the two Imacon tags
    # deliberately descending (46275 before 46274) — LibRaw requirement.
    # Entry count: 2 dims + 2 strip tags + optional ASCII strings + 2 Imacon.
    n_str_entries = sum(bool(metadata.get(k)) for k in ("make", "model"))
    n_entries = 2 + 2 + n_str_entries + 2
    ifd_len = 2 + _IFD_ENTRY_LEN * n_entries + 4
    meta_off = _FIRST_IFD_OFFSET + ifd_len
    data_off = meta_off + META_BLOCK_LEN

    entries: list[bytes] = [
        _ifd_entry(TAG_IMAGE_WIDTH, TYPE_LONG, 1, width),
        _ifd_entry(TAG_IMAGE_LENGTH, TYPE_LONG, 1, height),
        _ifd_entry(TAG_STRIP_OFFSETS, TYPE_LONG, 1, data_off),
        _ifd_entry(TAG_STRIP_BYTE_COUNTS, TYPE_LONG, 1, len(data)),
    ]
    strings_off = data_off + len(data)
    strings: list[bytes] = []
    cursor = strings_off
    for key, tag in (("make", TAG_MAKE), ("model", TAG_MODEL)):
        text = metadata.get(key)
        if text:
            entry, chunk = _ascii_entry(tag, str(text), cursor)
            entries.append(entry)
            strings.append(chunk)
            cursor += len(chunk)
    entries.append(_ifd_entry(TAG_IMACON_RAW, TYPE_UNDEFINED, len(data), data_off))
    entries.append(
        _ifd_entry(TAG_IMACON_META, TYPE_UNDEFINED, META_BLOCK_LEN, meta_off)
    )

    ifd = (
        struct.pack("<H", len(entries))
        + b"".join(entries)
        + struct.pack("<I", 0)  # no further IFDs
    )
    blob = _TIFF_HEADER + struct.pack("<I", _FIRST_IFD_OFFSET) + ifd
    blob += meta_block + data + b"".join(strings)

    try:
        path = os.fspath(path_or_buffer)
    except TypeError:
        path = None  # binary file-like object
    if path is not None:
        with open(path, "wb") as fh:
            fh.write(blob)
    else:
        path_or_buffer.write(blob)
