# SPDX-License-Identifier: GPL-2.0-or-later
"""Run libft's C unit tests (libft/test) from pytest."""
import shutil
import subprocess
from pathlib import Path

import pytest

LIBFT = Path(__file__).resolve().parent.parent / "libft"
needs_cc = pytest.mark.skipif(
    shutil.which("make") is None or shutil.which("cc") is None,
    reason="needs make and a C compiler")


@needs_cc
def test_libft_unit_tests():
    r = subprocess.run(["make", "-C", str(LIBFT), "test"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


import zlib


@needs_cc
def test_png_writer_decodes_with_zlib(tmp_path):
    r = subprocess.run(["make", "-C", str(LIBFT), "libft.a"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    exe = LIBFT / "test" / "png_dump"
    r = subprocess.run(["cc", "-std=c99", "-I", str(LIBFT), "-o", str(exe),
                        str(LIBFT / "test" / "png_dump.c"),
                        str(LIBFT / "libft.a"), "-lm"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    out = tmp_path / "t.png"
    subprocess.run([str(exe), str(out)], check=True)
    data = out.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, chunks = 8, []
    while pos < len(data):
        n = int.from_bytes(data[pos:pos + 4], "big")
        typ, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        crc = int.from_bytes(data[pos + 8 + n:pos + 12 + n], "big")
        assert zlib.crc32(typ + body) == crc, typ
        chunks.append((typ, body))
        pos += 12 + n
    assert [c[0] for c in chunks] == [b"IHDR", b"IDAT", b"IEND"]
    ihdr = chunks[0][1]
    assert int.from_bytes(ihdr[:4], "big") == 3
    assert int.from_bytes(ihdr[4:8], "big") == 2
    assert ihdr[8:] == bytes([8, 2, 0, 0, 0])
    assert zlib.decompress(chunks[1][1]) == bytes(
        [0, 255, 0, 0, 0, 255, 0, 0, 0, 255,
         0, 10, 20, 30, 40, 50, 60, 70, 80, 90])
