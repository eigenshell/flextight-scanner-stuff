# SPDX-License-Identifier: GPL-2.0-or-later
"""Hardware acceptance: enumerate a real attached scanner.

Skipped from default runs (pytestmark hardware); run explicitly with:
    .venv/bin/python -m pytest tests/test_hardware_enum.py -m hardware
or override addopts with -o addopts=.
"""

import glob
import os

import pytest

from flextight.scanner import Scanner
from flextight.transport import SGIOTransport

pytestmark = pytest.mark.hardware


def _device_path():
    p = os.environ.get("FLEXTIGHT_SG")
    if p:
        return p
    cands = sorted(glob.glob("/dev/sg*"))
    if not cands:
        pytest.skip("no /dev/sg* present")
    return cands[0]


def test_hardware_enumerate():
    t = SGIOTransport.open(_device_path())
    try:
        info = Scanner(t).enumerate()
    finally:
        t.close()
    assert info["vendor"].strip() in ("IMACON", "HASSELBLAD"), info
    assert "Precision" in info["model"] or "FlexTight" in info["model"], info
    assert info["pdt"] == 0x06, info  # 0x06 = scanner peripheral device type
