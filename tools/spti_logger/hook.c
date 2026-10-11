/*
 * hook.c - ft_hook.dll: in-process inline hook on kernel32!DeviceIoControl
 * for tracing FlexColor 4.0.3's SCSI traffic (private IOCTL_SCSISCAN_CMD into
 * Imacon's scsiscan.sys) as JSONL.
 *
 * Usage on the tracing rig
 * ------------------------
 *   1. Build ft_hook.dll (32-bit!) and inject.exe, e.g. with mingw:
 *        i686-w64-mingw32-gcc -shared -O2 -Wall -Wextra -o ft_hook.dll hook.c fmt.c
 *        i686-w64-mingw32-gcc -O2 -Wall -Wextra -o inject.exe inject.c
 *      or on Windows with build.bat (Visual Studio cl).
 *   2. Place ft_hook.dll NEXT TO inject.exe (inject derives the DLL path from
 *      its own location).
 *   3. Run:  inject.exe "C:\Program Files\Imacon\FlexColor\FlexColor.exe"
 *      FlexColor starts suspended, the DLL is injected, the hook installed,
 *      then the app resumes.
 *   4. Traces land in flextrace_ioctl.jsonl NEXT TO ft_hook.dll (the trace
 *      path is derived from the DLL's own location at load time; C:\ root is
 *      NOT used because non-elevated processes cannot create files there on
 *      Vista+, and the silent fopen failure cost a whole debugging session).
 *      Appended, flushed per record.
 *      A line {"tag":"hook_loaded","target":"DeviceIoControl"} marks a loaded
 *      hook; one {"h":...,"code":...,"in_len":...,"in_head":...,"ok":...,
 *      "out_len":...,"out_head":...} line per DeviceIoControl call.
 *   5. First capture goal: identify the numeric value of the private
 *      IOCTL_SCSISCAN_CMD control code. ALL DeviceIoControl calls are logged;
 *      the scanner-handle ones (h = handle opened on \\.\Scanner%d) dominate
 *      and repeat the same `code` on every CDB exchange.
 *
 * Hook design notes / caveats
 * ---------------------------
 * - Classic 5-byte E9 inline hook on the kernel32.dll export. FlexColor.exe
 *   and FlexColor.dll import DeviceIoControl from KERNEL32 (see
 *   docs/recon/transport.md), so this catches every call from any module in
 *   the process. On Win7+ kernel32!DeviceIoControl is a small thunk into
 *   kernelbase.dll; hooking the kernel32 entry is still correct for callers
 *   that import kernel32, but a module calling kernelbase directly would
 *   bypass the hook (none do here). On era hardware (Win2K/XP) kernel32 IS
 *   the implementation.
 * - Target is 32-bit code; rel32 offsets computed with
 *   (DWORD)((BYTE*)dst - (BYTE*)src - 5).
 * - Not thread-safe against concurrent first-call installation, but DllMain
 *   installs the hook before the app's threads run (injected into a suspended
 *   process), so there is no race window in practice.
 * - The CRT (fopen/fwrite) is used for logging. It does not call
 *   DeviceIoControl, and a simple depth guard breaks any accidental
 *   re-entry, so the hook cannot recurse into itself.
 * - Antivirus/EDR on modern Windows may flag inline hooking of system DLLs;
 *   prefer the era-appropriate rig (WinXP/2K x86) where this is unremarkable.
 * - Trace file grows unboundedly during long sessions; rotate
 *   flextrace_ioctl.jsonl (next to ft_hook.dll) between runs.
 *
 * Build: i686-w64-mingw32-gcc -shared -O2 -Wall -Wextra -static-libgcc -o ft_hook.dll hook.c fmt.c
 */
#include <windows.h>
#include <stdio.h>
#include "fmt.h"

#ifdef _WIN64
#error "ft_hook must be built 32-bit (FlexColor is a 32-bit app)"
#endif

#define FT_LOG_NAME  "flextrace_ioctl.jsonl"
#define FT_TRAMP_SZ  64

typedef BOOL (WINAPI *DeviceIoControl_fn)(HANDLE hDevice, DWORD dwIoControlCode,
                                          LPVOID lpInBuffer, DWORD nInBufferSize,
                                          LPVOID lpOutBuffer, DWORD nOutBufferSize,
                                          LPDWORD lpBytesReturned,
                                          LPOVERLAPPED lpOverlapped);

static DeviceIoControl_fn g_tramp;        /* RWX trampoline: saved 5 bytes + jmp back */
static LPVOID g_tramp_mem;                /* VirtualAlloc'd, PAGE_EXECUTE_READWRITE */
static int  g_installed;                  /* hook installed flag */
static int  g_inside;                     /* recursion guard */
static int  g_passthru;                   /* FT_PASSTHRU=1: hook installed, zero logging (bisect tool) */
static int  g_quiet;                      /* FT_QUIET=1: log only scanner SRB codes (bisect tool) */
static long g_quiet_logged;               /* quiet-mode record cap counter */
static CRITICAL_SECTION g_log_cs;         /* serializes trace-file writes */
static char g_log_path[MAX_PATH] = FT_LOG_NAME; /* resolved to the DLL's dir at load */
static FILE *g_log_file;                /* opened ONCE at boot; kept open
                                           (per-record fopen/fclose was 2+
                                           syscalls per ioctl and stalled the
                                           app's realtime SCSI thread — the
                                           driver answered the stalls with
                                           ERROR_IO_DEVICE (1117), which the
                                           app retried in a storm) */

/* FT_QUIET record cap: the SRB retry storm can hit thousands of identical
 * failures; beyond this many quiet-mode records the picture cannot get
 * clearer and the file only grows. */
#define FT_QUIET_LIMIT 500

/* The scanner SRB control codes (learned from the first live trace; see
 * README). Quiet mode logs ONLY these. */
#define FT_CODE_SRB   0x190012
#define FT_CODE_SRB2  0x190022

static void ft_copy(void *dst, const void *src, size_t n)
{
    BYTE *d = (BYTE *)dst;
    const BYTE *s = (const BYTE *)src;
    while (n--)
        *d++ = *s++;
}

/* Resolve the trace path to the directory containing ft_hook.dll. The C:\ root
 * is not writable for non-elevated processes on Vista+, and the earlier
 * silent fopen failure there cost a debugging session; the DLL's own
 * directory is always writable (inject.exe already requires the pair to be
 * co-located). Falls back to the bare filename (process cwd) if the module
 * path cannot be determined. */
static void ft_log_path_init(HINSTANCE hinst)
{
    char p[MAX_PATH];
    DWORD n = GetModuleFileNameA(hinst, p, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        char *slash = NULL;
        DWORD i;
        for (i = 0; i < n; i++)
            if (p[i] == '\\')
                slash = p + i;
        if (slash) {
            size_t namelen = sizeof FT_LOG_NAME - 1; /* compile-time constant */
            size_t j, newlen;
            slash++; /* keep the backslash, drop the DLL filename */
            for (j = 0; j < namelen; j++)
                slash[j] = FT_LOG_NAME[j];
            slash[namelen] = '\0';
            /* length AFTER the overwrite: dir + name (+ NUL). Copying the
             * original module-path length here truncates the appended name
             * (produced "flextrace_io" instead of flextrace_ioctl.jsonl). */
            newlen = (size_t)(slash - p) + namelen;
            ft_copy(g_log_path, p, newlen + 1);
        }
    }
}

/* Append one line (a full JSONL record) to the trace file as a SINGLE fwrite
 * of line+'\n' under the critical section: concurrent callers cannot
 * interleave records. The stream is opened once at boot and flushed per
 * record (fflush, not fclose — with a full stdio buffer most records cost
 * only a memcpy, no syscall). Per-record fopen/fclose measurably stalled the
 * host's realtime SCSI thread and made the scanner driver fail commands. */
static void ft_log_line(const char *line, size_t len)
{
    char full[522];

    if (len + 2 > sizeof full)
        return;
    ft_copy(full, line, len);
    full[len] = '\n';
    full[len + 1] = '\0';

    EnterCriticalSection(&g_log_cs);
    if (g_log_file) {
        fwrite(full, 1, len + 1, g_log_file); /* line + newline, one write */
        fflush(g_log_file);
    }
    LeaveCriticalSection(&g_log_cs);
}

/* Foreign-schema control record: the GetLastError of a failed ioctl, captured
 * immediately after the tramp returns (our CRT logging clobbers it). Lets us
 * see the DRIVER's real reason instead of whatever stale value the host app
 * prints in its error dialog. Parsers must skip lines that don't match the
 * ioctl-record schema, so this costs the main schema nothing. */
static void ft_log_lasterr(unsigned long h, unsigned long code)
{
    char line[160];
    int n = snprintf(line, sizeof line,
                     "{\"tag\":\"lasterr\",\"h\":\"0x%lx\","
                     "\"code\":\"0x%lx\",\"err\":%lu}",
                     h, code, (unsigned long)GetLastError());
    if (n > 0 && (size_t)n < sizeof line)
        ft_log_line(line, (size_t)n);
}

static BOOL WINAPI hooked_DeviceIoControl(HANDLE hDevice, DWORD dwIoControlCode,
                                          LPVOID lpInBuffer, DWORD nInBufferSize,
                                          LPVOID lpOutBuffer, DWORD nOutBufferSize,
                                          LPDWORD lpBytesReturned,
                                          LPOVERLAPPED lpOverlapped)
{
    unsigned char in_head[FT_MAX_HEAD];
    unsigned char out_head[FT_MAX_HEAD];
    size_t in_head_len = 0, out_head_len = 0;
    BOOL ret;
    char rec[520];
    int rlen;

    if (g_passthru || g_inside) /* never recurse through our own logging */
        return g_tramp(hDevice, dwIoControlCode, lpInBuffer, nInBufferSize,
                       lpOutBuffer, nOutBufferSize, lpBytesReturned,
                       lpOverlapped);

    g_inside = 1;

    /* pre-call state (quiet mode: only bother for the scanner SRB codes) */
    if (lpInBuffer != NULL && nInBufferSize > 0 &&
        (!g_quiet || dwIoControlCode == FT_CODE_SRB ||
         dwIoControlCode == FT_CODE_SRB2)) {
        in_head_len = nInBufferSize < FT_MAX_HEAD ? nInBufferSize : FT_MAX_HEAD;
        ft_copy(in_head, lpInBuffer, in_head_len);
    }

    ret = g_tramp(hDevice, dwIoControlCode, lpInBuffer, nInBufferSize,
                  lpOutBuffer, nOutBufferSize, lpBytesReturned,
                  lpOverlapped);

    /* The true failure reason must be captured NOW: our own CRT logging
     * calls below (fopen/fwrite/...) clobber GetLastError. Emitted as a
     * separate foreign-schema control record so the ioctl-record schema
     * stays untouched. */
    if (ret == 0 &&
        (!g_quiet || dwIoControlCode == FT_CODE_SRB ||
         dwIoControlCode == FT_CODE_SRB2))
        ft_log_lasterr((unsigned long)(ULONG_PTR)hDevice,
                       (unsigned long)dwIoControlCode);

    /* quiet mode: drop everything except the scanner SRB codes (and cap the
     * storm) — bisects "hook presence on the call stack" against "logging
     * volume/timing" without changing what the hook intercepts. */
    if (g_quiet && dwIoControlCode != FT_CODE_SRB &&
        dwIoControlCode != FT_CODE_SRB2) {
        g_inside = 0;
        return ret;
    }
    if (g_quiet && g_quiet_logged >= FT_QUIET_LIMIT) {
        g_inside = 0;
        return ret;
    }
    g_quiet_logged++;

    /* post-call state */
    if (ret != 0 && lpOutBuffer != NULL && lpBytesReturned != NULL &&
        *lpBytesReturned > 0) {
        DWORD ob = *lpBytesReturned;
        out_head_len = ob < FT_MAX_HEAD ? ob : FT_MAX_HEAD;
        ft_copy(out_head, lpOutBuffer, out_head_len);
    }

    rlen = ft_fmt_ioctl_record(rec, sizeof rec,
                               (unsigned long)(ULONG_PTR)hDevice,
                               (unsigned long)dwIoControlCode,
                               (size_t)nInBufferSize, in_head, in_head_len,
                               ret != 0,
                               (ret != 0 && lpBytesReturned != NULL)
                                   ? (size_t)*lpBytesReturned : 0,
                               out_head, out_head_len);
    if (rlen > 0)
        ft_log_line(rec, (size_t)rlen);

    g_inside = 0;
    return ret;
}

/* Overwrite the first 5 bytes of `target` with E9 rel32 -> `dst`. */
static int ft_patch5(void *target, const void *dst)
{
    BYTE *t = (BYTE *)target;
    DWORD old_prot;
    DWORD jmp_off;

    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &old_prot))
        return 0;
    jmp_off = (DWORD)((BYTE *)dst - t - 5);
    t[0] = 0xE9;
    ft_copy(t + 1, &jmp_off, 4);
    VirtualProtect(t, 5, old_prot, &old_prot);
    FlushInstructionCache(GetCurrentProcess(), t, 5);
    return 1;
}

/* Verify the 5 bytes we are about to steal form a known-good prologue of
 * complete instructions. Blindly copying 5 arbitrary bytes could split an
 * instruction mid-operand and crash the app. Known-good shapes:
 *   - XP/2K-era:  8B FF 55 8B EC   (mov edi,edi; push ebp; mov ebp,esp)
 *   - Win7+ kernel32 thunk: E9 rel32 (one complete jmp into kernelbase;
 *     the trampoline re-bases the rel32, see ft_install_hook)
 * Any 0xE8 (call rel32) is rejected outright (a call needs return-address
 * semantics the trampoline does not replicate). On mismatch the caller must
 * fail closed: no patch, DeviceIoControl left untouched. */
static int ft_prologue_ok(const BYTE *p, const char **reason)
{
    static const BYTE xp_pro[5] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};
    int i;

    for (i = 0; i < 5; i++) {
        if (p[i] == 0xE8) {
            *reason = "call_rel32_in_prologue";
            return 0;
        }
    }
    if (p[0] == 0xE9) {
        /* jmp-thunk: the five bytes are exactly one instruction, so stealing
         * them is safe. A copied E9 rel32 is NOT position-independent,
         * though: rel32 is measured from the NEXT instruction, so the copy
         * in the trampoline must be re-based by (target - tramp) to still
         * reach the original kernelbase destination (ft_install_hook does
         * this). The earlier verbatim copy sent every hooked call
         * (tramp - kernel32) bytes off the real function, which manifested
         * as DeviceIoControl "failing" with a stale unrelated LastError
         * while the app limped along on retries. */
        return 1;
    }
    for (i = 0; i < 5; i++) {
        if (p[i] != xp_pro[i]) {
            *reason = "unknown_prologue";
            return 0;
        }
    }
    return 1;
}

static int ft_install_hook(const char **reason)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    void *target;
    DWORD jmp_back;

    if (!k32) {
        *reason = "kernel32_not_found";
        return 0;
    }
    target = (void *)GetProcAddress(k32, "DeviceIoControl");
    if (!target) {
        *reason = "DeviceIoControl_not_found";
        return 0;
    }
    if (!ft_prologue_ok((const BYTE *)target, reason))
        return 0;

    /* 64 bytes of executable scratch: saved prologue + E9 back to target+5.
     * Cannot live in the DLL's .data (not executable) -> VirtualAlloc RWX. */
    g_tramp_mem = VirtualAlloc(NULL, FT_TRAMP_SZ,
                               MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
    if (!g_tramp_mem) {
        *reason = "VirtualAlloc_failed";
        return 0;
    }

    ft_copy(g_tramp_mem, target, 5);
    if (((BYTE *)g_tramp_mem)[0] == 0xE9) {
        /* Re-base the stolen jmp-thunk: rel' = rel + (target - tramp), since
         * the E9 sits at offset 0 in both places and rel32 is measured from
         * the next instruction. Without this, the trampoline dispatches to
         * kernelbase_target + (tramp - kernel32) — arbitrary memory. */
        LONG rel, rebased;
        ft_copy(&rel, (BYTE *)g_tramp_mem + 1, 4);
        rebased = rel + (LONG)((BYTE *)target - (BYTE *)g_tramp_mem);
        ft_copy((BYTE *)g_tramp_mem + 1, &rebased, 4);
    }
    ((BYTE *)g_tramp_mem)[5] = 0xE9;
    jmp_back = (DWORD)((BYTE *)target + 5 - (BYTE *)g_tramp_mem - 5 - 5);
    ft_copy((BYTE *)g_tramp_mem + 6, &jmp_back, 4);
    /* tramp layout: [0..4] saved bytes, [5] E9, [6..9] rel32 -> target+5 */

    g_tramp = (DeviceIoControl_fn)(void *)g_tramp_mem;

    if (!ft_patch5(target, (void *)hooked_DeviceIoControl)) {
        *reason = "VirtualProtect_failed";
        return 0;
    }
    return 1;
}

static void ft_log_boot(const char *status, const char *prologue)
{
    char line[200];
    int n;
    /* hand-written; does not depend on hook installation success */
    n = snprintf(line, sizeof line,
                 "{\"tag\":\"hook_loaded\",\"target\":\"DeviceIoControl\","
                 "\"status\":\"%s\",\"prologue\":\"%s\",\"pid\":%lu}",
                 status, prologue, (unsigned long)GetCurrentProcessId());
    if (n > 0 && (size_t)n < sizeof line)
        ft_log_line(line, (size_t)n);
}

static void ft_log_install_failed(const char *reason)
{
    char line[200];
    int n;
    n = snprintf(line, sizeof line,
                 "{\"tag\":\"install_failed\",\"target\":\"DeviceIoControl\","
                 "\"reason\":\"%s\",\"pid\":%lu}",
                 reason, (unsigned long)GetCurrentProcessId());
    if (n > 0 && (size_t)n < sizeof line)
        ft_log_line(line, (size_t)n);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)hinst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        const char *fail_reason = "unknown";
        const char *prologue = (*(BYTE *)GetProcAddress(
            GetModuleHandleA("kernel32.dll"), "DeviceIoControl") == 0xE9)
            ? "e9_thunk" : "xp_hotpatch";
        char envbuf[8];
        DWORD envn = GetEnvironmentVariableA("FT_PASSTHRU", envbuf,
                                             sizeof envbuf);
        DWORD qn = GetEnvironmentVariableA("FT_QUIET", envbuf,
                                           sizeof envbuf);
        InitializeCriticalSection(&g_log_cs);
        DisableThreadLibraryCalls(hinst);
        ft_log_path_init(hinst);
        /* open ONCE: per-record fopen/fclose dominated the per-call cost and
         * stalled the host's SCSI thread (see g_log_file comment) */
        g_log_file = fopen(g_log_path, "a");
        g_passthru = (envn > 0 && envn < sizeof envbuf);
        g_quiet = (qn > 0 && qn < sizeof envbuf);
        g_installed = ft_install_hook(&fail_reason);
        if (g_installed)
            ft_log_boot("ok", prologue);
        else
            ft_log_install_failed(fail_reason);
    } else if (reason == DLL_PROCESS_DETACH) {
        EnterCriticalSection(&g_log_cs);
        if (g_log_file) {
            fclose(g_log_file);
            g_log_file = NULL;
        }
        LeaveCriticalSection(&g_log_cs);
        DeleteCriticalSection(&g_log_cs);
    }
    return TRUE;
}
