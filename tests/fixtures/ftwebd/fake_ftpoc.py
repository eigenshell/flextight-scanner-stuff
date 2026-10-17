#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A stand-in for ftpoc on Linux, for ftwebd's tests and a local demo of
the page: `frames ... --progress -o BASE` and `scan --job JOB --progress`
print ftpoc's progress lines (libft/ft_progress.h) and write synthetic
files; SIGINT (ftwebd's stop) ends the run as `progress end stopped`.
Every call appends its argv (JSON) to fake_ftpoc.log in the cwd.

FAKE_FTPOC_DELAY: seconds per progress line (default 0.01)."""
import json
import os
import re
import signal
import struct
import sys
import time
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
DELAY = float(os.environ.get("FAKE_FTPOC_DELAY", "0.01"))
stopped = False


def on_int(signum, frame):
    global stopped
    stopped = True


def say(line):
    print(line, flush=True)


def png(path, w, h, boxes):
    """grey strip, darker film frames (boxes in px: x, y, w, h)"""
    row = bytearray(b"\x80" * (w * 3))
    rows = []
    for y in range(h):
        r = bytearray(row)
        for bx, by, bw, bh in boxes:
            if by <= y < by + bh:
                r[bx * 3:(bx + bw) * 3] = b"\x30\x28\x20" * bw
        rows.append(b"\x00" + bytes(r))
    raw = zlib.compress(b"".join(rows), 6)

    def chunk(t, d):
        return (struct.pack(">I", len(d)) + t + d +
                struct.pack(">I", zlib.crc32(t + d) & 0xffffffff))
    Path(path).write_bytes(
        b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
        chunk(b"IDAT", raw) + chunk(b"IEND", b""))


def lines(total):
    for k in range(50, total + 1, 50):
        if stopped:
            return False
        say("progress lines %d/%d" % (k, total))
        time.sleep(DELAY)
    say("progress lines %d/%d" % (total, total))
    return True


def frames(args):
    base = args[args.index("-o") + 1]
    say("progress begin preview 1")
    say("progress unit preview 1 1")
    if not lines(4795):
        say("progress end stopped")
        return 1
    Path(base + ".raw").write_bytes(b"\0" * 64)
    Path(base + ".raw.txt").write_text("px=666\nx=2\nbin=12\n")
    say("progress file " + base + ".raw")
    job = (HERE / "preview.job").read_text()
    Path(base + ".job").write_text(job)
    say("progress file " + base + ".job")
    # ~525 ppi, the frames of preview.job (y0 along, x0 across)
    s = 525 / 25.4
    boxes = [(int(float(m[0]) * s), int(float(m[1]) * s),
              int(float(m[2]) * s), int(float(m[3]) * s))
             for m in re.findall(r"window = (\S+) (\S+) (\S+) (\S+)", job)]
    png(base + ".png", 4795, 666, boxes)
    say("progress file " + base + ".png")
    print("wrote %s.job and %s.png" % (base, base))
    say("progress end ok")
    return 0


def scan(args):
    job = args[args.index("--job") + 1]
    text = Path(job).read_text()
    groups = sorted({int(g) for inc, g in re.findall(
        r"include = (\w+).*?group = (\d+)", text, re.S)
        if inc == "yes" and int(g) > 0})
    base = job[:-4] if job.endswith(".job") else job
    say("progress begin window %d" % len(groups))
    for i, g in enumerate(groups, 1):
        say("progress unit window %d %d" % (i, g))
        if not lines(1890):
            say("progress end stopped")
            return 1
        raw = "%s_win%02d.raw" % (base, g)
        Path(raw).write_bytes(b"\0" * 64)
        Path(raw + ".txt").write_text("share=2450\n")
        say("progress file " + raw)
    say("progress end ok")
    return 0


def main():
    signal.signal(signal.SIGINT, on_int)
    with open("fake_ftpoc.log", "a") as f:
        f.write(json.dumps(sys.argv[1:]) + "\n")
    args = sys.argv[1:]
    if args and args[0] == "frames":
        return frames(args)
    if args and args[0] == "scan":
        return scan(args)
    print("fake ftpoc: unknown command", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
