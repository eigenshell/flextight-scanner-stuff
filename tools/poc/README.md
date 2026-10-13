# ftpoc — FlexTight II proof-of-concept

Standalone Win32 console tool that talks to the scanner **without
FlexColor**, through the same `\\.\ScannerN` + 44-byte SRB +
`DeviceIoControl(0x190012)` transport FlexColor uses, validated against
hundreds of thousands of traced calls between FlexColor and our scanner.

## Why this exists

It's the de-risking step before the Linux SG_IO driver: if a single-file
C program can move the holder and pull pixels on Windows with the protocol
spec alone, the spec is right and the driver is just a port. If not, we
learn what the spec got wrong — cheaply.

## Usage (on the scanner box, FlexColor CLOSED — the driver opens exclusively)

```
build.bat            REM cl or i686-w64-mingw32-gcc, see header

ftpoc inquiry        identity; "SCSI Loader" means run `upload` first
ftpoc poll [n]       ScannerReady polls (00 00 idle, 08 00 motor, 04 00 busy)
ftpoc win            one SetWindow exchange with the idle template, hexdump
ftpoc park           rest: FlexColor's shutdown descriptor
ftpoc goto <ppi*1e4> idle template with the resolution field set, 0..63000000
ftpoc move <+/-d> [n]  n relative resolution steps, max 5000000/step, 50 steps
ftpoc stop           end-of-scan descriptor x2 (FlexColor's end/Stop)
ftpoc pages          the standard ReadBuffer pages (Faul Pixs map etc.)
ftpoc grab <n> [len] n ReadExt blocks (default 3996 B) -> grab.raw
ftpoc scan [preset] [-n lines] [-y line] [-f frame] [-h s] [-o file]
                     presets (captured, byte-exact):
                       preview  525 ppi, whole holder, 666 px x 4795 lines
                       2000     2000 ppi crop, 1818 px x 2807 lines
                       1000     1000 ppi crop, 1142 px x 1490 lines
                       3f       630 ppi frame, 655 px x 992 lines
                     synthesized whole-holder previews (same window as
                     preview, the 6000 crop optics, fast):
                       preview1000  1000 ppi, bin 6, 1269 px x 9133 lines
                       preview1500  1500 ppi, bin 4, 1903 px x 13700 lines
                       they take only -n, -h, -l and -o
                     -n  stop after n lines (FlexColor's Stop: END x2)
                     -y  start line along the strip (holder-clamped);
                         e.g. `scan 1000 -y 4600` = one frame earlier
                     -f  3f frame 1-6; 0 = whole strip -> frame1..6.raw
                     -h  hold before calibration (default 10 s; 35 cold;
                         the first scan of a run holds at least 10 s)
                     -p  <ccd>,<lens> positions (@56/@60): they set
                         magnification and focus; there is no exposure field
                     -l  <0..255> lamp level (@76): 0 keeps the current
                         level, 1..95 dims, 96..255 = full; it sticks
                     writes <file> (lines), <file>.cal (250 calibration
                     lines), <file>.faul (Faul Pixs table), <file>.txt
ftpoc crop <ppi> <x> <y> <w> <h> [-b bin] [-n lines] [-h s] [-p ccd,lens]
           [-l lamp] [-o file]
                     SYNTHESIZED window, mm from the holder origin, at
                     525..6300 ppi. bin = -b, or by default the largest
                     divisor of 12 with ppi x bin <= 6300 (the fastest;
                     see "Optics per frame" below); that optical
                     scale sets the lens/CCD pair from a thin-lens focus
                     model fitted to this scanner (sharper than
                     FlexColor's own pairs here).
                     e.g. café frame centre at 3150:
                       crop 3150 13.0 130.4 7.6 11.4
                     6300 sliver across the frame:
                       crop 6300 4.8 135.65 24 0.9
ftpoc upload MICROCOD.3XX   loader-mode firmware recovery only: refused
                     unless INQUIRY says "SCSI Loader"; sends FlexColor's
                     packets (libft/ft_upload.h). Once per power-up.
ftpoc scanstop       0xdb (kHwiScanStop) — device rejects it (CHECK COND.)
ftpoc busreset       IOCTL_STORAGE_RESET_BUS on the Imacon's HBA port only;
                     does NOT clear the scanner's scan state
```

### Smart scanning

```
ftpoc frames [--film neg|slide] [--ppi 2000] [--sprockets yes|no]
             [--preview standard|1000|1500] [-h s] [-o strip]
ftpoc scan --job strip.job [-h s]
ftpoc calibrate-focus <x_mm> <y_mm> <w_mm> [-h s]
```

`frames` scans a preview, finds every frame (full, half, panorama,
partial; blank frames dropped and counted, unexposed film at the strip
ends reported apart), checks focus at the first, middle and last frame,
and writes `strip.job` plus `strip.png` (numbered boxes; dim yellow =
excluded, orange = low confidence). Edit `strip.job` — `include`,
`ppi`, `sprockets`, `straighten`, `window` per frame — then run
`scan --job`. Detected windows are clamped to the holder's crop limits
(32 mm across, 231 mm along; the preview itself reaches 0.92..33.14 mm
across and ~232 mm along); if the job would still be refused, `frames`
warns per frame and writes it anyway.

`--preview` picks the preview (`scan preview`, `scan preview1000`,
`scan preview1500` take it alone). Previews are for framing: all three
use the fast, high-magnification optics.

| mode | ppi | optics / bin | time (strip) |
|---|---|---|---|
| standard | 525 | 6300 / 12 (FlexColor's) | ~40 s |
| 1000 | 1000 | 6000 / 6 | ~2.7 min |
| 1500 | 1500 | 6000 / 4 | ~5.7 min |

`strip.raw` keeps the full-resolution preview (render it with
`ftraw.py`); detection and `strip.png` use a copy box-averaged to
~500 ppi.

#### Optics per frame (the job's `bin` key, `crop -b`)

Any bin dividing 12 whose optics (ppi x bin) lie in 1500..6300 gives
the same output ppi. The trade-off, measured 2026-10-13:

- **Time follows bin**: the stock firmware costs ~84k output px/s for
  the full CCD line (7992 / bin px) whatever the window, so bin 1 is
  ~2x slower than bin 2 and ~12x slower than bin 12 at the same lines.
- **Light follows magnification**: a smaller bin is a lower
  magnification m, ~1/(1+m)^2 more light on every sample. The firmware
  does not average a bin (it keeps one CCD pixel), so a small bin also
  aliases less. Per-read Dmax (R): ~3.0 at 6300 optics, 3.60 at 2000,
  3.58 at 1500 (white near the ADC top there; ~3.73 is the ceiling).

`frames` prints every option per included frame (time, light per
sample, Dmax estimate) and totals; the job file lists them under each
frame. `bin = 0` (the default) is the fastest. Across the film the
low-magnification optics are mapped with a measured correction (below
scale 3150; libft/ft_geom.c), so crops land where intended.

Job file keys (`key = value`, `#` starts a comment anywhere):

```
[strip]
holder        holder geometry name (24x217)
film          negative | slide
focus         shifted (one offset for the strip) | per-frame (bracket each frame)
focus_offset  steps added to both CCD and lens on the focus curve
              (tandem; shifted mode; the per-frame brackets' default centre)
[frame N]
type          full | half | panorama | partial (informational)
include       yes | no: scan this frame or not
ppi           scan resolution: 525..6300
bin           CCD px per output px: 0 = fastest (default); smaller =
              lower magnification, more light, slower (options listed
              in a comment under each frame)
window        y0 x0 length width, mm from the holder origin (what is scanned)
tilt          strip tilt in degrees (widens the scan window; recorded)
sprockets     yes | no: set by `frames --sprockets`, whose window then
              spans the hole rows; changing it alone does not move the window
straighten    yes | no: recorded in the .txt for post-processing
confidence    high | low: detection confidence (informational)
focus_offset  per-frame mode: the measured offset; 0 = use the strip's
done          yes | no: scanned already (scan --job skips it; set no to redo)
```

`scan --job` validates the job and runs every remaining frame's scan
window through the crop checks before the scanner does anything; a
refused frame is listed then, skipped, left `done = no`, and makes the
exit status non-zero (the other frames are still scanned). Each frame
is scanned slightly wider than its window (tilt + 0.3 mm, clipped to the
holder and to what the CCD reaches at that ppi) into
`<jobbase>_frameNN.raw`, where `<jobbase>` is the job path without its
extension (`neg.job` -> `neg_frame01.raw`, `neg_frame02.raw`, ...), so
scans of different jobs never overwrite each other (+ `.cal`, `.faul`,
`.txt`); the tight crop is appended to the matching `.raw.txt`
(`crop_x/y/w/h` in output pixels of the scan as sent,
plus `tilt`, `straighten`, `focus_offset`). `crop_y`/`crop_h` count rows
of the RGB-aligned image (as `ftraw.py` writes it): row r is the scan's
green row r + k, k = 12 / bin lines dropped at each end. The job file is rewritten
after every frame (via `strip.job.tmp`, renamed over it), with finished
frames marked `done = yes`, so a run stopped by Ctrl+C or a failed scan
resumes where it left off.

Hold: the first scan of a run holds at least 10 s before its
calibration, whatever was asked, except that `-h` on `frames`,
`scan --job` and `calibrate-focus` sets that first hold explicitly
(0..120 s, e.g. `-h 35` after a cold power-up, `-h 0` when the lamp is
already warm). Their later scans hold 3 s.

Ctrl+C: scans (`scan`, `crop`, `frames`, `scan --job`,
`calibrate-focus`) stop at the next line read and send END; `park` and
`goto` finish their current move (Ctrl+C is held until then); `move`
stops before its next step. No new scan or move starts afterwards.
Other commands are stopped by Ctrl+C at once, as before.

Focus: the curve comes from `focus.txt` in the current directory
(written there by `calibrate-focus` on a textured window) or the
built-in curve for the development scanner. Focus offsets move the CCD
and the lens together (tandem, as the manual's manual focus does): the
gap between them, and so the magnification, stays, and only the focus
plane on the film side moves. Moving the CCD alone also changes the
magnification (~1 % for 9000 steps at 6300 ppi) and is ~5x less
sensitive. The check points and the per-frame brackets step 500 and stay
within ±4000 of the curve. If the three check points agree within ±300
steps of their mean the curve is shifted by the mean; otherwise every
frame gets its own focus bracket before it is
scanned, centred on its `focus_offset` (the strip's when 0); a frame
with no usable peak is scanned at that centre, with a warning. Focus
crops go to the scratch files `fb.raw`, `fb.raw.cal`, `fb.raw.faul`
and `fb.raw.txt`, overwritten by each crop.

`calibrate-focus` (which finds the curve itself, so it is the one place
that moves the CCD alone) brackets the CCD, lens on the current curve, at eight
optical scales (3800..6300) on a 1 mm band at `y` across `x..x+w`
(clipped to the CCD's reach): the first usable scale gets a coarse
bracket (5 crops, 3000 steps, moving up to ±12000 if the peak is at an
edge), then each scale a fine one (9 crops, 1000 steps) around the
coarse peak or the previous scale's. Scales without a usable peak are
skipped. The magnification at each peak follows from the current curve
(moving the CCD by d shortens the image distance by d), and the
thin-lens curve is fitted to the peaks. It writes `focus.txt` only with
at least six usable scales, rms residuals of at most 600 CCD and 300
lens steps, every point within ±1200 CCD steps of the fit, and a curve
that keeps every measured scale inside FlexColor's positions. Brackets
never leave ±12000 steps of the current curve; a peak further off is
skipped like any other unusable scale.

## Device selection

The scanner answers on all 8 LUNs of its SCSI target, so Windows creates
several `\\.\ScannerN`; only one is live (on the session-4 box:
`Scanner4`). The rest fail every command with SRB status 0x13
(UNEXPECTED_BUS_FREE) → ERROR_IO_DEVICE 1117. `ftpoc` probes each and
uses the one whose INQUIRY says Imacon — the same thing FlexColor's
FindDevices does, and the source of its "first INQUIRYs fail" pattern.

## Safety model

- Every descriptor is one FlexColor sent, byte for byte, except for
  these fields: the resolution (BE u32 @10, `goto`/`move` on the idle
  template), the start line (BE u32 @18, `scan -y` — FlexColor's six 3f
  frames differ only in that field), the window of a synthesized `crop`,
  the CCD/lens positions (BE u32 @56/@60, `-p`; `crop` computes them
  from its focus model) and the lamp level (@76, `-l`). Each stays
  within the values FlexColor sent, and the CCD always stays below the
  lens.
- `frames`, `scan --job` and `calibrate-focus` force the lamp level to
  255 (full) on every scan: the level persists in the scanner, so a
  dimmed level left by an earlier `-l` must not leak into them.
- Resolution is limited to FlexColor's range (0..63000000 = 6300 ppi); the
  start line to the holder length at that resolution.
- Every command has a 5 s timeout (overlapped I/O + `CancelIo`), matching
  FlexColor's own 5 s wait.
- `maint` sends only the whitelisted packets described below.

## Maintenance writes and memory reads (`maint`)

The uploaded microcode treats WRITE BUFFER (`3b`) data as a maintenance
packet: a 10-byte header (`[0]` command, `[1..2]` and `[3..4]` big-endian
words W1/W2, `[9]` flags) followed by a payload. Command `01` writes the
payload to memory W1:W2, or with flags bit 0 to I/O port W1 (bit 2 keeps
the port instead of incrementing it). The device copies length − 10
bytes without checking, acts only once idle, and reports good status
before it acts. READ BUFFER (`3c`) is a memory read: segment in CDB
bytes 2–3, offset in 4–5, length in 6–8 (so FlexColor's Faul Pixs "page
e0 @ 0x400" is memory `E000:0400`).

`ftpoc maint` (built on `libft/ft_maint.c`) can send only:

    ftpoc maint io 0x342 <byte> --confirm        FPGA mode register
    ftpoc maint io 0x344 <byte> --confirm        FPGA register, meaning under test
    ftpoc maint mem 4000:<off> <hex> --confirm   timing table 4000:7640..76BB
    ftpoc maint peek 3000:<off> <len>            microcode, len <= 0x400
    ftpoc maint peek 4000:<off> <len>            data segment, len <= 0x400

Every other command code is refused: among them `00` (jump), `02` (FPGA
configuration) and the flash commands `10`/`20`/`30`/`50`. Flash writes
are the only permanent change the protocol allows. So are every other
port (the motor DACs, the output latch, the SCSI chip, the 386EX
peripherals and the FPGA control register `0x340`) and every other
memory range. Each `maint` call checks that the scanner is idle first and
polls until it is idle again.

Everything `maint` can change lives in RAM: power-cycling the scanner
(≥ 10 s off) returns it to the loader, and `ftpoc upload MICROCOD.3XX`
restores the stock microcode. After any write, read it back with
`maint peek` and run a short preview before going further. Stop and
power off on grinding motors, an unexpected eject, a lamp change, an
unknown poll status or a scan that hangs for more than 30 s.

## Templates

All SetWindow templates are generated byte-for-byte from the session-4
sidecar (`flextrace_bufs.bin`, cand 0x60, when=0) and each carries a
compile-time size check. An earlier hand-typed copy dropped bytes and C
zero-padded the `[84]` array, which shifted every field: the scan
descriptor sent that way put the device into a scan it never finished
(df stuck at `0800`). Never hand-edit; regenerate.

`SCAN_WIN`/`END_WIN` come from session 5 (pid 412: a full preview and a
preview stopped halfway — both end with END ×2). The preview sequence
and its timing are documented in `cmd_scan` in ftpoc.c.
Seq 41–278 are *not* preview — they are the Position dialog's
"CCD & Optic" button (UI timeline), a service calibration that `ftpoc`
deliberately does not replay.

## Images: `ftraw.py`

```
python3 ftraw.py scan.raw --negative      # -> scan.png
python3 ftraw.py scan.raw --raw           # uncorrected, shows the lines
```

`<file>` is one block per line = px × RGB, u16 LE, linear. The scanner
does **not** correct its CCD: before every scan it sends 200 dark lines
and 50 white (open beam) lines, and the host corrects
`(raw − dark) / (white − dark)` per CCD pixel — FlexColor's
`CCalibrateLineHandler`. Without it, pixel gain differences show as fine
lines along the strip. Pixels in the device's Faul Pixs table flicker
line to line and are interpolated instead (output pixel j covers CCD
pixels `(x + j) × bin + 144 …`, `bin` = descriptor byte 51). The
tri-linear CCD's R and B rows are realigned to G by 12 / bin lines. `--negative`
is a rough inversion, not FlexColor's film profiles.
