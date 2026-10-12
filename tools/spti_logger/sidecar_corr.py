#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Correlate flextrace_bufs.bin (payload sidecar) with flextrace_ioctl.jsonl.

Sidecar record: 12-byte header <IIBBH (pid, seq, when, cand, len) + payload.
Candidates: 0x36 = SRB offset-36 buffer, 0x51/0x52 = nested dwords of the
0x36 descriptor, 0x60 = DeviceIoControl lpOutBuffer.
JSONL: one object per line; ioctl records keyed by (pid, seq). 'seq' appears
in both ioctl records and lasterr control records.

Usage: sidecar_corr.py TRACEDIR [CDBHEX ...]
  TRACEDIR: dir holding flextrace_ioctl.jsonl + flextrace_bufs.bin
  CDBHEX: CDB opcodes to show (default: 12 df 3c 24 e0)
Prints, per matching ioctl record, every sidecar dump for that (pid, seq).
"""
import json
import struct
import sys
from pathlib import Path


def load_sidecar(path):
    recs = {}
    data = Path(path).read_bytes()
    off = 0
    while off + 12 <= len(data):
        pid, seq, when, cand, ln = struct.unpack_from("<IIBBH", data, off)
        off += 12
        if off + ln > len(data):
            print(f"WARN: truncated payload at offset {off}", file=sys.stderr)
            break
        payload = data[off:off + ln]
        off += ln
        recs.setdefault((pid, seq), []).append((when, cand, payload))
    return recs


def load_jsonl(path):
    recs = []
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            if r.get("tag") == "lasterr":
                continue  # companion record, shares seq with the failed call
            if "seq" in r and "code" in r:
                recs.append(r)
    return recs


def brief(payload, limit=64):
    if len(payload) <= limit:
        return payload.hex()
    return payload[:limit].hex() + f"...({len(payload)}B)"


def main():
    d = Path(sys.argv[1])
    wanted = set(sys.argv[2:]) or {"12", "df", "3c", "24", "e0"}
    side = load_sidecar(d / "flextrace_bufs.bin")
    jl = load_jsonl(d / "flextrace_ioctl.jsonl")
    shown = 0
    for r in jl:
        ih = r.get("in_head", "")
        cdb = ih[40:42] if len(ih) >= 42 else ""  # SRB offset 20 -> hex 40
        if cdb not in wanted:
            continue
        pid = r.get("pid")
        if pid is None:
            # jsonl ioctl records may not carry pid; sidecar pid is per-process
            # and the smoke box runs one FlexColor process — match by seq.
            keys = [k for k in side if k[1] == r["seq"]]
        else:
            keys = [(pid, r["seq"])] if (pid, r["seq"]) in side else []
        if not keys:
            continue
        print(f"seq={r['seq']} cdb={cdb} ok={r.get('ok')} "
              f"out_len={r.get('out_len')} out_head={r.get('out_head', '')[:32]}")
        for k in keys:
            for when, cand, payload in side[k]:
                print(f"  when={when} cand=0x{cand:02x} len={len(payload)} "
                      f"{brief(payload)}")
        shown += 1
        if shown >= 40:
            print("...(capped at 40 matching records)")
            break


if __name__ == "__main__":
    main()
