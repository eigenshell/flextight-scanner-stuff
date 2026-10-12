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
 * Device: the scanner answers on all 8 LUNs of its target, so Windows makes
 * several \\.\ScannerN; only one is live (session-4 box: Scanner4). The
 * others fail every command with UNEXPECTED_BUS_FREE. open_scanner() picks
 * the one whose INQUIRY says Imacon.
 *
 * SAFETY: the only motion primitive is SetWindow's position field (BE u32
 * @byte 10), patched into a descriptor captured byte-for-byte from
 * FlexColor; every other byte is what FlexColor sent. Positions are limited
 * to the range FlexColor itself used (0..63000000); park is FlexColor's own
 * shutdown descriptor.
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

/* SRB status byte and 14-byte sense buffer of the last -3 failure
 * (SCSISCAN_CMD pSrbStatus @0x24 / pSenseBuffer @0x28). ERROR_IO_DEVICE
 * (1117) is a CHECK CONDITION; the sense data says why. */
static BYTE g_srb_status;
static BYTE g_sense[14];

static void die(const char *what)
{
    fprintf(stderr, "ftpoc: %s failed, GetLastError=%lu\n", what, GetLastError());
    exit(1);
}

static int srb_exec(const BYTE *cdb, int cdb_len, BYTE dir,
                    BYTE *data, DWORD data_len, DWORD timeout_ms);

/* Like FlexColor's FindDevices: open every \\.\ScannerN and keep the one
 * whose INQUIRY names Imacon. Other indexes can be stale or foreign
 * devices that never answer. A fresh handle can fail its first few
 * INQUIRYs with ERROR_IO_DEVICE (FlexColor sees 2-4), so retry briefly. */
static HANDLE open_scanner(void)
{
    static const BYTE inq[6] = { 0x12, 0, 0, 0, 0x60, 0 };
    int i, attempt;
    char path[32];

    for (i = 0; i < 16; i++) {
        HANDLE h;
        sprintf(path, "\\\\.\\Scanner%d", i);
        h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        g_dev = h;
        for (attempt = 0; attempt < 8; attempt++) {
            BYTE buf[96];
            memset(buf, 0, sizeof buf);
            if (srb_exec(inq, 6, 0x40, buf, sizeof buf, 3000) >= 0) {
                printf("%s: %.8s %.16s %.4s\n", path, buf + 8, buf + 16,
                       buf + 32);
                if (memcmp(buf + 8, "Imacon", 6) == 0)
                    return h;
                break;
            }
        }
        if (attempt == 8)
            printf("%s: no INQUIRY answer, skipping\n", path);
        CloseHandle(h);
        g_dev = INVALID_HANDLE_VALUE;
    }
    fprintf(stderr,
        "ftpoc: no \\\\.\\ScannerN answered as Imacon. Is the scanner powered,\n"
        "and FlexColor (or anything else holding it) closed?\n");
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
                         data, data_len, &transferred, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING)
            return -1;
        if (WaitForSingleObject(g_evt, timeout_ms) != WAIT_OBJECT_0) {
            CancelIo(g_dev);
            return -2;
        }
        if (!GetOverlappedResult(g_dev, &ov, &transferred, TRUE)) {
            g_srb_status = srb[0x2c];
            memcpy(g_sense, srb + 0x34, sizeof g_sense);
            return -3;
        }
    }
    return (int)transferred;
}

static const char *srb_errstr(int r)
{
    static char buf[128];
    switch (r) {
    case -1: sprintf(buf, "DeviceIoControl rejected (GLE=%lu)",
                     GetLastError()); break;
    case -2: sprintf(buf, "timed out after wait"); break;
    case -3: sprintf(buf, "I/O completed with error (GLE=%lu) srb=%02x "
                     "sense key=%x asc=%02x ascq=%02x",
                     GetLastError(), g_srb_status, g_sense[2] & 0x0f,
                     g_sense[12], g_sense[13]); break;
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
static const BYTE CDB_SCANSTOP[6]= { 0xdb, 0, 0, 0, 0, 0 };  /* kHwiScanStop */
static const BYTE CDB_UPLOAD[10] = { 0x3b, 1, 0, 0, 0, 0, 0, 0x10, 0, 0 };
static const BYTE CDB_UPTRAIL[10]= { 0x3b, 1, 0, 0x30, 1, 0, 0x0e, 0, 0, 0 };

/* SetWindow 84-byte descriptors, generated byte-for-byte from the session-4
 * sidecar (flextrace_bufs.bin, cand 0x60, when=0) — NOT typed by hand: an
 * earlier hand copy silently dropped bytes and C zero-padded the [84]
 * array, shifting every field. The typedefs fail the build unless each
 * template is exactly 84 bytes. Regenerate from the captures, never edit. */
/* seq 292: preview scan @5250000, 3996-B blocks (sent right after the page burst) */
static const BYTE SCAN_WIN[] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4c,0x00,0x00,0x00,0x50,
    0x1b,0xd0,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,
    0x02,0x9a,0x00,0x00,0x12,0xbb,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x0c,0x00,0xe6,0x00,0x01,0x00,0x03,0xe0,0xab,
    0x00,0x0f,0x55,0xbc,0x00,0x00,0x5f,0xb4,0x00,0x00,0x00,0x02,
    0x00,0x00,0x1e,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0xb4,
};
typedef char SCAN_WIN_is_84[sizeof SCAN_WIN == 84 ? 1 : -1];
/* seq 60399: end of scan, sent twice after the last ReadExt */
static const BYTE END_WIN[] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4c,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0xe6,0x00,0x01,0x00,0x03,0xe0,0xab,
    0x00,0x0f,0x55,0xbc,0x00,0x00,0x5f,0xb4,0x00,0x00,0x00,0x02,
    0x00,0x00,0x1e,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0xb4,
};
typedef char END_WIN_is_84[sizeof END_WIN == 84 ? 1 : -1];
/* seq 67521: monitor idle heartbeat @9000000 (template for moves) */
static const BYTE IDLE_WIN[] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4c,0x00,0x00,0x00,0x89,
    0x54,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x07,0xce,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x04,0x00,0x02,0x00,0x01,0x00,0x06,0xb7,0x31,
    0x00,0x0d,0xa8,0xe4,0x00,0x00,0x64,0xc8,0x00,0x00,0x00,0x03,
    0x00,0x00,0x1e,0x00,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0xb4,
};
typedef char IDLE_WIN_is_84[sizeof IDLE_WIN == 84 ? 1 : -1];
/* seq 67705: FlexColor shutdown descriptor @36000000 (rest position) */
static const BYTE EXIT_WIN[] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4c,0x00,0x00,0x02,0x25,
    0x51,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x01,0x00,0x02,0x00,0x01,0x00,0x06,0xb7,0x31,
    0x00,0x0d,0xa8,0xe4,0x00,0x00,0x5f,0xb4,0x00,0x00,0x00,0x02,
    0x00,0x00,0x1e,0x00,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0xb4,
};
typedef char EXIT_WIN_is_84[sizeof EXIT_WIN == 84 ? 1 : -1];

/* Carriage position: big-endian u32 @10. FlexColor's values are round
 * (833333, 5250000, 9000000, 36000000 ... 63000000), likely nm of holder
 * travel. Never command outside the observed range. */
#define POS_OFF    10
#define POS_MAX    63000000L

static long desc_pos(const BYTE *d)
{
    return ((long)d[POS_OFF] << 24) | ((long)d[POS_OFF + 1] << 16) |
           ((long)d[POS_OFF + 2] << 8) | (long)d[POS_OFF + 3];
}

static void set_pos(BYTE *d, long pos)
{
    d[POS_OFF]     = (BYTE)(pos >> 24);
    d[POS_OFF + 1] = (BYTE)(pos >> 16);
    d[POS_OFF + 2] = (BYTE)(pos >> 8);
    d[POS_OFF + 3] = (BYTE)pos;
}

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

/* Send one template verbatim (copied: the device overlays status). */
static int send_win(const BYTE *tmpl, const char *label)
{
    BYTE desc[84];
    int r;
    memcpy(desc, tmpl, 84);
    r = win_exchange(desc, 0);
    if (r < 0)
        printf("SetWindow (%s): %s\n", label, srb_errstr(r));
    return r < 0 ? 1 : 0;
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

/* Wait until the device reports idle (df == 0000) after a command that
 * may take a moment to start: a 0000 only counts once something else was
 * seen, or after 3 s of uninterrupted 0000. Prints status changes only. */
static int wait_settled(DWORD timeout_ms)
{
    DWORD start = GetTickCount(), now;
    int v, last = -1, seen_busy = 0;
    for (;;) {
        v = poll_once();
        if (v < 0)
            return v;
        now = GetTickCount() - start;
        if (v != last && v != 0x0000)
            printf("  ... %04x %s (%lu ms)\n", v, poll_name((unsigned)v),
                   (unsigned long)now);
        last = v;
        if (v != 0x0000)
            seen_busy = 1;
        else if (seen_busy || now > 3000)
            return 0;
        if (now > timeout_ms)
            return -100;
        Sleep(150);
    }
}

/* Absolute move: idle template with the position patched, then settle. */
static int move_to(long pos, const char *why)
{
    BYTE desc[84];
    int r;
    if (pos < 0 || pos > POS_MAX) {
        fprintf(stderr, "refusing position %ld (observed range 0..%ld)\n",
                pos, POS_MAX);
        return 1;
    }
    memcpy(desc, IDLE_WIN, 84);
    set_pos(desc, pos);
    printf("move -> %ld (%s)\n", pos, why);
    r = win_exchange(desc, 0);
    if (r < 0) { printf("SetWindow: %s\n", srb_errstr(r)); return 1; }
    r = wait_settled(60000);
    if (r != 0) {
        printf("warning: carriage did not report idle (%d)\n", r);
        return 1;
    }
    g_pos = pos;
    g_pos_known = 1;
    printf("at %ld, idle\n", pos);
    return 0;
}

/* Rest position = FlexColor's own shutdown descriptor, sent verbatim. */
static int cmd_park(void)
{
    printf("park -> %ld (FlexColor shutdown descriptor)\n", desc_pos(EXIT_WIN));
    if (send_win(EXIT_WIN, "park"))
        return 1;
    if (wait_settled(60000)) {
        printf("warning: carriage did not report idle\n");
        return 1;
    }
    g_pos = desc_pos(EXIT_WIN);
    g_pos_known = 1;
    printf("parked at %ld, idle\n", g_pos);
    return 0;
}

/* End-of-scan descriptor x2 (what FlexColor sends after the last ReadExt),
 * then park. Also the way out of a half-started scan. */
static int cmd_stop(void)
{
    if (send_win(END_WIN, "end") || send_win(END_WIN, "end"))
        return 1;
    return cmd_park();
}

/* 0xdb ScanStop: in FlexColor's opcode table (kHwiScanStop) but never seen
 * on the wire; 6-byte by the CDB length rule, no data phase. */
static int cmd_scanstop(void)
{
    /* scsiscan rejects a NULL carrier (GLE 87): try no-data flags with a
     * dummy buffer first, then a 2-byte data-in. */
    BYTE dummy[2] = { 0, 0 };
    int r = srb_exec(CDB_SCANSTOP, 6, 0x00, dummy, 0, TIMEOUT_MS);
    printf("ScanStop (no data): %s\n", r >= 0 ? "ok" : srb_errstr(r));
    if (r < 0) {
        r = srb_exec(CDB_SCANSTOP, 6, SRB_DIR_IN, dummy, 2, TIMEOUT_MS);
        printf("ScanStop (2-byte in): %s\n", r >= 0 ? "ok" : srb_errstr(r));
    }
    return r < 0;
}

typedef struct { BYTE PathId; } STORAGE_BUS_RESET_REQUEST_;

/* SCSI bus reset on the HBA port that hosts the Imacon: every device on the
 * bus returns to its power-on state. Refuses if the port has a disk (type
 * 0) or anything that is not the scanner on it. Needs admin. */
#define IOCTL_SCSI_GET_INQUIRY_DATA 0x0004100c
#define IOCTL_STORAGE_RESET_BUS     0x002d5000

static int cmd_busreset(void)
{
    static BYTE info[16384];
    int port;
    for (port = 0; port < 8; port++) {
        char path[24];
        HANDLE h;
        DWORD n = 0, nbus, b, off;
        int has_imacon = 0, foreign = 0, path_id = -1;
        sprintf(path, "\\\\.\\Scsi%d:", port);
        h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        memset(info, 0, sizeof info);
        if (!DeviceIoControl(h, IOCTL_SCSI_GET_INQUIRY_DATA, NULL, 0,
                             info, sizeof info, &n, NULL)) {
            printf("%s: inquiry list failed (GLE=%lu)\n", path, GetLastError());
            CloseHandle(h);
            continue;
        }
        /* SCSI_ADAPTER_BUS_INFO { u8 NumberOfBuses; SCSI_BUS_DATA[] (8 B:
         * u8 NumberOfLogicalUnits, u8 InitiatorBusId, u32 InquiryDataOffset) }
         * -> chain of SCSI_INQUIRY_DATA { u8 PathId, TargetId, Lun,
         * DeviceClaimed; u32 InquiryDataLength; u32 NextInquiryDataOffset;
         * u8 InquiryData[] } */
        nbus = info[0];
        for (b = 0; b < nbus; b++) {
            off = *(DWORD *)(info + 4 + b * 8 + 4);
            while (off && off + 16 + 36 <= n) {
                BYTE *e = info + off;
                BYTE *inq = e + 12;
                printf("%s path %u target %u lun %u: type %u %.8s %.16s\n",
                       path, e[0], e[1], e[2], inq[0] & 0x1f, inq + 8, inq + 16);
                if (memcmp(inq + 8, "Imacon", 6) == 0) {
                    has_imacon = 1;
                    path_id = e[0];
                } else {
                    foreign = 1;
                }
                off = *(DWORD *)(e + 8);
            }
        }
        if (has_imacon) {
            STORAGE_BUS_RESET_REQUEST_ rq;
            if (foreign) {
                printf("refusing: %s hosts other devices too\n", path);
                CloseHandle(h);
                return 1;
            }
            rq.PathId = (BYTE)path_id;
            if (!DeviceIoControl(h, IOCTL_STORAGE_RESET_BUS, &rq, sizeof rq,
                                 NULL, 0, &n, NULL)) {
                printf("bus reset failed (GLE=%lu)\n", GetLastError());
                CloseHandle(h);
                return 1;
            }
            printf("bus reset issued on %s path %d\n", path, path_id);
            CloseHandle(h);
            return 0;
        }
        CloseHandle(h);
    }
    printf("no SCSI port lists the Imacon\n");
    return 1;
}

static int cmd_goto(long pos)
{
    return move_to(pos, "absolute");
}

static int cmd_move(long delta, int steps)
{
    int s;
    if (steps < 1) steps = 1;
    if (steps > 50) { fprintf(stderr, "steps capped at 50\n"); return 1; }
    if (delta == 0 || delta > 5000000L || delta < -5000000L) {
        fprintf(stderr, "per-step delta must be within +/-5000000 "
                        "(given %ld)\n", delta);
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

static int read_pages(int verbose)
{
    /* CDB: 3c 00 II OO OO OO LL LL LL 00 — standard SCSI READ BUFFER(10):
     * 24-bit big-endian offset @3, 24-bit big-endian length @6. Every 0x3c
     * CDB in the session-4 trace (flextrace_ioctl.jsonl) fits this layout;
     * Faul Pixs lives at offset 0x400 of buffer e0, right after the 1 KiB
     * block FlexColor reads at offset 0. */
    static const struct { BYTE id; DWORD off; WORD len; const char *name; } pages[] = {
        { 0xd8, 0x000000, 0x0042, "descriptor page" },
        { 0xd8, 0x000037, 0x000a, "descriptor tail" },
        { 0xe0, 0x000400, 0x0016, "FaulPixs header" },
        { 0xe0, 0x000400, 0x013e, "FaulPixs table" },
        { 0xe0, 0x000000, 0x0400, "buffer e0/400" },
    };
    size_t i;
    for (i = 0; i < sizeof pages / sizeof pages[0]; i++) {
        BYTE cdb[10], buf[1024];
        int r, j;
        memcpy(cdb, CDB_PAGE, 10);
        cdb[2] = pages[i].id;
        cdb[3] = (BYTE)(pages[i].off >> 16);
        cdb[4] = (BYTE)(pages[i].off >> 8);
        cdb[5] = (BYTE)(pages[i].off & 0xff);
        cdb[7] = (BYTE)(pages[i].len >> 8);
        cdb[8] = (BYTE)(pages[i].len & 0xff);
        memset(buf, 0, sizeof buf);
        r = srb_exec(cdb, 10, SRB_DIR_IN, buf, pages[i].len, TIMEOUT_MS);
        if (r < 0) { printf("%s: %s\n", pages[i].name, srb_errstr(r)); continue; }
        if (!verbose) continue;
        printf("%s (%02x/%06lx, %u bytes):", pages[i].name,
               pages[i].id, (unsigned long)pages[i].off, pages[i].len);
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

/* ReadExt n blocks of blklen into f (NULL = discard). INQUIRY every 400
 * blocks as FlexColor does mid-flood. Returns blocks read. */
static long read_blocks(long n, WORD blklen, FILE *f)
{
    BYTE cdb[10], inq[96];
    BYTE *buf = malloc(blklen);
    long b;
    if (!buf) return 0;
    memcpy(cdb, CDB_GRAB, 10);
    cdb[7] = (BYTE)(blklen >> 8);
    cdb[8] = (BYTE)(blklen & 0xff);
    for (b = 0; b < n; b++) {
        int r = srb_exec(cdb, 10, SRB_DIR_IN, buf, blklen, TIMEOUT_MS);
        if (r < 0) {
            printf("\nread %ld: %s\n", b, srb_errstr(r));
            break;
        }
        if (f && fwrite(buf, 1, (size_t)r, f) != (size_t)r) break;
        if (b && b % 400 == 0)
            srb_exec(CDB_INQUIRY, 6, SRB_DIR_IN, inq, sizeof inq, TIMEOUT_MS);
        if (f && n > 20 && b % 200 == 0)
            printf("  %ld/%ld\r", b, n), fflush(stdout);
    }
    free(buf);
    return b;
}

/* Preview, as FlexColor's menu-driven preview (session 4, seq 279-60400):
 * page burst -> scan descriptor -> settle -> 200 + 50 calibration reads ->
 * poll for 1060 -> ReadExt flood -> end descriptor x2 -> park.
 * n = blocks to keep (full preview is 59592 x 3996 B = 238 MB). */
static int cmd_scan(long blocks, const char *out)
{
    FILE *f;
    long got;
    int v = -1;
    DWORD start;

    printf("page burst\n");
    read_pages(0);
    printf("scan descriptor (pos %ld)\n", desc_pos(SCAN_WIN));
    if (send_win(SCAN_WIN, "scan"))
        return 1;
    printf("waiting for carriage/lamp to settle (up to 120 s)\n");
    if (wait_settled(120000)) {
        printf("device never settled; stopping\n");
        cmd_stop();
        return 1;
    }

    printf("calibration reads\n");
    got = read_blocks(200, 3996, NULL);
    poll_once(); poll_once(); poll_once(); poll_once();
    got += read_blocks(50, 3996, NULL);
    printf("  %ld/250 calibration blocks\n", got);

    printf("polling for data-ready (up to 90 s)\n");
    start = GetTickCount();
    for (;;) {
        v = poll_once();
        if (v == 0x1060 || v < 0)
            break;
        if (v != 0x0000 && v != 0x0400)
            printf("  ... %04x %s\n", v, poll_name((unsigned)v));
        if (GetTickCount() - start > 90000) {
            v = -100;
            break;
        }
        Sleep(150);
    }
    if (v != 0x1060) {
        printf("no data-ready (%d); stopping\n", v);
        cmd_stop();
        return 1;
    }

    printf("data ready — reading %ld blocks -> %s\n", blocks, out);
    f = fopen(out, "wb");
    if (!f) { perror(out); cmd_stop(); return 1; }
    got = read_blocks(blocks, 3996, f);
    fclose(f);
    printf("\nwrote %ld x 3996 B -> %s%s\n", got, out,
           got == blocks ? "" : " (INCOMPLETE)");

    printf("end of scan, returning to rest\n");
    cmd_stop();
    return got == blocks ? 0 : 1;
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
            "  park               rest position (FlexColor's shutdown descriptor)\n"
            "  goto <pos>         absolute carriage position, 0..63000000\n"
            "  move <+/-d> [n]    n relative moves of d (max 5000000/step, 50 steps)\n"
            "  stop               end-of-scan descriptor x2, then park\n"
            "  pages              read the standard ReadBuffer pages\n"
            "  grab <n> [len]     n ReadExt blocks (default len 3996) -> grab.raw\n"
            "  scan [n]           preview: n x 3996-B blocks (default 2000) -> scan.raw\n"
            "                     (full preview is 59592 blocks, 238 MB)\n"
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
        } else if (strcmp(cmd, "goto") == 0 && argc > 2) {
            rc = cmd_goto(atol(argv[2]));
        } else if (strcmp(cmd, "busreset") == 0) {
            rc = cmd_busreset();
        } else if (strcmp(cmd, "scanstop") == 0) {
            rc = cmd_scanstop();
        } else if (strcmp(cmd, "stop") == 0) {
            rc = cmd_stop();
        } else if (strcmp(cmd, "pages") == 0) {
            rc = read_pages(1);
        } else if (strcmp(cmd, "grab") == 0 && argc > 2) {
            rc = cmd_grab(atol(argv[2]),
                          argc > 3 ? (WORD)atoi(argv[3]) : 3996, "grab.raw");
        } else if (strcmp(cmd, "scan") == 0) {
            rc = cmd_scan(argc > 2 ? atol(argv[2]) : 2000, "scan.raw");
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
