# SPDX-License-Identifier: GPL-2.0-or-later
"""tools/cube2c.py, the emulsions' 3D-table compiler."""
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
LIBFT = ROOT / "libft"
CUBE2C = ROOT / "tools" / "cube2c.py"
needs_cc = pytest.mark.skipif(
    shutil.which("make") is None or shutil.which("cc") is None,
    reason="needs make and a C compiler")


def test_cube2c_header_in_sync(tmp_path):
    out = tmp_path / "luts.h"
    subprocess.run([sys.executable, str(CUBE2C), str(LIBFT / "emulsions"),
                    str(out)], check=True)
    assert out.read_bytes() == (LIBFT / "ft_emulsion_luts.h").read_bytes(), \
        "libft/ft_emulsion_luts.h is stale: run make luts in libft/"


@needs_cc
def test_cube2c_compiles_a_table(tmp_path):
    src = tmp_path / "cubes"
    src.mkdir()
    rows = ["%g %g %g" % (r * 0.5, g * 0.25, b) for b in (0, 1)
            for g in (0, 1) for r in (0, 1)]
    (src / "Test-Look.cube").write_text(
        'TITLE "test"\n# a comment\nLUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\n'
        "DOMAIN_MAX 1 1 1\n" + "\n".join(rows) + "\n")
    out = tmp_path / "ft_emulsion_luts.h"
    subprocess.run([sys.executable, str(CUBE2C), str(src), str(out)],
                   check=True)
    text = out.read_text()
    assert '/* Test-Look.cube: "test", 2^3 */' in text
    assert "static const ft_cube CUBE_test_look = { 2, " \
           "CUBE_test_look_RGB };" in text
    prog = tmp_path / "t.c"
    prog.write_text(
        '#include <stdio.h>\n#include "ft_emulsion_luts.h"\n'
        "int main(void) { printf(\"%d %g %g\\n\", CUBE_test_look.n, "
        "(double)CUBE_test_look.rgb[3], (double)CUBE_test_look.rgb[22]);"
        " return 0; }\n")
    exe = tmp_path / "t"
    r = subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Wpedantic",
                        "-Werror", "-I", str(tmp_path), "-I", str(LIBFT),
                        "-o", str(exe), str(prog)], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stderr
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    assert r.stdout == "2 0.5 0.25\n"   # node (1,0,0) red; (1,1,1) green


@pytest.mark.parametrize("body, msg", [
    ("LUT_3D_SIZE 2\n0 0 0\n", "x.cube:0: 1 rows, LUT_3D_SIZE 2 needs 8"),
    ("LUT_3D_SIZE 40\n", "x.cube:1: LUT_3D_SIZE 40 (2..33)"),
    ("LUT_1D_SIZE 2\n", "x.cube:1: LUT_1D_SIZE is not supported"),
    ("DOMAIN_MAX 2 2 2\n", "x.cube:1: DOMAIN_MAX must be 1 1 1"),
    ("LUT_3D_SIZE 2\n0 0 1.5\n", "x.cube:2: values are 0..1"),
    ("LUT_3D_SIZE 2\n0 0\n", "x.cube:2: not three numbers"),
    ("0 0 0\n", "x.cube:0: no LUT_3D_SIZE"),
])
def test_cube2c_refuses(tmp_path, body, msg):
    src = tmp_path / "cubes"
    src.mkdir()
    (src / "x.cube").write_text(body)
    r = subprocess.run([sys.executable, str(CUBE2C), str(src),
                        str(tmp_path / "o.h")], capture_output=True,
                       text=True)
    assert r.returncode != 0
    assert msg in r.stderr, r.stderr
