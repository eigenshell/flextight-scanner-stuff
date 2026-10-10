"""Command-line interface for the Flextight scanner driver.

Transport source is selected globally, before the subcommand:

- ``--script PATH`` / ``$FLEXTIGHT_SCRIPT``: replay a JSON Step script
  (``{"steps": [step_to_dict(Step), ...]}``) through a FakeTransport.
- ``--device PATH`` / ``$FLEXTIGHT_DEVICE``: talk to a real ``/dev/sgX``.

Trace-derived replay is deferred until the Windows IOCTL payload layout
is pinned by hardware captures, so scripts use the explicit Step format.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys

from .scanner import Scanner
from .transport import (
    FakeTransport,
    SGIOTransport,
    TransportError,
    step_from_dict,
)

_STUB_MSG = "not implemented: requires protocol spec (Task 10, hardware-gated)"


class _UsageError(Exception):
    """Bad invocation (unresolvable/malformed source): exit 2."""


def _build_parser() -> argparse.ArgumentParser:
    # Shared parent so --device/--script parse both before AND after the
    # subcommand name.  SUPPRESS defaults are load-bearing: _SubParsersAction
    # re-parses into a fresh namespace and copies every var back, so a plain
    # None default here would clobber a flag given before the subcommand.
    # With SUPPRESS the attribute is only set when the flag is present, and
    # both positions share dests -> one env-default resolution path.
    source = argparse.ArgumentParser(add_help=False)
    source.add_argument("--device", default=argparse.SUPPRESS,
                        help="real scanner device node (e.g. /dev/sg0)")
    source.add_argument("--script", default=argparse.SUPPRESS,
                        help="JSON Step script to replay")
    p = argparse.ArgumentParser(prog="flextight", parents=[source])
    sub = p.add_subparsers(dest="subcommand", required=True)
    sub.add_parser("ls", parents=[source], help="list /dev/sg* device nodes")
    sub.add_parser("info", parents=[source], help="enumerate the scanner (INQUIRY)")
    sub.add_parser("upload-firmware", parents=[source], help="upload scanner firmware (stub)")
    sub.add_parser("preview", parents=[source], help="acquire a preview scan (stub)")
    sub.add_parser("scan", parents=[source], help="acquire a scan (stub)")
    return p


def _resolve_source(args) -> tuple[str, str]:
    """Return (kind, path) for the single configured transport source."""
    device = getattr(args, "device", None) or os.environ.get("FLEXTIGHT_DEVICE")
    script = getattr(args, "script", None) or os.environ.get("FLEXTIGHT_SCRIPT")
    if device and script:
        raise _UsageError(
            "both --device/--script configured: exactly one transport "
            "source (--device PATH or --script PATH) must be given"
        )
    if script:
        return "script", script
    if device:
        return "device", device
    raise _UsageError(
        "no transport source: pass --device PATH or --script PATH "
        "(or set FLEXTIGHT_DEVICE / FLEXTIGHT_SCRIPT)"
    )


def _err(code: int, msg: str) -> int:
    print(f"flextight: {msg}", file=sys.stderr)
    return code


def _make_transport(args):
    kind, path = _resolve_source(args)
    if kind == "script":
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
            steps = [step_from_dict(d) for d in data["steps"]]
        except TransportError as e:
            raise _UsageError(f"{path}: {e}") from e
        except (OSError, ValueError, TypeError, KeyError) as e:
            raise _UsageError(f"{path}: invalid script: {e}") from e
        return FakeTransport.from_script(steps)
    return SGIOTransport.open(path)


def _cmd_ls() -> int:
    nodes = sorted(glob.glob("/dev/sg*"))
    if nodes:
        for n in nodes:
            print(n)
    else:
        print("none found")
    return 0


def _cmd_info(transport) -> int:
    scanner = Scanner(transport)
    try:
        info = scanner.enumerate()
    finally:
        scanner.close()
    for key in ("vendor", "model", "rev", "pdt"):
        print(f"{key}: {info[key]}")
    return 0


def main(argv=None) -> int:
    args = _build_parser().parse_args(argv)

    if args.subcommand == "ls":
        return _cmd_ls()
    if args.subcommand in ("upload-firmware", "preview", "scan"):
        return _err(2, _STUB_MSG)

    transport = None
    try:
        transport = _make_transport(args)
        if args.subcommand == "info":
            return _cmd_info(transport)
        raise AssertionError(f"unhandled subcommand {args.subcommand}")
    except _UsageError as e:
        return _err(2, str(e))
    except TransportError as e:
        return _err(1, str(e))
    except (BrokenPipeError, KeyboardInterrupt):
        return 130
    finally:
        if transport is not None:
            transport.close()


if __name__ == "__main__":
    raise SystemExit(main())
