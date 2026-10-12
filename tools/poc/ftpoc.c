/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftpoc.c — FlexTight II (Precision II) proof-of-concept console tool.
 *
 * Talks to the scanner directly through the Microsoft scsiscan.sys driver,
 * replicating the exact 44-byte SRB + ioctl 0x190012 transport that
 * FlexColor.exe uses (validated against 67k+ hooked DeviceIoControl calls,
 * sessions 2-6).
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
 * idle template) and the start line (BE u32 @18, scan -y; FlexColor's own
 * 3f frames differ only there), both clamped to ranges FlexColor used, plus
 * the window and lens/CCD fields of synthesized crops (crop) and the -p/-l
 * overrides, each within the values FlexColor sent. Park is FlexColor's
 * shutdown descriptor.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ft_calib.h"
#include "ft_detect.h"
#include "ft_focus.h"
#include "ft_geom.h"
#include "ft_holder.h"
#include "ft_job.h"
#include "ft_png.h"

#define IOCTL_SRB 0x190012

#define SRB_DIR_OUT 0x80        /* host -> device */
#define SRB_DIR_IN  0x40        /* device -> host */

#define TIMEOUT_MS  5000

/* ---------------------------------------------------------------- transport */

static HANDLE g_dev = INVALID_HANDLE_VALUE;
static HANDLE g_evt = NULL;

/* Current resolution field (ppi x 10000), as far as this process has
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
/* session 5 (pid 412) seq 73: preview scan @5250000, 3996-B blocks; the CCD/lens
 * positions @56/@60 are per-session (session 4 seq 292 differed only in bytes
 * 58-59, 62-63) */
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
 *      max): line pitch along the strip.
 *  @14 first pixel across the film     @22 pixels per line (block = px*6)
 *  @18 first line along the strip      @26 number of lines
 *  @56 CCD position, @60 lens position, @64 drum position (motor steps,
 *      the CCD/OPTIC/DRUM columns of the scanner's FP<serial>.txt). The host
 *      picks them; they set magnification and focus, see F_CCD.
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

/* Lens and CCD positions (BE u32, motor steps; 2026-10-12 sweep): moving
 * either one changes magnification and focus, and with them the white
 * level (fewer CCD pixels per mm of film collect more light). Neither is an
 * exposure: the same pair gives the same white level at 1000-6300 ppi,
 * whatever the line rate. FlexColor sends one pair per optical scale,
 * ppi x bin = CCD pixels per inch of film:
 *   6300  254935 / 1004246   preview (session 4: 254123 / 1004988)
 *   6000  280943 /  997296   1000 and 2000 ppi crops (square pixels)
 *   3780  438790 /  911050   3f
 *   3600  440113 /  895204   idle and exit descriptors
 * and 1020000 / 1021000 in session 4. The two seem to share a rail: the
 * CCD stays below the lens (see parse_pos). The limits are FT_CCD_MIN..
 * FT_OPTIC_MAX in libft/ft_focus.h. */
#define F_CCD      56
#define F_OPTIC    60
static long g_ccd = -1, g_optic = -1;     /* -1: keep the template's */

/* With the CCD past the lens (1020000 / 997296) the calibration lines
 * flickered, the white batch never came and the carriage did not move to
 * the start line. FlexColor always keeps CCD < lens. */
static int pos_ok(long a, long b)
{
    if (ft_positions_ok(a, b))
        return 1;
    fprintf(stderr, "CCD/lens %ld/%ld outside what FlexColor sent (CCD "
                    "%ld..%ld, lens %ld..%ld, CCD below lens)\n", a, b,
            FT_CCD_MIN, FT_CCD_MAX, FT_OPTIC_MIN, FT_OPTIC_MAX);
    return 0;
}

static int parse_pos(const char *v)
{
    long a, b;
    if (sscanf(v, "%ld,%ld", &a, &b) != 2) {
        fprintf(stderr, "-p wants <ccd>,<lens>\n");
        return 1;
    }
    if (!pos_ok(a, b))
        return 1;
    g_ccd = a;
    g_optic = b;
    return 0;
}

/* Lamp level (u8 @76), the only light lever (2026-10-12): FlexColor's
 * maintenance Monitor sends 0..255 here as its lamp spinner moves; its
 * scan descriptors send 0, its idle/exit descriptors 1. 0 keeps the
 * current level, any other value sets it and it sticks across scans.
 * White level vs lamp (1000 ppi crop optics, R): 1 -> 47 %, 32 -> 67 %,
 * 64 -> 84 %, 80 -> 92 %, 96..255 -> 100 % (flat), no hysteresis; dimmer
 * is slightly redder. Nothing raises the light above 96; integration
 * time is not a descriptor field. */
#define F_LAMP     76
static int g_lamp = -1;                   /* -1: keep the template's */

static int parse_lamp(const char *v)
{
    char *end;
    long l = strtol(v, &end, 10);
    if (*end || !(l >= 0 && l <= 255)) {
        fprintf(stderr, "-l wants a lamp level 0..255\n");
        return 1;
    }
    g_lamp = (int)l;
    return 0;
}

/* -h: hold before calibration, 0..120 s */
static int parse_hold(const char *v, DWORD *ms)
{
    char *end;
    long s = strtol(v, &end, 10);
    if (end == v || *end || !(s >= 0 && s <= 120)) {
        fprintf(stderr, "-h wants a hold of 0..120 s\n");
        return 1;
    }
    *ms = (DWORD)s * 1000;
    return 0;
}

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
    if (!(pos >= 0 && pos <= POS_MAX)) {
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
    if (!(delta != 0 && delta >= -5000000L && delta <= 5000000L)) {
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
    static BYTE xscan[84], xend[84];
    static struct preset xp;
    BYTE desc[84];
    FILE *f, *fc;
    long got, px, lines, res, maxl;
    WORD blklen;
    int v, last = -1;
    DWORD start;
    char meta[260];

    if (g_ccd >= 0 || g_lamp >= 0) {
        /* the end descriptor carries these too (bytes 53+) */
        memcpy(xscan, p->scan, 84);
        if (g_ccd >= 0) {
            put_be32(xscan, F_CCD, g_ccd);
            put_be32(xscan, F_OPTIC, g_optic);
        }
        if (g_lamp >= 0)
            xscan[F_LAMP] = (BYTE)g_lamp;
        memcpy(xend, xscan, 84);
        memset(xend + 8, 0, 53 - 8);
        xp.name = p->name;
        xp.what = p->what;
        xp.scan = xscan;
        xp.end = xend;
        p = &xp;
    }
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
           "%u-byte lines; keeping %ld; CCD/lens %ld/%ld, lamp %u\n",
           p->name, p->what, res / 10000, px, be32(desc, F_X), lines,
           be32(desc, F_Y), blklen, keep, be32(desc, F_CCD),
           be32(desc, F_OPTIC), desc[F_LAMP]);

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
                   "bin=%u\nblklen=%u\nkept=%ld\ncal_lines=250\n"
                   "ccd=%ld\noptic=%ld\nlamp=%u\n",
                p->name, res / 10000, px, be32(desc, F_X), lines,
                be32(desc, F_Y), desc[F_BIN], blklen, got,
                be32(desc, F_CCD), be32(desc, F_OPTIC), desc[F_LAMP]);
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

/* ------------------------------------------------------- files and images */

/* Whole file into a malloc'd buffer (NULL on any error; *size set). */
static void *load_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    void *buf = NULL;
    long n;
    *size = 0;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 &&
        fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc(n > 0 ? (size_t)n : 1);
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        }
        if (buf)
            *size = n;
    }
    fclose(f);
    return buf;
}

/* key=value from a scan's .txt; -1 if absent */
static long meta_long(const char *scan, const char *key)
{
    char path[300], line[128];
    size_t n = strlen(key);
    long v = -1;
    FILE *f;
    if (snprintf(path, sizeof path, "%s.txt", scan) >= (int)sizeof path)
        return -1;
    f = fopen(path, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, key, n) == 0 && line[n] == '=')
            v = atol(line + n + 1);
    fclose(f);
    return v;
}

/* Load <scan>, <scan>.cal (and .faul when x >= 0: x = the scan's first
 * output pixel) as a flat-fielded, aligned image: returns lines*px*3
 * floats (caller frees), *lines_out set; NULL on failure. */
static float *load_corrected(const char *scan, int px, int bin, int x,
                             int *lines_out)
{
    char path[300];
    long nraw = 0, ncal = 0, nfaul = 0, lines, cl;
    unsigned short *raw = NULL, *cal = NULL;
    float *dark = NULL, *white = NULL, *t = NULL;
    ft_err e;

    if (px < 1 || px > 20000 || bin < 1 || bin > 12) {
        fprintf(stderr, "%s: bad geometry (px %d, bin %d)\n", scan, px, bin);
        return NULL;
    }
    if (snprintf(path, sizeof path, "%s.cal", scan) >= (int)sizeof path) {
        fprintf(stderr, "%s: name too long\n", scan);
        return NULL;
    }
    raw = load_file(scan, &nraw);
    cal = load_file(path, &ncal);
    dark = malloc((size_t)px * 3 * sizeof *dark);
    white = malloc((size_t)px * 3 * sizeof *white);
    lines = nraw / ((long)px * 6);
    cl = ncal / ((long)px * 6);
    if (!raw || !cal || !dark || !white || lines < 1 || cl < 1 ||
        lines > 1000000) {
        fprintf(stderr, "%s: cannot load scan data\n", scan);
        goto out;
    }
    if (ft_split_cal(cal, (int)cl, px, dark, white, &e)) {
        fprintf(stderr, "%s: %s\n", scan, e.msg);
        goto out;
    }
    t = malloc((size_t)lines * px * 3 * sizeof *t);
    if (!t) {
        fprintf(stderr, "%s: out of memory\n", scan);
        goto out;
    }
    ft_flatfield(raw, (int)lines, px, dark, white, t);
    if (x >= 0) {
        char *faul;
        snprintf(path, sizeof path, "%s.faul", scan);
        faul = load_file(path, &nfaul);
        if (faul) {
            int pixs[3][FT_FAUL_MAX], cnt[3];
            ft_faul_parse(faul, (size_t)nfaul, pixs, cnt);
            free(faul);
            if (ft_faul_fix(t, (int)lines, px, pixs, cnt, x, bin)) {
                fprintf(stderr, "%s: defective pixel fix failed\n", scan);
                free(t);
                t = NULL;
                goto out;
            }
        } else {
            printf("note: no %s; defective pixels left as they are\n", path);
        }
    }
    *lines_out = ft_align_rgb(t, (int)lines, px, 12 / bin);
out:
    free(raw);
    free(cal);
    free(dark);
    free(white);
    return t;
}

/* sharpness of the green channel of a lines x px RGB image; NaN when out
 * of memory (ft_peak refuses non-finite values) */
static double green_sharpness(const float *t, int lines, int px)
{
    size_t i, n = (size_t)lines * px;
    float *gch = malloc((n ? n : 1) * sizeof *gch);
    double s;
    if (!gch)
        return NAN;
    for (i = 0; i < n; i++)
        gch[i] = t[i * 3 + 1];
    s = ft_sharpness(gch, px, lines);
    free(gch);
    return s;
}

/* ------------------------------------------------------------------- driver */

/* Synthesized crop at any resolution: the 2000 ppi crop descriptor with
 * only the decoded fields replaced (session 6):
 *   @10 ppi x 10000, @14/@18 origin and @22/@26 size in output pixels at
 *   that ppi (square pixels; origin fixed on the holder: both captured
 *   crops centre the frame at 16.8 mm across), @51 bin = the largest divisor
 *   of 12 with ppi x bin <= 6300 (preview 12, 1000 -> 6, 2000 -> 3,
 *   1200 -> 4), @55 = 12 / bin.
 * CCD/lens @56/@60: the in-focus pair for the optical scale ppi x bin
 * (ft_optics_for on the focus curve, see load_lens), or -p.
 * mm in, clamped to the area the captured scans covered. */
static BYTE CROP_SCAN[84], CROP_END[84];

/* Focus: thin lens on one rail (libft/ft_focus.h), fitted to the sharpest
 * point of eight CCD focus brackets at 3807..6230 px/inch (2026-10-12).
 * FlexColor's own crop/preview pairs sit 9000-10000 CCD steps short of
 * that peak on this scanner, and are visibly softer, so crop does not use
 * them. The curve: focus.txt in the current directory if present (written
 * by calibrate-focus), else this scanner's fit compiled into libft. */
static ft_lens g_lens;

static void load_lens(void)
{
    FILE *f = fopen("focus.txt", "r");
    ft_err e;
    g_lens = ft_lens_default;
    if (!f)
        return;
    if (ft_lens_read(&g_lens, f, &e)) {
        fprintf(stderr, "focus.txt: %s; using the built-in curve\n", e.msg);
        g_lens = ft_lens_default;
    } else {
        printf("focus.txt: film=%.0f w=%.0f fl=%.0f p=%.4e\n", g_lens.film,
               g_lens.w, g_lens.fl, g_lens.p);
    }
    fclose(f);
}

/* One synthesized crop. ccd/optic >= 0: use them (after the safety check);
 * otherwise the focus curve for ppi x bin, with ccd_offset added to the
 * CCD (lens unchanged). Returns 0 ok, CROP_REFUSED when a check refused
 * the request before anything was sent, 1 when the scan failed. */
#define CROP_REFUSED 2
struct crop_req {
    double ppi, x, y, w, h;      /* mm */
    long ccd, optic, ccd_offset;
    long keep;
    DWORD hold;
    const char *out;
};

static int crop_scan(const struct crop_req *r)
{
    static const struct preset crop = { "crop", CROP_SCAN, CROP_END,
                                        "synthesized crop" };
    long bin, ccd = r->ccd, optic = r->optic;
    double scale, cx;
    int i, rc;

    /* every range check is written so that NaN fails it */
    if (!isfinite(r->ppi) || !isfinite(r->x) || !isfinite(r->y) ||
        !isfinite(r->w) || !isfinite(r->h)) {
        fprintf(stderr, "crop: non-finite window\n");
        return CROP_REFUSED;
    }
    if (!(r->ppi >= 525 && r->ppi <= 6300)) {
        fprintf(stderr, "crop: ppi 525..6300\n");
        return CROP_REFUSED;
    }
    /* captured coverage: across 0.1-32.3 mm (preview), along 0-232 mm */
    if (!(r->x >= 0 && r->w > 0 && r->x + r->w <= 32.0 && r->y >= 0 &&
          r->h > 0 && r->y + r->h <= 231.0)) {
        fprintf(stderr, "crop: window outside the holder area FlexColor "
                        "scanned (x+w <= 32 mm, y+h <= 231 mm)\n");
        return CROP_REFUSED;
    }
    /* largest bin dividing 12 (byte 55 = 12 / bin in every capture) that
     * keeps the optical scale ppi x bin within 6300 */
    bin = ft_bin_for_ppi(r->ppi);
    if (bin < 0)
        return CROP_REFUSED;
    scale = r->ppi * bin;
    if ((long)(r->w / 25.4 * r->ppi + 0.5) * 6 > 60000) {
        fprintf(stderr, "crop: line too long for one ReadExt\n");
        return CROP_REFUSED;
    }
    if (ccd < 0) {
        if (ft_optics_for(&g_lens, scale, &ccd, &optic)) {
            fprintf(stderr, "crop: %g ppi x bin %ld = optical scale %.0f; "
                            "focus is known for %.0f..%.0f (or give -p)\n",
                    r->ppi, bin, scale, FT_SCALE_MIN, FT_SCALE_MAX);
            return CROP_REFUSED;
        }
        ccd += r->ccd_offset;
    }
    if (!pos_ok(ccd, optic))
        return CROP_REFUSED;
    /* mm across are measured at the 6000 scale; other scales magnify
     * about the optical axis */
    cx = ft_mm_to_ccd(r->x, scale);
    if (!(cx >= FT_CCD_FIRST &&
          cx + r->w / 25.4 * scale <= FT_CCD_FIRST + FT_CCD_PIXELS)) {
        fprintf(stderr, "crop: window runs off the CCD at this scale\n");
        return CROP_REFUSED;
    }
    memcpy(CROP_SCAN, SCAN_2000, 84);
    put_be32(CROP_SCAN, POS_OFF, (long)(r->ppi * 10000 + 0.5));
    put_be32(CROP_SCAN, F_X, (long)((cx - FT_CCD_FIRST) / bin + 0.5));
    put_be32(CROP_SCAN, F_Y, (long)(r->y / 25.4 * r->ppi + 0.5));
    put_be32(CROP_SCAN, F_PX, (long)(r->w / 25.4 * r->ppi + 0.5));
    put_be32(CROP_SCAN, F_LINES, (long)(r->h / 25.4 * r->ppi + 0.5));
    put_be32(CROP_SCAN, F_CCD, ccd);
    put_be32(CROP_SCAN, F_OPTIC, optic);
    CROP_SCAN[F_BIN] = (BYTE)bin;
    CROP_SCAN[55] = (BYTE)(12 / bin);
    memcpy(CROP_END, CROP_SCAN, 84);
    memset(CROP_END + 8, 0, 53 - 8);
    printf("crop descriptor:");
    for (i = 0; i < 84; i++)
        printf("%s%02x", i % 28 ? " " : "\n  ", CROP_SCAN[i]);
    printf("\n");
    g_ccd = ccd;                 /* cmd_scan applies these to scan + end */
    g_optic = optic;
    rc = cmd_scan(&crop, -1, r->keep, r->hold, r->out);
    g_ccd = g_optic = -1;
    return rc ? 1 : 0;
}

static int cmd_crop(int argc, char **argv)
{
    struct crop_req r;
    int i;

    if (argc < 5) {
        fprintf(stderr, "crop <ppi> <x_mm> <y_mm> <w_mm> <h_mm> "
                        "[-n lines] [-h s] [-p ccd,lens] [-l lamp] [-o file]\n");
        return 2;
    }
    memset(&r, 0, sizeof r);
    r.ppi = atof(argv[0]); r.x = atof(argv[1]); r.y = atof(argv[2]);
    r.w = atof(argv[3]); r.h = atof(argv[4]);
    r.ccd = r.optic = -1;
    r.hold = 10000;
    r.out = "crop.raw";
    for (i = 5; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "-n") == 0) r.keep = atol(argv[i + 1]);
        else if (strcmp(argv[i], "-h") == 0) { if (parse_hold(argv[i + 1], &r.hold)) return 2; }
        else if (strcmp(argv[i], "-o") == 0) r.out = argv[i + 1];
        else if (strcmp(argv[i], "-p") == 0) {
            if (parse_pos(argv[i + 1])) return 2;
            r.ccd = g_ccd; r.optic = g_optic;
            g_ccd = g_optic = -1;
        }
        else if (strcmp(argv[i], "-l") == 0) { if (parse_lamp(argv[i + 1])) return 2; }
        else { fprintf(stderr, "crop: unknown option %s\n", argv[i]); return 2; }
    }
    return crop_scan(&r) ? 1 : 0;
}

/* ------------------------------------------------------------ focus, frames */

/* Focus bracket on a 1 mm band centred at (xc, yc), w mm across, at ppi:
 * n (3..9) crops with the CCD at curve + centre + (i - n/2) * step, lens
 * on the curve. Samples are kept by offset; on a peak at an end the
 * bracket moves there (up to twice), scanning only offsets not yet
 * sampled, at most FB_SAMPLES in all, and the peak is fitted over every
 * sample. The bracket is clamped to +-FT_BRACKET_MAX of the curve.
 * Returns 0 with *best = peak offset from the curve; FB_NO_PEAK when the
 * point is unusable (crop refused before scanning, flat response, peak
 * still at an edge); -1 on a scan or file failure. */
#define FB_SAMPLES 9
#define FB_NO_PEAK 2
#define FB_MIN_CONTRAST 0.03    /* (max - min) / max below this: flat */

static int focus_bracket(double ppi, double xc, double yc, double w,
                         long centre, long step, int n, long *best)
{
    long off[FB_SAMPLES];
    double sh[FB_SAMPLES];
    int ns = 0, tries, i, k, m, bin = ft_bin_for_ppi(ppi), dir = 0;
    long half;

    if (n < 3 || n > FB_SAMPLES || step <= 0 || bin < 1 ||
        (long)(n - 1) * step > 2 * FT_BRACKET_MAX) {
        fprintf(stderr, "focus: bad bracket (n %d, step %ld, ppi %g)\n", n,
                step, ppi);
        return FB_NO_PEAK;
    }
    half = (long)(n / 2) * step;
    for (tries = 0; tries < 3; tries++) {
        long lo, mn_i;
        double mx, mn, pk;
        int r;

        /* keep the whole bracket inside +-FT_BRACKET_MAX */
        lo = centre - half;
        if (lo < -FT_BRACKET_MAX)
            lo = -FT_BRACKET_MAX;
        if (lo + (long)(n - 1) * step > FT_BRACKET_MAX)
            lo = FT_BRACKET_MAX - (long)(n - 1) * step;
        for (i = 0; i < n && ns < FB_SAMPLES; i++) {
            struct crop_req q;
            int lines = 0, px, rc;
            long o = lo + i * step, x;
            float *t;
            double s;
            for (k = 0; k < ns && off[k] != o; k++)
                ;
            if (k < ns)
                continue;               /* sampled on an earlier bracket */
            if (labs(o) > FT_BRACKET_MAX) {   /* cannot happen; belt and braces */
                fprintf(stderr, "focus: offset %ld beyond +-%ld\n", o,
                        FT_BRACKET_MAX);
                return FB_NO_PEAK;
            }
            memset(&q, 0, sizeof q);
            q.ppi = ppi;
            q.x = xc - w / 2;
            q.y = yc - 0.5;
            q.w = w;
            q.h = 1.0;
            q.ccd = q.optic = -1;
            q.ccd_offset = o;
            q.hold = 3000;
            q.out = "fb.raw";
            rc = crop_scan(&q);
            if (rc == CROP_REFUSED)
                return FB_NO_PEAK;
            if (rc)
                return -1;
            px = (int)meta_long("fb.raw", "px");
            x = meta_long("fb.raw", "x");
            t = load_corrected("fb.raw", px, bin, x >= 0 ? (int)x : -1, &lines);
            if (!t)
                return -1;
            s = green_sharpness(t, lines, px);
            free(t);
            printf("  focus %+6ld: sharpness %.4f\n", o, s);
            /* insert sorted by offset */
            for (k = ns; k > 0 && off[k - 1] > o; k--) {
                off[k] = off[k - 1];
                sh[k] = sh[k - 1];
            }
            off[k] = o;
            sh[k] = s;
            ns++;
        }

        m = 0;
        mn_i = 0;
        for (k = 1; k < ns; k++) {
            if (sh[k] > sh[m]) m = k;
            if (sh[k] < sh[mn_i]) mn_i = k;
        }
        mx = sh[m];
        mn = sh[mn_i];
        if (ns < 3 || !isfinite(mx) || !isfinite(mn) || !(mx > 0) ||
            (mx - mn) / mx < FB_MIN_CONTRAST) {
            printf("  focus: flat response (%.4f..%.4f); no usable peak\n",
                   mn, mx);
            return FB_NO_PEAK;
        }
        r = ft_peak(off, sh, ns, &pk);
        if (r < 0) {
            printf("  focus: no usable sharpness peak\n");
            return FB_NO_PEAK;
        }
        *best = (long)(pk < 0 ? pk - 0.5 : pk + 0.5);
        if (r == 0)
            return 0;
        /* maximum at an end of everything sampled */
        if (dir != 0 && (m == 0 ? -1 : 1) != dir) {
            printf("  focus: maximum went back to the other end; no usable "
                   "peak\n");
            return FB_NO_PEAK;
        }
        dir = m == 0 ? -1 : 1;
        if (tries < 2)
            printf("  focus: still rising at %+ld; moving the bracket\n",
                   off[m]);
        centre = off[m];
    }
    printf("  focus: peak still at the bracket edge (%+ld); no usable peak\n",
           *best);
    return FB_NO_PEAK;
}

/* 3x5 digits, rows top to bottom, 3 bits each (MSB = left) */
static const unsigned char DIGITS[10][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},
    {7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7},
};

static void put_px(unsigned char *img, int w, int h, long x, long y,
                   unsigned char r, unsigned char g, unsigned char b)
{
    size_t o;
    if (x < 0 || y < 0 || x >= w || y >= h)
        return;
    o = ((size_t)y * w + (size_t)x) * 3;
    img[o] = r;
    img[o + 1] = g;
    img[o + 2] = b;
}

static void draw_number(unsigned char *img, int w, int h, long x, long y, int v)
{
    char s[16];
    int i, r, c, k, scale = 4;
    snprintf(s, sizeof s, "%d", v);
    for (i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9')
            continue;
        for (r = 0; r < 5; r++)
            for (c = 0; c < 3; c++)
                if (DIGITS[s[i] - '0'][r] & (4 >> c))
                    for (k = 0; k < scale * scale; k++)
                        put_px(img, w, h, x + (i * 4 + c) * scale + k % scale,
                               y + r * scale + k / scale, 255, 255, 0);
    }
}

/* mm -> pixel index, clamped to -1..n+1 (NaN -> -1) so a bad window
 * cannot run away; callers treat <0 and >n as outside */
static long mm_to_idx(double mm, double mm0, double per, int n)
{
    double v = floor((mm - mm0) / per + 0.5);
    if (!(v >= 0))
        return -1;
    if (v > n)
        return (long)n + 1;
    return (long)v;
}

/* strip horizontal: image x = preview line, image y = preview column.
 * Included frames yellow (dimmer when excluded), low confidence orange. */
static int write_strip_png(const char *path, const float *t, int lines, int px,
                           const ft_axes *ax, const ft_job *j)
{
    unsigned char *img;
    long l, c;
    int i, k, rc;
    ft_err e;
    if (lines < 1 || px < 1)
        return -1;
    img = malloc((size_t)lines * px * 3);
    if (!img) {
        fprintf(stderr, "%s: out of memory\n", path);
        return -1;
    }
    for (l = 0; l < lines; l++)
        for (c = 0; c < px; c++)
            for (k = 0; k < 3; k++) {
                double v = t[((size_t)l * px + c) * 3 + k];
                v = v > 0 ? pow(v < 1 ? v : 1, 1 / 2.2) : 0;
                img[((size_t)c * lines + l) * 3 + k] = (unsigned char)(v * 255 + 0.5);
            }
    for (i = 0; i < j->nframes; i++) {
        const ft_job_frame *f = &j->f[i];
        long x0 = mm_to_idx(f->y0, ax->y_mm0, ax->y_mm_per_line, lines);
        long x1 = mm_to_idx(f->y0 + f->len, ax->y_mm0, ax->y_mm_per_line, lines);
        long y0 = mm_to_idx(f->x0, ax->x_mm0, ax->x_mm_per_px, px);
        long y1 = mm_to_idx(f->x0 + f->width, ax->x_mm0, ax->x_mm_per_px, px);
        unsigned char r = f->include ? 255 : 128, g = f->confidence_low ? 64 : 255;
        for (l = x0; l <= x1; l++) {
            put_px(img, lines, px, l, y0, r, g, 0);
            put_px(img, lines, px, l, y1, r, g, 0);
        }
        for (c = y0; c <= y1; c++) {
            put_px(img, lines, px, x0, c, r, g, 0);
            put_px(img, lines, px, x1, c, r, g, 0);
        }
        draw_number(img, lines, px, x0 + 6, y0 + 6, f->index);
    }
    rc = ft_png_write_rgb8(path, img, lines, px, &e);
    if (rc)
        fprintf(stderr, "%s\n", e.msg);
    free(img);
    return rc;
}

static const char *type_name(ft_frame_type t)
{
    static const char *const names[] = { "full", "half", "panorama", "partial" };
    return (unsigned)t < sizeof names / sizeof names[0] ? names[t] : "?";
}

static void print_job(const ft_job *j)
{
    int i;
    printf("\n frame  type      y0     len    x0   width  tilt  ppi   conf\n");
    for (i = 0; i < j->nframes; i++) {
        const ft_job_frame *f = &j->f[i];
        printf(" %3d%s  %-8s %6.1f %6.1f %5.1f %6.1f %5.2f %5g  %s\n",
               f->index, f->include ? " " : "-", type_name(f->type), f->y0,
               f->len, f->x0, f->width, f->tilt_deg, f->ppi,
               f->confidence_low ? "LOW" : "ok");
    }
    printf(" focus: %s, offset %ld CCD steps\n",
           j->focus == FT_FOCUS_PER_FRAME ? "per frame" : "shifted curve",
           j->focus_offset);
}

/* most textured 1 mm band of a frame, by preview sharpness: centre (mm
 * along) returned, *score = its sharpness (0 if no band fits) */
static double textured_band(const float *t, int lines, int px,
                            const ft_axes *ax, const ft_job_frame *f,
                            double *score)
{
    double best_y = f->y0 + f->len / 2;
    int k;
    *score = 0;
    for (k = 1; k <= 9; k++) {
        double yc = f->y0 + 2 + (f->len - 4) * k / 10.0;
        long l0 = mm_to_idx(yc - 0.5, ax->y_mm0, ax->y_mm_per_line, lines);
        long l1 = mm_to_idx(yc + 0.5, ax->y_mm0, ax->y_mm_per_line, lines);
        long c0 = mm_to_idx(f->x0, ax->x_mm0, ax->x_mm_per_px, px);
        long c1 = mm_to_idx(f->x0 + f->width, ax->x_mm0, ax->x_mm_per_px, px);
        double s;
        float *band;
        long l, c;
        if (c0 < 0)                 /* across: the part inside the preview */
            c0 = 0;
        if (c1 > px)
            c1 = px;
        if (l0 < 0 || l1 > lines || l1 - l0 < 3 || c1 - c0 < 3)
            continue;
        band = malloc((size_t)(l1 - l0) * (c1 - c0) * sizeof *band);
        if (!band)
            continue;
        for (l = l0; l < l1; l++)
            for (c = c0; c < c1; c++)
                band[(size_t)(l - l0) * (c1 - c0) + (c - c0)] =
                    t[((size_t)l * px + c) * 3 + 1];
        s = ft_sharpness(band, (int)(c1 - c0), (int)(l1 - l0));
        free(band);
        if (s > *score) {
            *score = s;
            best_y = yc;
        }
    }
    return best_y;
}

#define TEXTURE_MIN 0.01    /* preview sharpness below this: featureless */

/* preview axes: k = 1 line dropped by the RGB alignment; across from
 * output pixel 2 (SCAN_WIN @14), bin 12, at the preview's 6300 scale */
static void preview_axes(ft_axes *ax)
{
    ax->y_mm_per_line = 25.4 / 525;
    ax->y_mm0 = 1 * ax->y_mm_per_line;
    ax->x_mm0 = ft_ccd_to_mm(ft_out_to_ccd(2, 0, 12) + 5.5, 6300);
    ax->x_mm_per_px = ft_ccd_to_mm(ft_out_to_ccd(2, 1, 12) + 5.5, 6300) -
                      ax->x_mm0;
}

/* crop_scan's limits across (x >= 0, x + w <= 32 mm) less a margin */
#define FOCUS_X_MIN 0.3
#define FOCUS_X_MAX 31.8
#define FOCUS_W_MIN 2.0     /* narrower band: not worth a bracket */
#define FOCUS_N     5       /* crops in the first bracket */
#define CROP_S      12      /* rough seconds per focus crop */

/* One check point on frame fr: 0 with *off, FB_NO_PEAK when the frame
 * cannot give one (no texture, outside the crop area, no peak), -1 on a
 * scan or file failure. */
static int focus_point(const float *t, int lines, const ft_axes *ax,
                       double ppi, const ft_job_frame *fr, long *off)
{
    double score, x0, x1, yc = textured_band(t, lines, 666, ax, fr, &score);
    if (!(score >= TEXTURE_MIN)) {
        printf("frame %d: no texture for focus\n", fr->index);
        return FB_NO_PEAK;
    }
    x0 = fr->x0 > FOCUS_X_MIN ? fr->x0 : FOCUS_X_MIN;
    x1 = fr->x0 + fr->width < FOCUS_X_MAX ? fr->x0 + fr->width : FOCUS_X_MAX;
    if (!(x1 - x0 >= FOCUS_W_MIN)) {
        printf("frame %d: outside the area a crop can scan\n", fr->index);
        return FB_NO_PEAK;
    }
    printf("focus check, frame %d at %.1f mm (%.1f..%.1f mm across)\n",
           fr->index, yc, x0, x1);
    return focus_bracket(ppi, (x0 + x1) / 2, yc, x1 - x0, 0, 2000, FOCUS_N,
                         off);
}

/* 3-point focus check on the first, middle and last whole frame; a point
 * that gives no peak moves to the next whole frame once, then is dropped.
 * Sets j->focus / focus_offset. Returns -1 on a scan or file failure
 * (j untouched), else 0. */
static int focus_check(const float *t, int lines, const ft_axes *ax,
                       double ppi, ft_job *j)
{
    int cand[FT_MAX_FRAMES], used[FT_MAX_FRAMES], nc = 0, np = 0, npt = 0;
    int pos[3], dir[3], p, i, k, rc;
    long off[3];

    for (i = 0; i < j->nframes && i < FT_MAX_FRAMES; i++)
        if (j->f[i].type != FT_FRAME_PARTIAL) {
            used[nc] = 0;
            cand[nc++] = i;
        }
    if (nc > 0) { pos[npt] = 0; dir[npt++] = 1; }
    if (nc > 2) { pos[npt] = nc / 2; dir[npt++] = 1; }
    if (nc > 1) { pos[npt] = nc - 1; dir[npt++] = -1; }
    if (npt)
        printf("focus check: %d points, ~%d crops (up to %d), ~%d min\n",
               npt, npt * FOCUS_N, npt * 2 * FB_SAMPLES,
               (npt * FOCUS_N * CROP_S + 59) / 60);
    for (p = 0; p < npt; p++) {
        int tried = 0, ok = 0;
        for (i = pos[p]; tried < 2 && i >= 0 && i < nc; i += dir[p]) {
            if (used[i])
                continue;
            used[i] = 1;
            tried++;
            rc = focus_point(t, lines, ax, ppi, &j->f[cand[i]], &off[np]);
            if (rc < 0)
                return -1;
            if (rc == 0) {
                np++;
                ok = 1;
                break;
            }
            if (tried < 2)
                printf("  trying the next frame for this point\n");
        }
        if (!ok)
            printf("focus point %d of %d dropped\n", p + 1, npt);
    }
    if (np == 0) {
        printf("WARNING: no usable focus point; using the focus curve as is\n");
        j->focus = FT_FOCUS_SHIFTED;
        j->focus_offset = 0;
        return 0;
    }
    {
        double s = 0, m;
        long mean;
        for (k = 0; k < np; k++)
            s += off[k];
        m = s / np;
        mean = (long)(m < 0 ? m - 0.5 : m + 0.5);
        while (np < 3)                   /* fewer points: repeat the mean */
            off[np++] = mean;
    }
    j->focus = ft_focus_decide(off, &j->focus_offset);
    printf("focus offsets %ld %ld %ld -> %s\n", off[0], off[1], off[2],
           j->focus == FT_FOCUS_PER_FRAME ? "per-frame focus"
                                          : "shifted curve");
    if (j->focus == FT_FOCUS_PER_FRAME) {
        int n = 0;
        for (i = 0; i < j->nframes; i++)
            n += j->f[i].include != 0;
        printf("per-frame focus adds ~%d crops (~%d min) to scan --job\n",
               n * FOCUS_N, (n * FOCUS_N * CROP_S + 59) / 60);
    }
    return 0;
}

static int cmd_frames(int argc, char **argv)
{
    const char *base = "strip";
    char raw[260], path[260];
    ft_film film = FT_FILM_NEGATIVE;
    double ppi = 2000;
    int sprockets = 0, i, lines = 0, rc, focus_failed;
    DWORD hold = 10000;
    const ft_holder *h = ft_holder_find("24x217");
    static ft_layout L;
    static ft_job j;
    ft_axes ax;
    ft_err e;
    float *t;
    FILE *f;

    for (i = 0; i < argc; i += 2) {
        if (i + 1 >= argc) {
            fprintf(stderr, "frames: %s needs a value\n", argv[i]);
            return 2;
        }
        if (strcmp(argv[i], "--film") == 0) {
            if (strcmp(argv[i + 1], "slide") == 0) film = FT_FILM_SLIDE;
            else if (strcmp(argv[i + 1], "neg") == 0) film = FT_FILM_NEGATIVE;
            else {
                fprintf(stderr, "frames: --film neg|slide\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--ppi") == 0) ppi = atof(argv[i + 1]);
        else if (strcmp(argv[i], "--sprockets") == 0) {
            if (strcmp(argv[i + 1], "yes") == 0) sprockets = 1;
            else if (strcmp(argv[i + 1], "no") == 0) sprockets = 0;
            else {
                fprintf(stderr, "frames: --sprockets yes|no\n");
                return 2;
            }
        }
        else if (strcmp(argv[i], "-h") == 0) {
            if (parse_hold(argv[i + 1], &hold)) return 2;
        }
        else if (strcmp(argv[i], "-o") == 0) base = argv[i + 1];
        else {
            fprintf(stderr, "frames: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (!ft_ppi_supported(ppi)) {
        fprintf(stderr, "frames: ppi %g not supported\n", ppi);
        return 2;
    }
    if (!h) {
        fprintf(stderr, "frames: no 24x217 holder geometry\n");
        return 1;
    }
    if (snprintf(raw, sizeof raw, "%s.raw", base) >= (int)sizeof raw ||
        snprintf(path, sizeof path, "%s.job", base) >= (int)sizeof path) {
        fprintf(stderr, "frames: -o name too long\n");
        return 2;
    }

    g_lamp = 255;                    /* the lamp level persists: set it */
    if (cmd_scan(&PRESETS[0], -1, 0, hold, raw))
        return 1;
    /* the axes below assume SCAN_WIN's geometry: 666 px from pixel 2 */
    if (meta_long(raw, "px") != 666 || meta_long(raw, "x") != 2) {
        fprintf(stderr, "frames: %s.txt: expected px=666 x=2 (got %ld, %ld)\n",
                raw, meta_long(raw, "px"), meta_long(raw, "x"));
        return 1;
    }
    t = load_corrected(raw, 666, 12, 2, &lines);
    if (!t)
        return 1;

    preview_axes(&ax);
    if (ft_detect(t, lines, 666, &ax, film, h, &L, &e)) {
        fprintf(stderr, "frames: %s\n", e.msg);
        free(t);
        return 1;
    }
    if (L.fallback)
        printf("WARNING: no frame edge found; using the fixed %g mm grid\n",
               h->grid_pitch_mm);
    if (L.nblank)
        printf("note: %d blank frame(s) dropped\n", L.nblank);
    ft_layout_to_job(&L, h->name, film, ppi, sprockets, &j);

    focus_failed = focus_check(t, lines, &ax, ppi, &j) != 0;
    if (focus_failed) {
        j.focus = FT_FOCUS_SHIFTED;
        j.focus_offset = 0;
    }

    f = fopen(path, "w");
    rc = !f;
    if (f) {
        rc = ft_job_write(&j, f) != 0;
        rc |= fclose(f) != 0;
    }
    if (rc) {
        fprintf(stderr, "frames: cannot write %s\n", path);
        free(t);
        return 1;
    }
    snprintf(path, sizeof path, "%s.png", base);
    rc = write_strip_png(path, t, lines, 666, &ax, &j);
    free(t);
    print_job(&j);
    printf("\nwrote %s.job and %s.png; edit the job, then: ftpoc scan --job %s.job\n",
           base, base, base);
    if (focus_failed)
        printf("\n*** FOCUS NOT CHECKED: a focus crop failed. The job uses the\n"
               "*** focus curve as is (shifted, offset 0); check the scanner and\n"
               "*** re-run frames, or edit focus/focus_offset by hand.\n");
    return rc || focus_failed ? 1 : 0;
}

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
            case 'h': if (parse_hold(v, &hold)) return 2; break;
            case 'o': out = v; break;
            case 'p': if (parse_pos(v)) return 2; break;
            case 'l': if (parse_lamp(v)) return 2; break;
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

    load_lens();
    if (argc < 2) {
        fprintf(stderr,
            "usage: ftpoc <command> [args]\n"
            "  inquiry            identity (run first)\n"
            "  poll [n]           n ScannerReady polls (default 10)\n"
            "  win                one SetWindow exchange (idle template), hexdump\n"
            "  park               rest position (FlexColor's shutdown descriptor)\n"
            "  goto <ppi*10000>   idle template with @10 set, 0..63000000\n"
            "  move <+/-d> [n]    n relative @10 steps of d (max 5000000, 50 steps)\n"
            "  stop               end-of-scan descriptor x2 (FlexColor end/Stop)\n"
            "  pages              read the standard ReadBuffer pages\n"
            "  grab <n> [len]     n ReadExt blocks (default len 3996) -> grab.raw\n"
            "  scan [preset] [-n lines] [-y line] [-f frame] [-h hold_s] [-o file]\n"
            "                     presets: preview (525 ppi), 2000, 1000, 3f (630 ppi)\n"
            "                     -n stop after n lines (default: the whole window)\n"
            "                     -y start line along the strip (holder-clamped)\n"
            "                     -f 3f frame 1-6, or 0 = whole strip (frameN.raw)\n"
            "                     -h hold before calibration, 0..120 s (default 10; 35 cold)\n"
            "                     -p <ccd>,<lens> positions @56/@60 (clamped to\n"
            "                        FlexColor's values, CCD below lens)\n"
            "                     -l <0..255> lamp level @76 (scans send 0)\n"
            "                     -> scan.raw (+ .cal 250 calibration lines, .txt)\n"
            "  crop <ppi> <x> <y> <w> <h> [-n lines] [-h s] [-p ccd,lens] [-o file]\n"
            "                     SYNTHESIZED window in mm at any ppi (525..6300)\n"
            "                     -> crop.raw (+ .cal .faul .txt)\n"
            "  frames [--film neg|slide] [--ppi n] [--sprockets yes|no] [-h s] [-o strip]\n"
            "                     preview (lamp 255), detect frames, 3-point focus\n"
            "                     check -> strip.job (edit it) + strip.png;\n"
            "                     fb.raw* are scratch focus crops\n"
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
        } else if (strcmp(cmd, "crop") == 0) {
            rc = cmd_crop(argc - 2, argv + 2);
        } else if (strcmp(cmd, "frames") == 0) {
            rc = cmd_frames(argc - 2, argv + 2);
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
