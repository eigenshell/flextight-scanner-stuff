/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft_fff.h"

#define META_LEN 244            /* Imacon metadata block (tag 46274) */
#define META_W_OFF 40           /* u32 width, height, 4 zero margins */
#define META_CAM_MUL_OFF 116    /* 3 x f32 white balance */

static void le16(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void le32(unsigned char *p, unsigned long v)
{
    le16(p, v & 0xffff);
    le16(p + 2, v >> 16);
}

static unsigned char *entry(unsigned char *p, unsigned tag, unsigned type,
                            unsigned long count, unsigned long value)
{
    le16(p, tag);
    le16(p + 2, type);
    le32(p + 4, count);
    le32(p + 8, value);
    return p + 12;
}

int ft_fff_write(const char *path, const ft_image *im, ft_err *e)
{
    static const unsigned char one_f32[4] = { 0, 0, 0x80, 0x3f }; /* 1.0f */
    unsigned char ifd[2 + 12 * 8 + 4], meta[META_LEN], hdr[8], *p, *buf;
    unsigned long row_bytes, data_len, nent, ifd_len, meta_off, data_off, str;
    size_t lm = strlen(im->make), lmod = strlen(im->model);
    FILE *f;
    long r;
    int ok, i;

    if (!im->rgb || im->w < 1 || im->h < 1)
        return ft_fail(e, "fff: empty image");
    row_bytes = (unsigned long)im->w * 6;
    if ((double)row_bytes * im->h > 4.0e9)
        return ft_fail(e, "fff: %dx%d is over 4 GB", im->w, im->h);
    data_len = row_bytes * (unsigned long)im->h;

    nent = 6 + (lm > 0) + (lmod > 0);
    ifd_len = 2 + 12 * nent + 4;
    meta_off = 8 + ifd_len;
    data_off = meta_off + META_LEN;
    str = data_off + data_len;

    p = ifd;
    le16(p, nent);
    p += 2;
    p = entry(p, 256, 4, 1, (unsigned long)im->w);
    p = entry(p, 257, 4, 1, (unsigned long)im->h);
    p = entry(p, 273, 4, 1, data_off);
    p = entry(p, 279, 4, 1, data_len);
    if (lm) {
        p = entry(p, 271, 2, (unsigned long)lm + 1, str);
        str += lm + 1;
    }
    if (lmod)
        p = entry(p, 272, 2, (unsigned long)lmod + 1, str);
    /* LibRaw parses 46275 before 46274: descending on purpose */
    p = entry(p, 46275, 7, data_len, data_off);
    p = entry(p, 46274, 7, META_LEN, meta_off);
    le32(p, 0);

    memset(meta, 0, sizeof meta);
    le32(meta + META_W_OFF, (unsigned long)im->w);
    le32(meta + META_W_OFF + 4, (unsigned long)im->h);
    for (i = 0; i < 3; i++)
        memcpy(meta + META_CAM_MUL_OFF + 4 * i, one_f32, 4);
    /* rotation (u16 at 242) stays 0: the pixels are already oriented */

    buf = malloc(row_bytes);
    if (!buf)
        return ft_fail(e, "fff: out of memory");
    f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return ft_fail(e, "fff: cannot write %s", path);
    }
    memcpy(hdr, "II*\0", 4);
    le32(hdr + 4, 8);
    ok = fwrite(hdr, 1, 8, f) == 8 &&
         fwrite(ifd, 1, ifd_len, f) == ifd_len &&
         fwrite(meta, 1, META_LEN, f) == META_LEN;
    for (r = 0; ok && r < im->h; r++) {
        const float *src = im->rgb + (size_t)r * im->w * 3;
        unsigned long k;
        for (k = 0; k < (unsigned long)im->w * 3; k++) {
            float v = src[k];
            le16(buf + 2 * k, v <= 0 ? 0 : v >= 1 ? 65535
                              : (unsigned long)(v * 65535.0f + 0.5f));
        }
        ok = fwrite(buf, 1, row_bytes, f) == row_bytes;
    }
    if (ok && lm)
        ok = fwrite(im->make, 1, lm + 1, f) == lm + 1;
    if (ok && lmod)
        ok = fwrite(im->model, 1, lmod + 1, f) == lmod + 1;
    ok = fclose(f) == 0 && ok;
    free(buf);
    return ok ? 0 : ft_fail(e, "fff: write error on %s", path);
}
