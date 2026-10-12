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
