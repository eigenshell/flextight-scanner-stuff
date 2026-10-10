# spti_logger — FlexColor 4.0.3 DeviceIoControl transport trace

In-process inline hook on `kernel32!DeviceIoControl`, injected into
`FlexColor.exe` at launch, logging every call (including Imacon's private
`IOCTL_SCSISCAN_CMD` — its numeric value is recovered from the trace) as JSONL.
Rationale and transport evidence: `docs/recon/transport.md`. Statically
recovered CDB opcodes (0xD0..0xE3): `docs/recon/static-recon.md`.

## Files

| File | Role |
|---|---|
| `fmt.h` / `fmt.c` | platform-neutral JSONL record formatter (pure C90, no libc beyond `<stddef.h>`) — the testable seam |
| `hook.c` | the logger DLL (`ft_hook.dll`), Windows-only |
| `inject.c` | the launcher (`inject.exe`), Windows-only console exe |
| `test_fmt.c` | native unit test for the formatter (runs on Linux) |
| `build.bat` | Windows-side build fallback (cl, or mingw commands in the header) |
| `test/win32stub/windows.h` | minimal Win32 stub used ONLY for offline `gcc -fsyntax-only` checks of hook.c/inject.c (no mingw toolchain on the dev box) |

## Record format (trace parser contract)

One compact JSON object per line, lowercase hex, no spaces:

```
{"h":"0x12","code":"0x2a002c","in_len":36,"in_head":"12000000240000","ok":1,"out_len":36,"out_head":"..."}
```

Fields: `h` (handle, hex), `code` (IOCTL code, hex — **the unknown
IOCTL_SCSISCAN_CMD value shows up here**), `in_len` (decimal), `in_head`
(lowercase hex of up to the first 64 bytes of the input buffer, `""` if none),
`ok` (1 on return != 0), `out_len` (bytes returned), `out_head` (up to 64 bytes
of output buffer, `""` unless ok). A `{"tag":"hook_loaded",...}` line is
written when the DLL loads.

## Usage on the tracing rig

1. Build **32-bit** binaries (`i686-w64-mingw32-gcc` or `build.bat` on
   Windows; BOTH must be 32-bit — FlexColor.exe is PE32 i386):
   ```
   i686-w64-mingw32-gcc -shared -O2 -Wall -Wextra -o ft_hook.dll hook.c fmt.c
   i686-w64-mingw32-gcc -O2 -Wall -Wextra -o inject.exe inject.c
   ```
2. Place `ft_hook.dll` **next to** `inject.exe` (inject derives the DLL path
   from its own location).
3. Run: `inject.exe "C:\Program Files\Imacon\FlexColor\FlexColor.exe"`
   FlexColor starts suspended, the hook installs, the app resumes; inject's
   console stays attached until FlexColor exits and propagates its exit code.
4. Traces land in `C:\flextrace_ioctl.jsonl` (append, flushed per record).
5. **First capture goal:** the scanner-handle calls (opened on `\\.\Scanner%d`)
   dominate and repeat the same `code` on every CDB exchange — that hex value
   IS `IOCTL_SCSISCAN_CMD`. Then diff `in_head` CDB bytes against the 0xD0..0xE3
   opcodes from static recon.

## Offline build/test (Linux dev box)

```
gcc -Wall -Wextra -O1 -o /tmp/test_fmt test_fmt.c fmt.c && /tmp/test_fmt
gcc -fsyntax-only -Wall -Wextra -I test/win32stub hook.c inject.c
```

No mingw cross toolchain exists on the dev box (and no sudo to install one),
so hook.c/inject.c are syntax-checked against `test/win32stub/windows.h`; the
real build must run with `i686-w64-mingw32-gcc` here or `build.bat` on the
Windows rig. Do NOT substitute `x86_64-w64-mingw32-gcc` — 64-bit binaries are
useless against a 32-bit target process.

## Design notes / known concerns (deliberate, not silently redesigned)

- **LoadLibraryA, not W.** An early design sketch said to resolve
  `LoadLibraryW` while writing an ANSI path buffer — that combination would
  misread the path. inject.c resolves `LoadLibraryA` to match the ANSI buffer.
- **5-byte E9 hook on a modern export.** On Win7+ `kernel32!DeviceIoControl`
  is a jmp thunk into kernelbase.dll. Hooking the kernel32 entry is correct for
  callers importing kernel32 (FlexColor.exe and FlexColor.dll both do); a
  module calling kernelbase directly would bypass the hook (none in FlexColor).
  On era hardware (Win2K/XP) kernel32 is the real implementation.
- **Install race.** No lock around installation; safe here because the DLL is
  injected into a suspended process and installs the hook in `DllMain` before
  app threads run.
- **CRT inside the hook.** Logging uses fopen/fwrite/fflush per record; the CRT
  does not call DeviceIoControl, and a depth guard prevents recursion.
- **AV/EDR** on modern Windows may flag inline hooking of system DLLs; prefer
  the era-appropriate rig (WinXP/2K x86).
- **Unbounded trace growth** during long sessions; rotate
  `C:\flextrace_ioctl.jsonl` between runs.
- If `CreateRemoteThread` is ever blocked by exploit mitigations, the fallback
  is to hijack the suspended main thread's context to run `LoadLibraryA`
  (documented here; not implemented).
