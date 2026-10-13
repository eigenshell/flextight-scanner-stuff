# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
from pathlib import Path

import pytest

spec = importlib.util.spec_from_file_location(
    "ftlinetime", Path(__file__).parent.parent / "tools/poc/ftlinetime.py")
ftlinetime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ftlinetime)


def test_parse_flood_line():
    log = "calibration ok\n  [flood: 412 reads in 4120 ms; 300 full, 112 short (0 bytes)]\nexit 0\n"
    f = ftlinetime.parse_flood(log)
    assert f == {"reads": 412, "ms": 4120, "full": 300, "short": 112}


def test_parse_flood_missing():
    with pytest.raises(ValueError):
        ftlinetime.parse_flood("no flood here\n")


def test_parse_meta():
    meta = ftlinetime.parse_meta("preset=crop\nppi=2000\npx=630\nbin=3\n")
    assert (meta["ppi"], meta["px"], meta["bin"]) == (2000.0, 630, 3)


def test_fit_recovers_per_pixel_model():
    # t = 2 ms + 0.01 ms * px  (pure per-output-pixel cost, no bin term)
    rows = [(px, b, 2.0 + 0.01 * px) for px in (300, 600, 1200) for b in (1, 3)]
    a, b, c = ftlinetime.fit(rows)
    assert a == pytest.approx(2.0, abs=1e-6)
    assert b == pytest.approx(0.01, abs=1e-9)
    assert c == pytest.approx(0.0, abs=1e-9)


def test_fit_recovers_ccd_span_model():
    # t = 1 ms + 0.004 ms * px * bin  (cost per CCD pixel spanned)
    rows = [(px, b, 1.0 + 0.004 * px * b) for px in (300, 600, 1200) for b in (1, 2, 6)]
    a, b, c = ftlinetime.fit(rows)
    assert a == pytest.approx(1.0, abs=1e-6)
    assert b == pytest.approx(0.0, abs=1e-9)
    assert c == pytest.approx(0.004, abs=1e-9)


def test_main_reads_log_and_sibling_metadata(tmp_path, capsys):
    for name, px, b, ms in (("w04", 315, 3, 900), ("w08", 630, 3, 1800),
                            ("w16", 1260, 3, 3600)):
        (tmp_path / f"{name}.log").write_text(
            f"[flood: 300 reads in {ms} ms; 300 full, 0 short (0 bytes)]\n")
        (tmp_path / f"{name}.raw.txt").write_text(f"ppi=2000\npx={px}\nbin={b}\n")
    logs = sorted(str(p) for p in tmp_path.glob("*.log"))
    assert ftlinetime.main(logs) == 0
    out = capsys.readouterr().out
    assert "w08" in out and "fit:" in out
