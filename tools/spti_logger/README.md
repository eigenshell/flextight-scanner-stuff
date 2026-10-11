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
| `uiev.c` | UI event timeline logger (`uiev.exe`), Windows-only; system-wide LL input hooks, same `t` clock as the ioctl records |
| `sidecar_corr.py` | correlate `flextrace_bufs.bin` payloads with JSONL records by (pid, seq) |
| `merge_trace.py` | interleave ioctl + UI-event timelines on the shared GetTickCount clock |
| `test_fmt.c` | native unit test for the formatter (runs on Linux) |
| `build.bat` | Windows-side build fallback (cl, or mingw commands in the header) |
| `test/win32stub/windows.h` | minimal Win32 stub used ONLY for offline `gcc -fsyntax-only` checks of hook.c/inject.c (no mingw toolchain on the dev box) |

## Record format (trace parser contract)

One compact JSON object per line, lowercase hex, no spaces:

```
{"h":"0x12","seq":0,"t":61234,"code":"0x2a002c","in_len":36,"in_head":"12000000240000","ok":1,"out_len":36,"out_head":"..."}
```

Fields: `h` (handle, hex), `seq` (per-process call counter, decimal — keys the
payload sidecar), `t` (**GetTickCount() ms — the same system-wide clock
uiev.exe stamps UI events with; merge the two files on this field**),
`code` (IOCTL code, hex — **the unknown
IOCTL_SCSISCAN_CMD value shows up here**), `in_len` (decimal), `in_head`
(lowercase hex of up to the first 64 bytes of the input buffer, `""` if none),
`ok` (1 on return != 0), `out_len` (bytes returned), `out_head` (up to 64 bytes
of output buffer, `""` unless ok). A `{"tag":"hook_loaded",...}` line marks a
successfully installed hook; `{"tag":"install_failed",...}` one that refused to
install.

### Notes for the trace parser (Task 7 contract)

- **Foreign-schema lines share the file.** Control records
  (`{"tag":"hook_loaded",...}` / `{"tag":"install_failed","reason":...}`) are
  written with the same one-object-per-line framing but a DIFFERENT schema —
  parsers must skip any line that does not match the ioctl-record schema
  rather than fail on it.
- **Overlapped (async) calls are a known asymmetry.** A call issued with an
  `LPOVERLAPPED` completes later via the overlapped structure; the hook logs
  it at call time as `ok:0,out_len:0,out_head:""` (DeviceIoControl returns 0
  for pending overlapped I/O) and there is NO completion record when the I/O
  actually finishes. FlexColor's scanner traffic is synchronous, so this
  should rarely matter, but parsers must not treat `ok:0` as proof of a failed
  command without checking for an async pattern.
- **Payloads live in the sidecar, not the record.** Command payloads (df/e0
  writes, 24/3c/INQUIRY responses) are dumped into `flextrace_bufs.bin` keyed
  by `(pid, seq)`; see "Payload sidecar" below.

## Usage on the tracing rig

1. Build **32-bit** binaries (`i686-w64-mingw32-gcc` or `build.bat` on
   Windows; BOTH must be 32-bit — FlexColor.exe is PE32 i386):
   ```
   i686-w64-mingw32-gcc -shared -O2 -Wall -Wextra -static-libgcc -o ft_hook.dll hook.c fmt.c
   i686-w64-mingw32-gcc -O2 -Wall -Wextra -static-libgcc -o inject.exe inject.c
   ```
   `-static-libgcc` is REQUIRED, not an optimization: the default links the
   shared DWARF-unwind runtime (`libgcc_s_dw2-1.dll`), which inject.exe then
   fails to find at launch, and which ft_hook.dll would have to resolve from
   inside FlexColor's process (its search path, not yours).
2. Place `ft_hook.dll` **next to** `inject.exe` (inject derives the DLL path
   from its own location).
3. Run: `inject.exe "C:\Program Files (x86)\Hasselblad\FlexColor English v4.0.3\FlexColor.exe"`
   (the actual install path on the 2940AU rig — quotes required, the path has
   spaces; `C:\Program Files\Imacon\...` does NOT exist there).
   FlexColor starts suspended, the hook installs, the app resumes; inject's
   console stays attached until FlexColor exits and propagates its exit code.
4. Traces land in `flextrace_ioctl.jsonl` **next to `ft_hook.dll`** (the path
   is derived from the DLL's own location at load; the old `C:\` root path
   failed silently for non-elevated processes on Vista+ and is gone).
   Append mode, flushed per record. The boot line
   `{"tag":"hook_loaded","status":"ok","prologue":"e9_thunk"|"xp_hotpatch",...}`
   tells you which prologue shape the hook saw — `e9_thunk` means a Win7+
   kernel32→kernelbase jmp-thunk (its rel32 is re-based in the trampoline;
   a verbatim copy dispatches to wild memory and breaks every hooked call).
5. **First capture goal:** the scanner-handle calls (opened on `\\.\Scanner%d`)
   dominate and repeat the same `code` on every CDB exchange — that hex value
   IS `IOCTL_SCSISCAN_CMD`. Then diff `in_head` CDB bytes against the 0xD0..0xE3
   opcodes from static recon.

### Diagnostic aids (added after the first live session)

- **`{"tag":"lasterr",...}` records**: for every failed ioctl (`ok:0`), the
  hook emits a separate control record with the real `GetLastError`,
  captured immediately after the tramp returns (the hook's own CRT logging
  clobbers it afterwards, and the host app's error dialog only shows a
  *stale* value — e.g. 183 from an unrelated earlier call). This is how we
  learned the first Windows rig's failure storm wasn't dispatch corruption.
- **`FT_PASSTHRU=1` environment variable**: installs the hook but forwards
  every call with zero logging work — bisects {patch/dispatch} vs
  {our logging code} when a hooked app misbehaves. If pass-through is clean
  and logging is not, the logging path (timing/memory) is the culprit.
- **`FT_QUIET=1` environment variable**: logs only the two scanner SRB codes
  (0x190012/0x190022), capped at 500 records — the middle rung of the bisect
  ladder (pass-thru → quiet → full). Used on the Adaptec 2940AU rig to prove
  the SRB failure storm was caused by per-record `fopen/fclose` syscalls
  stalling the app's realtime SCSI thread (the driver answered the stalls
  with ERROR_IO_DEVICE, the app retried into a storm). The shipped build
  opens the trace file ONCE at boot for the same reason; per-record
  fopen/fclose is not coming back.

## Captured traces

First live session (2026-10-11, interactive: preview, TIFF scan, 3F scan,
settings changes): distilled command vocabulary, SRB layout, and phase map in
`docs/recon/live-session-trace.md`. The full 73 MB trace stays on the Windows
box next to the tool; rotate it between sessions.

## Payload sidecar (`flextrace_bufs.bin`)

The JSONL record is header-only; payloads live in a second file written next to
it. For every non-`28` scanner SRB, the hook dumps up to four payload candidates
**pre-call** (what the app sent) and **post-call when the call returned success**
(what the device answered; overlapped-pending calls are skipped to avoid racing
in-flight I/O):

| cand | source | finding |
|---|---|---|
| `0x36` | dword at SRB offset 36 | a descriptor `{u32 count=1, ptr}` for read-type CDBs; raw payload for `df` (`01 00` = the poll code) |
| `0x51` | `*(u32*)(cand36 buf + 0)` | one indirection into the descriptor |
| `0x52` | `*(u32*)(cand36 buf + 4)` | the other — one of 51/52 is the real data pointer |
| `0x60` | DeviceIoControl `lpOutBuffer` | the METHOD_OUT_DIRECT carrier; driver copies responses here ("Imacon" confirmed pre-sidecar) |

Record: 12-byte header `<IIBBH` = u32 pid, u32 seq, u8 when (0=pre/1=post),
u8 cand, u16 len, then len payload bytes — little-endian. Correlate to the
JSONL record by `(pid, seq)`; `when` picks sent (0), answered-sync (1), or
**answered-async (2)** — FlexColor issues nearly everything overlapped
(err 997), so the device data for those calls only exists in the `when=2`
late-completion dump (fired at the next scanner-SRB hook, when the app has
waited for the I/O). All pointers are VirtualQuery-guarded (`MEM_COMMIT` +
readable protect); anything doubtful is skipped, never dereferenced blindly.
Per-dump cap 8192 bytes (the startup `3b` microcode chunks are 4106).

## UI event timeline (`flextrace_uiev.jsonl`)

`uiev.exe` answers "which UI action caused which SCSI traffic". It installs
system-wide low-level input hooks (`WH_MOUSE_LL`/`WH_KEYBOARD_LL` — callbacks
run in its own process, so NO injection into FlexColor and no 32-bit
constraint) and logs to `flextrace_uiev.jsonl` next to itself:

- mouse button down/up with the **target resolved at click time**:
  window class, control ID (`GetDlgCtrlID` — stable across runs, mappable to
  names via FlexColor.exe resources), window text (via
  `SendMessageTimeout(WM_GETTEXT)`, 100 ms ABORTIFHUNG — can never hang),
  screen rect, and up to two ancestor texts (the dialog/page chain)
- drag moves while a button is held, throttled to 50 ms (slider trajectories)
- wheel events, and keystrokes **only while FlexColor owns the foreground
  window** (pid cache refreshed at 4 Hz)
- foreground **title changes polled at 4 Hz** — catches progress text
  ("Scanning ... 47%") that changes without any input

Every record carries `t` = `GetTickCount()` — the SAME clock as the ioctl
records' `t` field — so the two files interleave exactly:

```
python3 merge_trace.py <tracedir>               # unified timeline
python3 merge_trace.py <tracedir> --from T --to T   # slice one action
```

Run it any time before or during the session (hooks are system-wide); close it
with the console close button or `taskkill /im uiev.exe`.



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
- **Prologue verification / instruction splitting.** The 5-byte hook only
  steals bytes after verifying them against known-good prologues (XP/2K
  `8B FF 55 8B EC`, or a Win7+ `E9 rel32` jmp-thunk; any `0xE8` call-rel32 is
  rejected). If a future Windows changes the prologue, the hook fails CLOSED
  (no patch, `{"tag":"install_failed","reason":...}` record, DeviceIoControl
  untouched). Blind 5-byte stealing without this check would risk splitting an
  instruction mid-operand and crashing the host.
- **Thread-safe logging.** Each record is emitted as ONE fwrite of
  line+'\n' in append mode (each individual write is atomic at the filesystem
  level) and further serialized by a CRITICAL_SECTION, so concurrent
  DeviceIoControl callers cannot interleave or corrupt JSONL lines.
- **CRT inside the hook.** Logging uses fopen/fwrite/fflush per record; the CRT
  does not call DeviceIoControl, and a depth guard prevents recursion.
- **32-bit guard.** Both hook.c and inject.c carry `#ifdef _WIN64 #error`, so
  an x64 cl/gcc build fails at compile time instead of producing a silently
  unusable 64-bit binary.
- **Remote-thread wait.** inject.c waits INFINITE on the remote LoadLibrary
  thread: the thread always terminates, and under on-access AV scanning the
  first LoadLibrary can exceed any fixed timeout (a timeout used to kill
  FlexColor needlessly).
- **AV/EDR** on modern Windows may flag inline hooking of system DLLs; prefer
  the era-appropriate rig (WinXP/2K x86).
- **Unbounded trace growth** during long sessions; rotate
  `flextrace_ioctl.jsonl` (next to the DLL) between runs.
- If `CreateRemoteThread` is ever blocked by exploit mitigations, the fallback
  is to hijack the suspended main thread's context to run `LoadLibraryA`
  (documented here; not implemented).
