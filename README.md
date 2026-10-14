# Flextight SCSI Scanner Software

Some time ago I managed to get my hands on an old 1999 Flextight Precision II
scanner that was sold as is without any guarantees for quite a reasonable price.

It took me a while to get everything working.

First of all, it didn't come with anything at all, so after scouring ebay, I managed
to find a compatible powersupply, and got a 24x36x6 transparency holder from 
https://www.filmchrome.net/

With the power supply I was at least able to verify that the machine turned on,
but not much else. It took me another while to find a suitable SCSI adapter, 
and a computer that still has the, now ancient, PCI connector that the SCSI 
adapter plugs into. 

The first SCSI card I bought was unfortunately a dud, and the second one used a
different connector (68 pin) than the cable I had laying around.
As I wasn't able to find a 68 pin hpdb to 50 pin centronics cable for a reasonable 
price, I eventually settled on an adaptec 2940AU with a 50 pin hpdb connector.

In order to get the SCSI card working on 64 bit windows, I followed the guide published on
https://www.savagetaylor.com/2018/02/11/scsi-on-windows-10-adaptec-aha-2940-adaptec-29xx-ultra-or-aic-7870-adaptec-78xx/
from which I downloaded the relevant driver 
https://www.savagetaylor.com/hw/scsi/Adaptec_AHA29xx_modified_Aic78xx_driver.zip
and installed it.

Thanks to the brilliant writeup from https://tinker.koraks.nl/photography/hold-tight-operating-an-old-imacon-scsi-flextight-precision-ii-scanner-on-windows-10-x64/
I managed to work out the last kinks in the process to getting everything working under
windows 11. Primarily on how to obtain the 64 bit driver of the scanner, which doesn't 
actually ship with the last version of the FlexColor software compatible with this scanner.

One thing I immediately noticed was that the focus had shifted, and that there was a single
pixel line that didn't respond as expected. I don't have the original focus and CCD calibration
sheets, so thats what brings us here. The fact that the software is ancient, doesn't support 
quality of life features one might expect from modern vuescan compatible scanners, doesn't have
profiles for modern film stocks producted after the turn of the millenium, can't automatically
adjust focus without a target, can't interpolate a stuck pixel on the CCD and a whole lot more 
nitpicks I have, made me wonder how complicated the whole protocol is, and if something like
this would be possible to reverse engineer, and build a custsom, more modern application around.




Independent, interoperability-driven work to run an **Imacon / Hasselblad
Flextight II (Precision II)** film scanner without the discontinued
FlexColor software — first on Windows, then on Linux.

## Status

- `tools/poc/` — `ftpoc`, a Windows console tool that drives the scanner
  directly (preview, crops at any resolution up to 6300 ppi, 3f strips),
  and `ftraw.py`, which turns its output into images. Its README and
  comments are, for now, the protocol documentation: command set,
  firmware upload, scan descriptor, per-scan sequence and the host-side
  image corrections.
- `tools/ftdev/` — `ftdev`, which develops an `ftpoc` scan on any OS:
  dark/white correction, the scanner's Faul Pixs table plus your own
  defect map (`-d`), RGB realignment and the job's crop, then one file
  per selected format (`-f dng,png,positive`). DNG is linear 16-bit
  LinearRaw for darktable, RawTherapee or Lightroom. `positive` renders
  the frame (`--film neg|slide|bw`; `libft/ft_render.h`): film base from
  the preview, automatic range and exposure, tint pairs in CC, paper
  grade and curve presets, as a 16-bit sRGB PNG. Formats are entries in
  `libft/ft_out.c`; FlexColor 3F (.fff) output is planned there as a
  legacy option. Scan to share (`ftpoc frames --share`, then
  `ftdev cut` and `ftdev export`): a low-magnification scan per window of
  frames, a working DNG per frame, and a 2000 px JPEG in sRGB, Display P3
  or Adobe RGB with soft gamut compression and an embedded ICC profile
  (`libft/ft_output.h`; `export` needs libjpeg-turbo). Export reduces
  sensor noise from the scan's own calibration (`--nr-sensor`, the DNG's
  NoiseProfile tag; `libft/ft_nr.h`), film grain on request
  (`--nr-grain`) and sharpens after the resize (`--sharpen`;
  `libft/ft_sharpen.h`).
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
yourself.[^firmware] Test fixtures are synthetic.

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
power-cycled.

[^firmware]: An eventual goal is an independent reimplementation of the
    scanner firmware, so that no vendor code is needed at all.
