/*
 * ftpoc.c — FlexTight II (Precision II) proof-of-concept console tool.
 *
 * Talks to the scanner directly through the Microsoft scsiscan.sys driver,
 * replicating the exact 44-byte SRB + ioctl 0x190012 transport that
 * FlexColor.exe uses (validated against 67k+ hooked DeviceIoControl calls,
 * sessions 2-4 — see docs/protocol.md and docs/recon/static-decode.md).
 *
 * Standalone process: FlexColor must be CLOSED (the driver opens exclusively;
 * CreateFile fails with ERROR_ACCESS_DENIED otherwise — that's the check).
 *
 * Build: 32-bit (i686-w64-mingw32-gcc or cl); see build.bat.
 *
 * SAFETY: the only motion primitive is SetWindow's position field (u32 LE
 * @byte 11), patched into the captured idle template — everything else byte
 * identical to what FlexColor sent on the wire. All motion is relative to a
 * freshly-parked carriage (position 0x35b70c, the observed rest position),
 * clamped, and polled with ScannerReady between steps. Ctrl+C mid-move leaves
 * the device finishing the current small step only.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IOCTL_SRB 0x190012

#define SRB_DIR_OUT 0x80        /* host -> device */
#define SRB_DIR_IN  0x40        /* device -> host */

#define TIMEOUT_MS  5000

/* ---------------------------------------------------------------- transport */

static HANDLE g_dev = INVALID_HANDLE_VALUE;
static HANDLE g_evt = NULL;

/* Current carriage position in microsteps, as far as this process has
 * commanded it. Seeded by park(), updated by move/step. Display only. */
static long g_pos = -1;
static int  g_pos_known = 0;

static void die(const char *what)
{
    fprintf(stderr, "ftpoc: %s failed, GetLastError=%lu\n", what, GetLastError());
    exit(1);
}

static HANDLE open_scanner(void)
{
    int i;
    char path[32];

    for (i = 0; i < 16; i++) {
        HANDLE h;
        sprintf(path, "\\\\.\\Scanner%d", i);
        h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        printf("opened %s (handle %p)\n", path, (void *)h);
        return h;
    }
    fprintf(stderr,
        "ftpoc: no \\\\.\\ScannerN answered. Is the scanner powered, the driver\n"
        "installed, and FlexColor (or anything else holding it) closed?\n");
    exit(1);
}

/* Send one 44-byte SRB; SCSI payload rides lpOutBuffer (METHOD_OUT_DIRECT).
 * Returns bytes transferred, or a negative code:
 *  -1 DeviceIoControl rejected, -2 timed out (cancelled), -3 completion error */
static int srb_exec(const BYTE *cdb, int cdb_len, BYTE dir,
                    BYTE *data, DWORD data_len, DWORD timeout_ms)
{
    BYTE srb[0x70];
    OVERLAPPED ov;
    DWORD transferred = 0;

    memset(srb, 0, sizeof srb);
    *(DWORD *)(srb + 0x04) = 44;                    /* SRB header length      */
    srb[0x08] = dir;                                /* 0x40 in / 0x80 out     */
    srb[0x0c] = (BYTE)cdb_len;                      /* 6 or 10 on the wire    */
    srb[0x0d] = 0x0e;                               /* constant in all traces */
    *(DWORD *)(srb + 0x10) = data_len;              /* payload length (LE)    */
    memcpy(srb + 0x14, cdb, cdb_len);               /* 16-byte CDB slot       */
    *(DWORD *)(srb + 0x24) = (DWORD)(ULONG_PTR)(srb + 0x2c); /* postproc ptr  */
    *(DWORD *)(srb + 0x28) = (DWORD)(ULONG_PTR)(srb + 0x34); /* sense ptr     */
    *(DWORD *)(srb + 0x30) = (DWORD)(ULONG_PTR)data;         /* data ptr      */
    *(DWORD *)(srb + 0x68) = (DWORD)(ULONG_PTR)g_evt;        /* event (async) */

    memset(&ov, 0, sizeof ov);
    ov.hEvent = g_evt;
    ResetEvent(g_evt);

    if (!DeviceIoControl(g_dev, IOCTL_SRB, srb, 44,
                         data, data_len, NULL, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING)
            return -1;
        if (WaitForSingleObject(g_evt, timeout_ms) != WAIT_OBJECT_0) {
            CancelIo(g_dev);
            return -2;
        }
        if (!GetOverlappedResult(g_dev, &ov, &transferred, TRUE))
            return -3;
    }
    return (int)transferred;
}

static const char *srb_errstr(int r)
{
    static char buf[64];
    switch (r) {
    case -1: sprintf(buf, "DeviceIoControl rejected (GLE=%lu)",
                     GetLastError()); break;
    case -2: sprintf(buf, "timed out after wait"); break;
    case -3: sprintf(buf, "I/O completed with error (GLE=%lu)",
                     GetLastError()); break;
    default: sprintf(buf, "code %d", r); break;
    }
    return buf;
}

/* ----------------------------------------------------------------- commands */

static const BYTE CDB_INQUIRY[6] = { 0x12, 0, 0, 0, 0x60, 0 };
static const BYTE CDB_POLL[6]    = { 0xdf, 0, 0, 0, 0x02, 0 };
static const BYTE CDB_WIN[10]    = { 0x24, 0, 0, 0, 0, 0, 0, 0x54, 0, 0 };
static const BYTE CDB_GRAB[10]   = { 0x28, 0, 0, 0, 0x28, 0, 0, 0, 0, 0 };
static const BYTE CDB_PAGE[10]   = { 0x3c, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const BYTE CDB_UPLOAD[10] = { 0x3b, 1, 0, 0, 0, 0, 0, 0x10, 0, 0 };
static const BYTE CDB_UPTRAIL[10]= { 0x3b, 1, 0, 0x30, 1, 0, 0x0e, 0, 0, 0 };

/* SetWindow 84-byte descriptors, pinned verbatim from session-4 sidecar
 * (flextrace_bufs.bin, candidate 0x60, when=0):
 *  IDLE_WIN: modal idle heartbeat, seq 67521, count 31
 *  SCAN_WIN: last descriptor before first df->0x1060, seq 292
 * Do NOT hand-edit: if a field is wrong, re-derive from the captures. */
static const BYTE IDLE_WIN[84] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4c,0x00,0x00,0x00,0x89,0x54,0x40,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x07,0xce,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x02,
    0x00,0x01,0x00,0x06,0xb7,0x31,0x00,0x0d,0xa8,0xe4,0x00,0x00,0x64,0xc8,0x00,0x00,
    0x00,0x03,0x00,0x00,0x1e,0x00,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0xb4
};
static const BYTE SCAN_WIN[84] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4c,0x00,0x00,0x00,0x50,0x1b,0xd0,0x00,
    0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x9a,0x00,0x00,0x12,0xbb,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x0c,0x00,0xe6,0x00,0x01,0x00,0x03,0xe0,0xab,0x00,0x0f,0x55,0xbc,0x00,0x00,
    0x5f,0xb4,0x00,0x00,0x00,0x02,0x00,0x00,0x1e,0x00,0x00,0x01,0x00,0x00,0x00,0x00,
    0x00,0x00,0xb4
};

/* Observed carriage rest position (idle heartbeat, session start). */
#define PARK_POS 0x0035b70cL
/* Never command outside [0, HARD_LIMIT] microsteps. */
#define HARD_LIMIT 16000000L

static int cmd_inquiry(void)
{
    BYTE buf[96];
    int r, i;
    memset(buf, 0, sizeof buf);
    r = srb_exec(CDB_INQUIRY, 6, SRB_DIR_IN, buf, sizeof buf, TIMEOUT_MS);
    if (r < 0) { printf("INQUIRY: %s\n", srb_errstr(r)); return 1; }
    printf("INQUIRY: %d bytes\n", r);
    printf("  vendor : %.8s\n", buf + 8);
    printf("  product: %.16s\n", buf + 16);
    printf("  version: %.4s\n", buf + 32);
    printf("  raw    :");
    for (i = 0; i < 36; i++) printf(" %02x", buf[i]);
    printf("\n");
    /* Loader identity means the microcode upload dance is required first. */
    if (memcmp(buf + 16, "SCSI Loader", 11) == 0)
        printf("  NOTE: loader firmware — run `upload MICROCOD.3XX` before "
               "anything else.\n");
    return 0;
}

/* The device needs ~1 s after driver open before it answers (FlexColor's
 * own first INQUIRYs fail with ERROR_IO_DEVICE too — session 4, seq 5-19).
 * Retry INQUIRY until it answers; give up after ~20 s. */
static int wait_ready(void)
{
    int attempt;
    char last[64] = "";
    for (attempt = 0; attempt < 20; attempt++) {
        BYTE buf[96];
        int r = srb_exec(CDB_INQUIRY, 6, SRB_DIR_IN, buf, sizeof buf, 3000);
        if (r >= 0) {
            if (attempt > 0)
                printf("ready after %d attempt(s)\n", attempt + 1);
            return 0;
        }
        /* format now: GetLastError() is only meaningful right after the call */
        strcpy(last, srb_errstr(r));
        Sleep(1000);
    }
    fprintf(stderr, "scanner did not become ready within 20 s "
                    "(last: %s)\n", last);
    return 1;
}

static const char *poll_name(unsigned v)
{
    switch (v) {
    case 0x0000: return "idle";
    case 0x0400: return "busy";
    case 0x1060: return "DATA READY";
    case 0x9299: return "monitor-live";
    case 0x0800: return "motor op";
    case 0x0500: return "move ack";
    case 0x4894: return "settling";
    case 0xbb12: return "position done";
    case 0x38c9: return "upload commit";
    case 0x9000: return "scan armed";
    default:     return "?";
    }
}

/* One ScannerReady poll; returns the 16-bit status (big-endian on the wire)
 * or a negative transport code. */
static int poll_once(void)
{
    BYTE buf[2];
    int r;
    memset(buf, 0, sizeof buf);
    r = srb_exec(CDB_POLL, 6, SRB_DIR_IN, buf, sizeof buf, TIMEOUT_MS);
    if (r < 0)
        return r;
    return (buf[0] << 8) | buf[1];
}

static int cmd_poll(int n, int quiet)
{
    int i;
    for (i = 0; i < n; i++) {
        int v = poll_once();
        if (v < 0) { printf("poll %d: %s\n", i, srb_errstr(v)); return 1; }
        if (!quiet)
            printf("poll %d: %04x  %s\n", i, v, poll_name((unsigned)v));
        else if (i == 0 || i == n - 1)
            printf("poll %d: %04x  %s\n", i, v, poll_name((unsigned)v));
    }
    return 0;
}

/* One SetWindow exchange: send descriptor, device overlays status into the
 * same buffer. Returns transport code (>=0 = bytes) and prints both sides
 * when trace is set. */
static int win_exchange(BYTE desc[84], int trace)
{
    BYTE send[84];
    int r, i;
    memcpy(send, desc, 84);
    r = srb_exec(CDB_WIN, 10, SRB_DIR_OUT, desc, 84, TIMEOUT_MS);
    if (trace) {
        printf("  sent:");
        for (i = 0; i < 84; i++) printf(" %02x", send[i]);
        printf("\n  recv:");
        for (i = 0; i < 84; i++) printf(" %02x", desc[i]);
        printf("\n");
    }
    return r;
}

static int cmd_win(void)
{
    BYTE desc[84];
    int r;
    memcpy(desc, IDLE_WIN, 84);
    r = win_exchange(desc, 1);
    if (r < 0) { printf("SetWindow: %s\n", srb_errstr(r)); return 1; }
    printf("SetWindow: %d bytes; device state bytes 0-7 = %02x %02x %02x %02x "
           "%02x %02x %02x %02x\n", r,
           desc[0], desc[1], desc[2], desc[3],
           desc[4], desc[5], desc[6], desc[7]);
    return 0;
}

/* Wait for the carriage to go idle (df == 0000), up to timeout_ms. */
static int wait_idle(DWORD timeout_ms)
{
    DWORD start = GetTickCount();
    int v = poll_once();
    while (v != 0x0000) {
        if (v < 0)
            return v;
        if (GetTickCount() - start > timeout_ms)
            return -100;
        if (v != 0x0400)   /* progress chatter is interesting */
            printf("  ... %04x %s\n", v, poll_name((unsigned)v));
        Sleep(120);
        v = poll_once();
    }
    return 0;
}

/* Send a SetWindow that commands an absolute carriage position (everything
 * else from the idle template). Then wait for idle. */
static int move_to(long pos, const char *why)
{
    BYTE desc[84];
    int r;
    if (pos < 0 || pos > HARD_LIMIT) {
        fprintf(stderr, "refusing position %ld (limit %ld)\n", pos, HARD_LIMIT);
        return 1;
    }
    memcpy(desc, IDLE_WIN, 84);
    desc[11] = (BYTE)(pos & 0xff);
    desc[12] = (BYTE)((pos >> 8) & 0xff);
    desc[13] = (BYTE)((pos >> 16) & 0xff);
    desc[14] = (BYTE)((pos >> 24) & 0xff);
    printf("move -> %ld (%s)\n", pos, why);
    r = win_exchange(desc, 0);
    if (r < 0) { printf("SetWindow: %s\n", srb_errstr(r)); return 1; }
    r = wait_idle(30000);
    if (r != 0) {
        printf("warning: carriage did not report idle (%d)\n", r);
        return 1;
    }
    g_pos = pos;
    g_pos_known = 1;
    printf("at %ld, idle\n", pos);
    return 0;
}

static int cmd_park(void)
{
    return move_to(PARK_POS, "park");
}

static int cmd_move(long delta, int steps)
{
    int s;
    if (steps < 1) steps = 1;
    if (steps > 50) { fprintf(stderr, "steps capped at 50\n"); return 1; }
    if (delta == 0 || delta > 200000L || delta < -200000L) {
        fprintf(stderr, "per-step delta must be within +/-200000 microsteps "
                        "(given %ld); keep moves small until we know the rig\n",
                delta);
        return 1;
    }
    if (!g_pos_known) {
        printf("position unknown — parking first for a known reference\n");
        if (cmd_park())
            return 1;
    }
    for (s = 0; s < steps; s++) {
        if (move_to(g_pos + delta, "relative step"))
            return 1;
    }
    return 0;
}

static int cmd_pages(void)
{
    /* CDB: 3c 00 II 00 OFF_HI OFF_LO 00 LL_HI LL_LO 00 — offset and length
     * are big-endian 16-bit fields (pinned from session-4 captures). */
    static const struct { BYTE id; WORD off, len; const char *name; } pages[] = {
        { 0xd8, 0x0000, 0x0042, "descriptor page" },
        { 0xd8, 0x0037, 0x000a, "descriptor tail" },
        { 0xe0, 0x0004, 0x0016, "FaulPixs header" },
        { 0xe0, 0x0004, 0x013e, "FaulPixs table" },
        { 0xe0, 0x0000, 0x0400, "buffer e0/400" },
    };
    size_t i;
    for (i = 0; i < sizeof pages / sizeof pages[0]; i++) {
        BYTE cdb[10], buf[1024];
        int r, j;
        memcpy(cdb, CDB_PAGE, 10);
        cdb[2] = pages[i].id;
        cdb[4] = (BYTE)(pages[i].off >> 8);
        cdb[5] = (BYTE)(pages[i].off & 0xff);
        cdb[7] = (BYTE)(pages[i].len >> 8);
        cdb[8] = (BYTE)(pages[i].len & 0xff);
        memset(buf, 0, sizeof buf);
        r = srb_exec(cdb, 10, SRB_DIR_IN, buf, pages[i].len, TIMEOUT_MS);
        if (r < 0) { printf("%s: %s\n", pages[i].name, srb_errstr(r)); continue; }
        printf("%s (%02x/%04x, %u bytes):", pages[i].name,
               pages[i].id, pages[i].off, pages[i].len);
        for (j = 0; j < r && j < 48; j++)
            printf(" %02x", buf[j]);
        printf("%s\n", r > 48 ? " ..." : "");
    }
    return 0;
}

static int cmd_grab(long blocks, WORD blklen, const char *out)
{
    BYTE cdb[10];
    BYTE *buf;
    FILE *f;
    long b;
    int bad = 0;

    if (blklen == 0) blklen = 3996;
    if (blklen > 65000) { fprintf(stderr, "block len too big\n"); return 1; }
    buf = malloc(blklen);
    if (!buf) return 1;
    memcpy(cdb, CDB_GRAB, 10);
    cdb[7] = (BYTE)(blklen >> 8);
    cdb[8] = (BYTE)(blklen & 0xff);

    f = fopen(out, "wb");
    if (!f) { perror(out); free(buf); return 1; }

    for (b = 0; b < blocks; b++) {
        int r = srb_exec(cdb, 10, SRB_DIR_IN, buf, blklen, TIMEOUT_MS);
        if (r < 0) {
            printf("grab %ld: %s, stopping\n", b, srb_errstr(r));
            bad = 1;
            break;
        }
        if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) { bad = 1; break; }
        if (blocks > 20 && (b % 100) == 0)
            printf("grab %ld/%ld\r", b, blocks), fflush(stdout);
    }
    fclose(f);
    free(buf);
    printf("\nwrote %ld blocks x %u bytes -> %s%s\n",
           blocks, blklen, out, bad ? " (INCOMPLETE)" : "");
    return bad;
}

/* Best-effort low-res scan: replay the captured scan-arming descriptor,
 * run the calibration-read shape (200+50 blocks, discarded), poll for
 * data-ready, then grab. Falls back to idle afterwards unconditionally. */
static int cmd_scan(long blocks, const char *out)
{
    BYTE desc[84];
    long b;
    int v, r;

    memcpy(desc, SCAN_WIN, 84);
    printf("sending scan-arm descriptor (pos %ld)\n",
           (long)((desc[14] << 24) | (desc[13] << 16) | (desc[12] << 8) | desc[11]));
    r = win_exchange(desc, 0);
    if (r < 0) { printf("SetWindow: %s\n", srb_errstr(r)); return 1; }

    printf("calibration reads (200+50, discarded)\n");
    {
        BYTE cdb[10], junk[3996];
        memcpy(cdb, CDB_GRAB, 10);
        cdb[7] = 0x0f; cdb[8] = 0x9c;   /* 3996 */
        for (b = 0; b < 250; b++)
            srb_exec(cdb, 10, SRB_DIR_IN, junk, sizeof junk, TIMEOUT_MS);
    }

    printf("polling for data-ready (up to 90 s)\n");
    {
        DWORD start = GetTickCount();
        for (;;) {
            v = poll_once();
            if (v == 0x1060)
                break;
            if (v < 0) { printf("poll: %s\n", srb_errstr(v)); break; }
            if (v != 0x0400 && v != 0x0000)
                printf("  ... %04x %s\n", v, poll_name((unsigned)v));
            if (GetTickCount() - start > 90000) {
                printf("timed out waiting for 1060; df never went data-ready\n");
                v = -1;
                break;
            }
            Sleep(150);
        }
    }
    if (v == 0x1060) {
        printf("data ready — grabbing %ld blocks -> %s\n", blocks, out);
        cmd_grab(blocks, 3996, out);
    }

    printf("returning carriage to park\n");
    cmd_park();
    return v == 0x1060 ? 0 : 1;
}

/* MICROCOD.3XX upload: 16 x 4096-byte chunks + 10-byte trailer, then poll
 * until idle. Only valid when the device answers INQUIRY with the loader
 * identity ("SCSI Loader") — i.e. after a cold power-up. */
static int cmd_upload(const char *file)
{
    FILE *f = fopen(file, "rb");
    BYTE chunk[4096];
    int i, r, v;
    DWORD start;
    long sz;

    if (!f) { perror(file); return 1; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz != 65536) {
        fprintf(stderr, "%s is %ld bytes, expected 65536 (MICROCOD.3XX)\n",
                file, sz);
        fclose(f);
        return 1;
    }
    for (i = 0; i < 16; i++) {
        BYTE cdb[10];
        if (fread(chunk, 1, sizeof chunk, f) != sizeof chunk) return 1;
        memcpy(cdb, CDB_UPLOAD, 10);
        cdb[2] = (BYTE)(0x30 + i);
        r = srb_exec(cdb, 10, SRB_DIR_OUT, chunk, sizeof chunk, 10000);
        printf("chunk %2d (id 0x%02x): %s\n", i, 0x30 + i,
               r < 0 ? "ERROR" : "ok");
    }
    fclose(f);
    {
        BYTE payload[10] = { 0x00, 0x30, 0x01, 0x00, 0x0e,
                             0x00, 0x00, 0x00, 0x00, 0x00 };
        r = srb_exec(CDB_UPTRAIL, 10, SRB_DIR_OUT, payload, 10, 10000);
        printf("trailer: %s\n", r < 0 ? "ERROR" : "ok");
    }
    printf("waiting for firmware commit (poll until idle)\n");
    start = GetTickCount();
    for (;;) {
        v = poll_once();
        if (v == 0x0000) break;
        if (v != 0x0400 && v >= 0)
            printf("  ... %04x %s\n", v, poll_name((unsigned)v));
        if (GetTickCount() - start > 60000) {
            printf("timed out; upload may have failed\n");
            return 1;
        }
        Sleep(200);
    }
    printf("device idle — upload complete; INQUIRY should now name "
           "FlexTight II\n");
    return 0;
}

/* ------------------------------------------------------------------- driver */

int main(int argc, char **argv)
{
    const char *cmd;
    int rc = 0;

    if (argc < 2) {
        fprintf(stderr,
            "usage: ftpoc <command> [args]\n"
            "  inquiry            identity (run first)\n"
            "  poll [n]           n ScannerReady polls (default 10)\n"
            "  win                one SetWindow exchange (idle template), hexdump\n"
            "  park               carriage to rest position\n"
            "  move <+/-us> [n]   n relative moves of +/-us microsteps "
                                 "(max 200000/step, 50 steps)\n"
            "  pages              read the standard ReadBuffer pages\n"
            "  grab <n> [len]     n ReadExt blocks (default len 3996) -> grab.raw\n"
            "  scan [n]           replay scan-arm template, poll data-ready,\n"
            "                     grab n blocks (default 480) -> scan.raw\n"
            "  upload <file>      MICROCOD.3XX upload (loader mode only!)\n"
            "\n"
            "CLOSE FlexColor first. Motion is slow and clamped; Ctrl+C aborts.\n");
        return 2;
    }
    cmd = argv[1];

    g_evt = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_evt) die("CreateEvent");

    if (strcmp(cmd, "inquiry") == 0 || strcmp(cmd, "devices") == 0) {
        g_dev = open_scanner();
        rc = cmd_inquiry();
    } else {
        /* every other command needs a live device */
        g_dev = open_scanner();
        if (wait_ready())
            return 1;

        if (0) { }
        else if (strcmp(cmd, "poll") == 0) {
            int n = argc > 2 ? atoi(argv[2]) : 10;
            rc = cmd_poll(n, n > 20);
        } else if (strcmp(cmd, "win") == 0) {
            rc = cmd_win();
        } else if (strcmp(cmd, "park") == 0) {
            rc = cmd_park();
        } else if (strcmp(cmd, "move") == 0 && argc > 2) {
            rc = cmd_move(atol(argv[2]), argc > 3 ? atoi(argv[3]) : 1);
        } else if (strcmp(cmd, "pages") == 0) {
            rc = cmd_pages();
        } else if (strcmp(cmd, "grab") == 0 && argc > 2) {
            rc = cmd_grab(atol(argv[2]),
                          argc > 3 ? (WORD)atoi(argv[3]) : 3996, "grab.raw");
        } else if (strcmp(cmd, "scan") == 0) {
            rc = cmd_scan(argc > 2 ? atol(argv[2]) : 480, "scan.raw");
        } else if (strcmp(cmd, "upload") == 0 && argc > 2) {
            rc = cmd_upload(argv[2]);
        } else {
            fprintf(stderr, "ftpoc: unknown command '%s' (try without args "
                            "for usage)\n", cmd);
            rc = 2;
        }
    }

    if (g_dev != INVALID_HANDLE_VALUE)
        CloseHandle(g_dev);
    return rc;
}
