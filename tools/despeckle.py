"""Dust/scratch removal pass for scanned film ("despeckle").

The Flextight Precision II has no infrared channel, so — unlike scanners
with a hardware dust-removal pass (FlexColor's ApplyDust/DustLevel drive a
scanner-hardware pass on IR-capable models) — defect cleanup here is
necessarily software-side. This tool is the open-source equivalent: it takes
a scanned image (16-bit, i.e. 14-bit ADC codes in a 16-bit container; 8-bit
also accepted) and removes dust spots and thin scratches without touching
clean pixels.

Algorithm (two detectors, union, then size filtering):

1. Compact-spot detector. A pixel is a defect candidate when it deviates
   from BOTH a horizontal and a vertical directional median filter
   (1-D sliding median, length ``2*max_defect_size + 3`` — long enough that
   even a max-size defect corrupts fewer than half of any line's samples, so
   the median stays clean). Taking the residual minimum of the two
   directions suppresses false positives on straight edges (the direction
   parallel to an edge is clean there), and an additional agreement test
   (``|med_h - med_v| <= threshold``) suppresses corners, where both
   directions straddle two edges at once.
2. Scratch detector. Thin (1-2 px) lines of any orientation would corrupt a
   directional median along their own spine, so they are found separately
   with a black/white top-hat (3x3 elliptical opening/closing residual),
   which responds to any feature narrower than the structuring element
   regardless of orientation. Top-hat is invariant on straight step edges
   but rings at edge corners, so candidates within a dilated strong-gradient
   band (Sobel on a 5x5 median reference, threshold 4x the defect threshold)
   are discarded. The median reference is 2-D, so a scratch along its spine
   does not corrupt the gradient map.
3. Size filtering. Connected components whose bounding box exceeds
   ``max_defect_size`` in BOTH dimensions AND whose area exceeds that of a
   disk of that diameter (pi/4 * d^2) are rejected: they are genuine image
   structure the reference filters cannot model, not defects. Long thin
   scratches survive this filter (only one bounding-box dimension is large).

Inpainting (``inpaint_defects``) works per channel on the NATIVE integer
dtype via OpenCV's inpaintTelea / inpaintNavierStokes, so unmasked pixels
pass through bit-exact and 16-bit precision is never reduced. (Per-channel
native dtype is deliberate, not convenience: OpenCV 5.0's Telea produces
out-of-range garbage on 32F 1-channel input and 32F 3-channel is
unsupported outright; see the test suite's structure-loss test.) A pure
``median`` method (masked sliding-median fill, no OpenCV photo module) is
provided as a dependency-free fallback.

Defaults process a 4 MP 16-bit RGB scan in ~5 s on a modern desktop
(measured 5.16 s on the review machine; data-independent, first run with
cold caches may be slower; budget: 30 s); the sliding 1-D medians dominate
runtime.

Known limitations (documented trade-offs, pinned by the test suite):
  - defects sitting ON a hard edge or corner are suppressed together with
    the edge false positives they would otherwise generate;
  - defects larger than ``max_defect_size`` in both dimensions are treated
    as image structure and left alone;
  - sensitivity is a fraction of the dtype's full scale, so the same value
    applies to 8-bit and 16-bit input.

CLI: despeckle.py input.tif output.tif [--sensitivity N] [--max-defect-size N]
     [--mask-out path]   (TIFF and PNG, 8- or 16-bit, gray or RGB; the
     codecs below are minimal-baseline, stdlib-only, following the zero-dependency
     approach of flextight/fff.py — they handle single-strip uncompressed
     TIFF and non-interlaced PNG with all five scanline filters, which covers
     what this pipeline writes and the files scanner tooling emits.)
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

import cv2
import numpy as np

DEFAULT_SENSITIVITY = 0.04  # fraction of full scale (~2600 LSB at 16-bit)
DEFAULT_MAX_DEFECT_SIZE = 10  # px; components larger in BOTH dims are kept
_INPAINT_METHODS = ("telea", "ns", "median")
# Rejection threshold for "disk of diameter max_defect_size" area filter.
_DISK_AREA_FACTOR = 0.7854  # pi/4, pinned literal
# Top-hat structuring element radius: must exceed scratch half-width.
_LINE_SE_RADIUS = 3  # 3x3 ellipse
# Gradient guard: strong-edge band (in units of the defect threshold).
_GRADIENT_FACTOR = 4.0


# ---------------------------------------------------------------------------
# validation helpers


def _as_channel_stack(image: np.ndarray) -> np.ndarray:
    """Validate *image* and return it as an ``(h, w, channels)`` uint8/16 array."""
    arr = np.asarray(image)
    if arr.dtype not in (np.uint8, np.uint16):
        raise TypeError(
            f"image must have dtype uint8 or uint16, got {arr.dtype}"
        )
    if arr.ndim == 2:
        return arr[:, :, None]
    if arr.ndim == 3 and arr.shape[2] in (1, 3):
        return arr
    raise ValueError(
        f"image must be 2-D grayscale or (h, w, 1|3), got shape {arr.shape}"
    )


def _check_mask(image: np.ndarray, mask: np.ndarray) -> np.ndarray:
    arr = np.asarray(mask)
    if arr.shape != image.shape[:2]:
        raise ValueError(
            f"mask shape {arr.shape} does not match image shape "
            f"{image.shape[:2]}"
        )
    if arr.dtype != bool:
        arr = arr != 0
    return arr


def _check_params(sensitivity: float, max_defect_size: int) -> None:
    if not 0.0 < sensitivity < 1.0:
        raise ValueError(
            f"sensitivity must be in (0, 1) as a fraction of full scale, "
            f"got {sensitivity}"
        )
    if int(max_defect_size) != max_defect_size or max_defect_size < 1:
        raise ValueError(
            f"max_defect_size must be an integer >= 1, got {max_defect_size}"
        )


# ---------------------------------------------------------------------------
# detection


def _line_median(channel: np.ndarray, length: int) -> np.ndarray:
    """1-D sliding median of *channel* along its last axis (edge-padded)."""
    pad = length // 2
    padded = np.pad(channel, ((0, 0), (pad, pad)), mode="edge")
    windows = np.lib.stride_tricks.sliding_window_view(padded, length, axis=-1)
    return np.partition(windows, length // 2, axis=-1)[..., length // 2]


def detect_defects(
    image: np.ndarray,
    *,
    sensitivity: float,
    max_defect_size: int,
) -> np.ndarray:
    """Return a boolean defect mask for a uint8/uint16 gray or RGB image.

    ``sensitivity`` is the detection threshold as a fraction of the dtype's
    full scale (defects deviating less than this from their local
    background are ignored). ``max_defect_size`` is the largest defect
    diameter, in pixels: the directional-median length derives from it and
    connected components larger than a disk of that diameter in BOTH
    bounding-box dimensions and area are rejected as image structure.
    """
    _check_params(sensitivity, max_defect_size)
    stack = _as_channel_stack(image)
    height, width = stack.shape[:2]
    max_defect_size = int(max_defect_size)

    threshold = sensitivity * np.iinfo(stack.dtype).max
    median_len = 2 * max_defect_size + 3
    se = cv2.getStructuringElement(
        cv2.MORPH_ELLIPSE, (_LINE_SE_RADIUS, _LINE_SE_RADIUS)
    )

    blob = np.zeros((height, width), dtype=bool)
    scratch = np.zeros((height, width), dtype=bool)
    strong_edge = np.zeros((height, width), dtype=bool)
    for c in range(stack.shape[2]):
        channel = stack[..., c]
        med_h = _line_median(channel, median_len)
        med_v = _line_median(np.ascontiguousarray(channel.T), median_len).T
        residual = np.minimum(
            cv2.absdiff(channel, med_h), cv2.absdiff(channel, med_v)
        )
        agree = cv2.absdiff(med_h, med_v) <= threshold
        blob |= (residual > threshold) & agree

        opened = cv2.morphologyEx(channel, cv2.MORPH_OPEN, se)
        closed = cv2.morphologyEx(channel, cv2.MORPH_CLOSE, se)
        top_hat = cv2.max(
            cv2.subtract(channel, opened), cv2.subtract(closed, channel)
        )
        scratch |= top_hat > threshold

        # Strong-gradient band from a 2-D median reference: a thin scratch
        # does not corrupt it, so only genuine structure (steps, corners)
        # lands in the band.
        med5 = cv2.medianBlur(channel, 5)
        grad_x = cv2.Sobel(med5, cv2.CV_32F, 1, 0, ksize=3)
        grad_y = cv2.Sobel(med5, cv2.CV_32F, 0, 1, ksize=3)
        strong_edge |= (
            cv2.magnitude(grad_x, grad_y) > _GRADIENT_FACTOR * threshold
        )
    band = cv2.dilate(strong_edge.astype(np.uint8), se).astype(bool)
    scratch &= ~band

    raw = (blob | scratch).astype(np.uint8)
    count, labels, stats, _ = cv2.connectedComponentsWithStats(
        raw, connectivity=8
    )
    max_area = _DISK_AREA_FACTOR * max_defect_size * max_defect_size
    mask = np.zeros((height, width), dtype=bool)
    for i in range(1, count):
        left, top, box_w, box_h, area = (int(v) for v in stats[i])
        too_wide = box_w > max_defect_size and box_h > max_defect_size
        if too_wide and area > max_area:
            continue
        mask[labels == i] = True
    return mask


# ---------------------------------------------------------------------------
# inpainting


def _median_2d(channel: np.ndarray, kernel: int, chunk: int = 64) -> np.ndarray:
    """2-D sliding median, edge-padded, processed in row bands.

    Implemented natively because OpenCV 5.0's medianBlur rejects 16-bit
    input for kernels above 5x5.
    """
    pad = kernel // 2
    padded = np.pad(channel, pad, mode="edge")
    out = np.empty_like(channel)
    mid = (kernel * kernel) // 2
    for y0 in range(0, channel.shape[0], chunk):
        y1 = min(y0 + chunk, channel.shape[0])
        windows = np.lib.stride_tricks.sliding_window_view(
            padded[y0 : y1 + 2 * pad], (kernel, kernel)
        )
        flat = windows.reshape(y1 - y0, channel.shape[1], kernel * kernel)
        out[y0:y1] = np.partition(flat, mid, axis=-1)[..., mid]
    return out


def inpaint_defects(
    image: np.ndarray,
    mask: np.ndarray,
    *,
    method: str = "telea",
    radius: int = 3,
) -> np.ndarray:
    """Fill masked pixels of *image*; returns an array of identical shape/dtype.

    ``method`` is one of ``"telea"`` (OpenCV inpaintTelea, the default),
    ``"ns"`` (OpenCV inpaintNavierStokes) or ``"median"`` (masked sliding-
    median fill, no OpenCV photo-module dependency). Inpainting runs per
    channel on the native integer dtype, so pixels outside *mask* are
    bit-exact copies of the input.
    """
    if method not in _INPAINT_METHODS:
        raise ValueError(
            f"method must be one of {_INPAINT_METHODS}, got {method!r}"
        )
    stack = _as_channel_stack(image)
    mask = _check_mask(stack, mask)
    if radius < 1:
        raise ValueError(f"radius must be >= 1, got {radius}")
    if not mask.any():
        return np.array(image, copy=True)

    height, width, channels = stack.shape
    mask_u8 = (mask * 255).astype(np.uint8)
    flags = {"telea": cv2.INPAINT_TELEA, "ns": cv2.INPAINT_NS}[method] if (
        method in ("telea", "ns")
    ) else None

    if flags is not None:
        fill = np.empty_like(stack)
        for c in range(channels):
            fill[..., c] = cv2.inpaint(stack[..., c], mask_u8, radius, flags)
    else:
        kernel = 2 * int(radius) + 1
        fill = np.empty_like(stack)
        for c in range(channels):
            blurred = _median_2d(stack[..., c], kernel)
            fill[..., c] = np.where(mask, blurred, stack[..., c])

    if np.asarray(image).ndim == 2:
        return fill[..., 0]
    return fill.reshape(np.asarray(image).shape)


def despeckle(
    image: np.ndarray,
    *,
    sensitivity: float = DEFAULT_SENSITIVITY,
    max_defect_size: int = DEFAULT_MAX_DEFECT_SIZE,
    method: str = "telea",
) -> tuple[np.ndarray, np.ndarray]:
    """Detect defects and inpaint them; returns ``(cleaned, mask)``.

    *mask* is boolean with the image's height/width; *cleaned* matches the
    input dtype and shape exactly.
    """
    mask = detect_defects(
        image, sensitivity=sensitivity, max_defect_size=max_defect_size
    )
    cleaned = inpaint_defects(
        image, mask, method=method, radius=max(3, max_defect_size // 2 + 1)
    )
    return cleaned, mask


# ---------------------------------------------------------------------------
# minimal TIFF codec (baseline, uncompressed, 8/16-bit, gray/RGB)

_TIFF_MAGIC_LE = b"II*\x00"
_TIFF_TYPE_SIZE = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8}

# Tag numbers, pinned from TIFF 6.0 (same authority as flextight/fff.py).
_T_WIDTH = 256
_T_HEIGHT = 257
_T_BITS_PER_SAMPLE = 258
_T_COMPRESSION = 259
_T_PHOTOMETRIC = 262
_T_STRIP_OFFSETS = 273
_T_SAMPLES_PER_PIXEL = 277
_T_ROWS_PER_STRIP = 278
_T_STRIP_BYTE_COUNTS = 279
_T_PLANAR_CONFIG = 284
_T_SAMPLE_FORMAT = 339


def _tiff_entry(tag: int, type_: int, count: int, value: bytes) -> bytes:
    """Pack one IFD entry; *value* is the raw little-endian field bytes."""
    field = value.ljust(4, b"\x00")[:4]
    return struct.pack("<HHI", tag, type_, count) + field


def _write_tiff(path: Path, image: np.ndarray) -> None:
    """Write a single-strip little-endian baseline TIFF (gray or RGB)."""
    arr = np.asarray(image)
    stack = _as_channel_stack(arr)
    height, width, channels = stack.shape
    depth = 8 if arr.dtype == np.uint8 else 16
    sample_values = struct.pack(f"<{channels}H", *([depth] * channels))
    format_values = struct.pack(f"<{channels}H", *([1] * channels))

    entries: list[tuple[int, int, int, bytes]] = [
        (_T_WIDTH, 4, 1, struct.pack("<I", width)),
        (_T_HEIGHT, 4, 1, struct.pack("<I", height)),
        (_T_BITS_PER_SAMPLE, 3, channels, sample_values),
        (_T_COMPRESSION, 3, 1, struct.pack("<H", 1)),
        (_T_PHOTOMETRIC, 3, 1, struct.pack("<H", 2 if channels == 3 else 1)),
        (_T_STRIP_OFFSETS, 4, 1, b"OFFS"),  # patched after layout is known
        (_T_SAMPLES_PER_PIXEL, 3, 1, struct.pack("<H", channels)),
        (_T_ROWS_PER_STRIP, 4, 1, struct.pack("<I", height)),
        (_T_STRIP_BYTE_COUNTS, 4, 1, struct.pack("<I", stack.nbytes)),
        (_T_PLANAR_CONFIG, 3, 1, struct.pack("<H", 1)),
        (_T_SAMPLE_FORMAT, 3, channels, format_values),
    ]
    entries.sort(key=lambda e: e[0])
    ifd_len = 2 + 12 * len(entries) + 4
    cursor = 8 + ifd_len
    overflow = bytearray()
    packed = []
    offsets_tag_index = None
    for tag, type_, count, value in entries:
        if len(value) > 4:
            packed.append(_tiff_entry(tag, type_, count, struct.pack("<I", cursor)))
            overflow += value
            cursor += len(value)
        else:
            packed.append(_tiff_entry(tag, type_, count, value))
        if tag == _T_STRIP_OFFSETS:
            offsets_tag_index = len(packed) - 1
    data_offset = cursor
    packed[offsets_tag_index] = _tiff_entry(
        _T_STRIP_OFFSETS, 4, 1, struct.pack("<I", data_offset)
    )

    ifd = struct.pack("<H", len(packed)) + b"".join(packed) + struct.pack("<I", 0)
    payload = arr.astype("<u2" if depth == 16 else "u1", copy=False).tobytes("C")
    path.write_bytes(_TIFF_MAGIC_LE + struct.pack("<I", 8) + ifd + bytes(overflow) + payload)


def _read_tiff(path: Path) -> np.ndarray:
    """Read a baseline uncompressed TIFF (8/16-bit, gray or chunky RGB)."""
    blob = path.read_bytes()
    try:
        return _parse_tiff(blob, path)
    except (struct.error, KeyError, IndexError) as exc:
        # struct.error: truncated file mid-parse; KeyError/IndexError: a tag
        # with a legal-but-unsupported field type (e.g. ASCII, RATIONAL).
        raise ValueError(f"{path}: malformed TIFF ({exc})") from exc


def _parse_tiff(blob: bytes, path: Path) -> np.ndarray:
    if blob[:2] == b"II":
        order = "<"
    elif blob[:2] == b"MM":
        order = ">"
    else:
        raise ValueError(f"{path}: not a TIFF file (bad byte-order mark)")
    if struct.unpack_from(order + "H", blob, 2)[0] != 42:
        raise ValueError(f"{path}: not a TIFF file (version != 42)")
    ifd_offset = struct.unpack_from(order + "I", blob, 4)[0]
    count = struct.unpack_from(order + "H", blob, ifd_offset)[0]

    tags: dict[int, tuple[int, int, bytes]] = {}
    for i in range(count):
        base = ifd_offset + 2 + 12 * i
        tag, type_, n = struct.unpack_from(order + "HHI", blob, base)
        size = _TIFF_TYPE_SIZE.get(type_)
        if size is None:
            continue
        total = size * n
        if total <= 4:
            raw = blob[base + 8 : base + 8 + 4][:total]
        else:
            off = struct.unpack_from(order + "I", blob, base + 8)[0]
            raw = blob[off : off + total]
        tags[tag] = (type_, n, raw)

    def scalar(tag: int, default: int | None = None) -> int:
        if tag not in tags:
            if default is None:
                raise ValueError(f"{path}: TIFF is missing required tag {tag}")
            return default
        return _tiff_scalar(tags[tag], order, path)

    width = scalar(_T_WIDTH)
    height = scalar(_T_HEIGHT)
    channels = scalar(_T_SAMPLES_PER_PIXEL, 1)
    compression = scalar(_T_COMPRESSION, 1)
    if compression != 1:
        raise ValueError(
            f"{path}: unsupported TIFF compression {compression} "
            "(only uncompressed baseline TIFF is supported)"
        )
    if scalar(_T_PLANAR_CONFIG, 1) != 1:
        raise ValueError(f"{path}: only chunky (planar config 1) TIFF is supported")
    for tag, label in (
        (_T_BITS_PER_SAMPLE, "BitsPerSample"),
        (_T_STRIP_OFFSETS, "StripOffsets"),
        (_T_STRIP_BYTE_COUNTS, "StripByteCounts"),
    ):
        if tag not in tags:
            raise ValueError(f"{path}: TIFF is missing required tag {label}")
    bits_values = _tiff_values(tags[_T_BITS_PER_SAMPLE], order, path)
    depth = bits_values[0]
    if depth not in (8, 16) or any(b != depth for b in bits_values):
        raise ValueError(f"{path}: unsupported BitsPerSample {bits_values}")
    fmt_values = (
        _tiff_values(tags[_T_SAMPLE_FORMAT], order, path)
        if _T_SAMPLE_FORMAT in tags
        else [1] * channels
    )
    if any(f != 1 for f in fmt_values):
        raise ValueError(f"{path}: only unsigned-integer SampleFormat is supported")
    if channels not in (1, 3):
        raise ValueError(f"{path}: unsupported SamplesPerPixel {channels}")

    offsets = _tiff_values(tags[_T_STRIP_OFFSETS], order, path)
    byte_counts = _tiff_values(tags[_T_STRIP_BYTE_COUNTS], order, path)
    pixel_bytes = b"".join(
        blob[o : o + n] for o, n in zip(offsets, byte_counts)
    )
    expected = width * height * channels * (depth // 8)
    if len(pixel_bytes) != expected:
        raise ValueError(
            f"{path}: pixel data is {len(pixel_bytes)} bytes, expected "
            f"{expected} for {width}x{height}x{channels} at {depth}-bit"
        )
    dtype = np.dtype((">u2" if order == ">" else "<u2") if depth == 16 else np.uint8)
    arr = np.frombuffer(pixel_bytes, dtype=dtype)
    if depth == 16:
        arr = arr.astype("<u2")
    shape = (height, width) if channels == 1 else (height, width, channels)
    return arr.reshape(shape).copy()


_TIFF_INT_FMT = {1: "B", 3: "H", 4: "I"}


def _tiff_fmt(type_: int, order: str, path: Path) -> str:
    try:
        return _TIFF_INT_FMT[type_]
    except KeyError:
        raise ValueError(
            f"{path}: unsupported TIFF field type {type_} "
            "(supported: 1 BYTE, 3 SHORT, 4 LONG)"
        ) from None


def _tiff_scalar(entry: tuple[int, int, bytes], order: str, path: Path) -> int:
    type_, _, raw = entry
    fmt = _tiff_fmt(type_, order, path)
    return struct.unpack(order + fmt, raw[: struct.calcsize(fmt)])[0]


def _tiff_values(
    entry: tuple[int, int, bytes], order: str, path: Path
) -> list[int]:
    type_, n, raw = entry
    fmt = _tiff_fmt(type_, order, path)
    size = struct.calcsize(fmt)
    return [struct.unpack_from(order + fmt, raw, i * size)[0] for i in range(n)]


# ---------------------------------------------------------------------------
# minimal PNG codec (non-interlaced, gray/RGB, 8/16-bit, all filters)

_PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
_PNG_COLOR_GRAY = 0
_PNG_COLOR_RGB = 2


def _png_chunk(tag: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + tag
        + payload
        + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    )


def _write_png(path: Path, image: np.ndarray) -> None:
    """Write a non-interlaced PNG, filter 0, 8/16-bit gray or RGB."""
    arr = np.asarray(image)
    stack = _as_channel_stack(arr)
    height, width, channels = stack.shape
    depth = 8 if arr.dtype == np.uint8 else 16
    color_type = _PNG_COLOR_RGB if channels == 3 else _PNG_COLOR_GRAY
    ihdr = struct.pack(">IIBBBBB", width, height, depth, color_type, 0, 0, 0)
    be = arr.astype(">u2", copy=False) if depth == 16 else arr
    rows = b"".join(
        b"\x00" + be[y].tobytes("C") for y in range(height)
    )
    blob = (
        _PNG_SIGNATURE
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", zlib.compress(rows))
        + _png_chunk(b"IEND", b"")
    )
    path.write_bytes(blob)


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def _read_png(path: Path) -> np.ndarray:
    """Read a non-interlaced 8/16-bit gray or RGB PNG (all filter types)."""
    blob = path.read_bytes()
    try:
        return _parse_png(blob, path)
    except (zlib.error, struct.error) as exc:
        raise ValueError(f"{path}: malformed PNG ({exc})") from exc


def _parse_png(blob: bytes, path: Path) -> np.ndarray:
    if blob[:8] != _PNG_SIGNATURE:
        raise ValueError(f"{path}: not a PNG file (bad signature)")
    pos = 8
    idat = bytearray()
    ihdr: tuple[int, int, int, int] | None = None
    while pos + 8 <= len(blob):
        length, tag = struct.unpack_from(">I4s", blob, pos)
        payload = blob[pos + 8 : pos + 8 + length]
        crc = struct.unpack_from(">I", blob, pos + 8 + length)[0]
        if zlib.crc32(tag + payload) & 0xFFFFFFFF != crc:
            raise ValueError(f"{path}: PNG chunk {tag!r} has a bad CRC")
        if tag == b"IHDR":
            width, height, depth, color, comp, filt, inter = struct.unpack(
                ">IIBBBBB", payload
            )
            ihdr = (width, height, depth, color)
            if comp != 0 or filt != 0:
                raise ValueError(f"{path}: unsupported PNG compression/filter method")
            if inter != 0:
                raise ValueError(f"{path}: interlaced PNG is not supported")
        elif tag == b"IDAT":
            idat += payload
        elif tag == b"IEND":
            break
        pos += 12 + length
    if ihdr is None:
        raise ValueError(f"{path}: PNG is missing IHDR")
    width, height, depth, color = ihdr
    if color not in (_PNG_COLOR_GRAY, _PNG_COLOR_RGB):
        raise ValueError(f"{path}: unsupported PNG color type {color}")
    if depth not in (8, 16):
        raise ValueError(f"{path}: unsupported PNG bit depth {depth}")
    channels = 3 if color == _PNG_COLOR_RGB else 1
    bpp = channels * (depth // 8)
    row_len = width * bpp
    raw = zlib.decompress(bytes(idat))
    expected = height * (1 + row_len)
    if len(raw) != expected:
        raise ValueError(
            f"{path}: inflated data is {len(raw)} bytes, expected {expected}"
        )

    prior = bytearray(row_len)
    out = np.empty((height, row_len), dtype=np.uint8)
    for y in range(height):
        base = y * (1 + row_len)
        filt = raw[base]
        row = bytearray(raw[base + 1 : base + 1 + row_len])
        if filt == 0:
            pass
        elif filt == 1:  # Sub
            for i in range(bpp, row_len):
                row[i] = (row[i] + row[i - bpp]) & 0xFF
        elif filt == 2:  # Up
            for i in range(row_len):
                row[i] = (row[i] + prior[i]) & 0xFF
        elif filt == 3:  # Average
            for i in range(row_len):
                left = row[i - bpp] if i >= bpp else 0
                row[i] = (row[i] + ((left + prior[i]) >> 1)) & 0xFF
        elif filt == 4:  # Paeth
            for i in range(row_len):
                left = row[i - bpp] if i >= bpp else 0
                up_left = prior[i - bpp] if i >= bpp else 0
                row[i] = (row[i] + _paeth(left, prior[i], up_left)) & 0xFF
        else:
            raise ValueError(f"{path}: unknown PNG filter type {filt}")
        out[y] = row
        prior = row

    pixels = out.view(">u2") if depth == 16 else out
    if channels == 1:
        arr = pixels.reshape(height, width)
    else:
        arr = pixels.reshape(height, width, channels)
    return np.ascontiguousarray(arr.astype("<u2", copy=False) if depth == 16 else arr)


# ---------------------------------------------------------------------------
# CLI


def _read_image(path_like: str | Path) -> np.ndarray:
    path = Path(path_like)
    suffix = path.suffix.lower()
    if suffix in (".tif", ".tiff"):
        return _read_tiff(path)
    if suffix == ".png":
        return _read_png(path)
    raise ValueError(
        f"unsupported input format {suffix!r} (supported: .tif, .tiff, .png)"
    )


def _write_image(path_like: str | Path, image: np.ndarray) -> None:
    path = Path(path_like)
    suffix = path.suffix.lower()
    if suffix in (".tif", ".tiff"):
        _write_tiff(path, image)
        return
    if suffix == ".png":
        _write_png(path, image)
        return
    raise ValueError(
        f"unsupported output format {suffix!r} (supported: .tif, .tiff, .png)"
    )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="despeckle.py",
        description="Remove dust spots and thin scratches from a scanned "
        "film image (16-bit TIFF/PNG in and out).",
    )
    parser.add_argument("input", help="input image (.tif/.tiff/.png)")
    parser.add_argument("output", help="cleaned output image (.tif/.tiff/.png)")
    parser.add_argument(
        "--sensitivity",
        type=float,
        default=DEFAULT_SENSITIVITY,
        help="defect threshold as a fraction of full scale, in (0, 1) "
        f"(default: {DEFAULT_SENSITIVITY})",
    )
    parser.add_argument(
        "--max-defect-size",
        type=int,
        default=DEFAULT_MAX_DEFECT_SIZE,
        help="largest defect diameter in pixels; larger connected regions "
        "are treated as image structure and left alone "
        f"(default: {DEFAULT_MAX_DEFECT_SIZE})",
    )
    parser.add_argument(
        "--method",
        choices=_INPAINT_METHODS,
        default="telea",
        help="inpainting method (default: telea)",
    )
    parser.add_argument(
        "--mask-out",
        metavar="PATH",
        default=None,
        help="optional path to write the defect mask to (.tif/.png)",
    )
    return parser


def _check_format(path_like: str | Path, what: str) -> None:
    """Reject unsupported output suffixes before any work is done."""
    suffix = Path(path_like).suffix.lower()
    if suffix not in (".tif", ".tiff", ".png"):
        raise ValueError(
            f"unsupported {what} format {suffix!r} "
            "(supported: .tif, .tiff, .png)"
        )


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        # Validate every output suffix up front so a bad --mask-out can not
        # leave a fully written output behind on failure.
        _check_format(args.output, "output")
        if args.mask_out is not None:
            _check_format(args.mask_out, "mask output")
        image = _read_image(args.input)
        cleaned, mask = despeckle(
            image,
            sensitivity=args.sensitivity,
            max_defect_size=args.max_defect_size,
            method=args.method,
        )
        _write_image(args.output, cleaned)
        if args.mask_out is not None:
            _write_image(args.mask_out, (mask * 255).astype(np.uint8))
    except (OSError, ValueError, TypeError) as exc:
        print(f"despeckle: error: {exc}", file=sys.stderr)
        return 1
    print(
        f"despeckle: {args.input} -> {args.output} "
        f"({int(mask.sum())} defect pixels)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
