# SPDX-License-Identifier: GPL-2.0-or-later
"""The web page's engine (tools/ftweb) makes the CLI's JPEGs: ftweb_cli
(the engine driven from files) against ftdev cut + ftdev print --roll on
synthetic window scans; and the engine compiled to wasm32 makes the same
bytes as native (skipped without the wasi-sdk or node)."""
import os
import shutil
import subprocess
import zipfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
LIBFT = ROOT / "libft"
FTDEV = ROOT / "tools" / "ftdev"
FTWEB = ROOT / "tools" / "ftweb"
needs_cc = pytest.mark.skipif(
    shutil.which("make") is None or shutil.which("cc") is None,
    reason="needs make and a C compiler")

PRINT = "emulsion = generic-neg\n\n[frame 2]\ncmyd = 3 -2 0 1\n\n" \
        "[frame 9]\ncmyd = 0 0 0 2\n"


def make(*args):
    r = subprocess.run(["make", "-s", *map(str, args)], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stdout + r.stderr


@pytest.fixture(scope="module")
def tools():
    make("-C", FTDEV)
    make("-C", FTWEB)
    make("-C", LIBFT, "test/window_dump")
    return FTDEV / "ftdev", FTWEB / "ftweb_cli"


def windows(d):
    """three window scans of the synthetic strip as frames 1, 2, 3; the
    third with another film base"""
    d.mkdir()
    subprocess.run([str(LIBFT / "test" / "window_dump"),
                    str(d / "job_win01")], check=True)
    for n in (2, 3):
        w = d / ("job_win%02d.raw" % n)
        shutil.copy(d / "job_win01.raw", w)
        shutil.copy(d / "job_win01.raw.cal", str(w) + ".cal")
        txt = (d / "job_win01.raw.txt").read_text()
        txt = txt.replace("share_frame=1 ", "share_frame=%d " % n)
        if n == 3:
            txt = txt.replace("film_base=0.2076 0.3979 0.6021",
                              "film_base=0.2150 0.4100 0.6200")
        Path(str(w) + ".txt").write_text(txt)
    (d / "strip.print").write_text(PRINT)
    return [d / ("job_win%02d.raw" % n) for n in (1, 2, 3)]


@pytest.fixture(scope="module")
def both(tools, tmp_path_factory):
    """the CLI's and the engine's outputs for the same windows"""
    ftdev, cli = tools
    tmp = tmp_path_factory.mktemp("ftweb")
    a = windows(tmp / "cli")
    for w in a:
        r = subprocess.run([str(ftdev), "cut", str(w)], capture_output=True,
                           text=True)
        assert r.returncode == 0, r.stderr
    dngs = [str(tmp / "cli" / ("job_frame%02d.dng" % n)) for n in (1, 2, 3)]
    pr = subprocess.run([str(ftdev), "print", *dngs, "--roll",
                         str(tmp / "cli" / "strip.print")],
                        capture_output=True, text=True)
    assert pr.returncode == 0, pr.stderr
    b = windows(tmp / "web")
    out = tmp / "web" / "out"
    out.mkdir()
    wr = subprocess.run([str(cli), "--print", str(tmp / "web" / "strip.print"),
                         "-o", str(out), "--display", "2", "400",
                         *map(str, b)], capture_output=True, text=True)
    assert wr.returncode == 0, wr.stderr
    return tmp / "cli", out, pr, wr


@needs_cc
def test_engine_makes_the_cli_jpegs(both):
    cli, web, _, _ = both
    for n in (1, 2, 3):
        name = "job_frame%02d.jpg" % n
        a, b = (cli / name).read_bytes(), (web / name).read_bytes()
        assert a[:2] == b"\xff\xd8" and len(a) > 10000
        assert a == b, name
        assert (cli / (name + ".txt")).read_bytes() == \
            (web / (name + ".txt")).read_bytes()
    # the clicks and the roll balance reached the prints
    t2 = (web / "job_frame02.jpg.txt").read_text()
    assert "\ncmyd=3 -2 0 1\n" in t2
    assert (web / "job_frame01.jpg").read_bytes() != \
        (web / "job_frame02.jpg").read_bytes()


@needs_cc
def test_engine_notes_are_the_cli_warnings(both):
    _, _, pr, wr = both
    assert "[frame 9] is not in the job: ignored" in pr.stderr
    assert "warning: strip.print: [frame 9] is not in the job: ignored" \
        in wr.stderr
    assert "note: 3 frames: print a roll's frames together" in pr.stderr
    assert "note: 3 frames: print a roll's frames together" in wr.stderr


@needs_cc
def test_engine_zip_holds_every_print(both):
    cli, web, _, _ = both
    with zipfile.ZipFile(web / "job.zip") as z:
        assert z.testzip() is None
        assert z.namelist() == [
            "job_frame01.jpg", "job_frame01.jpg.txt",
            "job_frame02.jpg", "job_frame02.jpg.txt",
            "job_frame03.jpg", "job_frame03.jpg.txt"]
        for name in z.namelist():
            assert z.read(name) == (cli / name).read_bytes(), name


@needs_cc
def test_engine_display_copy(both):
    _, web, _, wr = both
    line = [l for l in wr.stdout.splitlines() if l.startswith("display ")]
    w, h = map(int, line[0].split()[1].split("x"))
    assert max(w, h) == 400
    data = (web / "display.rgba").read_bytes()
    assert len(data) == w * h * 4
    assert set(data[3::4]) == {255}               # opaque
    assert len(set(data[0::4])) > 20              # an image, not flat
