/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "ft_maint.h"
#include "test/ftest.h"

int main(void)
{
    unsigned char cdb[10], buf[FT_MAINT_HDR + FT_MAINT_MAX_DATA];
    ft_err e;
    static const unsigned char want_io_cdb[10] =
        { 0x3b, 0x01, 0, 0, 0, 0, 0x00, 0x00, 0x0b, 0 };
    static const unsigned char want_io_buf[11] =
        { 0x01, 0x03, 0x44, 0x00, 0x00, 0, 0, 0, 0, 0x05, 0x7f };
    static const unsigned char want_mem_cdb[10] =
        { 0x3b, 0x01, 0, 0, 0, 0, 0x00, 0x00, 0x0e, 0 };
    static const unsigned char want_mem_buf[14] =
        { 0x01, 0x40, 0x00, 0x76, 0x88, 0, 0, 0, 0, 0x00,
          0x10, 0x00, 0x00, 0x00 };
    static const unsigned char want_peek_cdb[10] =
        { 0x3c, 0x00, 0x40, 0x00, 0x76, 0x40, 0x00, 0x00, 0x7c, 0 };
    static const unsigned char want_peek_max_cdb[10] =
        { 0x3c, 0x00, 0x30, 0x00, 0xfc, 0x00, 0x00, 0x04, 0x00, 0 };
    const unsigned char dword16[4] = { 0x10, 0x00, 0x00, 0x00 };
    unsigned char big[FT_MAINT_MAX_DATA + 1] = { 0 };

    /* I/O byte write to 0x344: pinned wire bytes */
    CHECK(ft_maint_io(0x344, 0x7f, cdb, buf, sizeof buf, &e) == 11);
    CHECK(memcmp(cdb, want_io_cdb, 10) == 0);
    CHECK(memcmp(buf, want_io_buf, 11) == 0);
    CHECK(ft_maint_io(0x342, 0x02, cdb, buf, sizeof buf, &e) == 11);

    /* forbidden ports */
    CHECK(ft_maint_io(0x340, 0, cdb, buf, sizeof buf, &e) == -1);
    CHECK(ft_maint_io(0x300, 0, cdb, buf, sizeof buf, &e) == -1);
    CHECK(ft_maint_io(0x323, 0, cdb, buf, sizeof buf, &e) == -1);
    CHECK(ft_maint_io(0xf872, 0, cdb, buf, sizeof buf, &e) == -1);
    CHECK(ft_maint_io(0x346, 0, cdb, buf, sizeof buf, &e) == -1);
    CHECK(ft_maint_io(0x343, 0, cdb, buf, sizeof buf, &e) == -1);

    /* memory write inside the timing table: pinned wire bytes */
    CHECK(ft_maint_mem(0x4000, 0x7688, dword16, 4, cdb, buf, sizeof buf, &e) == 14);
    CHECK(memcmp(cdb, want_mem_cdb, 10) == 0);
    CHECK(memcmp(buf, want_mem_buf, 14) == 0);

    /* table edges (last allowed byte 4000:76BB) */
    CHECK(ft_maint_mem(0x4000, 0x7640, dword16, 4, cdb, buf, sizeof buf, &e) == 14);
    CHECK(ft_maint_mem(0x4000, 0x76b8, dword16, 4, cdb, buf, sizeof buf, &e) == 14);
    CHECK(ft_maint_mem(0x4000, 0x76b9, dword16, 4, cdb, buf, sizeof buf, &e) == -1);
    CHECK(ft_maint_mem(0x4000, 0x763f, dword16, 4, cdb, buf, sizeof buf, &e) == -1);

    /* other segments, empty, oversized, small buffers */
    CHECK(ft_maint_mem(0x3000, 0x7688, dword16, 4, cdb, buf, sizeof buf, &e) == -1);
    CHECK(ft_maint_mem(0x6108, 0x0000, dword16, 4, cdb, buf, sizeof buf, &e) == -1);
    CHECK(ft_maint_mem(0x4000, 0x7688, dword16, 0, cdb, buf, sizeof buf, &e) == -1);
    CHECK(ft_maint_mem(0x4000, 0x7640, big, sizeof big, cdb, buf, sizeof buf,
                       &e) == -1);
    CHECK(ft_maint_mem(0x4000, 0x7688, dword16, 4, cdb, buf, 13, &e) == -1);
    CHECK(ft_maint_io(0x344, 0, cdb, buf, 10, &e) == -1);
    CHECK(strlen(e.msg) > 0);

    /* every packet built is longer than the header: the firmware computes
     * len - 10 unchecked, so len < 11 would underflow (Phase A, A1) */
    CHECK(ft_maint_mem(0x4000, 0x7640, dword16, 1, cdb, buf, sizeof buf, &e) == 11);
    CHECK(cdb[8] == 11);

    /* peek (READ BUFFER = memory read): pinned wire bytes */
    CHECK(ft_maint_peek(0x4000, 0x7640, 0x7c, cdb, &e) == 0);
    CHECK(memcmp(cdb, want_peek_cdb, 10) == 0);
    CHECK(ft_maint_peek(0x3000, 0xfc00, FT_MAINT_MAX_PEEK, cdb, &e) == 0);
    CHECK(memcmp(cdb, want_peek_max_cdb, 10) == 0);

    /* peek refusals: other segments, length bounds, segment wrap */
    CHECK(ft_maint_peek(0xe000, 0x0400, 0x16, cdb, &e) == -1);
    CHECK(ft_maint_peek(0x6108, 0x0000, 0x10, cdb, &e) == -1);
    CHECK(ft_maint_peek(0x4000, 0x7640, 0, cdb, &e) == -1);
    CHECK(ft_maint_peek(0x4000, 0x0000, FT_MAINT_MAX_PEEK + 1, cdb, &e) == -1);
    CHECK(ft_maint_peek(0x4000, 0xfe00, 0x201, cdb, &e) == -1);

    return TEST_DONE();
}
