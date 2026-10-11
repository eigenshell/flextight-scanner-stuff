"""Tests for tools/despeckle — dust/scratch removal pass for scanned film.

The Flextight Precision II has no infrared channel, so defect cleanup is
purely software-side (FlexColor's own ApplyDust is a scanner-hardware pass;
see tools/xml2dtstyle.py's docstring). This suite pins the behaviour of the
software despeckler against synthetic fixtures: a clean 16-bit image built
from a smooth gradient, two hard step edges, and fine sinusoidal texture,
dusted with disks of 3-9 px diameter and 1-2 px wide scratch lines.

All fixture constants are pinned here as literals (project rule: tests must
not import constants from the implementation). Assertions:
  - recall of defect pixels > 90 %, false positives in spot-free regions
    < 0.1 % (measured on this fixture: ~99.5 % recall, 0 false positives)
  - PSNR improvement >= +6 dB (measured: ~+20 dB)
  - zero structure loss: max |cleaned - original| <= 1 in spot-free pixels
    (measured: 0 — clean pixels pass through bit-exact)
"""

import shutil
import struct
import subprocess
import time
import zlib

import numpy as np
import pytest

from tools.despeckle import (
    detect_defects,
    despeckle,
    inpaint_defects,
    main,
)

# --- Fixture constants (pinned literals) -----------------------------------
IMG_H = IMG_W = 384
BASE_LEVEL = 8000
GRAD_X = 40  # per-column rise
GRAD_Y = 25  # per-row rise
VSTEP_COL = 96  # vertical edge column
VSTEP_DELTA = 12000
HSTEP_ROW = 288  # horizontal edge row
HSTEP_DELTA = -9000
SIN_AMP = 1500  # ~2.3 % of full scale; below the default 4 % threshold
SIN_PERIOD = 6  # fine texture, close to defect scale on purpose

# (row, col, radius) — radii give disk diameters 3..9 px (spec: 2-10 px).
DARK_DISKS = [(50, 200, 2), (120, 300, 3), (200, 150, 4), (330, 333, 4)]
DARK_DELTA = -9000  # ~14 % of full scale
BRIGHT_DISKS = [(60, 60, 1), (160, 250, 2), (250, 330, 3)]
BRIGHT_DELTA = 11000  # ~17 % of full scale

# (r0, c0, r1, c1, width, delta): horizontal, vertical, 45-degree and
# shallow diagonal scratches, 1-2 px wide.
SCRATCHES = [
    (30, 180, 30, 240, 1, 10000),
    (150, 40, 195, 40, 1, -10000),
    (300, 120, 320, 160, 1, 10000),
    (100, 130, 130, 131, 2, -10000),
]

# Per-channel dust deltas for the RGB fixture (pinned, non-uniform so the
# per-channel detection path is exercised). One row per scratch, sign
# matching the grayscale delta in SCRATCHES.
COLOR_DARK_DELTAS = [-9000, -7500, -10500]
COLOR_BRIGHT_DELTAS = [11000, 9000, 12500]
COLOR_SCRATCH_DELTAS = [
    [10000, 8500, 11000],
    [-10000, -8500, -11000],
    [10000, 8500, 11000],
    [-10000, -8500, -11000],
]

DEFAULT_SENSITIVITY = 0.04  # fraction of full scale
DEFAULT_MAX_DEFECT_SIZE = 10  # px diameter cap


def make_clean_color():
    """Clean 16-bit RGB image: gradient + two hard edges + sinusoid.

    Channels are rolled copies so each channel has its edges/texture at
    different positions while sharing the same defect map.
    """
    yy, xx = np.mgrid[0:IMG_H, 0:IMG_W].astype(np.float64)
    gray = BASE_LEVEL + GRAD_X * xx + GRAD_Y * yy
    gray = gray + VSTEP_DELTA * (xx >= VSTEP_COL)
    gray = gray + HSTEP_DELTA * (yy >= HSTEP_ROW)
    gray = gray + SIN_AMP * np.sin(2 * np.pi * xx / SIN_PERIOD) * np.sin(
        2 * np.pi * yy / SIN_PERIOD
    )
    gray = np.clip(gray, 0, 65535).astype(np.uint16)
    return np.dstack(
        [gray, np.roll(gray, 3, axis=0), np.roll(gray, 5, axis=1)]
    )


def dust_image(clean):
    """Stamp the pinned defect set onto *clean*; returns (dusted, defect)."""
    dusted = clean.copy()
    defect = np.zeros(clean.shape[:2], dtype=bool)
    yy, xx = np.mgrid[0:IMG_H, 0:IMG_W]

    def stamp(mask, deltas):
        nonlocal dusted, defect
        for c in range(clean.shape[2]):
            dusted[..., c][mask] = np.clip(
                dusted[..., c][mask].astype(np.int32) + deltas[c], 0, 65535
            ).astype(np.uint16)
        defect |= mask

    for row, col, r in DARK_DISKS:
        stamp((yy - row) ** 2 + (xx - col) ** 2 <= r * r, COLOR_DARK_DELTAS)
    for row, col, r in BRIGHT_DISKS:
        stamp((yy - row) ** 2 + (xx - col) ** 2 <= r * r, COLOR_BRIGHT_DELTAS)
    for i, (r0, c0, r1, c1, w, _d) in enumerate(SCRATCHES):
        n = max(abs(r1 - r0), abs(c1 - c0)) + 1
        rr = np.linspace(r0, r1, n).astype(int)
        cc = np.linspace(c0, c1, n).astype(int)
        for dr in range(w):
            for dc in range(w):
                m = np.zeros((IMG_H, IMG_W), dtype=bool)
                m[np.clip(rr + dr, 0, IMG_H - 1), np.clip(cc + dc, 0, IMG_W - 1)] = True
                stamp(m, COLOR_SCRATCH_DELTAS[i])

    return dusted, defect


def dilate(mask, iterations=2):
    """Binary dilation with a 3x3 cross, pure numpy (no OpenCV in tests)."""
    out = mask.copy()
    for _ in range(iterations):
        grown = out.copy()
        grown[1:, :] |= out[:-1, :]
        grown[:-1, :] |= out[1:, :]
        grown[:, 1:] |= out[:, :-1]
        grown[:, :-1] |= out[:, 1:]
        out = grown
    return out


def psnr(a, b, maxval):
    """PSNR in dB; implemented here rather than imported from the tool."""
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2)
    if mse == 0:
        return float("inf")
    return 10 * np.log10(maxval * maxval / mse)


@pytest.fixture(scope="module")
def color_fixture():
    clean = make_clean_color()
    dusted, defect = dust_image(clean)
    return clean, dusted, defect


# --- Detection quality ------------------------------------------------------


def test_detect_recall(color_fixture):
    _, dusted, defect = color_fixture
    mask = detect_defects(
        dusted,
        sensitivity=DEFAULT_SENSITIVITY,
        max_defect_size=DEFAULT_MAX_DEFECT_SIZE,
    )
    assert mask.shape == defect.shape
    assert mask.dtype == bool
    recall = (mask & defect).sum() / defect.sum()
    assert recall > 0.90, f"recall {recall:.4f} below 0.90"


def test_detect_false_positive_rate(color_fixture):
    clean, dusted, defect = color_fixture
    mask = detect_defects(
        dusted,
        sensitivity=DEFAULT_SENSITIVITY,
        max_defect_size=DEFAULT_MAX_DEFECT_SIZE,
    )
    # Spot-free = anything not within 2 px of a defect (covers detection
    # halos around real defects); edges and texture are inside this region.
    spot_free = ~dilate(defect, iterations=2)
    fp_rate = (mask & spot_free).sum() / spot_free.sum()
    assert fp_rate < 0.001, f"false-positive rate {fp_rate:.6f} >= 0.001"
    # The clean image itself must yield an empty mask.
    mask_clean = detect_defects(
        clean,
        sensitivity=DEFAULT_SENSITIVITY,
        max_defect_size=DEFAULT_MAX_DEFECT_SIZE,
    )
    assert not mask_clean.any()


def test_detect_lower_sensitivity_operating_point(color_fixture):
    """Second operating point: a lower threshold trades false positives on
    the fine sinusoidal texture for marginally better recall. Bounds are
    deliberately relaxed relative to the default-sensitivity test.
    Measured on this fixture: recall 0.970, fp_rate 0.001205."""
    _, dusted, defect = color_fixture
    mask = detect_defects(
        dusted,
        sensitivity=0.02,
        max_defect_size=DEFAULT_MAX_DEFECT_SIZE,
    )
    spot_free = ~dilate(defect, iterations=2)
    recall = (mask & defect).sum() / defect.sum()
    fp_rate = (mask & spot_free).sum() / spot_free.sum()
    assert recall > 0.95, f"recall {recall:.4f} below 0.95 at sensitivity 0.02"
    assert fp_rate < 0.005, f"fp_rate {fp_rate:.6f} >= 0.005 at sensitivity 0.02"


# --- Inpainting quality -----------------------------------------------------


def test_despeckle_improves_psnr(color_fixture):
    clean, dusted, defect = color_fixture
    cleaned, mask = despeckle(
        dusted,
        sensitivity=DEFAULT_SENSITIVITY,
        max_defect_size=DEFAULT_MAX_DEFECT_SIZE,
    )
    assert cleaned.shape == dusted.shape
    assert cleaned.dtype == dusted.dtype
    before = psnr(dusted, clean, 65535)
    after = psnr(cleaned, clean, 65535)
    # Measured on this fixture: before ~45 dB, after ~64 dB.
    assert after - before >= 6.0, f"PSNR gain {after - before:.2f} dB < 6 dB"


def test_despeckle_no_structure_loss(color_fixture):
    """The acceptance test: clean pixels must pass through untouched."""
    clean, dusted, defect = color_fixture
    cleaned, _ = despeckle(
        dusted,
        sensitivity=DEFAULT_SENSITIVITY,
        max_defect_size=DEFAULT_MAX_DEFECT_SIZE,
    )
    spot_free = ~dilate(defect, iterations=2)
    max_diff = np.abs(cleaned.astype(np.int32) - clean.astype(np.int32))[
        spot_free
    ].max()
    # 16-bit LSB tolerance; measured on this fixture: 0 (bit-exact).
    assert max_diff <= 1, f"max |diff| in spot-free regions = {max_diff}"


def test_masked_region_actually_fixed(color_fixture):
    """Inpainting must change defect pixels, not just pass them through."""
    clean, dusted, defect = color_fixture
    cleaned, _ = despeckle(
        dusted,
        sensitivity=DEFAULT_SENSITIVITY,
        max_defect_size=DEFAULT_MAX_DEFECT_SIZE,
    )
    improved = (
        np.abs(cleaned.astype(np.int32) - clean.astype(np.int32))[defect]
        < np.abs(dusted.astype(np.int32) - clean.astype(np.int32))[defect]
    )
    assert improved.mean() > 0.95


# --- Method variants --------------------------------------------------------


@pytest.mark.parametrize("method", ["telea", "ns", "median"])
def test_inpaint_methods(color_fixture, method):
    clean, dusted, defect = color_fixture
    mask = detect_defects(
        dusted,
        sensitivity=DEFAULT_SENSITIVITY,
        max_defect_size=DEFAULT_MAX_DEFECT_SIZE,
    )
    cleaned = inpaint_defects(dusted, mask, method=method)
    assert cleaned.dtype == dusted.dtype
    assert cleaned.shape == dusted.shape
    after = psnr(cleaned, clean, 65535)
    before = psnr(dusted, clean, 65535)
    assert after > before


def test_inpaint_unknown_method_rejected(color_fixture):
    _, dusted, _ = color_fixture
    mask = np.zeros(dusted.shape[:2], dtype=bool)
    with pytest.raises(ValueError, match="method"):
        inpaint_defects(dusted, mask, method="blur")


# --- Edge cases -------------------------------------------------------------


def test_empty_mask_output_identical(color_fixture):
    _, dusted, _ = color_fixture
    mask = np.zeros(dusted.shape[:2], dtype=bool)
    cleaned = inpaint_defects(dusted, mask)
    np.testing.assert_array_equal(cleaned, dusted)


def test_all_defects_image_does_not_crash():
    """Pure noise: the size filter rejects everything; must not crash."""
    rng = np.random.default_rng(1234)
    noise = rng.integers(0, 65536, size=(128, 128, 3)).astype(np.uint16)
    cleaned, mask = despeckle(
        noise,
        sensitivity=DEFAULT_SENSITIVITY,
        max_defect_size=DEFAULT_MAX_DEFECT_SIZE,
    )
    assert cleaned.shape == noise.shape
    assert cleaned.dtype == noise.dtype


def test_8bit_input(color_fixture):
    clean16, dusted16, _ = color_fixture
    clean = (clean16 // 257).astype(np.uint8)
    dusted = (dusted16 // 257).astype(np.uint8)
    cleaned, mask = despeckle(
        dusted,
        sensitivity=DEFAULT_SENSITIVITY,
        max_defect_size=DEFAULT_MAX_DEFECT_SIZE,
    )
    assert cleaned.dtype == np.uint8
    before = psnr(dusted, clean, 255)
    after = psnr(cleaned, clean, 255)
    assert after - before >= 6.0


@pytest.mark.parametrize("shape", [(1, 1), (2, 2), (5, 5), (7, 1), (1, 7)])
def test_tiny_images(shape):
    img = np.full(shape, 1000, dtype=np.uint16)
    mask = detect_defects(
        img, sensitivity=DEFAULT_SENSITIVITY, max_defect_size=DEFAULT_MAX_DEFECT_SIZE
    )
    assert mask.shape == shape
    cleaned = inpaint_defects(img, mask)
    assert cleaned.shape == img.shape
    assert cleaned.dtype == img.dtype


def test_grayscale_two_dim(color_fixture):
    _, dusted, defect = color_fixture
    gray = dusted[..., 0]
    cleaned, mask = despeckle(
        gray,
        sensitivity=DEFAULT_SENSITIVITY,
        max_defect_size=DEFAULT_MAX_DEFECT_SIZE,
    )
    assert cleaned.ndim == 2
    recall = (mask & defect).sum() / defect.sum()
    assert recall > 0.90


# --- Strict validation ------------------------------------------------------

BAD_IMAGES = [
    pytest.param(np.zeros((8, 8), np.float64), id="float64"),
    pytest.param(np.zeros((8, 8), np.int32), id="int32"),
    pytest.param(np.zeros((8, 8, 2), np.uint16), id="two-channels"),
    pytest.param(np.zeros((8, 8, 4), np.uint16), id="four-channels"),
    pytest.param(np.zeros((8,), np.uint16), id="one-dim"),
]


@pytest.mark.parametrize("image", BAD_IMAGES)
def test_detect_rejects_bad_input(image):
    with pytest.raises((TypeError, ValueError)):
        detect_defects(
            image, sensitivity=DEFAULT_SENSITIVITY,
            max_defect_size=DEFAULT_MAX_DEFECT_SIZE,
        )


@pytest.mark.parametrize("sensitivity", [0.0, -0.1, 1.0, 1.5])
def test_detect_rejects_bad_sensitivity(color_fixture, sensitivity):
    _, dusted, _ = color_fixture
    with pytest.raises(ValueError, match="sensitivity"):
        detect_defects(
            dusted, sensitivity=sensitivity,
            max_defect_size=DEFAULT_MAX_DEFECT_SIZE,
        )


@pytest.mark.parametrize("size", [0, -3])
def test_detect_rejects_bad_max_size(color_fixture, size):
    _, dusted, _ = color_fixture
    with pytest.raises(ValueError, match="max_defect_size"):
        detect_defects(
            dusted, sensitivity=DEFAULT_SENSITIVITY, max_defect_size=size
        )


def test_inpaint_rejects_mask_shape_mismatch(color_fixture):
    _, dusted, _ = color_fixture
    mask = np.zeros((8, 8), dtype=bool)
    with pytest.raises(ValueError, match="mask"):
        inpaint_defects(dusted, mask)


def test_despeckle_defaults_run(color_fixture):
    """Convenience API: defaults exist and process the fixture."""
    clean, dusted, _ = color_fixture
    cleaned, mask = despeckle(dusted)
    assert cleaned.dtype == dusted.dtype
    assert psnr(cleaned, clean, 65535) > psnr(dusted, clean, 65535)


# --- CLI --------------------------------------------------------------------


def run_cli(args):
    return main(args)


def test_cli_tiff_roundtrip(color_fixture, tmp_path):
    from tools.despeckle import _read_image, _write_image

    _, dusted, _ = color_fixture
    src = tmp_path / "in.tif"
    dst = tmp_path / "out.tif"
    mask_path = tmp_path / "mask.png"
    _write_image(src, dusted)
    rc = run_cli(
        [
            str(src), str(dst),
            "--sensitivity", str(DEFAULT_SENSITIVITY),
            "--max-defect-size", str(DEFAULT_MAX_DEFECT_SIZE),
            "--mask-out", str(mask_path),
        ]
    )
    assert rc == 0
    out = _read_image(dst)
    mask_out = _read_image(mask_path)
    assert out.dtype == np.uint16
    assert out.shape == dusted.shape
    assert mask_out.shape == dusted.shape[:2]
    # CLI must produce exactly what the API produces.
    expected, mask = despeckle(
        dusted,
        sensitivity=DEFAULT_SENSITIVITY,
        max_defect_size=DEFAULT_MAX_DEFECT_SIZE,
    )
    np.testing.assert_array_equal(out, expected)
    np.testing.assert_array_equal(mask_out > 0, mask)


def test_cli_png_roundtrip_16bit(tmp_path):
    from tools.despeckle import _read_image, _write_image

    yy, xx = np.mgrid[0:33, 0:17]
    img = (2000 + 30 * xx + 20 * yy).astype(np.uint16)  # smooth: no defects
    src = tmp_path / "in.png"
    dst = tmp_path / "out.png"
    _write_image(src, img)
    assert run_cli([str(src), str(dst)]) == 0
    np.testing.assert_array_equal(_read_image(dst), img)


def test_cli_8bit_tiff(tmp_path):
    from tools.despeckle import _read_image, _write_image

    yy, xx = np.mgrid[0:24, 0:40]
    img = np.dstack([(10 + xx) % 256, (20 + yy) % 256, (30 + xx + yy) % 256]
                    ).astype(np.uint8)  # smooth: no defects
    src = tmp_path / "in8.tif"
    dst = tmp_path / "out8.tif"
    _write_image(src, img)
    assert run_cli([str(src), str(dst)]) == 0
    np.testing.assert_array_equal(_read_image(dst), img)


def test_cli_rejects_unknown_extension(tmp_path, capsys):
    src = tmp_path / "in.bmp"
    src.write_bytes(b"not an image")
    rc = run_cli([str(src), str(tmp_path / "out.tif")])
    assert rc == 1
    assert "bmp" in capsys.readouterr().err.lower()


def _png_chunk(tag: bytes, payload: bytes) -> bytes:
    """One PNG chunk with a correct CRC; pinned wire format."""
    return (
        struct.pack(">I", len(payload))
        + tag
        + payload
        + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    )


@pytest.mark.parametrize(
    "name, blob, needle",
    [
        pytest.param(
            "truncated.tif",
            b"II*\x00" + struct.pack("<I", 8) + b"\x01\x00" + b"\x00" * 6,
            "malformed TIFF",
            id="tiff-truncated",
        ),
        pytest.param(
            "corrupt.png",
            b"\x89PNG\r\n\x1a\n"
            + _png_chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 0, 0, 0, 0))
            + _png_chunk(b"IDAT", b"not zlib data")
            + _png_chunk(b"IEND", b""),
            "malformed PNG",
            id="png-bad-idat",
        ),
    ],
)
def test_cli_malformed_input_no_traceback(tmp_path, capsys, name, blob, needle):
    """Truncated/corrupt input must exit 1 with an actionable message —
    never a traceback (struct.error/zlib.error are wrapped in ValueError)."""
    src = tmp_path / name
    src.write_bytes(blob)
    rc = run_cli([str(src), str(tmp_path / "out.tif")])
    err = capsys.readouterr().err
    assert rc == 1
    assert "Traceback" not in err
    assert needle in err


def test_cli_missing_input(tmp_path, capsys):
    rc = run_cli([str(tmp_path / "nope.tif"), str(tmp_path / "out.tif")])
    assert rc == 1
    assert "nope" in capsys.readouterr().err


def test_cli_bad_maskout_suffix_writes_nothing(color_fixture, tmp_path, capsys):
    """--mask-out is validated up front: a bad suffix must fail before the
    output image is written, not after (no partial success)."""
    from tools.despeckle import _write_image

    _, dusted, _ = color_fixture
    src = tmp_path / "in.tif"
    dst = tmp_path / "out.tif"
    _write_image(src, dusted)
    rc = run_cli([str(src), str(dst), "--mask-out", str(tmp_path / "m.bmp")])
    assert rc == 1
    assert "bmp" in capsys.readouterr().err.lower()
    assert not dst.exists()
    assert not (tmp_path / "m.bmp").exists()


def test_cli_argparse_usage_error(color_fixture, tmp_path, capsys):
    _, dusted, _ = color_fixture
    from tools.despeckle import _write_image

    src = tmp_path / "in.tif"
    _write_image(src, dusted)
    with pytest.raises(SystemExit) as exc:
        run_cli([str(src), str(tmp_path / "o.tif"), "--sensitivity", "abc"])
    assert exc.value.code == 2


# --- External authority / perf ---------------------------------------------


@pytest.mark.skipif(shutil.which("gmic") is None, reason="gmic not installed")
def test_written_tiff_readable_by_external_tool(color_fixture, tmp_path):
    """Our TIFF writer must produce files an independent reader accepts."""
    from tools.despeckle import _write_image

    _, dusted, _ = color_fixture
    src = tmp_path / "ext.tif"
    _write_image(src, dusted)
    proc = subprocess.run(
        ["gmic", str(src), "-o", str(tmp_path / "ext.png")],
        capture_output=True, text=True, timeout=60,
    )
    assert proc.returncode == 0, proc.stderr
    assert (tmp_path / "ext.png").exists()


def test_perf_4mp_color_under_30s():
    """Smoke: 4 MP 16-bit RGB must process in well under 30 s."""
    yy, xx = np.mgrid[0:2048, 0:2048]
    img = np.empty((2048, 2048, 3), np.uint16)
    base = 8000 + 40 * xx + 25 * yy
    for c in range(3):
        img[..., c] = (base + c * 2000) % 60000
    t0 = time.monotonic()
    cleaned, mask = despeckle(img)  # default settings
    elapsed = time.monotonic() - t0
    assert cleaned.shape == img.shape and cleaned.dtype == img.dtype
    assert elapsed < 25.0, f"4 MP despeckle took {elapsed:.1f}s (budget 30s)"


def test_perf_4mp_color_with_defects_full_pass():
    """Perf on a defect-laden 4 MP image: exercises detection AND inpainting
    (the clean-image variant above early-returns on an empty mask)."""
    yy, xx = np.mgrid[0:2048, 0:2048]
    base = (8000 + 40 * xx + 25 * yy).astype(np.uint16)
    img = np.dstack([base, np.roll(base, 7, axis=0), np.roll(base, 11, axis=1)])
    # Long scratches at pinned rows/columns.
    for row, c0, c1 in [(100, 300, 900), (1500, 100, 1900)]:
        seg = img[row, c0:c1, :].astype(np.int32) + 9000
        img[row, c0:c1, :] = np.clip(seg, 0, 65535).astype(np.uint16)
    for col, r0, r1 in [(500, 200, 800), (1800, 1200, 1800)]:
        seg = img[r0:r1, col, :].astype(np.int32) - 9000
        img[r0:r1, col, :] = np.clip(seg, 0, 65535).astype(np.uint16)
    # 200 random disks, radius 1-5 px, stamped in local windows.
    rng = np.random.default_rng(42)
    for _ in range(200):
        cy = int(rng.integers(10, 2038))
        cx = int(rng.integers(10, 2038))
        r = int(rng.integers(1, 6))
        y0, y1 = cy - r - 1, cy + r + 2
        x0, x1 = cx - r - 1, cx + r + 2
        ly, lx = np.mgrid[y0:y1, x0:x1]
        m = (ly - cy) ** 2 + (lx - cx) ** 2 <= r * r
        win = img[y0:y1, x0:x1, :].astype(np.int32)
        win[m] += 10000
        img[y0:y1, x0:x1, :] = np.clip(win, 0, 65535).astype(np.uint16)
    t0 = time.monotonic()
    cleaned, mask = despeckle(img)  # default settings
    elapsed = time.monotonic() - t0
    assert cleaned.shape == img.shape and cleaned.dtype == img.dtype
    assert mask.any(), "expected the stamped defects to be detected"
    assert mask.sum() < 50000, f"mask unexpectedly large: {mask.sum()} px"
    assert elapsed < 25.0, f"4 MP defect-laden despeckle took {elapsed:.1f}s"
