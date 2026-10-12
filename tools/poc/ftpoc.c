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
 * SAFETY: every descriptor sent is one FlexColor sent, byte for byte. The
 * only fields ever patched are the resolution (BE u32 @10, goto/move on the
 * idle template; the firmware moves CCD + optics for it) and the start line
 * (BE u32 @18, scan -y; FlexColor's own 3f frames differ only there), both
 * clamped to ranges FlexColor used. Park is FlexColor's shutdown descriptor.
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

/* Current resolution field (ppi x 10000 = zoom setting), as far as this process has
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
 * sidecars (flextrace_bufs.bin, cand 0x60, when=0) — NOT typed by hand: an
 * earlier hand copy silently dropped bytes and C zero-padded the [84]
 * array, shifting every field. The typedefs fail the build unless each
 * template is exactly 84 bytes. Regenerate from the captures, never edit. */
/* session 5 (pid 412) seq 73: preview scan @5250000, 3996-B blocks; exposures @56/@60
 * are per-session (session 4 seq 292 differed only in bytes 58-59, 62-63) */
static const BYTE SCAN_WIN[] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4c,0x00,0x00,0x00,0x50,
    0x1b,0xd0,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,
    0x02,0x9a,0x00,0x00,0x12,0xbb,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x0c,0x00,0xe6,0x00,0x01,0x00,0x03,0xe3,0xd7,
    0x00,0x0f,0x52,0xd6,0x00,0x00,0x5f,0xb4,0x00,0x00,0x00,0x02,
    0x00,0x00,0x1e,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0xb4,
};
typedef char SCAN_WIN_is_84[sizeof SCAN_WIN == 84 ? 1 : -1];
/* session 5 (pid 412) seq 64719: end of scan / Stop, sent twice */
static const BYTE END_WIN[] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4c,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0xe6,0x00,0x01,0x00,0x03,0xe3,0xd7,
    0x00,0x0f,0x52,0xd6,0x00,0x00,0x5f,0xb4,0x00,0x00,0x00,0x02,
    0x00,0x00,0x1e,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0xb4,
};
typedef char END_WIN_is_84[sizeof END_WIN == 84 ? 1 : -1];
/* session 4 seq 67521: monitor idle heartbeat @9000000 (template for moves) */
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
/* session 4 seq 67705: FlexColor shutdown descriptor @36000000 (rest position) */
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
/* session 6 (pid 1428) seq 64676: 2000 ppi crop, 1818 px x 2807 lines from line 9314 */
static const BYTE SCAN_2000[] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4c,0x00,0x00,0x01,0x31,
    0x2d,0x00,0x00,0x00,0x01,0x9b,0x00,0x00,0x24,0x62,0x00,0x00,
    0x07,0x1a,0x00,0x00,0x0a,0xf7,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x03,0x00,0xe6,0x00,0x04,0x00,0x04,0x49,0x6f,
    0x00,0x0f,0x37,0xb0,0x00,0x00,0x5f,0xb4,0x00,0x00,0x00,0x02,
    0x00,0x00,0x1e,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0xb4,
};
typedef char SCAN_2000_is_84[sizeof SCAN_2000 == 84 ? 1 : -1];
/* session 6 seq 225610: its end descriptor, sent twice */
static const BYTE END_2000[] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4c,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0xe6,0x00,0x04,0x00,0x04,0x49,0x6f,
    0x00,0x0f,0x37,0xb0,0x00,0x00,0x5f,0xb4,0x00,0x00,0x00,0x02,
    0x00,0x00,0x1e,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0xb4,
};
typedef char END_2000_is_84[sizeof END_2000 == 84 ? 1 : -1];
/* session 6 seq 225620: 1000 ppi crop, 1142 px x 1490 lines from line 6097 */
static const BYTE SCAN_1000[] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4c,0x00,0x00,0x00,0x98,
    0x96,0x80,0x00,0x00,0x00,0x62,0x00,0x00,0x17,0xd1,0x00,0x00,
    0x04,0x76,0x00,0x00,0x05,0xd2,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x06,0x00,0xe6,0x00,0x02,0x00,0x04,0x49,0x6f,
    0x00,0x0f,0x37,0xb0,0x00,0x00,0x5f,0xb4,0x00,0x00,0x00,0x02,
    0x00,0x00,0x1e,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0xb4,
};
typedef char SCAN_1000_is_84[sizeof SCAN_1000 == 84 ? 1 : -1];
/* session 6 seq 265797: its end descriptor, sent twice */
static const BYTE END_1000[] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4c,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0xe6,0x00,0x02,0x00,0x04,0x49,0x6f,
    0x00,0x0f,0x37,0xb0,0x00,0x00,0x5f,0xb4,0x00,0x00,0x00,0x02,
    0x00,0x00,0x1e,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0xb4,
};
typedef char END_1000_is_84[sizeof END_1000 == 84 ? 1 : -1];
/* session 6 seq 265806: 3f strip frame 1 of 6, 630 ppi, 655 px x 992 lines from line 50 */
static const BYTE SCAN_3F[] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4c,0x00,0x00,0x00,0x60,
    0x21,0x60,0x00,0x00,0x01,0x56,0x00,0x00,0x00,0x32,0x00,0x00,
    0x02,0x8f,0x00,0x00,0x03,0xe0,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x06,0x00,0xe6,0x00,0x02,0x00,0x06,0xb2,0x06,
    0x00,0x0d,0xe6,0xca,0x00,0x00,0x5f,0xb4,0x00,0x00,0x00,0x02,
    0x00,0x00,0x1e,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0xb4,
};
typedef char SCAN_3F_is_84[sizeof SCAN_3F == 84 ? 1 : -1];
/* session 6 seq 296339: its end descriptor (all six frames), sent twice */
static const BYTE END_3F[] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x4c,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0xe6,0x00,0x02,0x00,0x06,0xb2,0x06,
    0x00,0x0d,0xe6,0xca,0x00,0x00,0x5f,0xb4,0x00,0x00,0x00,0x02,
    0x00,0x00,0x1e,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0xb4,
};
typedef char END_3F_is_84[sizeof END_3F == 84 ? 1 : -1];

/* Descriptor geometry, all big-endian u32 (session 6: 2000/1000 ppi crops
 * and a 3f strip, checked against the TIFFs FlexColor saved):
 *  @10 resolution x 10000 (5250000 = 525 ppi preview, 63000000 = 6300 ppi
 *      max). The firmware moves CCD + optics (zoom/focus) and sets the drum
 *      speed for it from its flashed motor table (FP...txt CCD/OPTIC/DRUM).
 *  @14 first pixel across the film     @22 pixels per line (block = px*6)
 *  @18 first line along the strip      @26 number of lines
 * The six 3f frames differ ONLY in @18; each end descriptor is its scan
 * descriptor with bytes 8-52 zeroed. Never command outside the observed
 * range. */
#define POS_OFF    10
#define POS_MAX    63000000L
#define F_X        14
#define F_Y        18
#define F_PX       22
#define F_LINES    26
#define F_BIN      51   /* CCD pixels summed per output pixel: 12 preview,
                           6 for 1000 ppi and 3f, 3 for 2000 ppi; byte 55
                           is always 12 / bin. Output pixel j covers CCD
                           pixels (x + j) * bin + 144 ... + bin - 1 */

static long be32(const BYTE *d, int off)
{
    return ((long)d[off] << 24) | ((long)d[off + 1] << 16) |
           ((long)d[off + 2] << 8) | (long)d[off + 3];
}

static void put_be32(BYTE *d, int off, long v)
{
    d[off]     = (BYTE)(v >> 24);
    d[off + 1] = (BYTE)(v >> 16);
    d[off + 2] = (BYTE)(v >> 8);
    d[off + 3] = (BYTE)v;
}

/* Captured scan setups. Each pairs a scan descriptor with the end
 * descriptor FlexColor sent after it. */
static const struct preset {
    const char *name;
    const BYTE *scan, *end;
    const char *what;
} PRESETS[] = {
    { "preview", SCAN_WIN,  END_WIN,  "525 ppi, whole holder" },
    { "2000",    SCAN_2000, END_2000, "2000 ppi, one frame (session 6 crop)" },
    { "1000",    SCAN_1000, END_1000, "1000 ppi, one frame (session 6 crop)" },
    { "3f",      SCAN_3F,   END_3F,   "3f raw, 630 ppi, autoframe frame" },
};
/* 3f autoframe start lines for frames 1-6 of the 24x36x6 holder */
static const long F3_Y[6] = { 50, 967, 1910, 2852, 3795, 4737 };
/* holder length in lines at 525 ppi (preview @26); scales with resolution */
#define HOLDER_LINES_525 4795L

static long desc_pos(const BYTE *d)
{
    return be32(d, POS_OFF);
}

static void set_pos(BYTE *d, long pos)
{
    put_be32(d, POS_OFF, pos);
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
        if (v != last)
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

/* End-of-scan descriptor x2: what FlexColor sends both after the last
 * ReadExt of a full preview and when the user stops a preview halfway
 * (session 5, pid 412 seq 64719 / 80710). FlexColor does not park after. */
static int cmd_stop(void)
{
    printf("end of scan (END_WIN x2)\n");
    return send_win(END_WIN, "end") || send_win(END_WIN, "end");
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

/* Save the device's Faul Pixs table (e0 @ 0x400, 318 B; ASCII, per
 * channel a "100 0" line then "<ccd pixel>\t<+999 | -n>" lines) so the
 * host can interpolate the defective CCD pixels, as FlexColor does. */
static void save_faul(const char *path)
{
    BYTE cdb[10], buf[0x13e];
    FILE *f;
    int r;
    memcpy(cdb, CDB_PAGE, 10);
    cdb[2] = 0xe0;
    cdb[4] = 0x04;
    cdb[7] = 0x01;
    cdb[8] = 0x3e;
    r = srb_exec(cdb, 10, SRB_DIR_IN, buf, sizeof buf, TIMEOUT_MS);
    if (r < 0) { printf("Faul Pixs: %s\n", srb_errstr(r)); return; }
    f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fwrite(buf, 1, (size_t)r, f);
    fclose(f);
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

/* Pipelined flood: keep DEPTH overlapped ReadExt requests queued so the
 * port driver issues them back-to-back (FlexColor reaches ~1700 reads/s;
 * one-at-a-time reads top out at ~460/s).
 * Most reads come back EMPTY (SRB 0x12 = underrun, 0 bytes): the CCD
 * delivers ~130 lines/s and the flood is really a polling loop. Reads
 * continue until n full blocks are kept.
 * Each slot owns its SRB (the driver writes status/sense back through
 * pointers into it), buffer, event and OVERLAPPED. Completes in order;
 * returns the clean in-order prefix written to f. */
#define DEPTH 8

static long read_flood(long n, WORD blklen, FILE *f)
{
    struct slot {
        BYTE srb[0x70];
        OVERLAPPED ov;
        BYTE *buf;
        int busy;
    } *sl;
    BYTE cdb[10];
    long issued = 0, done = 0, kept = 0, shorts = 0, short_bytes = 0;
    DWORD last_full;
    int i, err = 0;
    DWORD t0 = GetTickCount(), dt;

    sl = calloc(DEPTH, sizeof *sl);
    if (!sl) return 0;
    memcpy(cdb, CDB_GRAB, 10);
    cdb[7] = (BYTE)(blklen >> 8);
    cdb[8] = (BYTE)(blklen & 0xff);
    for (i = 0; i < DEPTH; i++) {
        sl[i].buf = malloc(blklen);
        sl[i].ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!sl[i].buf || !sl[i].ov.hEvent) { err = 1; goto out; }
    }

    last_full = GetTickCount();
    while (kept < n) {
        /* top up the queue: empty reads are normal, so the number of reads
         * is open-ended; stop issuing once the queue could fill n */
        while (!err && kept + (issued - done) < n && issued - done < DEPTH) {
            struct slot *q = &sl[issued % DEPTH];
            BYTE *srb = q->srb;
            memset(srb, 0, sizeof q->srb);
            *(DWORD *)(srb + 0x04) = 44;
            srb[0x08] = SRB_DIR_IN;
            srb[0x0c] = 10;
            srb[0x0d] = 0x0e;
            *(DWORD *)(srb + 0x10) = blklen;
            memcpy(srb + 0x14, cdb, 10);
            *(DWORD *)(srb + 0x24) = (DWORD)(ULONG_PTR)(srb + 0x2c);
            *(DWORD *)(srb + 0x28) = (DWORD)(ULONG_PTR)(srb + 0x34);
            ResetEvent(q->ov.hEvent);
            q->ov.Internal = q->ov.InternalHigh = 0;
            if (!DeviceIoControl(g_dev, IOCTL_SRB, srb, 44, q->buf, blklen,
                                 NULL, &q->ov) &&
                GetLastError() != ERROR_IO_PENDING) {
                printf("\nflood issue %ld: GLE=%lu\n", issued, GetLastError());
                err = 1;
                break;
            }
            q->busy = 1;
            issued++;
        }
        if (done == issued)
            break;
        if (GetTickCount() - last_full > 5000) {
            printf("\nno full block for 5 s; scan data ended?\n");
            err = 1;
        }
        /* reap the oldest */
        {
            struct slot *q = &sl[done % DEPTH];
            DWORD got = 0;
            if (WaitForSingleObject(q->ov.hEvent, TIMEOUT_MS) != WAIT_OBJECT_0) {
                printf("\nflood read %ld: timed out\n", done);
                err = 1;
                break;
            }
            q->busy = 0;
            if (!GetOverlappedResult(g_dev, &q->ov, &got, TRUE)) {
                DWORD gle = GetLastError();
                /* SRB_STATUS_DATA_OVERRUN (0x12) also means UNDERRUN on
                 * Windows: a short transfer, the host is ahead of the CCD.
                 * Keep the bytes that did arrive and carry on. */
                if (gle == ERROR_MORE_DATA && (q->srb[0x2c] & 0x3f) == 0x12) {
                    shorts++;
                    short_bytes += got;
                    if (f && got && fwrite(q->buf, 1, got, f) != got) err = 1;
                    if (got)
                        printf("\nflood read %ld: partial, %lu bytes\n",
                               done, (unsigned long)got);
                } else {
                    printf("\nflood read %ld: GLE=%lu srb=%02x\n", done,
                           gle, q->srb[0x2c]);
                    err = 1;
                }
            } else if (!err) {
                if (f && fwrite(q->buf, 1, got, f) != got) err = 1;
                else kept++;
                last_full = GetTickCount();
            }
            done++;
            if (!err && kept && kept % 500 == 0 && got == blklen)
                printf("  %ld/%ld\r", kept, n), fflush(stdout);
        }
    }
out:
    if (err) {
        CancelIo(g_dev);
        for (i = 0; i < DEPTH; i++)
            if (sl[i].busy && sl[i].ov.hEvent)
                WaitForSingleObject(sl[i].ov.hEvent, TIMEOUT_MS);
    }
    for (i = 0; i < DEPTH; i++) {
        free(sl[i].buf);
        if (sl[i].ov.hEvent) CloseHandle(sl[i].ov.hEvent);
    }
    free(sl);
    dt = GetTickCount() - t0;
    printf("  [flood: %ld reads in %lu ms; %ld full, %ld short (%ld bytes)]\n",
           done, (unsigned long)dt, kept, shorts, short_bytes);
    return kept;
}

/* End descriptor x2, as FlexColor after the last line or on Stop. */
static int send_end(const struct preset *p)
{
    printf("end of scan (%s end descriptor x2)\n", p->name);
    return send_win(p->end, "end") || send_win(p->end, "end");
}

/* Poll (265 ms, like FlexColor) until the carriage move that follows a
 * calibration batch is over: idle, and at least min_ms since the batch. */
static int wait_move(DWORD min_ms, DWORD timeout_ms, const char *what)
{
    DWORD t0 = GetTickCount();
    int v, last = -1;
    for (;;) {
        Sleep(265);
        v = poll_once();
        if (v < 0) { printf("poll: %s\n", srb_errstr(v)); return 1; }
        if (v != last)
            printf("  ... %04x %s (%lu ms)\n", v, poll_name((unsigned)v),
                   (unsigned long)(GetTickCount() - t0));
        last = v;
        if (v == 0x0000 && GetTickCount() - t0 >= min_ms)
            return 0;
        if (GetTickCount() - t0 > timeout_ms) {
            printf("%s: carriage never went idle\n", what);
            return 1;
        }
    }
}

/* One scan, as FlexColor does it for previews, crops and each 3f frame
 * (sessions 5-6): page burst -> scan descriptor -> hold (FlexColor 0.8-8 s
 * warm, 32 s cold) -> 200 calibration lines -> carriage move -> 50 lines
 * -> move to the start line -> line flood -> end descriptor x2.
 *
 * The 250 calibration lines are reference data for the HOST: FlexColor's
 * CCalibrateLineHandler/CLineHandlerCorrection build the per-pixel
 * correction from them (the device does not correct the CCD). They go to
 * <out>.cal; geometry goes to <out>.txt.
 *
 * y >= 0 moves the window along the strip (only @18 changes, exactly as
 * between FlexColor's 3f frames). keep > 0 stops after that many lines
 * with the end descriptor, as FlexColor's Stop does. */
static int cmd_scan(const struct preset *p, long y, long keep,
                    DWORD hold_ms, const char *out)
{
    BYTE desc[84];
    FILE *f, *fc;
    long got, px, lines, res, maxl;
    WORD blklen;
    int v, last = -1;
    DWORD start;
    char meta[260];

    memcpy(desc, p->scan, 84);
    res = be32(desc, POS_OFF);
    px = be32(desc, F_PX);
    lines = be32(desc, F_LINES);
    maxl = (long)((double)HOLDER_LINES_525 * res / 5250000.0);
    if (y >= 0) {
        if (y + lines > maxl) {
            fprintf(stderr, "start line %ld + %ld lines is past the holder end "
                            "(%ld lines at %ld ppi)\n", y, lines, maxl,
                    res / 10000);
            return 1;
        }
        put_be32(desc, F_Y, y);
    }
    if (keep <= 0 || keep > lines)
        keep = lines;
    blklen = (WORD)(px * 6);
    printf("%s: %s\n  %ld ppi, %ld px from %ld x %ld lines from %ld, "
           "%u-byte lines; keeping %ld\n", p->name, p->what, res / 10000, px,
           be32(desc, F_X), lines, be32(desc, F_Y), blklen, keep);

    printf("page burst\n");
    read_pages(0);
    sprintf(meta, "%s.faul", out);
    save_faul(meta);
    printf("scan descriptor\n");
    {
        int r = win_exchange(desc, 0);
        if (r < 0) { printf("SetWindow (scan): %s\n", srb_errstr(r)); return 1; }
    }
    start = GetTickCount();
    if (wait_settled(120000)) {
        printf("device never settled; stopping\n");
        send_end(p);
        return 1;
    }
    printf("holding %lu ms before calibration\n", (unsigned long)hold_ms);
    while (GetTickCount() - start < hold_ms) {
        v = poll_once();
        if (v < 0) { printf("poll: %s\n", srb_errstr(v)); send_end(p); return 1; }
        if (v != last)
            printf("  ... %04x %s (%lu ms)\n", v, poll_name((unsigned)v),
                   (unsigned long)(GetTickCount() - start));
        last = v;
        Sleep(265);
    }

    /* Calibration: count FULL lines — at high resolution many reads come
     * back empty (FlexColor issues ~300 reads for the 200 at 2000 ppi). */
    sprintf(meta, "%s.cal", out);
    fc = fopen(meta, "wb");
    if (!fc) { perror(meta); send_end(p); return 1; }
    printf("calibration: 200 lines\n");
    got = read_flood(200, blklen, fc);
    if (got < 200) {
        printf("calibration got %ld/200 lines; stopping\n", got);
        fclose(fc);
        send_end(p);
        return 1;
    }
    /* carriage move between the batches (~550 ms); reading during it
     * means the scan never starts */
    if (wait_move(1000, 30000, "between calibration batches")) {
        fclose(fc);
        send_end(p);
        return 1;
    }
    printf("calibration: 50 lines\n");
    got = read_flood(50, blklen, fc);
    fclose(fc);
    if (got < 50) {
        printf("calibration got %ld/50 lines; stopping\n", got);
        send_end(p);
        return 1;
    }

    /* move to the start line (2.5 s preview, ~7.5 s for a crop deep in
     * the holder); the flood starts on busy -> idle */
    printf("waiting for carriage at the start line (up to 90 s)\n");
    {
        int seen_busy = 0;
        start = GetTickCount();
        last = -1;
        for (;;) {
            v = poll_once();
            if (v < 0) break;
            if (v != last)
                printf("  ... %04x %s (%lu ms)\n", v, poll_name((unsigned)v),
                       (unsigned long)(GetTickCount() - start));
            last = v;
            if (v == 0x1060 || (v == 0x0000 && seen_busy))
                break;
            if (v != 0x0000)
                seen_busy = 1;
            if (GetTickCount() - start > 90000) {
                v = -100;
                break;
            }
            Sleep(150);
        }
    }
    if (v != 0x1060 && v != 0x0000) {
        printf("carriage never reached the start line (%d); stopping\n", v);
        send_end(p);
        return 1;
    }

    printf("reading %ld lines -> %s\n", keep, out);
    f = fopen(out, "wb");
    if (!f) { perror(out); send_end(p); return 1; }
    got = read_flood(keep, blklen, f);
    fclose(f);
    printf("\nwrote %ld x %u B -> %s%s\n", got, blklen, out,
           got == keep ? "" : " (INCOMPLETE)");
    send_end(p);

    sprintf(meta, "%s.txt", out);
    f = fopen(meta, "w");
    if (f) {
        fprintf(f, "preset=%s\nppi=%ld\npx=%ld\nx=%ld\nlines=%ld\ny=%ld\n"
                   "bin=%u\nblklen=%u\nkept=%ld\ncal_lines=250\n",
                p->name, res / 10000, px, be32(desc, F_X), lines,
                be32(desc, F_Y), desc[F_BIN], blklen, got);
        fclose(f);
    }
    return got == keep ? 0 : 1;
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

static int scan_args(int argc, char **argv)
{
    const struct preset *p = &PRESETS[0];
    long keep = 0, y = -1;
    int frame = -1, i, rc = 0;
    DWORD hold = 10000;
    const char *out = "scan.raw";
    char name[32];

    for (i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] && !a[2] && i + 1 < argc) {
            const char *v = argv[++i];
            switch (a[1]) {
            case 'n': keep = atol(v); break;
            case 'y': y = atol(v); break;
            case 'f': frame = atoi(v); break;
            case 'h': hold = (DWORD)atol(v) * 1000; break;
            case 'o': out = v; break;
            default: fprintf(stderr, "scan: unknown option %s\n", a); return 2;
            }
        } else {
            size_t k;
            for (k = 0; k < sizeof PRESETS / sizeof PRESETS[0]; k++)
                if (strcmp(a, PRESETS[k].name) == 0) break;
            if (k == sizeof PRESETS / sizeof PRESETS[0]) {
                fprintf(stderr, "scan: unknown preset %s\n", a);
                return 2;
            }
            p = &PRESETS[k];
        }
    }
    if (frame >= 0 && strcmp(p->name, "3f") != 0) {
        fprintf(stderr, "scan: -f only applies to the 3f preset\n");
        return 2;
    }
    if (frame > 6) { fprintf(stderr, "scan: frame 1-6 (0 = all)\n"); return 2; }
    if (frame > 0)
        return cmd_scan(p, F3_Y[frame - 1], keep, hold, out);
    if (frame == 0) {
        /* FlexColor's 3f strip: one full scan cycle per frame */
        for (i = 0; i < 6 && rc == 0; i++) {
            sprintf(name, "frame%d.raw", i + 1);
            printf("\n=== frame %d/6\n", i + 1);
            rc = cmd_scan(p, F3_Y[i], keep, hold, name);
            hold = 2500;     /* lamp warm after the first frame */
        }
        return rc;
    }
    return cmd_scan(p, y, keep, hold, out);
}

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
            "  goto <ppi*10000>   CCD + optics to that resolution's zoom, 0..63000000\n"
            "  move <+/-d> [n]    n relative zoom steps of d (max 5000000, 50 steps)\n"
            "  stop               end-of-scan descriptor x2 (FlexColor end/Stop)\n"
            "  pages              read the standard ReadBuffer pages\n"
            "  grab <n> [len]     n ReadExt blocks (default len 3996) -> grab.raw\n"
            "  scan [preset] [-n lines] [-y line] [-f frame] [-h hold_s] [-o file]\n"
            "                     presets: preview (525 ppi), 2000, 1000, 3f (630 ppi)\n"
            "                     -n stop after n lines (default: the whole window)\n"
            "                     -y start line along the strip (holder-clamped)\n"
            "                     -f 3f frame 1-6, or 0 = whole strip (frameN.raw)\n"
            "                     -h hold before calibration, s (default 10; 35 cold)\n"
            "                     -> scan.raw (+ .cal 250 calibration lines, .txt)\n"
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
            rc = scan_args(argc - 2, argv + 2);
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
