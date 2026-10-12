# flextight-scanner-stuff

Independent, interoperability-driven work to run an **Imacon / Hasselblad
Flextight II (Precision II)** film scanner without the discontinued
FlexColor software — first on Windows, then on Linux.

## Status

- `docs/protocol.md` — the scanner's SCSI protocol as observed on our own
  hardware: command set, startup/firmware upload, the scan descriptor
  (resolution, window, binning), the per-scan sequence, and the host-side
  image corrections (dark/white reference, defective pixels, tri-linear
  CCD row alignment).
- `tools/poc/` — `ftpoc`, a Windows console tool that drives the scanner
  directly (preview, crops at any resolution up to 6300 ppi, 3f strips),
  and `ftraw.py`, which turns its output into images.
- `src/flextight/` — the Python package for the Linux (SG_IO) stack.
- `tools/spti_logger/` — the DeviceIoControl tracer used to observe the
  protocol.
- `tools/xml2dtstyle.py` — converts film profiles from *your own*
  FlexColor installation into darktable styles.

## What is NOT in this repository

No Hasselblad/Imacon software, firmware, film profiles, ICC profiles or
disassembly is distributed here. Where the scanner needs vendor data —
most importantly the `MICROCOD.3XX` firmware the host uploads after a cold
power-up — the tools read it from a FlexColor installation you obtained
yourself. Test fixtures are synthetic.

The protocol was documented by observing the traffic between FlexColor
and a scanner we own, and by limited analysis of the software for the
sole purpose of interoperability with that hardware. Descriptions are our
own.

## Legal

Not affiliated with, endorsed by, or supported by Hasselblad or Imacon.
"Hasselblad", "Imacon", "Flextight" and "FlexColor" are trademarks of
their respective owners, used here only to identify the hardware and
software this project interoperates with.

Use at your own risk: this talks to expensive, irreplaceable hardware.
A malformed scan descriptor can leave the scanner busy until it is
power-cycled (see `docs/protocol.md`).
