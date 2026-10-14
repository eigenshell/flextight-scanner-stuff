# SPDX-License-Identifier: GPL-2.0-or-later
"""Detailed-scan passes (sub-project 2b) end to end through ftdev: a kept
raw pass re-developed through the ring and worker thread ftpoc runs during
a scan, against the classic whole-image develop of the same lines."""
import math
import shutil
import struct
import subprocess
from pathlib import Path

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parent.parent
FTDEV = ROOT / "tools" / "ftdev"
needs_cc = pytest.mark.skipif(
    shutil.which("make") is None or shutil.which("cc") is None,
    reason="needs make and a C compiler")

BASE = (0.2, 0.4, 0.6)          # the job's film_base, D per channel


@pytest.fixture(scope="module")
def ftdev():
    r = subprocess.run(["make", "-C", str(FTDEV)], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return FTDEV / "ftdev"


def read_tiff(path):
    """(tags {tag: (type, count, bytes)}, pixels h x w x spp u16) of a
    little-endian TIFF/DNG, tiled (ours) or in strips."""
    b = path.read_bytes()
    assert b[:4] == b"II*\x00"
    ifd = struct.unpack_from("<I", b, 4)[0]
    n = struct.unpack_from("<H", b, ifd)[0]
    size = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 7: 1, 10: 8, 12: 8}
    tags = {}
    for i in range(n):
        tag, typ, cnt, val = struct.unpack_from("<HHII", b, ifd + 2 + 12 * i)
        ln = size[typ] * cnt
        off = ifd + 2 + 12 * i + 8 if ln <= 4 else val
        tags[tag] = (typ, cnt, b[off:off + ln])

    def num(tag):
        typ, cnt, raw = tags[tag]
        return struct.unpack("<%d%s" % (cnt, "H" if typ == 3 else "I"), raw)

    w, h, spp = num(256)[0], num(257)[0], num(277)[0]
    if 322 in tags:
        tw, th = num(322)[0], num(323)[0]
        offs, counts = num(324), num(325)
        ntx = (w + tw - 1) // tw
        img = np.zeros(((h + th - 1) // th * th, ntx * tw, spp), "<u2")
        for i, (o, c) in enumerate(zip(offs, counts)):
            ty, tx = divmod(i, ntx)
            tile = np.frombuffer(b[o:o + c], "<u2").reshape(th, tw, spp)
            img[ty * th:(ty + 1) * th, tx * tw:(tx + 1) * tw] = tile
        return tags, img[:h, :w]
    data = b"".join(b[o:o + c] for o, c in zip(num(273), num(279)))
    return tags, np.frombuffer(data, "<u2").reshape(h, w, spp)


def make_pass(tmp_path, px=200, lines=400):
    """a kept pass (s_pass01.raw + .cal .faul .txt) and its job; the frame
    box plus margin covers the whole scan, so the frame file is the whole
    developed pass"""
    rng = np.random.default_rng(11)
    dark = 280 + rng.integers(0, 40, (px, 3))
    white = 3000 + rng.integers(0, 3000, (px, 3))
    noise = rng.integers(-3, 4, (50, px, 3))
    cal = np.concatenate([np.broadcast_to(dark, (200, px, 3)),
                          white + noise]).astype("<u2")
    t = rng.uniform(0.003, 0.98, (lines, px, 3))
    raw = (dark + t * (white - dark)).round().astype("<u2")
    scan = tmp_path / "s_pass01.raw"
    raw.tofile(scan)
    cal.tofile(str(scan) + ".cal")
    x = 2000
    Path(str(scan) + ".faul").write_text(
        "Faul Pixs\n100\t0\n%d\t+999\n100\t0\n100\t0\n" % (x + 37 + 144))
    Path(str(scan) + ".txt").write_text(
        f"preset=detail\nppi=3150\npx={px}\nx={x}\nlines={lines}\n"
        f"y=14000\nbin=1\nkept={lines}\ncal_lines=250\npass=1\n"
        "focus_offset=-120\n")
    job = tmp_path / "s.job"
    job.write_text(
        "[strip]\nholder = 24x217\nfilm = negative\nfocus = shifted\n"
        "focus_offset = 0\nfilm_base = %.4f %.4f %.4f\ndetail = neg\n"
        "detail_fmt = dng\ndetail_bw = off\nkeep_raw = yes\nring_mb = 8\n"
        "\n[frame 1]\ntype = full\ninclude = yes\nppi = 3150\nbin = 1\n"
        "window = 113.40 1.31 2.0 0.8\npass = 1\n" % BASE)
    return scan, job


def run(ftdev, *args):
    return subprocess.run([str(ftdev), *map(str, args)], capture_output=True,
                          text=True)


def classic(ftdev, scan, tmp_path):
    """the whole-image develop: (h, w, 3) u16"""
    r = run(ftdev, scan, "--no-crop", "-f", "dng", "-o", tmp_path / "classic")
    assert r.returncode == 0, r.stderr
    return read_tiff(tmp_path / "classic.dng")[1]


def sidecar(path):
    txt = Path(str(path) + ".txt").read_text()
    return dict(line.split("=", 1) for line in txt.splitlines())


def density_codes(u, base):
    """ft_density.h by hand: D above base, Cineon-style, per channel"""
    with np.errstate(divide="ignore"):
        d = -np.log10(u / 65535.0)
    code = np.floor((d - base + 0.1) / 4.0 * 65535 + 0.5)
    code[u == 0] = 65535
    return np.clip(code, 0, 65535)


@needs_cc
def test_negative_matches_whole_image_develop(ftdev, tmp_path):
    scan, job = make_pass(tmp_path)
    r = run(ftdev, "detail", scan, "--job", job)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "ring peak" in r.stdout and "s_frame01.dng" in r.stdout
    out = tmp_path / "s_frame01.dng"
    assert out.exists() and not Path(str(out) + ".partial").exists()
    tags, px = read_tiff(out)
    assert struct.unpack("<H", tags[262][2])[0] == 34892     # LinearRaw
    assert 322 in tags and 273 not in tags                  # tiled
    want = classic(ftdev, scan, tmp_path)
    assert px.shape == want.shape
    assert np.array_equal(px, want)
    meta = sidecar(out)
    assert meta["pass"] == "1" and meta["focus_offset"] == "-120"
    assert meta["out"] == "neg" and meta["fmt"] == "dng"
    assert meta["scan_range"] == "0 %d 0 %d" % (want.shape[1], want.shape[0])


@needs_cc
def test_stalls_and_a_small_ring_change_nothing(ftdev, tmp_path):
    scan, job = make_pass(tmp_path)
    r = run(ftdev, "detail", scan, "--job", job, "-o", tmp_path / "a")
    assert r.returncode == 0, r.stderr
    r = run(ftdev, "detail", scan, "--job", job, "-o", tmp_path / "b",
            "--ring-mb", "8", "--stall", "30,40")
    assert r.returncode == 0, r.stderr
    assert "longest line" in r.stdout
    a = read_tiff(tmp_path / "a_frame01.dng")[1]
    b = read_tiff(tmp_path / "b_frame01.dng")[1]
    assert np.array_equal(a, b)


@needs_cc
def test_archive_positive_codes(ftdev, tmp_path):
    scan, job = make_pass(tmp_path)
    r = run(ftdev, "detail", scan, "--job", job, "--out", "pos")
    assert r.returncode == 0, r.stderr
    out = tmp_path / "s_frame01.tif"
    tags, px = read_tiff(out)
    assert struct.unpack("<H", tags[262][2])[0] == 2         # RGB
    desc = tags[270][2].decode()
    assert desc.startswith("ft-density v1 base=0.2000 0.4000 0.6000 "
                           "offset=0.1 range=4.0 bw=off")
    t16 = classic(ftdev, scan, tmp_path).astype(np.float64)
    want = np.stack([density_codes(t16[..., c], BASE[c]) for c in range(3)],
                    axis=-1)
    assert np.array_equal(px, want.astype("<u2"))
    # the film base itself lands on code 1638 (Cineon put it at 95/1023)
    assert density_codes(np.array([round(10 ** -0.2 * 65535)]), 0.2)[0] \
        == 1638


@needs_cc
def test_archive_positive_mono(ftdev, tmp_path):
    scan, job = make_pass(tmp_path)
    r = run(ftdev, "detail", scan, "--job", job, "--out", "pos",
            "--bw", "mono")
    assert r.returncode == 0, r.stderr
    out = tmp_path / "s_frame01.tif"
    tags, px = read_tiff(out)
    assert struct.unpack("<H", tags[262][2])[0] == 1         # BlackIsZero
    assert px.shape[2] == 1
    meta = sidecar(out)
    assert meta["bw"] == "mono"
    w = np.array([float(v) for v in meta["weights"].split()])
    assert abs(w.sum() - 1) < 1e-3
    t16 = classic(ftdev, scan, tmp_path).astype(np.float64)
    codes = np.stack([density_codes(t16[..., c], BASE[c]) for c in range(3)],
                     axis=-1)
    want = np.floor((codes * w).sum(axis=-1) + 0.5)
    # the sidecar prints the weights to 4 decimals: within 2 codes
    assert np.abs(px[..., 0].astype(int) - want).max() <= 2


@needs_cc
def test_refusals(ftdev, tmp_path):
    scan, job = make_pass(tmp_path)
    r = run(ftdev, "detail", scan)                          # no job
    assert r.returncode == 2
    txt = Path(str(scan) + ".txt")
    txt.write_text(txt.read_text().replace("pass=1\n", ""))
    r = run(ftdev, "detail", scan, "--job", job)
    assert r.returncode == 1 and "pass geometry" in r.stderr
    scan, job = make_pass(tmp_path)
    job.write_text(job.read_text().replace("pass = 1", "pass = 2"))
    r = run(ftdev, "detail", scan, "--job", job)
    assert r.returncode == 1 and "no frame in pass 1" in r.stderr
    scan, job = make_pass(tmp_path)
    r = run(ftdev, "detail", scan, "--job", job, "--bw", "grey")
    assert r.returncode == 2


@needs_cc
def test_bench(ftdev, tmp_path):
    r = subprocess.run([str(ftdev), "bench", "--mm", "1", "--ppi", "3150",
                        "-o", str(tmp_path / "bench")],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert "headroom" in r.stdout
    assert not list(tmp_path.glob("bench_frame*"))           # cleaned up
