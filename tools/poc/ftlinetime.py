#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Line time per ftpoc run, and a fit t = a + b*px + c*px*bin.

Usage: ftlinetime.py run1.log [run2.log ...]
Each run.log (ftpoc stdout) needs its scan metadata next to it as
<log stem>.raw.txt (ftpoc writes <-o file>.txt; use -o <stem>.raw).
a = fixed per-line cost (exposure/schedule), b = per output pixel
(digitise or transfer), c = per CCD pixel spanned (CCD clocked over the
whole span). short% = share of reads that found no line ready: high means
the host waited on the scanner.
"""
import re
import sys
from pathlib import Path

import numpy as np

FLOOD = re.compile(r"\[flood: (\d+) reads in (\d+) ms; (\d+) full, (\d+) short")


def parse_flood(text):
    m = FLOOD.search(text)
    if not m:
        raise ValueError("no [flood: ...] line")
    reads, ms, full, short = map(int, m.groups())
    return {"reads": reads, "ms": ms, "full": full, "short": short}


def parse_meta(text):
    kv = dict(line.split("=", 1) for line in text.splitlines() if "=" in line)
    return {"ppi": float(kv["ppi"]), "px": int(kv["px"]), "bin": int(kv["bin"])}


def fit(rows):
    """rows: (px, bin, ms_per_line). Returns (a, b, c) least squares."""
    px = np.array([r[0] for r in rows], float)
    bn = np.array([r[1] for r in rows], float)
    t = np.array([r[2] for r in rows], float)
    m = np.column_stack([np.ones_like(px), px, px * bn])
    (a, b, c), *_ = np.linalg.lstsq(m, t, rcond=None)
    return float(a), float(b), float(c)


def main(argv):
    rows = []
    print(f"{'run':24} {'ppi':>6} {'bin':>3} {'px':>5} {'ms/line':>8} {'kpx/s':>7} {'short%':>6}")
    for log in map(Path, argv):
        f = parse_flood(log.read_text(errors="replace"))
        meta = parse_meta(log.with_suffix(".raw.txt").read_text())
        ms = f["ms"] / f["full"]
        rows.append((meta["px"], meta["bin"], ms))
        print(f"{log.stem:24} {meta['ppi']:6.0f} {meta['bin']:3d} {meta['px']:5d} "
              f"{ms:8.2f} {meta['px'] / ms:7.1f} {100 * f['short'] / f['reads']:6.1f}")
    if len(rows) >= 3:
        a, b, c = fit(rows)
        print(f"\nfit: t = {a:.3f} ms + {b * 1000:.3f} us/px + {c * 1000:.3f} us/(px*bin)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
