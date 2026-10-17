# ftwebd — the scanner box's web server

One page for a strip in the holder: preview, tap frames to `—` / `4MP` /
`6MP`, scan with a progress bar and Stop, download the window raws. The
server runs whitelisted `ftpoc` tasks (`frames --share 2450 --progress`,
`scan --job ... --progress`) and streams their output; the client never
sends a command line.

## Run it (Windows scanner box)

```
build.bat                      REM winlibs mingw (posix threads) on PATH
copy ftwebd.exe, ..\poc\ftpoc.exe and www\index.html into one folder:
  web\ftwebd.exe  web\ftpoc.exe  web\www\index.html
cd web
ftwebd                         REM --port 8080 --strips strips --www www --ftpoc ftpoc.exe
```

Then open `http://<box>:8080/` from a phone or PC on the LAN (Windows
Firewall: allow TCP 8080 from the local subnet). Start it from a console
window (Stop sends Ctrl+Break to ftpoc's process group, which needs one),
from ftpoc's folder: ftpoc reads `focus.txt` there, and so does the
planner. After a power-up run `ftpoc upload MICROCOD.3XX` first.

Strips live in `strips\<name>\`: `strip.raw` + `strip.png` + `strip.job`
from the preview, `strip.preview.job` (the preview's own job: every
re-plan starts from it), and `strip_winNN.raw` per scanned window.

## Not yet (web UI plans 2-3)

Share JPEGs and C/M/Y/D printing in the browser (the wasm engine), the
`.print` file and emulsion menu, ARC (detail) frames, byte ranges. Until
then, per window: `ftdev cut strip_winNN.raw` and `ftdev print` on a PC.
The share-size planner runs in ftwebd (libft's pure planner) until the
page does it in wasm.

## Linux

`make` builds `ftwebd` (POSIX); `tests/test_ftwebd.py` drives it against
`tests/fixtures/ftwebd/fake_ftpoc.py`, which also gives a local demo:

```
ftwebd --ftpoc tests/fixtures/ftwebd/fake_ftpoc.py --www tools/ftwebd/www
```
