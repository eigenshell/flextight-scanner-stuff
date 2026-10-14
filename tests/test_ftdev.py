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
    size = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 10: 8, 12: 8}
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
    r = subprocess.run([str(ftdev), str(scan), "-f", "positive,dng",
                        "--film", "neg", "--base", "5,5,5"],
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
    # b&w, with a quoted points curve
    r1 = subprocess.run([str(ftdev), str(scan), "-f", "positive", "--film",
                         "bw", "--curve", "0,0 0.4,0.3 1,1", "--grade",
                         "2.25", "-o", str(tmp_path / "c")],
                        capture_output=True, text=True)
    assert r1.returncode == 0, r1.stderr
    r2 = subprocess.run([str(ftdev), str(scan), "-f", "positive", "-o",
                         str(tmp_path / "d"), *shlex.split(recipe_of(r1))],
                        capture_output=True, text=True)
    assert r2.returncode == 0, r2.stderr
    assert (tmp_path / "c_positive.png").read_bytes() == \
        (tmp_path / "d_positive.png").read_bytes()


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
    _, pa = read_dng(tmp_path / "pl_a.dng")
    _, pb = read_dng(tmp_path / "pl_b.dng")
    assert np.array_equal(pa, pb)


@needs_cc
def test_render_flags_ignored_without_positive(ftdev, tmp_path):
    scan = synth_negative(tmp_path)
    r = subprocess.run([str(ftdev), str(scan), "-f", "dng", "--grade", "2"],
                       capture_output=True, text=True)
    assert r.returncode == 0
    assert "render flags ignored without -f positive" in r.stderr


def share_window(tmp_path):
    """A synthetic share window scan (libft/test/window_dump.c): one full
    frame at holder 100..136 mm, scanned 1500 ppi bin 1 over 99..139 mm,
    the preview box 1 mm off; a bare film-base patch at 105..110 mm along,
    10..14 mm across."""
    lib = ROOT / "libft"
    r = subprocess.run(["make", "-C", str(lib), "test/window_dump"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    base = tmp_path / "job_win01"
    subprocess.run([str(lib / "test" / "window_dump"), str(base)], check=True)
    return Path(str(base) + ".raw")


def sidecar(path):
    return dict(l.split("=", 1) for l in path.read_text().splitlines())


@needs_cc
def test_share_cut_and_export(ftdev, tmp_path):
    cv2 = pytest.importorskip("cv2")
    win = share_window(tmp_path)
    r = subprocess.run([str(ftdev), "cut", str(win)], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stderr
    dng = tmp_path / "job_frame01.dng"        # "_win01" dropped from the base
    sc = sidecar(Path(str(dng) + ".txt"))
    bx, by, bw, bh = map(int, sc["box"].split())
    px_mm = 1500 / 25.4
    assert abs(bw - 36 * px_mm) < 0.2 * px_mm      # along: the full frame
    assert abs(bh - 24 * px_mm) < 0.4 * px_mm      # across: the image area
    assert abs(float(sc["shift_mm"]) + 1.0) < 0.1  # found where it crept
    assert sc["estimated"] == "0" and sc["film"] == "negative"
    assert abs(float(sc["export_scale"]) - 2000 / bw) < 1e-5
    assert len(sc["film_base"].split()) == 3
    _, pix = read_dng(dng)
    assert abs(bx - 0.5 * px_mm) < 2 and abs(by - 0.5 * px_mm) < 3   # bleed
    assert abs(pix.shape[1] - bw - px_mm) < 3 and abs(pix.shape[0] - bh - px_mm) < 5

    # export: 2000 px on the frame's long side, sRGB with its profile
    r = subprocess.run([str(ftdev), "export", str(dng)], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stderr
    assert "recipe: --film neg" in r.stdout
    jpg = tmp_path / "job_frame01.jpg"
    img = cv2.imread(str(jpg), cv2.IMREAD_COLOR)
    assert img.shape[1] == 2000
    assert img.shape[0] == round(bh * 2000 / bw)
    data = jpg.read_bytes()
    assert b"ICC_PROFILE\x00" in data and b"sRGB (flextight)" in data
    # the bare-base patch is neutral (and dark: base = the scene's black)
    s = 2000 / bw
    cx = round(((107.5 - 100) * px_mm) * s)
    cy = round(((12 - 4.1) * px_mm) * s)
    b, g, rr = img[cy - 3:cy + 4, cx - 3:cx + 4].reshape(-1, 3).mean(0)
    assert abs(rr - g) <= 3 and abs(g - b) <= 3
    assert g < img[:, :, 1].mean()

    # Display P3, another size, a crop (the same scale: fewer px)
    out = tmp_path / "p3.jpg"
    r = subprocess.run([str(ftdev), "export", str(dng), "--space", "p3",
                        "--long-edge", "1080", "-o", str(out)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert b"Display P3 (flextight)" in out.read_bytes()
    assert cv2.imread(str(out)).shape[1] == 1080
    out = tmp_path / "crop.jpg"
    crop = f"{bx},{by},{bw // 2},{bh // 2}"
    r = subprocess.run([str(ftdev), "export", str(dng), "--crop", crop,
                        "--gamut", "clip", "-o", str(out)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert cv2.imread(str(out)).shape[1] == round(bw // 2 * 2000 / bw)

    # raw + JPEG: the working raw and its sidecar next to the JPEG
    (tmp_path / "out").mkdir()
    out = tmp_path / "out" / "share.jpg"
    r = subprocess.run([str(ftdev), "export", str(dng), "--raw-too", "-o",
                        str(out)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert (tmp_path / "out" / "share.dng").read_bytes() == dng.read_bytes()
    assert sidecar(tmp_path / "out" / "share.dng.txt") == sc


@needs_cc
def test_share_export_refusals(ftdev, tmp_path):
    win = share_window(tmp_path)
    subprocess.run([str(ftdev), "cut", str(win)], check=True,
                   capture_output=True)
    dng = tmp_path / "job_frame01.dng"
    r = subprocess.run([str(ftdev), "export", str(dng), "--space", "rec2020"],
                       capture_output=True, text=True)
    assert r.returncode == 2 and "8-bit Rec.2020" in r.stderr
    r = subprocess.run([str(ftdev), "export", str(dng), "--crop",
                        "0,0,99999,10"], capture_output=True, text=True)
    assert r.returncode == 2 and "outside" in r.stderr
    # without its sidecar the film type must be given
    Path(str(dng) + ".txt").unlink()
    r = subprocess.run([str(ftdev), "export", str(dng)], capture_output=True,
                       text=True)
    assert r.returncode == 2 and "--film" in r.stderr
    r = subprocess.run([str(ftdev), "export", str(dng), "--film", "neg"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert "estimated from the frame" in r.stderr
    # an ordinary scan is not a window scan
    scan, *_ = synth_scan(tmp_path)
    r = subprocess.run([str(ftdev), "cut", str(scan)], capture_output=True,
                       text=True)
    assert r.returncode == 1 and "not a share window scan" in r.stderr


@needs_cc
def test_share_export_limits(ftdev, tmp_path):
    cv2 = pytest.importorskip("cv2")
    win = share_window(tmp_path)
    subprocess.run([str(ftdev), "cut", str(win)], check=True,
                   capture_output=True)
    dng = tmp_path / "job_frame01.dng"
    sc = sidecar(Path(str(dng) + ".txt"))
    # the film base is the one the window scan was written with
    assert sc["film_base"] == "0.2076 0.3979 0.6021"
    assert sidecar(Path(str(win) + ".txt"))["film_base"] == sc["film_base"]
    bw = int(sc["box"].split()[2])

    # never upscaled: a huge --long-edge gives the frame's own size
    out = tmp_path / "big.jpg"
    r = subprocess.run([str(ftdev), "export", str(dng), "--long-edge",
                        "65000", "-o", str(out)], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stderr
    assert cv2.imread(str(out)).shape[1] == bw

    # --quality: q1 is smaller than q100; out of range is refused
    sizes = []
    for q in ("1", "100"):
        out = tmp_path / ("q%s.jpg" % q)
        r = subprocess.run([str(ftdev), "export", str(dng), "--quality", q,
                            "--long-edge", "500", "-o", str(out)],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        sizes.append(out.stat().st_size)
    assert sizes[0] < sizes[1]
    r = subprocess.run([str(ftdev), "export", str(dng), "--quality",
                        "99999999999"], capture_output=True, text=True)
    assert r.returncode == 2 and "1..100" in r.stderr

    # a crop whose x + w overflows int is refused, not a crash
    r = subprocess.run([str(ftdev), "export", str(dng), "--crop",
                        "2147483000,0,1000,10"], capture_output=True,
                       text=True)
    assert r.returncode == 2 and "outside" in r.stderr
    # so is a sidecar box that does not fit this DNG
    bad = tmp_path / "bad.dng"
    bad.write_bytes(dng.read_bytes())
    txt = Path(str(dng) + ".txt").read_text().splitlines()
    txt = ["box=2147483000 29 1000 1413" if l.startswith("box=") else l
           for l in txt]
    Path(str(bad) + ".txt").write_text("\n".join(txt) + "\n")
    r = subprocess.run([str(ftdev), "export", str(bad)], capture_output=True,
                       text=True)
    assert r.returncode == 2 and "sidecar box" in r.stderr

    # a path that does not fit a buffer is refused, not overflowed
    long = str(tmp_path / ("a" * 4200)) + ".jpg"
    r = subprocess.run([str(ftdev), "export", str(dng), "--raw-too", "-o",
                        long], capture_output=True, text=True)
    assert r.returncode != 0 and r.returncode != -11

    # no sidecar, no --long-edge: full size, with a note
    Path(str(dng) + ".txt").unlink()
    r = subprocess.run([str(ftdev), "export", str(dng), "--film", "neg"],
                       capture_output=True, text=True)
    assert r.returncode == 0 and "exporting at full size" in r.stderr


@needs_cc
def test_export_nr_flags_refused(ftdev, tmp_path):
    win = share_window(tmp_path)
    subprocess.run([str(ftdev), "cut", str(win)], check=True,
                   capture_output=True)
    dng = tmp_path / "job_frame01.dng"
    for flag, val in [("--nr-sensor", "4.1"), ("--nr-chroma", "8.1"),
                      ("--nr-grain", "4.1"), ("--sharpen", "3.1"),
                      ("--nr-sensor", "-1"), ("--sharpen", "x")]:
        r = subprocess.run([str(ftdev), "export", str(dng), flag, val],
                           capture_output=True, text=True)
        assert r.returncode == 2 and flag in r.stderr, (flag, r.stderr)


def synth_noisy_scan(tmp_path, ndark=200):
    """A scan with real sensor noise: Poisson-like shot noise on the signal
    above the black level plus read noise, in the .cal lines and the
    image alike. Three flat bands across (T 0.5 | 0.2 | 0.08); the
    developed image has them as rows 0-31, 32-63, 64-95."""
    rng = np.random.default_rng(11)
    px, lines, binning, x, rd, g = 96, 160, 3, 25, 3.0, 1.5
    dark = 290 + rng.integers(0, 20, (px, 3))
    span = 5500 + rng.integers(0, 1000, (px, 3))

    def dn(level, sig):
        return np.clip(np.rint(level + rng.normal(0, 1, level.shape) * sig),
                       0, 65535).astype("<u2")
    cal = np.concatenate([
        dn(np.broadcast_to(dark, (ndark, px, 3)).astype(float), rd),
        dn(np.broadcast_to(dark + span, (50, px, 3)).astype(float),
           np.sqrt(g * span + rd * rd))])
    # three bands across: the base (thinnest), a mid tone, a dense one
    t = np.select([np.arange(px) < 32, np.arange(px) < 64], [0.5, 0.2], 0.08)
    t = np.broadcast_to(t[None, :, None], (lines, px, 3))
    raw = dn(dark + t * span, np.sqrt(g * t * span + rd * rd))
    scan = tmp_path / "noisy_frame01.raw"
    raw.tofile(scan)
    cal.tofile(str(scan) + ".cal")
    Path(str(scan) + ".txt").write_text(
        f"preset=crop\nppi=2000\npx={px}\nx={x}\nlines={lines}\ny=0\n"
        f"bin={binning}\nkept={lines}\n")
    return scan


def flat_std(ftdev, dng, out, *extra):
    cv2 = pytest.importorskip("cv2")
    r = subprocess.run([str(ftdev), "export", str(dng), "--film", "neg",
                        "-o", str(out), *extra], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stderr
    img = cv2.imread(str(out), cv2.IMREAD_COLOR).astype(float)
    mid = img[38:58, 8:-8, 1]                   # inside the mid-tone band
    assert 20 < mid.mean() < 235                # not clipped
    return mid.std(), r.stderr


@needs_cc
def test_export_noise_reduction_e2e(ftdev, tmp_path):
    pytest.importorskip("cv2")
    scan = synth_noisy_scan(tmp_path)
    r = subprocess.run([str(ftdev), str(scan)], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stderr
    dng = tmp_path / "noisy_frame01.dng"
    tags, _ = read_dng(dng)
    typ, cnt, raw = tags[51041]                 # NoiseProfile
    assert (typ, cnt) == (12, 6)
    s_r, o_r = struct.unpack("<6d", raw)[:2]
    # g / span and read^2 / span^2 (+1/12 DN^2 rounding), span ~6000
    assert 1.5 / 6500 < s_r < 1.5 / 5500
    assert 9.08 / 6500 ** 2 < o_r < 9.1 / 5500 ** 2
    on, err = flat_std(ftdev, dng, tmp_path / "on.jpg")
    assert "no noise profile" not in err
    off, _ = flat_std(ftdev, dng, tmp_path / "off.jpg", "--nr-sensor", "0")
    assert on < 0.5 * off, (on, off)    # measured 1.55 vs 6.48 code values
    grain, _ = flat_std(ftdev, dng, tmp_path / "gr.jpg", "--nr-sensor", "0",
                        "--nr-grain", "2")
    assert grain < off


@needs_cc
def test_export_no_profile_warns_and_estimates(ftdev, tmp_path):
    pytest.importorskip("cv2")
    scan = synth_noisy_scan(tmp_path, ndark=30)  # too few dark lines
    r = subprocess.run([str(ftdev), str(scan)], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stderr
    assert "noise profile: 30 dark" in r.stderr
    dng = tmp_path / "noisy_frame01.dng"
    tags, _ = read_dng(dng)
    assert 51041 not in tags
    _, err = flat_std(ftdev, dng, tmp_path / "e.jpg")
    assert "warning: no noise profile, estimated" in err
    _, err = flat_std(ftdev, dng, tmp_path / "z.jpg", "--nr-sensor", "0")
    assert "no noise profile" not in err


def cut_frame(ftdev, tmp_path):
    """The synthetic share window cut: frame 1's working raw + sidecar."""
    win = share_window(tmp_path)
    subprocess.run([str(ftdev), "cut", str(win)], check=True,
                   capture_output=True)
    return tmp_path / "job_frame01.dng"


def export(ftdev, dng, out, *args):
    r = subprocess.run([str(ftdev), "export", str(dng), "-o", str(out),
                        *args], capture_output=True, text=True)
    return r, (sidecar(Path(str(out) + ".txt")) if r.returncode == 0
               else None)


@needs_cc
def test_export_records_its_recipe(ftdev, tmp_path):
    dng = cut_frame(ftdev, tmp_path)
    out = tmp_path / "a.jpg"
    r, rec = export(ftdev, dng, out, "--grade", "2", "--curve", "soft",
                    "--sharpen", "0.8", "--space", "p3")
    assert r.returncode == 0, r.stderr
    assert rec["look"] == "none" and rec["frame"] == "1"
    assert rec["film"] == "neg" and rec["base"] == "0.2076 0.3979 0.6021"
    assert rec["grade"] == "2" and rec["curve"] == "soft"
    assert rec["tint"] == "0 0 0" and len(rec["range"].split()) == 3
    assert (rec["nr_sensor"], rec["nr_chroma"], rec["nr_grain"],
            rec["sharpen"]) == ("1", "2", "0", "0.8")
    assert rec["space"] == "p3" and rec["gamut"] == "soft"
    assert "curve_r" not in rec and "bw_weights" not in rec
    # the exposure recorded is the one the paste-back line prints
    assert "--exposure %s " % rec["exposure"] in r.stdout
    # the same text rides in the JPEG's comment (COM marker)
    text = Path(str(out) + ".txt").read_bytes()
    data = out.read_bytes()
    at = data.index(b"\xff\xfe")
    assert data[at + 4:at + 4 + len(text)] == text
    assert struct.unpack(">H", data[at + 2:at + 4])[0] == len(text) + 2


@needs_cc
def test_export_with_edit(ftdev, tmp_path):
    dng = cut_frame(ftdev, tmp_path)
    roll = tmp_path / "roll.edit"
    roll.write_text("from = portra400\n"
                    "[taste]\nexposure = 0.5\ngrade = 2\nsharpen = 0\n"
                    "nr_grain = 1\n"
                    "[frame 1]\ngrade = 1.2\ncurve = strong\n"
                    "curve_b = 0,0 0.5,0.45 1,1\n"
                    "[frame 2]\nexposure = -1\n")
    # the auto exposure at each grade, from plain exports
    _, g12 = export(ftdev, dng, tmp_path / "g12.jpg", "--grade", "1.2")
    _, g2 = export(ftdev, dng, tmp_path / "g2.jpg", "--grade", "2")

    # frame 1 (from the sidecar): the frame's grade and curves replace the
    # roll's; the roll's exposure is an offset from the auto value
    r, rec = export(ftdev, dng, tmp_path / "f1.jpg", "--edit", str(roll))
    assert r.returncode == 0, r.stderr
    assert "recipe: look portra400, frame 1; frame overrides: grade curve " \
           "curve_b" in r.stdout
    assert rec["look"] == "portra400" and rec["frame"] == "1"
    assert rec["grade"] == "1.2" and rec["curve"] == "strong"
    assert rec["curve_b"] == "0,0 0.5,0.45 1,1"
    assert abs(float(rec["exposure"]) - float(g12["exposure"]) - 0.5) < 1e-5
    assert rec["range"] == g12["range"] and rec["base"] == g12["base"]
    assert (rec["nr_grain"], rec["sharpen"], rec["nr_sensor"]) == \
           ("1", "0", "1")

    # --frame 2: the roll's grade, the frame's exposure offset replaces
    # the roll's (not added to it)
    r, rec = export(ftdev, dng, tmp_path / "f2.jpg", "--edit", str(roll),
                    "--frame", "2")
    assert r.returncode == 0, r.stderr
    assert "frame overrides: exposure" in r.stdout
    assert rec["frame"] == "2" and rec["grade"] == "2"
    assert rec["curve"] == "linear" and "curve_b" not in rec
    assert abs(float(rec["exposure"]) - float(g2["exposure"]) + 1) < 1e-5

    # a look file is a valid edit: copy one to start a roll
    look = tmp_path / "hp.look"
    look.write_text("[taste]\ngrade = 2\n")
    r, rec = export(ftdev, dng, tmp_path / "l.jpg", "--edit", str(look))
    assert r.returncode == 0, r.stderr
    assert "recipe: look (unnamed), frame 1\n" in r.stdout
    assert rec["look"] == "(unnamed)" and rec["exposure"] == g2["exposure"]


@needs_cc
def test_export_edit_refusals(ftdev, tmp_path):
    dng = cut_frame(ftdev, tmp_path)
    roll = tmp_path / "roll.edit"
    roll.write_text("[taste]\ngrade = 2\n")
    out = tmp_path / "x.jpg"
    for flags in (["--grade", "3"], ["--exposure", "1"], ["--nr-grain", "1"],
                  ["--sharpen", "0"]):
        r, _ = export(ftdev, dng, out, "--edit", str(roll), *flags)
        assert r.returncode == 2 and "with --edit the render and detail " \
               "settings come from the .edit" in r.stderr, flags
    r, _ = export(ftdev, dng, out, "--frame", "1")
    assert r.returncode == 2 and "--frame needs --edit" in r.stderr
    r, _ = export(ftdev, dng, out, "--edit", str(roll), "--frame", "x")
    assert r.returncode == 2 and "--frame is a frame number" in r.stderr
    bad = tmp_path / "bad.edit"
    bad.write_text("[taste]\ngrade = 9\n")
    r, _ = export(ftdev, dng, out, "--edit", str(bad))
    assert r.returncode == 2
    assert "bad.edit: line 2: grade: 9 is out of range (0.2..5)" in r.stderr
    assert not out.exists()
    r, _ = export(ftdev, dng, out, "--edit", str(tmp_path / "none.edit"))
    assert r.returncode == 1 and "cannot read" in r.stderr
    # no sidecar: the frame must be given
    Path(str(dng) + ".txt").unlink()
    r, _ = export(ftdev, dng, out, "--film", "neg", "--edit", str(roll))
    assert r.returncode == 2 and "--edit needs --frame N" in r.stderr
    r, rec = export(ftdev, dng, out, "--film", "neg", "--edit", str(roll),
                    "--frame", "4")
    assert r.returncode == 0, r.stderr
    assert rec["frame"] == "4" and "estimated from the frame" in r.stderr


def look_keys(path):
    return dict(l.split(" = ", 1) for l in path.read_text().splitlines()
                if " = " in l)


@needs_cc
def test_stock_learn(ftdev, tmp_path):
    dng = cut_frame(ftdev, tmp_path)
    _, plain = export(ftdev, dng, tmp_path / "p.jpg")
    rr, rg, rb = map(float, plain["range"].split())
    roll = tmp_path / "roll.edit"
    roll.write_text("from = x\n[taste]\ngrade = 1.8\n[frame 1]\n"
                    "exposure = 1\n")
    look = tmp_path / "portra400.look"
    learn = [str(ftdev), "stock", "learn", str(roll)]

    # one frame: the ratio of the stock-free estimate; the roll's taste
    # without the frame's overrides
    r = subprocess.run(learn + [str(dng), "-o", str(look)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert "frame 1: r/g %.3f, b/g %.3f" % (rr / rg, rb / rg) in r.stdout
    assert "note: 1 frames from this roll" in r.stderr
    k = look_keys(look)
    q = [float(v) for v in k["range_ratio"].split()]
    assert abs(q[0] - rr / rg) < 2e-6 and q[1] == 1
    assert abs(q[2] - rb / rg) < 2e-6
    assert k["n_frames"] == "1" and k["stock_version"] == "1"
    assert k["grade"] == "1.8" and "exposure" not in k
    assert "from" not in k and "[frame" not in look.read_text()

    # again with two frames: pooled with the earlier one, weighted
    r = subprocess.run(learn + [str(dng), str(dng), "-o", str(look)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert look_keys(look)["n_frames"] == "3"
    assert "from 3 frames (this roll: 2, spread 0.000)" in r.stdout
    # --replace starts over
    r = subprocess.run(learn + [str(dng), "-o", str(look), "--replace"],
                       capture_output=True, text=True)
    assert r.returncode == 0 and look_keys(look)["n_frames"] == "1"

    # the learned look steers the render: r and b follow green
    r, rec = export(ftdev, dng, tmp_path / "s.jpg", "--edit", str(look))
    assert r.returncode == 0, r.stderr
    sr, sg, sb = map(float, rec["range"].split())
    assert sg == rg
    assert abs(sr - sg * q[0]) < 1e-5 and abs(sb - sg * q[2]) < 1e-5


@needs_cc
def test_stock_learn_refusals(ftdev, tmp_path):
    dng = cut_frame(ftdev, tmp_path)
    roll = tmp_path / "roll.edit"
    roll.write_text("[taste]\n")
    look = tmp_path / "a.look"
    learn = [str(ftdev), "stock", "learn", str(roll)]
    r = subprocess.run([str(ftdev), "stock", "forget"], capture_output=True,
                       text=True)
    assert r.returncode == 2 and "usage: ftdev stock learn" in r.stderr
    r = subprocess.run(learn + ["-o", str(look)], capture_output=True,
                       text=True)
    assert r.returncode == 2 and "usage" in r.stderr
    # a broken look at -o is not overwritten
    look.write_text("[frame 1]\n")
    r = subprocess.run(learn + [str(dng), "-o", str(look)],
                       capture_output=True, text=True)
    assert r.returncode == 2 and "a.look: line 1: [frame N] belongs in a " \
           ".edit" in r.stderr
    assert look.read_text() == "[frame 1]\n"
    # slides have no stock ranges
    side = Path(str(dng) + ".txt")
    side.write_text(side.read_text().replace("film=negative", "film=slide"))
    r = subprocess.run(learn + [str(dng), "-o", str(tmp_path / "b.look")],
                       capture_output=True, text=True)
    assert r.returncode == 2 and "a slide" in r.stderr
    assert not (tmp_path / "b.look").exists()
    # learning needs the sidecar
    side.unlink()
    r = subprocess.run(learn + [str(dng), "-o", str(tmp_path / "b.look")],
                       capture_output=True, text=True)
    assert r.returncode == 2 and "no sidecar" in r.stderr
