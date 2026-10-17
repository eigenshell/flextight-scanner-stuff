# ftwebd — the scanner box's web server

One page for a strip in the holder: preview, tap frames to `—` / `4MP` /
`6MP` / `ARC`, pick the emulsion, scan with a progress bar and Stop, then
print in the browser: a grid of the strip's frames, the selected one
large, C/M/Y/D clicks, Download (JPEG + its `.txt`) and All (zip).

The box does no image or planning work. The page does, in C compiled to
wasm (`tools/ftweb`, `ftweb.wasm` in a worker): it plans the share
windows, cuts each window scan as it lands, measures the strip's colour
balance and prints, with the same bytes as `ftdev cut` + `ftdev print
--roll`. The server runs whitelisted `ftpoc` tasks and streams their
output (the client never sends a command line), and keeps the strip's
files, checking `strip.job` and `strip.print` with libft before it writes
them.

## Run it (Windows scanner box)

```
build.bat                      REM winlibs mingw (posix threads) on PATH
REM on a PC with the wasi-sdk: make -C tools/ftweb www  -> www\ftweb.wasm
copy ftwebd.exe, ..\poc\ftpoc.exe and www\ (index.html, print.js,
ftweb.js, ftweb.wasm) into one folder:
  web\ftwebd.exe  web\ftpoc.exe  web\www\...
cd web
ftwebd --archive D:\scans       REM --port 8080 --strips strips --www www
                                REM --ftpoc ftpoc.exe --archive-ppi 6300
```

Then open `http://<box>:8080/` from a phone or PC on the LAN (Windows
Firewall: allow TCP 8080 from the local subnet). Start it from a console
window (Stop sends Ctrl+Break to ftpoc's process group, which needs one),
from ftpoc's folder: ftpoc reads `focus.txt` there, and the page's
planner gets it from ftwebd. After a power-up run `ftpoc upload
MICROCOD.3XX` first.

Strips live in `strips\<name>\`: `strip.raw` + `strip.png` + `strip.job`
from the preview, `strip.preview.job` (the preview's own job: every plan
starts from it), `strip_winNN.raw` (+ `.cal`, `.txt`) per scanned window,
and `strip.print` (emulsion and clicks). Reloading the page restores the
strip from these files (the windows are cut again in the browser).

## Scan

Scan runs up to three steps in one task, each after the last one ends
well: the share windows (`ftpoc scan --job strip.job`), then the `ARC`
frames as a detailed scan of their own into `<archive>\<strip>\`
(`ftpoc frames --detail neg --ppi 6300 --select ...`, then `ftpoc scan
--job` on that job: its own preview, focus per frame, DNGs). Without
`--archive`, `ARC` is refused. Stop ends the step that runs and the
chain. Which frames are `ARC` is kept in the browser only.

## The page's files

`index.html` (< 14 KB: the shell renders from the first packet),
`print.js` (the print stage, deferred), `ftweb.js` (the worker) and
`ftweb.wasm` (~210 KB, ~90 KB gzip). They are sent with an ETag and
`no-cache`: the browser keeps them and asks only whether they changed.

## Not yet

Byte ranges; pushing JPEGs to the phone (Web Share) or a NAS; the `ARC`
choice is not in the box's files.

## Linux

`make` builds `ftwebd` (POSIX); `tests/test_ftwebd.py` drives it against
`tests/fixtures/ftwebd/fake_ftpoc.py`, and `tests/test_page.py` drives
the page in a headless Chromium. A local demo with real (synthetic)
pixels:

```
make -C tools/ftweb www
make -C libft test/window_dump && libft/test/window_dump /tmp/win
FAKE_FTPOC_WINDOW=/tmp/win ftwebd --ftpoc tests/fixtures/ftwebd/fake_ftpoc.py \
    --www tools/ftwebd/www --archive /tmp/archive
```
