#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""merge_trace.py - interleave the ioctl and UI-event timelines.

Both flextrace_ioctl.jsonl (ft_hook.dll) and flextrace_uiev.jsonl (uiev.exe)
stamp records with t = GetTickCount() ms — the same system-wide clock — so
the files interleave exactly and a UI action can be tied to the SCSI traffic
it caused:

    [t=61234] UIEV  mdown btn=l class=Button ctl=1042 wtext="Descreen"
    [t=61235] SCSI  seq=12034 cdb=e0 ok=1 out_len=4   <- the apply that followed

Usage: merge_trace.py TRACEDIR [--from T --to T] [--scsi-only|--uiev-only]
  TRACEDIR: dir holding flextrace_ioctl.jsonl and flextrace_uiev.jsonl
  --from/--to: only records with from <= t <= to (GetTickCount ms values)
"""
import json
import sys
from pathlib import Path

CDB_NAMES = {
    "12": "INQUIRY", "24": "STATUS?", "28": "READ(10)", "3c": "READ_BUF",
    "df": "VENDOR_POLL", "e0": "VENDOR_SET",
}


def load(path, kinds):
    if not path.exists():
        return []
    out = []
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            if r.get("kind") in kinds or (kinds == ("scsi",) and "code" in r and "seq" in r):
                if "t" in r:
                    out.append(r)
    return out


def fmt_scsi(r):
    ih = r.get("in_head", "")
    cdb = ih[40:42] if len(ih) >= 42 else "?"
    name = CDB_NAMES.get(cdb, "")
    s = f"seq={r['seq']} cdb={cdb}"
    if name:
        s += f"({name})"
    s += f" ok={r.get('ok')} out_len={r.get('out_len')}"
    return s


def fmt_uiev(r):
    kind = r["kind"]
    if kind in ("mdown", "mup"):
        tgt = ""
        if r.get("class"):
            tgt = f" class={r['class']} ctl={r.get('ctl')} wtext={r.get('wtext','')!r}"
        return f"mdown/mup btn={r.get('btn')} x={r['x']} y={r['y']}{tgt} fg={r.get('fg','')!r}"
    if kind == "move":
        return f"drag-move x={r['x']} y={r['y']}"
    if kind == "wheel":
        return f"wheel {'up' if r.get('wheel', 0) > 0 else 'down'} x={r['x']} y={r['y']}"
    if kind == "key":
        return f"key vk={r.get('vk')} fg={r.get('fg','')!r}"
    if kind == "title":
        return f"TITLE -> {r.get('fg','')!r}"
    return json.dumps(r)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    d = Path(args[0])
    t0 = t1 = None
    for i, a in enumerate(sys.argv[1:]):
        if a == "--from" and i + 2 <= len(sys.argv):
            t0 = int(sys.argv[i + 2])
        if a == "--to" and i + 2 <= len(sys.argv):
            t1 = int(sys.argv[i + 2])
    scsi_only = "--scsi-only" in sys.argv
    uiev_only = "--uiev-only" in sys.argv

    recs = []
    if not uiev_only:
        for r in load(d / "flextrace_ioctl.jsonl", ("scsi",)):
            recs.append((r["t"], 1, "SCSI", r))
    if not scsi_only:
        for r in load(d / "flextrace_uiev.jsonl", ("mdown", "mup", "move", "wheel", "key", "title")):
            recs.append((r["t"], 0, "UIEV", r))

    recs.sort(key=lambda x: (x[0], x[1]))  # uiev first within the same ms
    n = 0
    for t, _, src, r in recs:
        if t0 is not None and t < t0:
            continue
        if t1 is not None and t > t1:
            break
        body = fmt_scsi(r) if src == "SCSI" else fmt_uiev(r)
        print(f"[t={t}] {src:4s} {body}")
        n += 1
    print(f"-- {n} records", file=sys.stderr)


if __name__ == "__main__":
    main()
