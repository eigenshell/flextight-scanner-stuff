# SPDX-License-Identifier: GPL-2.0-or-later
"""ftdev (libft's develop pipeline + writers) against ftraw.py, the Python
reference it was ported from, on a synthetic scan."""
import importlib.util
import shutil
import struct
import subprocess
import sys
import types
from pathlib import Path

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parent.parent
FTDEV = ROOT / "tools" / "ftdev"
needs_cc = pytest.mark.skipif(
    shutil.which("make") is None or shutil.which("cc") is None,
    reason="needs make and a C compiler")


def load_ftraw(monkeypatch):
    # ftraw imports PIL only to save PNGs; the stages under test don't
    monkeypatch.setitem(sys.modules, "PIL", types.SimpleNamespace(Image=None))
    spec = importlib.util.spec_from_file_location(
        "ftraw", ROOT / "tools/poc/ftraw.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_dng(path):
    """(tags {tag: (type, count, raw value bytes)}, pixels h x w x 3 u16)."""
    b = path.read_bytes()
    assert b[:4] == b"II*\x00"
    ifd = struct.unpack_from("<I", b, 4)[0]
    n = struct.unpack_from("<H", b, ifd)[0]
    size = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 10: 8}
    tags = {}
    for i in range(n):
        tag, typ, cnt, val = struct.unpack_from("<HHII", b, ifd + 2 + 12 * i)
        ln = size[typ] * cnt
        off = ifd + 2 + 12 * i + 8 if ln <= 4 else val
        tags[tag] = (typ, cnt, b[off:off + ln])
    w = struct.unpack("<I", tags[256][2])[0]
    h = struct.unpack("<I", tags[257][2])[0]
    offs = struct.unpack(f"<{tags[273][1]}I", tags[273][2])
    counts = struct.unpack(f"<{tags[279][1]}I", tags[279][2])
    data = b"".join(b[o:o + c] for o, c in zip(offs, counts))
    return tags, np.frombuffer(data, "<u2").reshape(h, w, 3)


def synth_scan(tmp_path, crop=True):
    rng = np.random.default_rng(7)
    px, lines, binning, x = 40, 60, 3, 25
    dark = 280 + rng.integers(0, 40, (px, 3))
    white = 3000 + rng.integers(0, 3000, (px, 3))
    cal = np.concatenate([np.broadcast_to(dark, (200, px, 3)),
                          np.broadcast_to(white, (50, px, 3))])
    t = rng.uniform(0.02, 0.98, (lines, px, 3))
    raw = (dark + t * (white - dark)).round().astype("<u2")
    scan = tmp_path / "neg_frame01.raw"
    raw.tofile(scan)
    cal.astype("<u2").tofile(str(scan) + ".cal")
    # Faul Pixs: green CCD pixel (x + 7) * bin + 144 -> output px 7
    faul = "Faul Pixs\n100\t0\n100\t0\n%d\t+999\n100\t0\n" % ((x + 7) * binning + 144)
    Path(str(scan) + ".faul").write_text(faul)
    meta = (f"preset=crop\nppi=2000\npx={px}\nx={x}\nlines={lines}\ny=0\n"
            f"bin={binning}\nkept={lines}\n")
    if crop:
        meta += "crop_x=3\ncrop_y=5\ncrop_w=30\ncrop_h=40\ntilt=0.000\n"
    Path(str(scan) + ".txt").write_text(meta)
    return scan, raw, cal, x, binning


@pytest.fixture(scope="module")
def ftdev():
    r = subprocess.run(["make", "-C", str(FTDEV)], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return FTDEV / "ftdev"


@needs_cc
@pytest.mark.parametrize("crop", [True, False])
def test_dng_matches_ftraw(ftdev, tmp_path, monkeypatch, crop):
    ftraw = load_ftraw(monkeypatch)
    scan, raw, cal, x, binning = synth_scan(tmp_path, crop)
    r = subprocess.run([str(ftdev), str(scan), "-f", "dng,png"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert (tmp_path / "neg_frame01.png").exists()

    t = ftraw.flatfield(raw, cal)
    fixed = ftraw.fix_faul(t, ftraw.faul_pixels(Path(str(scan) + ".faul")),
                           x, binning)
    assert fixed == [(1, 7)]
    t = ftraw.align_rgb(t, 12 // binning)
    if crop:
        t = t[5:45, 3:33]
    want = np.round(t.transpose(1, 0, 2) * 65535)

    tags, got = read_dng(tmp_path / "neg_frame01.dng")
    assert got.shape == want.shape
    assert np.abs(got.astype(np.int64) - want).max() <= 1
    assert struct.unpack("<H", tags[262][2])[0] == 34892     # LinearRaw
    assert tags[50706][2] == bytes([1, 4, 0, 0])
    num, den = struct.unpack("<II", tags[282][2])
    assert num / den == 2000


@needs_cc
def test_dng_opens_in_libraw(ftdev, tmp_path):
    rawpy = pytest.importorskip("rawpy")
    scan, *_ = synth_scan(tmp_path)
    subprocess.run([str(ftdev), str(scan)], check=True, capture_output=True)
    _, got = read_dng(tmp_path / "neg_frame01.dng")
    with rawpy.imread(str(tmp_path / "neg_frame01.dng")) as im:
        assert (im.sizes.raw_height, im.sizes.raw_width) == got.shape[:2]
        rgb = im.postprocess(gamma=(1, 1), no_auto_bright=True,
                             output_bps=16, use_camera_wb=True,
                             output_color=rawpy.ColorSpace.raw,
                             user_flip=0)
    assert rgb.shape == got.shape
    # LibRaw rescales to its own white; the image itself must survive
    corr = np.corrcoef(rgb[..., 1].ravel(), got[..., 1].ravel())[0, 1]
    assert corr > 0.99


@needs_cc
def test_defect_map_and_bad_format(ftdev, tmp_path):
    scan, *_ = synth_scan(tmp_path, crop=False)
    d = tmp_path / "defects.txt"
    d.write_text("# stuck\n%d\n" % ((25 + 12) * 3 + 144))   # output px 12
    r = subprocess.run([str(ftdev), str(scan), "-d", str(d)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    _, got = read_dng(tmp_path / "neg_frame01.dng")
    row = got[12].astype(np.int64)
    mid = (got[11].astype(np.int64) + got[13]) / 2
    assert np.abs(row - mid).max() <= 1

    r = subprocess.run([str(ftdev), str(scan), "-f", "fff"],
                       capture_output=True, text=True)
    assert r.returncode == 2 and "unknown format" in r.stderr
