/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tag set and layout per the DNG 1.4 specification (Adobe, 2012) and
 * TIFF 6.0 (ft_tiff_ifd.h): little-endian, one IFD, tags ascending,
 * values over 4 bytes after the IFD (word-aligned), then the strips. */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft_dng.h"
#include "ft_tiff_ifd.h"

#define STRIP_BYTES 262144L

int ft_dng_write(const char *path, const ft_image *im, ft_err *e)
{
    ft_ifd d;
    ft_ifd_entry *so;
    unsigned char *p, *buf = NULL;
    unsigned long rps, nstrips, row_bytes, data0, s;
    FILE *f = NULL;
    int rc = -1;
    long r;

    if (!im->rgb || im->w < 1 || im->h < 1)
        return ft_fail(e, "dng: empty image");
    row_bytes = (unsigned long)im->w * 6;
    if ((double)row_bytes * im->h > 4.0e9)
        return ft_fail(e, "dng: %dx%d is over 4 GB", im->w, im->h);
    rps = (unsigned long)(STRIP_BYTES / (long)row_bytes);
    if (rps < 1)
        rps = 1;
    if (rps > (unsigned long)im->h)
        rps = (unsigned long)im->h;
    nstrips = ((unsigned long)im->h + rps - 1) / rps;

    memset(&d, 0, sizeof d);
    ft_ifd_common(&d, im, im->w, im->h, 3, 1);
    ft_ifd_add(&d, 273, FT_T_LONG, nstrips);      /* StripOffsets: below */
    ft_ifd_long(&d, 278, rps);
    if ((p = ft_ifd_add(&d, 279, FT_T_LONG, nstrips)) != NULL)
        for (s = 0; s < nstrips; s++) {
            unsigned long rows = s + 1 < nstrips ? rps
                                 : (unsigned long)im->h - s * rps;
            ft_le32(p + 4 * s, rows * row_bytes);
        }
    if (d.oom) {
        ft_fail(e, "dng: out of memory");
        goto out;
    }
    data0 = ft_ifd_layout(&d);
    so = ft_ifd_find(&d, 273);
    for (s = 0; s < nstrips; s++)
        ft_le32(so->data + 4 * s, data0 + s * rps * row_bytes);

    f = fopen(path, "wb");
    if (!f) {
        ft_fail(e, "dng: cannot write %s", path);
        goto out;
    }
    buf = malloc(row_bytes);
    if (!buf) {
        ft_fail(e, "dng: out of memory");
        goto out;
    }
    if (ft_ifd_write(&d, f)) {
        ft_fail(e, "dng: write error on %s", path);
        goto out;
    }
    for (r = 0; r < im->h; r++) {
        const float *src = im->rgb + (size_t)r * im->w * 3;
        unsigned long k;
        for (k = 0; k < (unsigned long)im->w * 3; k++) {
            float v = src[k];
            ft_le16(buf + 2 * k, v <= 0 ? 0 : v >= 1 ? 65535
                                 : (unsigned long)(v * 65535.0f + 0.5f));
        }
        if (fwrite(buf, 1, row_bytes, f) != row_bytes)
            break;
    }
    rc = r == im->h ? 0 : ft_fail(e, "dng: write error on %s", path);
out:
    if (f && fclose(f) != 0 && rc == 0)
        rc = ft_fail(e, "dng: write error on %s", path);
    free(buf);
    ft_ifd_free(&d);
    return rc;
}

/* ------------------------------------------------------------- reader */

static unsigned long rd16(const unsigned char *p)
{
    return (unsigned long)p[0] | (unsigned long)p[1] << 8;
}

static size_t type_size(unsigned type)
{
    return type == FT_T_SHORT ? 2 : type == FT_T_LONG ? 4 :
           type == FT_T_RATIONAL || type == FT_T_SRATIONAL ? 8 : 1;
}

static unsigned long rd32(const unsigned char *p)
{
    return rd16(p) | rd16(p + 2) << 16;
}

typedef struct {
    unsigned long type, count, at;  /* at: offset of the value bytes */
} tag_ref;

/* the tags the reader looks at */
static const unsigned WANT[] = { 256, 257, 258, 259, 262, 270, 271, 272, 273,
                                 277, 278, 279, 282, 284, 50706, 50721 };
#define NWANT (sizeof WANT / sizeof WANT[0])

static int want(unsigned long tag)
{
    unsigned i;
    for (i = 0; i < NWANT; i++)
        if (WANT[i] == tag)
            return (int)i;
    return -1;
}

/* NULL if the file has no such tag */
static const tag_ref *find(const tag_ref *t, unsigned tag)
{
    int i = want(tag);
    return i >= 0 && t[i].count ? &t[i] : NULL;
}

static int tag_value(const unsigned char *b, const tag_ref *r, unsigned long i,
                     unsigned long *v)
{
    if (!r || i >= r->count)
        return -1;
    if (r->type == FT_T_SHORT)
        *v = rd16(b + r->at + 2 * i);
    else if (r->type == FT_T_LONG)
        *v = rd32(b + r->at + 4 * i);
    else
        return -1;
    return 0;
}

int ft_dng_parse(const unsigned char *b, size_t n, ft_image *im, ft_err *e)
{
    tag_ref tags[NWANT];
    unsigned long ifd, cnt, i, w, h, v, rps, nstrips, row_bytes, s;
    const tag_ref *off, *len;

    memset(tags, 0, sizeof tags);
    if (n < 8 || memcmp(b, "II*\0", 4) != 0)
        return ft_fail(e, "dng: not a little-endian TIFF");
    ifd = rd32(b + 4);
    if (ifd < 8 || ifd > n || n - ifd < 2)
        return ft_fail(e, "dng: bad IFD offset");
    cnt = rd16(b + ifd);
    if (cnt > 200 || n - ifd - 2 < 12 * cnt + 4)
        return ft_fail(e, "dng: truncated IFD");
    for (i = 0; i < cnt; i++) {
        const unsigned char *p = b + ifd + 2 + 12 * i;
        unsigned long tag = rd16(p), type = rd16(p + 2), c = rd32(p + 4);
        unsigned long sz = type_size((unsigned)type), bytes;
        int k = want(tag);
        tag_ref *r;
        if (k < 0 || !c)
            continue;
        if (c > 0x10000000UL / sz)
            return ft_fail(e, "dng: tag %lu too large", tag);
        bytes = sz * c;
        r = &tags[k];
        r->type = type;
        r->count = c;
        r->at = bytes <= 4 ? (unsigned long)(p + 8 - b) : rd32(p + 8);
        if (r->at > n || bytes > n - r->at)
            return ft_fail(e, "dng: tag %lu points past the end", tag);
    }
    if (!find(tags, 50706))
        return ft_fail(e, "dng: no DNGVersion: not a DNG");
    if (tag_value(b, find(tags, 262), 0, &v) || v != 34892)
        return ft_fail(e, "dng: not LinearRaw (only this writer's DNGs)");
    if (tag_value(b, find(tags, 259), 0, &v) || v != 1)
        return ft_fail(e, "dng: compressed (only uncompressed is read)");
    if (tag_value(b, find(tags, 277), 0, &v) || v != 3)
        return ft_fail(e, "dng: not 3 samples per pixel");
    if (find(tags, 284) && (tag_value(b, find(tags, 284), 0, &v) || v != 1))
        return ft_fail(e, "dng: not chunky (interleaved) RGB");
    for (i = 0; i < 3; i++)
        if (tag_value(b, find(tags, 258), i, &v) || v != 16)
            return ft_fail(e, "dng: not 16 bits per sample");
    if (tag_value(b, find(tags, 256), 0, &w) ||
        tag_value(b, find(tags, 257), 0, &h) || w < 1 || h < 1 ||
        w > 100000 || h > 100000)
        return ft_fail(e, "dng: bad size");
    row_bytes = w * 6;
    if (tag_value(b, find(tags, 278), 0, &rps) || rps < 1)
        rps = h;
    nstrips = (h + rps - 1) / rps;
    off = find(tags, 273);
    len = find(tags, 279);
    if (!off || !len || off->count != nstrips || len->count != nstrips)
        return ft_fail(e, "dng: strip tags do not match the size");
    /* every strip in the file before anything is allocated */
    for (s = 0; s < nstrips; s++) {
        unsigned long o, l, rows = s + 1 < nstrips ? rps : h - s * rps;
        /* in double: rows x row_bytes may wrap a 32-bit long */
        if (tag_value(b, off, s, &o) || tag_value(b, len, s, &l) ||
            (double)rows * row_bytes > (double)n || l != rows * row_bytes ||
            o > n || l > n - o)
            return ft_fail(e, "dng: strip %lu outside the file", s);
    }
    /* strips may overlap, so they alone do not bound w x h: the pixels
       must fit in the file (double: no wrap on 32 bits) */
    if ((double)w * h * 6 > (double)n)
        return ft_fail(e, "dng: %lux%lu pixels do not fit in the file", w, h);
    if (ft_image_alloc(im, (int)w, (int)h, e))
        return -1;
    for (s = 0; s < nstrips; s++) {
        unsigned long o, rows = s + 1 < nstrips ? rps : h - s * rps, k;
        tag_value(b, off, s, &o);
        for (k = 0; k < rows * w * 3; k++)
            im->rgb[s * rps * w * 3 + k] = (float)rd16(b + o + 2 * k) /
                                           65535.0f;
    }
    {
        const tag_ref *x = find(tags, 282);
        if (x && x->type == FT_T_RATIONAL && rd32(b + x->at + 4))
            im->ppi = (double)rd32(b + x->at) / rd32(b + x->at + 4);
    }
    {
        const tag_ref *cm = find(tags, 50721);
        if (cm && cm->type == FT_T_SRATIONAL && cm->count == 9)
            for (i = 0; i < 9; i++) {
                long num = (long)(int)rd32(b + cm->at + 8 * i);
                long den = (long)(int)rd32(b + cm->at + 8 * i + 4);
                if (den)
                    im->xyz_to_rgb[i] = (double)num / den;
            }
    }
    {
        static const struct { unsigned tag; size_t cap; size_t off; } S[] = {
            { 270, sizeof im->desc, offsetof(ft_image, desc) },
            { 271, sizeof im->make, offsetof(ft_image, make) },
            { 272, sizeof im->model, offsetof(ft_image, model) },
        };
        for (i = 0; i < 3; i++) {
            const tag_ref *t = find(tags, S[i].tag);
            char *dst = (char *)im + S[i].off;
            size_t m;
            if (!t || t->type != FT_T_ASCII)
                continue;
            m = t->count < S[i].cap ? t->count : S[i].cap - 1;
            memcpy(dst, b + t->at, m);
            dst[m] = 0;
        }
    }
    return 0;
}

int ft_dng_read(const char *path, ft_image *im, ft_err *e)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b = NULL;
    long n;
    int rc = -1;
    if (!f)
        return ft_fail(e, "dng: cannot read %s", path);
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 &&
        fseek(f, 0, SEEK_SET) == 0 && (b = malloc(n ? (size_t)n : 1)) != NULL &&
        fread(b, 1, (size_t)n, f) == (size_t)n) {
        rc = ft_dng_parse(b, (size_t)n, im, e);
        if (rc && strncmp(e->msg, "dng: ", 5) == 0) {
            char m[sizeof e->msg];
            snprintf(m, sizeof m, "%s", e->msg + 5);
            ft_fail(e, "dng: %s: %s", path, m);
        }
    } else {
        ft_fail(e, "dng: cannot read %s", path);
    }
    free(b);
    fclose(f);
    return rc;
}
