#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Turn an ftpoc scan (<file>, <file>.cal, <file>.txt) into an image.

Each line the scanner sends is one CCD readout across the film: px pixels,
RGB interleaved, u16 LE, linear, 14-bit (dark ~300, open beam ~3400-6400
set by the lens/CCD positions, not by resolution or line rate). The
scanner does NOT correct the CCD; it hands the host two reference batches
before every scan and FlexColor (CCalibrateLineHandler) corrects on the PC:

  .cal batch 1 (200 lines)  dark reference  (lamp path blocked, ~300 counts)
  .cal batch 2 (50 lines)   white reference (open beam, per-pixel lamp/lens/PRNU)
  (the switch is timed in the device; lines are classified by level)

  T = (raw - dark) / (white - dark)      per CCD pixel and channel

Uncorrected, single CCD pixels with a different gain show as fine lines
along the scan direction.

Defective CCD pixels flicker line to line, so no gain fixes them. The
scanner lists them in its "Faul Pixs" table (<file>.faul): per channel,
"<ccd pixel>\t+999" = replace. Output pixel j sums CCD pixels
(x + j) * bin + 144 ... + bin - 1 (bin = descriptor byte 51); flagged
pixels are interpolated from their neighbours. Entries with a negative
value instead of +999 are not understood yet and are left alone.

The CCD is tri-linear: its R, G and B rows see a film point k lines
apart, k = 12 / bin (= descriptor byte 55; measured 1, 2, 2, 4, 6 lines at
525, 630, 1000, 2000, 3150 ppi). R leads G by k lines, B trails by k; the
host realigns them and drops k lines at each end.

usage: ftraw.py scan.raw [-o out.png] [--negative] [--raw]
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

DARK_LINES = 200
WHITE_LINES = 50
CCD_OFFSET = 144             # CCD pixel under output pixel -x (fit on 3 resolutions)
BIN_BY_PRESET = {"preview": 12, "1000": 6, "3f": 6, "2000": 3}


def load(path: Path):
    meta = {}
    for line in Path(str(path) + ".txt").read_text().splitlines():
        k, _, v = line.partition("=")
        meta[k.strip()] = v.strip()
    px = int(meta["px"])
    raw = np.fromfile(path, "<u2")
    raw = raw[: raw.size // (px * 3) * px * 3].reshape(-1, px, 3)
    cal = np.fromfile(str(path) + ".cal", "<u2").reshape(-1, px, 3)
    if cal.shape[0] != DARK_LINES + WHITE_LINES:
        sys.exit(f"{path}.cal has {cal.shape[0]} lines, expected "
                 f"{DARK_LINES + WHITE_LINES}")
    return raw, cal, meta


def faul_pixels(path: Path):
    """{channel: [ccd pixel, ...]} for the +999 entries of a Faul Pixs dump."""
    try:
        data = path.read_bytes()
    except FileNotFoundError:
        return {}
    lines = data.replace(b"\0", b"\n").decode("latin1").split("\n")[1:]
    out, ch = {}, -1
    for ln in lines:
        parts = ln.split("\t")
        if len(parts) != 2:
            continue
        pix, val = parts[0].strip(), parts[1].strip()
        if pix == "100" and val == "0":
            ch += 1
            continue
        if ch >= 0 and val == "+999" and pix.isdigit():
            out.setdefault(ch, []).append(int(pix))
    return out


def fix_faul(t, faul, x, binning):
    """Interpolate output pixels whose CCD span contains a flagged pixel."""
    px = t.shape[1]
    fixed = []
    for ch, pixels in faul.items():
        bad = sorted({(p - CCD_OFFSET) // binning - x for p in pixels})
        bad = [j for j in bad if 0 <= j < px]
        badset = set(bad)
        for j in bad:
            lo, hi = j - 1, j + 1
            while lo in badset:
                lo -= 1
            while hi in badset:
                hi += 1
            if lo < 0 and hi >= px:
                continue
            if lo < 0:
                t[:, j, ch] = t[:, hi, ch]
            elif hi >= px:
                t[:, j, ch] = t[:, lo, ch]
            else:
                w = (j - lo) / (hi - lo)
                t[:, j, ch] = (1 - w) * t[:, lo, ch] + w * t[:, hi, ch]
            fixed.append((ch, j))
    return fixed


def split_cal(cal):
    """Dark and white reference lines, classified by level: the switch is
    timed in the device, so at slow line rates (3150 ppi: 188 dark + 12
    white in the first batch) it does not fall on line 200. Lines within
    2 % of either extreme count (libft's FT_CAL_BAND); transition lines,
    which can be half lit, are dropped."""
    c = cal.astype(np.float64)
    lm = c.mean(axis=(1, 2))
    lo, hi = lm.min(), lm.max()
    band = 0.02 * (hi - lo)
    dark, white = c[lm < lo + band], c[lm > hi - band]
    if len(dark) < 10 or len(white) < 10:
        sys.exit(f"calibration: {len(dark)} dark / {len(white)} white lines")
    return dark.mean(axis=0), white.mean(axis=0)


def align_rgb(t, k):
    """Realign the tri-linear rows: R[l + k] and B[l - k] see G[l]."""
    if k <= 0 or t.shape[0] <= 2 * k:
        return t
    n = t.shape[0] - 2 * k
    out = np.empty((n,) + t.shape[1:], t.dtype)
    out[..., 0] = t[2 * k:, :, 0]
    out[..., 1] = t[k:k + n, :, 1]
    out[..., 2] = t[:n, :, 2]
    return out


def flatfield(raw, cal):
    """Transmittance 0..1 per pixel; per-column dark/white from .cal."""
    dark, white = split_cal(cal)
    span = np.maximum(white - dark, 1.0)
    return np.clip((raw.astype(np.float64) - dark) / span, 0.0, 1.0)


def to_positive(t):
    """Rough colour-negative inversion (not FlexColor's film profiles):
    density per channel, stretched between the film base and the densest
    highlights. Open-beam pixels (sprocket holes, gaps) are left out of the
    statistics, or they would set every channel's black point."""
    d = -np.log10(np.maximum(t, 1e-4))
    film = (t < 0.9).all(axis=-1)
    if film.sum() < 1000:
        film[:] = True
    out = np.empty_like(d)
    for c in range(3):
        lo, hi = np.percentile(d[..., c][film], (0.5, 99.8))
        out[..., c] = (d[..., c] - lo) / max(hi - lo, 1e-6)
    return np.clip(out, 0.0, 1.0) ** (1 / 1.8)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scan", type=Path)
    ap.add_argument("-o", "--out", type=Path)
    ap.add_argument("--negative", action="store_true",
                    help="colour negative: invert to a rough positive")
    ap.add_argument("--raw", action="store_true",
                    help="skip the dark/white correction (shows the lines)")
    a = ap.parse_args()

    raw, cal, meta = load(a.scan)
    if a.raw:
        t = raw.astype(np.float64) / raw.max()
    else:
        t = flatfield(raw, cal)
        binning = int(meta.get("bin") or BIN_BY_PRESET[meta["preset"]])
        fixed = fix_faul(t, faul_pixels(Path(str(a.scan) + ".faul")),
                         int(meta["x"]), binning)
        if fixed:
            print("Faul Pixs interpolated (channel, pixel):", fixed)
        t = align_rgb(t, 12 // binning)
    # scanner lines run along the strip: put them on the x axis, the way
    # FlexColor saves crops (2000 ppi crop: 2807 lines wide x 1818 px high)
    t = t.transpose(1, 0, 2)
    img = to_positive(t) if a.negative else t ** (1 / 2.2)
    out = a.out or a.scan.with_suffix(".png")
    Image.fromarray((img * 255 + 0.5).astype(np.uint8)).save(out)
    print(f"{out}: {img.shape[1]}x{img.shape[0]} ({meta.get('ppi')} ppi, "
          f"{'raw' if a.raw else 'dark/white corrected'})")


if __name__ == "__main__":
    main()
