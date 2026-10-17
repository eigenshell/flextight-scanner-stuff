# SPDX-License-Identifier: GPL-2.0-or-later
"""ftwebd, the scanner box's web server, against a fake ftpoc
(tests/fixtures/ftwebd/fake_ftpoc.py): the argv built per op, the event
stream and its resume, one task at a time, stop, the planner endpoint and
the checks on names and bodies."""
import json
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "tools" / "ftwebd"
FAKE = ROOT / "tests" / "fixtures" / "ftwebd" / "fake_ftpoc.py"
pytestmark = pytest.mark.skipif(
    shutil.which("make") is None or shutil.which("cc") is None or
    sys.platform == "win32", reason="needs make, a C compiler and POSIX")


@pytest.fixture(scope="module")
def binary():
    r = subprocess.run(["make", "-C", str(SRC)], capture_output=True,
                       text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return SRC / "ftwebd"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    def __init__(self, binary, cwd, delay):
        self.port = free_port()
        env = dict(os.environ, FAKE_FTPOC_DELAY=str(delay))
        self.cwd = cwd
        self.p = subprocess.Popen(
            [str(binary), "--port", str(self.port), "--ftpoc", str(FAKE),
             "--www", str(SRC / "www")], cwd=cwd, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", self.port), 0.1).close()
                return
            except OSError:
                time.sleep(0.02)
        raise RuntimeError("ftwebd did not start")

    def req(self, method, path, body=None, raw=False):
        data = None
        if isinstance(body, dict):
            data = urllib.parse.urlencode(body).encode()
        elif isinstance(body, str):
            data = body.encode()
        r = urllib.request.Request("http://127.0.0.1:%d%s" % (self.port, path),
                                   data=data, method=method)
        try:
            with urllib.request.urlopen(r, timeout=10) as f:
                code, text = f.status, f.read()
        except urllib.error.HTTPError as e:
            code, text = e.code, e.read()
        return code, text if raw else json.loads(text)

    def events(self, tid, since=0):
        """[(id, data)] until the end event"""
        out = []
        with urllib.request.urlopen(
                "http://127.0.0.1:%d/api/tasks/%d/events?since=%d" %
                (self.port, tid, since), timeout=20) as f:
            assert f.headers["Content-Type"] == "text/event-stream"
            ev, eid = None, None
            for line in f:
                line = line.decode().rstrip("\n")
                if line.startswith("id: "):
                    eid = int(line[4:])
                elif line.startswith("event: "):
                    ev = line[7:]
                elif line.startswith("data: "):
                    if ev == "end":
                        return out
                    out.append((eid, line[6:]))
                elif line == "":
                    ev = None
        return out

    def wait(self, tid):
        for _ in range(500):
            code, t = self.req("GET", "/api/tasks/%d" % tid)
            if t["state"] == "done":
                return t
            time.sleep(0.02)
        raise RuntimeError("task did not end")

    def close(self):
        self.p.kill()
        self.p.wait()


@pytest.fixture
def srv(binary, tmp_path):
    s = Server(binary, tmp_path, 0.001)
    yield s
    s.close()


@pytest.fixture
def slow(binary, tmp_path):
    s = Server(binary, tmp_path, 0.05)
    yield s
    s.close()


def previewed(s, name="a"):
    assert s.req("POST", "/api/strips", {"name": name}) == (200, {"name": name})
    code, t = s.req("POST", "/api/tasks",
                    {"op": "preview", "strip": name, "film": "neg"})
    assert code == 202
    assert s.wait(t["id"])["exit"] == 0
    return t["id"]


def argv_log(s):
    return [json.loads(l) for l in
            (s.cwd / "fake_ftpoc.log").read_text().splitlines()]


def test_preview_argv_events_and_files(srv):
    tid = previewed(srv)
    # the client names an op; ftwebd builds the argv
    assert argv_log(srv) == [["frames", "--film", "neg", "--share", "2450",
                              "--progress", "-o", "strips/a/strip"]]
    ev = srv.events(tid)
    assert [i for i, _ in ev] == list(range(1, len(ev) + 1))
    assert ev[0][1] == "%s frames --film neg --share 2450 --progress -o " \
        "strips/a/strip" % FAKE
    assert ev[1][1] == "progress begin preview 1"
    assert ev[-2][1] == "progress end ok"
    assert ev[-1][1] == "ftwebd exit 0"
    # resume after line k: exactly the rest
    assert srv.events(tid, since=5) == ev[5:]
    d = srv.cwd / "strips" / "a"
    # a good preview keeps its job as the planner's starting point
    assert (d / "strip.preview.job").read_text() == \
        (d / "strip.job").read_text()
    code, files = srv.req("GET", "/api/strips/a/files")
    assert {f["name"] for f in files} >= {"strip.job", "strip.png",
                                          "strip.preview.job"}
    code, png = srv.req("GET", "/api/strips/a/files/strip.png", raw=True)
    assert code == 200 and png.startswith(b"\x89PNG")
    assert srv.req("GET", "/api/strips")[1] == \
        [{"name": "a", "job": True, "png": True}]
    code, t = srv.req("GET", "/api/tasks/current")
    assert t == {"id": tid, "op": "preview", "strip": "a", "state": "done",
                 "exit": 0, "stopped": False, "lines": len(ev)}


def test_frames(srv):
    previewed(srv)
    code, f = srv.req("GET", "/api/strips/a/frames")
    assert code == 200 and f["film"] == "neg"
    fr = f["frames"]
    assert [x["n"] for x in fr] == [1, 2, 3, 4, 5, 6]
    assert [x["size"] for x in fr] == [2450] * 5 + [0]
    assert fr[5]["type"] == "partial"
    # boxes in strip.png pixels, along the strip in order, ~525 ppi
    assert all(a["x"] < b["x"] for a, b in zip(fr, fr[1:]))
    assert fr[0]["w"] == pytest.approx(36 / 25.4 * 525, rel=0.02)


def test_plan_and_scan(srv):
    previewed(srv)
    code, p = srv.req("POST", "/api/strips/a/plan",
                      {"f1": 2450, "f2": 2450, "f3": 3000, "f5": 2450})
    assert code == 200
    w = p["windows"]
    assert [x["frames"] for x in w] == [[1, 2], [3], [5]]
    assert [x["px"] for x in w] == [2450, 3000, 2450]
    d = srv.cwd / "strips" / "a"
    job = (d / "strip.job").read_text()
    assert "share_px = 3000" in job
    # the next plan starts again from the preview's job: frame 4 is back
    code, p = srv.req("POST", "/api/strips/a/plan", {"f4": 3000})
    assert code == 200 and p["windows"][0]["frames"] == [4]
    code, f = srv.req("GET", "/api/strips/a/frames")
    assert [x["size"] for x in f["frames"]] == [0, 0, 0, 3000, 0, 0]
    code, t = srv.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})
    assert code == 202
    assert srv.wait(t["id"])["exit"] == 0
    assert argv_log(srv)[-1] == ["scan", "--job", "strips/a/strip.job",
                                 "--progress"]
    assert (d / "strip_win01.raw").exists()


def test_plan_refusals(srv):
    previewed(srv)
    assert srv.req("POST", "/api/strips/a/plan", {"f1": 0}) == \
        (422, {"error": "no frame chosen"})
    assert srv.req("POST", "/api/strips/a/plan", {"f1": "big"})[0] == 422
    assert srv.req("POST", "/api/strips/a/plan", {"f1": 99999})[0] == 422


def test_busy_and_stop(slow):
    slow.req("POST", "/api/strips", {"name": "a"})
    code, t = slow.req("POST", "/api/tasks",
                       {"op": "preview", "strip": "a", "film": "slide"})
    assert code == 202
    code, e = slow.req("POST", "/api/tasks", {"op": "scan", "strip": "a"})
    assert code == 409
    assert e["error"] == "scanner busy: preview a (task %d)" % t["id"]
    time.sleep(0.3)
    assert slow.req("POST", "/api/tasks/%d/stop" % t["id"]) == \
        (200, {"ok": True})
    done = slow.wait(t["id"])
    assert done["stopped"] and done["exit"] == 1
    ev = slow.events(t["id"])
    assert [d for _, d in ev[-2:]] == ["progress end stopped",
                                       "ftwebd exit 1"]
    # stopped: not a good preview, nothing kept for the planner
    assert not (slow.cwd / "strips" / "a" / "strip.preview.job").exists()
    assert slow.req("POST", "/api/tasks/%d/stop" % t["id"])[0] == 409
    assert slow.req("POST", "/api/tasks/99/stop")[0] == 404


@pytest.mark.parametrize("body", [
    {"op": "rm", "strip": "a"},
    {"op": "preview", "strip": "a"},                  # no film
    {"op": "preview", "strip": "a", "film": "x; rm"},
    {"op": "scan", "strip": "../a"},
    {"op": "scan"},
])
def test_task_refusals(srv, body):
    srv.req("POST", "/api/strips", {"name": "a"})
    assert srv.req("POST", "/api/tasks", body)[0] == 422
    assert not (srv.cwd / "fake_ftpoc.log").exists()


@pytest.mark.parametrize("name", ["", ".a", "a/b", "..", "a b", "x" * 65])
def test_strip_names(srv, name):
    assert srv.req("POST", "/api/strips", {"name": name})[0] == 422


@pytest.mark.parametrize("path", [
    "/api/strips/a/files/..%2fstrip.job",
    "/api/strips/a/files/.hidden",
    "/api/strips/../files",
    "/..%2f..%2fetc%2fpasswd",
])
def test_traversal_refused(srv, path):
    srv.req("POST", "/api/strips", {"name": "a"})
    assert srv.req("GET", path, raw=True)[0] == 404


def test_job_put_validated(srv):
    previewed(srv)
    d = srv.cwd / "strips" / "a"
    before = (d / "strip.job").read_text()
    code, e = srv.req("PUT", "/api/strips/a/strip.job", "[strip]\nbogus = 1\n")
    assert code == 422 and "bogus" in e["error"]
    assert (d / "strip.job").read_text() == before
    assert srv.req("PUT", "/api/strips/a/strip.job", before) == \
        (200, {"ok": True})


def test_page_fits_the_first_packet(srv):
    code, page = srv.req("GET", "/", raw=True)
    assert code == 200 and page.startswith(b"<!doctype html>")
    assert len(page) < 14 * 1024
