# ftpoc — FlexTight II proof-of-concept

Standalone Win32 console tool that talks to the scanner **without
FlexColor**, through the same `\\.\ScannerN` + 44-byte SRB +
`DeviceIoControl(0x190012)` transport FlexColor uses (reversed from
`FlexColor.dll` and validated against 67k+ hooked calls — see
`docs/protocol.md`, `docs/recon/static-decode.md`).

## Why this exists

It's the de-risking step before the Linux SG_IO driver: if a 300-line C
program can move the holder and pull pixels on Windows with the protocol
spec alone, the spec is right and the driver is just a port. If not, we
learn what the spec got wrong — cheaply.

## Usage (on the scanner box, FlexColor CLOSED — the driver opens exclusively)

```
build.bat            REM cl or i686-w64-mingw32-gcc, see header

ftpoc inquiry        identity; "SCSI Loader" means run `upload` first
ftpoc poll [n]       ScannerReady polls (status vocabulary in protocol.md)
ftpoc win            one SetWindow exchange with the idle template, hexdump
ftpoc park           carriage to the rest position (0x35b70c)
ftpoc move <+/-us> [n]   n relative moves, max 200000 us/step, 50 steps;
                         always parks first (known reference), polls idle
                         between steps
ftpoc pages          the standard ReadBuffer pages (FaulPixs map etc.)
ftpoc grab <n> [len] n ReadExt blocks (default 3996 B) -> grab.raw
ftpoc scan [n]       stretch goal: replay captured scan-arm descriptor,
                     250 calibration reads, poll for 0x1060, grab n blocks
                     -> scan.raw, then park
ftpoc upload MICROCOD.3XX   loader-mode firmware recovery only
```

Suggested first session, in order: `inquiry`, `poll 10`, `win`, `park`,
`move 5000`, `pages`, `grab 10`, and only then `scan`.

## Safety model

- The only motion primitive is the SetWindow position field (u32 LE @byte
  11) patched into the captured idle descriptor; every other byte is what
  FlexColor sent on the wire.
- Every `move` run starts by parking (0x35b70c was the observed rest
  position), so absolute position is never guessed.
- Per-step delta is clamped to ±200000 µsteps (observed full travel is
  ~9M), steps to 50, and every step waits for df == `0000`.
- Ctrl+C mid-move only abandons the console; the device finishes the
  current small step to a commanded position.
- Every command except `inquiry` first retries INQUIRY (1 s apart, up to
  20 s) until the device answers: right after the driver opens, the
  scanner fails I/O with ERROR_IO_DEVICE for ~1 s (FlexColor's own first
  7 INQUIRYs fail the same way, session 4).
- Every command has a 5 s timeout (overlapped I/O + `CancelIo`), matching
  FlexColor's own 5 s wait.

## Templates

`IDLE_WIN` / `SCAN_WIN` in ftpoc.c are pinned verbatim from the session-4
sidecar (`flextrace_bufs.bin`, candidate 0x60). `SCAN_WIN` is the last
SetWindow before the first `df -> 0x1060` (seq 292). If a template is
suspect, re-derive from the captures — do not hand-fix bytes.
