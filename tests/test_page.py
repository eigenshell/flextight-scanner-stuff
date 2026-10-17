# SPDX-License-Identifier: GPL-2.0-or-later
"""The web page (tools/ftwebd/www): its worker (ftweb.js, the engine's
wasm behind messages) under node, and the whole page end to end in a
headless Chromium against ftwebd and the fake ftpoc: the JPEGs it
downloads are ftdev cut + ftdev print --roll's, byte for byte. Skipped
without the wasi-sdk, node >= 22 (and Chromium for the end-to-end)."""
import json
import shutil
import subprocess
import zipfile
from pathlib import Path

import pytest

from tests.test_ftweb import NODE, WASI_SDK, make
from tests.test_ftwebd import Server

ROOT = Path(__file__).resolve().parent.parent
WWW = ROOT / "tools" / "ftwebd" / "www"
FIX = ROOT / "tests" / "fixtures" / "ftwebd"
FTDEV = ROOT / "tools" / "ftdev" / "ftdev"
CHROME = next((c for c in (shutil.which(n) for n in (
    "chromium", "chromium-browser", "google-chrome", "google-chrome-stable"))
    if c), None)
pytestmark = pytest.mark.skipif(
    shutil.which("make") is None or shutil.which("cc") is None or
    not (WASI_SDK / "bin" / "clang").exists() or NODE is None,
    reason="needs make, cc, the wasi-sdk (WASI_SDK) and node >= 22 (NODE)")


@pytest.fixture(scope="module")
def built():
    make("-C", ROOT / "tools" / "ftweb", "www", "WASI_SDK=%s" % WASI_SDK)
    make("-C", ROOT / "tools" / "ftwebd")
    make("-C", ROOT / "tools" / "ftdev")
    make("-C", ROOT / "libft", "test/window_dump")
    return WWW / "ftweb.wasm"


def worker(tmp, calls):
    """the page's worker runs calls (worker_run.mjs) -> one reply each"""
    p = tmp / "calls.json"
    p.write_text(json.dumps(calls))
    r = subprocess.run([NODE, "--no-warnings", str(FIX / "worker_run.mjs"),
                        str(WWW), str(p)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return json.loads(r.stdout)


def test_worker_plans_as_ftdev(built, tmp_path):
    """the page's planner (ftw_plan through the worker) writes ftdev
    plan's job; every plan starts again from the preview's job"""
    pv = (FIX / "preview.job").read_text()
    sizes = [2450, 2450, 3000, 0, 2450, 0]
    rep = worker(tmp_path, [
        {"op": "job", "a": [pv]},
        {"op": "plan", "a": [sizes]},
        {"op": "plan", "a": [[0, 0, 0, 3000, 0, 0]]},
        {"op": "plan", "a": [sizes]},
        {"op": "jobJson", "a": ["px=666\nx=2\nbin=12\n"]},
    ])
    assert all("r" in x for x in rep), rep
    first, again = rep[1]["r"], rep[3]["r"]
    assert first == again                     # from the base, not the plan
    assert rep[2]["r"]["windows"] == 1
    shutil.copy(FIX / "preview.job", tmp_path / "strip.job")
    r = subprocess.run([str(FTDEV), "plan", str(tmp_path / "strip.job"),
                        "--size", "4mp:1-2,5", "--size", "6mp:3"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert first["text"] == (tmp_path / "strip.job").read_text()
    assert first["windows"] == int(r.stdout.split("\n")[-2].split()[0])
    j = rep[4]["r"]
    assert j["film"] == "neg" and j["holder"] == "24x217"
    fr = j["frames"]
    assert [f["n"] for f in fr] == [1, 2, 3, 4, 5, 6]
    assert [f["px"] for f in fr] == [2450, 2450, 3000, 0, 2450, 0]
    assert fr[0]["group"] == fr[1]["group"] != fr[2]["group"]
    assert fr[5]["type"] == "partial" and not any(f["done"] for f in fr)
    # boxes in strip.png pixels (525 ppi), along the strip in order
    assert all(a["x"] < b["x"] for a, b in zip(fr, fr[1:]))
    assert fr[0]["w"] == pytest.approx(36 / 25.4 * 525, rel=0.02)


def test_worker_job_from_the_box(built, tmp_path):
    """strip.job as the box keeps it (planLoad): its sizes and done
    frames show; without one, no sizes"""
    pv = (FIX / "preview.job").read_text()
    done = pv.replace("done = no", "done = yes", 1)
    rep = worker(tmp_path, [
        {"op": "job", "a": [pv]},
        {"op": "jobJson", "a": [None]},
        {"op": "planLoad", "a": [done]},
        {"op": "jobJson", "a": [None]},
        {"op": "planLoad", "a": ["[strip]\nbogus = 1\n"]},
    ])
    assert [f["px"] for f in rep[1]["r"]["frames"]] == [0] * 6
    fr = rep[3]["r"]["frames"]
    assert fr[0]["done"] and not fr[1]["done"]
    assert fr[0]["px"] == 2450
    assert "bogus" in rep[4]["err"]


def test_worker_lens(built, tmp_path):
    """the box's focus.txt reaches the planner, checked by C"""
    rep = worker(tmp_path, [
        {"op": "lens", "a": ["film = 1\nw = 2\n"]},
        {"op": "lens", "a": ["# fit\nfilm = 1468601.0\nw = 268105.0\n"
                             "fl = 317836.0\np = 3.4799e-04\n"]},
    ])
    assert rep[0] == {"err": "focus file: needs film, w, fl and p"}
    assert rep[1] == {"r": None}


def test_worker_print_file(built, tmp_path):
    """strip.print through the worker: clicks read and set, the emulsion
    changed (clicks that fit stay), the file text"""
    rep = worker(tmp_path, [
        {"op": "emulsions"},
        {"op": "printLoad", "a": ["emulsion = generic-neg\n\n[frame 2]\n"
                                  "cmyd = 3 -2 0 1\n"]},
        {"op": "printGet", "a": [2]},
        {"op": "printGet", "a": [5]},
        {"op": "clicks", "a": [5, [0, 1, 0, -1]]},
        {"op": "printText"},
        {"op": "printEmulsion", "a": ["generic-neg"]},
        {"op": "printEmulsion", "a": ["nope"]},
        {"op": "clicks", "a": [5, [100, 0, 0, 0]]},
        {"op": "rotate", "a": [2, 270]},
        {"op": "rotation", "a": [2]},
        {"op": "rotation", "a": [7]},
        {"op": "rotate", "a": [7, 45]},
        {"op": "printEmulsion", "a": ["generic-bw"]},
        {"op": "printText"},
    ])
    ems = rep[0]["r"]
    assert {"id", "name", "mode"} <= set(ems[0])
    assert any(e["id"] == "generic-neg" for e in ems)
    assert rep[2]["r"] == [3, -2, 0, 1]
    assert rep[3]["r"] == [0, 0, 0, 0]
    assert rep[5]["r"] == ("emulsion = generic-neg\n\n[frame 2]\n"
                           "cmyd = 3 -2 0 1\n\n[frame 5]\ncmyd = 0 1 0 -1\n")
    assert rep[6]["r"] == 0                   # nothing dropped
    assert rep[7] == {"err": "not an emulsion"}
    assert rep[8] == {"err": "cmyd: C 100 is out of range (-99..99)"}
    # the turn: read and set; it stays when the emulsion drops the clicks
    assert rep[10]["r"] == 270 and rep[11]["r"] == 0
    assert rep[12] == {"err": "rotate: 45 is not 0, 90, 180 or 270"}
    assert rep[13]["r"] == 2                  # both frames' clicks dropped
    assert rep[14]["r"] == ("emulsion = generic-bw\n\n[frame 2]\n"
                            "rotate = 270\n")


def run_page(built, tmp_path):
    """the page in Chromium: new strip, preview, taps (3: 6MP, 4: ARC,
    5: skip), scan, clicks c c M and a turn (r) on frame 1, zooms (z, a
    tap, the strip's +), Download and All"""
    win = tmp_path / "wd" / "win"
    win.parent.mkdir()
    subprocess.run([str(ROOT / "libft" / "test" / "window_dump"), str(win)],
                   check=True)
    box = tmp_path / "box"
    box.mkdir()
    dl = tmp_path / "dl"
    dl.mkdir()
    srv = Server(ROOT / "tools" / "ftwebd" / "ftwebd", box, 0.001,
                 args=["--archive", box / "archive"],
                 env={"FAKE_FTPOC_WINDOW": str(win)})
    try:
        r = subprocess.run([NODE, "--no-warnings", str(FIX / "page_e2e.mjs"),
                            CHROME, "http://127.0.0.1:%d/" % srv.port,
                            str(dl)], capture_output=True, text=True,
                           timeout=240)
    finally:
        srv.close()
    assert r.returncode == 0, r.stderr[-4000:]
    return json.loads(r.stdout), box, dl


@pytest.mark.skipif(CHROME is None, reason="needs Chromium")
def test_page_end_to_end(built, tmp_path):
    out, box, dl = run_page(built, tmp_path)
    s = out["strip"]
    strip = box / "strips" / s
    assert out["landed"] == [1, 3]            # one fake frame per window
    assert out["print"] == ("emulsion = generic-neg\n\n[frame 1]\n"
                            "cmyd = 2 -1 0 0\nrotate = 90\n")
    log = [json.loads(l) for l in (box / "fake_ftpoc.log").read_text()
           .splitlines()]
    arc = str(box / "archive" / s / "strip")
    assert log[1:] == [
        ["scan", "--job", "strips/%s/strip.job" % s, "--progress"],
        ["frames", "--detail", "neg", "--ppi", "6300", "--film", "neg",
         "--select", "4", "--progress", "-o", arc],
        ["scan", "--job", arc + ".job", "--progress"]]
    job = (strip / "strip.job").read_text()
    assert "share_px = 3000" in job and "done = yes" in job
    # the CLI on the same strip folder
    for w in sorted(strip.glob("strip_win*.raw")):
        subprocess.run([str(FTDEV), "cut", str(w)], check=True,
                       capture_output=True)
    dngs = [str(strip / "strip_frame01.dng"), str(strip / "strip_frame03.dng")]
    r = subprocess.run([str(FTDEV), "print", *dngs, "--roll",
                        str(strip / "strip.print"), "--job",
                        str(strip / "strip.job")], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stderr
    page = dl / (s + "_frame01.jpg")
    assert page.read_bytes() == (strip / "strip_frame01.jpg").read_bytes()
    assert (dl / (s + "_frame01.jpg.txt")).read_bytes() == \
        (strip / "strip_frame01.jpg.txt").read_bytes()
    assert "\ncmyd=2 -1 0 0\n" in (strip / "strip_frame01.jpg.txt").read_text()
    assert "\nrotate=90\n" in (strip / "strip_frame01.jpg.txt").read_text()
    with zipfile.ZipFile(dl / (s + ".zip")) as z:
        assert z.testzip() is None
        assert z.namelist() == [s + "_frame01.jpg", s + "_frame01.jpg.txt",
                                s + "_frame03.jpg", s + "_frame03.jpg.txt"]
        for n in z.namelist():
            assert z.read(n) == (strip / ("strip" + n[len(s):])).read_bytes()
    assert "note: 2 frames" in out["notes"]
