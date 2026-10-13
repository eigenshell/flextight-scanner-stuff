# SPDX-License-Identifier: GPL-2.0-or-later
"""ftdev (libft's develop pipeline + writers) against ftraw.py, the Python
reference it was ported from, on a synthetic scan."""
import importlib.util
import shutil
import struct
import shlex
import subprocess
import sys
import types
import zlib
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


def read_png16(path):
    """h x w x 3 uint16 from ft_png's 16-bit RGB PNG (filter 0 rows)."""
    b = path.read_bytes()
    assert b[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat = 8, b""
    while pos < len(b):
        n = struct.unpack(">I", b[pos:pos + 4])[0]
        kind, data = b[pos + 4:pos + 8], b[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", data[:10])
            assert (depth, ctype) == (16, 2)
        elif kind == b"IDAT":
            idat += data
        pos += 12 + n
    rows = np.frombuffer(zlib.decompress(idat), np.uint8).reshape(h, 1 + 6 * w)
    assert (rows[:, 0] == 0).all()
    return rows[:, 1:].copy().view(">u2").reshape(h, w, 3)


BASE = (0.2, 0.45, 0.7)                      # orange mask, D
GAMMA = (0.45, 0.50, 0.55)                   # per dye layer


def synth_negative(tmp_path, film_base=True):
    """A colour negative of 12 grey stripes (exposure 0.01..1) across the
    CCD line, through per-channel characteristic curves and the orange
    mask; .txt with the preview's film_base unless film_base=False."""
    px, lines, binning, x = 120, 80, 3, 25
    dark = np.full((px, 3), 300)
    white = np.full((px, 3), 6300)
    s = np.arange(px) // 10
    expo = 10.0 ** (-2 + 2 * s / 11)
    d = np.array(BASE) + np.array(GAMMA) * np.log10(expo[:, None] / 0.002)
    t = np.broadcast_to(10.0 ** -d, (lines, px, 3))
    raw = (dark + t * (white - dark)).round().astype("<u2")
    cal = np.concatenate([np.broadcast_to(dark, (200, px, 3)),
                          np.broadcast_to(white, (50, px, 3))])
    scan = tmp_path / "neg_frame02.raw"
    raw.tofile(scan)
    cal.astype("<u2").tofile(str(scan) + ".cal")
    meta = (f"preset=crop\nppi=2000\npx={px}\nx={x}\nlines={lines}\ny=0\n"
            f"bin={binning}\nkept={lines}\n")
    if film_base:
        meta += "film_base=%.4f %.4f %.4f\n" % BASE
    Path(str(scan) + ".txt").write_text(meta)
    return scan


@needs_cc
def test_positive_greys_neutral(ftdev, tmp_path):
    scan = synth_negative(tmp_path)
    r = subprocess.run([str(ftdev), str(scan), "-f", "positive",
                        "--film", "neg"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert "--base 0.2,0.45,0.7" in r.stdout      # from the .txt
    img = read_png16(tmp_path / "neg_frame02_positive.png").astype(float)
    # image rows are CCD pixels: stripe s is rows 10 s .. 10 s + 9
    means = np.array([img[10 * s + 2:10 * s + 8].mean(axis=(0, 1))
                      for s in range(12)]) / 65535
    assert np.abs(means[:, 0] - means[:, 1]).max() < 0.01
    assert np.abs(means[:, 2] - means[:, 1]).max() < 0.01
    assert (np.diff(means[:, 1]) > 0).all()        # more exposure, brighter
    assert means[-1, 1] > 0.9 and means[0, 1] < 0.35


@needs_cc
def test_positive_flags(ftdev, tmp_path):
    scan = synth_negative(tmp_path, film_base=False)
    r = subprocess.run([str(ftdev), str(scan), "-f", "positive"],
                       capture_output=True, text=True)
    assert r.returncode == 2 and "--film" in r.stderr
    r = subprocess.run([str(ftdev), str(scan), "-f", "positive", "--film",
                        "neg", "--curve", "0,0 0.5"], capture_output=True,
                       text=True)
    assert r.returncode == 2 and "curve" in r.stderr
    r = subprocess.run([str(ftdev), str(scan), "-f", "positive", "--film",
                        "neg", "--tint", "60,0,0"], capture_output=True,
                       text=True)
    assert r.returncode == 2 and "tint r" in r.stderr
    # no film_base in the .txt: estimated from the frame, with a note
    r = subprocess.run([str(ftdev), str(scan), "-f", "positive,dng",
                        "--film", "bw", "--curve", "strong"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert "estimated from the frame" in r.stderr
    assert "--film bw" in r.stdout and '--curve "strong"' in r.stdout
    assert (tmp_path / "neg_frame02.dng").exists()
    img = read_png16(tmp_path / "neg_frame02_positive.png")
    assert (img[..., 0] == img[..., 1]).all() and (img[..., 1] == img[..., 2]).all()


def run_positive(ftdev, scan, *args):
    return subprocess.run([str(ftdev), str(scan), "-f", "positive", "--film",
                           "neg", *args], capture_output=True, text=True)


def recipe_of(r):
    line = [l for l in r.stdout.splitlines() if l.startswith("recipe: ")]
    assert len(line) == 1, r.stdout
    return line[0][len("recipe: "):]


def recipe_exposure(r):
    t = shlex.split(recipe_of(r))
    return float(t[t.index("--exposure") + 1])


@needs_cc
def test_positive_strict_flags(ftdev, tmp_path):
    scan = synth_negative(tmp_path)
    for args, msg in ((("--exposure", "abc"), "--exposure is stops"),
                      (("--exposure", "1x"), "--exposure is stops"),
                      (("--exposure", "nan"), "--exposure is stops"),
                      (("--grade", "x"), "--grade is a number"),
                      (("--grade", "inf"), "--grade is a number"),
                      (("--base", "nan,0,0"), "--base is"),
                      (("--tint", "inf,0,0"), "--tint is"),
                      (("--tint", "1,2"), "--tint is")):
        r = run_positive(ftdev, scan, *args)
        assert r.returncode == 2 and msg in r.stderr, (args, r.stderr)
    r = run_positive(ftdev, scan, "--curve", "0,0 0.5")
    assert r.returncode == 2 and "curve: curve" not in r.stderr


@needs_cc
def test_positive_validates_before_writing(ftdev, tmp_path):
    scan = synth_negative(tmp_path)
    r = subprocess.run([str(ftdev), str(scan), "-f", "positive,dng",
                        "--film", "neg", "--grade", "0"],
                       capture_output=True, text=True)
    assert r.returncode == 2 and "ftdev:" in r.stderr
    assert not (tmp_path / "neg_frame02.dng").exists()
    assert not (tmp_path / "neg_frame02_positive.png").exists()


@needs_cc
def test_positive_overrides(ftdev, tmp_path):
    scan = synth_negative(tmp_path)
    r = run_positive(ftdev, scan, "--base", "0.3,0.5,0.8")
    assert r.returncode == 0, r.stderr
    assert "--base 0.3,0.5,0.8" in r.stdout
    e0 = recipe_exposure(run_positive(ftdev, scan))
    r = run_positive(ftdev, scan, "--grade", "3")
    assert r.returncode == 0, r.stderr
    assert recipe_exposure(r) != e0                  # grade before auto
    r = run_positive(ftdev, scan, "--tint", "10,0,0", "--exposure", "1.5")
    assert r.returncode == 0, r.stderr
    assert "--exposure 1.5 " in recipe_of(r) + " "
    assert "--tint 10,0,0 " in recipe_of(r) + " "


@needs_cc
def test_positive_paste_back(ftdev, tmp_path):
    scan = synth_negative(tmp_path)
    r1 = run_positive(ftdev, scan, "--tint", "3,0,-2", "--grade", "2",
                      "--curve", "strong", "-o", str(tmp_path / "a"))
    assert r1.returncode == 0, r1.stderr
    flags = shlex.split(recipe_of(r1))
    r2 = subprocess.run([str(ftdev), str(scan), "-f", "positive", "-o",
                         str(tmp_path / "b"), *flags],
                        capture_output=True, text=True)
    assert r2.returncode == 0, r2.stderr
    assert (tmp_path / "a_positive.png").read_bytes() == \
        (tmp_path / "b_positive.png").read_bytes()


@needs_cc
def test_positive_dng_unrendered(ftdev, tmp_path):
    scan = synth_negative(tmp_path)
    r = subprocess.run([str(ftdev), str(scan), "-f", "dng", "--film", "neg", "-o",
                        str(tmp_path / "pl_a")], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    r = subprocess.run([str(ftdev), str(scan), "-f", "positive,dng", "--film",
                        "neg", "-o", str(tmp_path / "pl_b")],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    a = (tmp_path / "pl_a.dng").read_bytes()
    b = (tmp_path / "pl_b.dng").read_bytes()
    # the header carries a timestamp: compare size and the 120x80x3 pixels
    assert len(a) == len(b) and a[-120 * 80 * 3 * 2:] == b[-120 * 80 * 3 * 2:]


@needs_cc
def test_render_flags_ignored_without_positive(ftdev, tmp_path):
    scan = synth_negative(tmp_path)
    r = subprocess.run([str(ftdev), str(scan), "-f", "dng", "--grade", "2"],
                       capture_output=True, text=True)
    assert r.returncode == 0
    assert "render flags ignored without -f positive" in r.stderr
