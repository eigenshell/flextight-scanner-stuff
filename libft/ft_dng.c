/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tag set and layout per the DNG 1.4 specification (Adobe, 2012) and
 * TIFF 6.0: little-endian, one IFD, tags ascending, values over 4 bytes
 * after the IFD (word-aligned), then the strips. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ft_dng.h"

enum { T_BYTE = 1, T_ASCII = 2, T_SHORT = 3, T_LONG = 4, T_RATIONAL = 5,
       T_SRATIONAL = 10 };

#define MAX_TAGS 32
#define STRIP_BYTES 262144L

typedef struct {
    unsigned tag, type;
    unsigned long count;
    unsigned char *data;        /* count values, little-endian */
    size_t len;
} entry;

typedef struct {
    entry e[MAX_TAGS];
    int n, oom;
} ifd;

static size_t type_size(unsigned type)
{
    return type == T_SHORT ? 2 : type == T_LONG ? 4 :
           type == T_RATIONAL || type == T_SRATIONAL ? 8 : 1;
}

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

/* a new entry with room for count values; NULL (and oom set) on failure */
static unsigned char *add(ifd *d, unsigned tag, unsigned type,
                          unsigned long count)
{
    entry *x;
    if (d->n == MAX_TAGS || d->oom) {
        d->oom = 1;
        return NULL;
    }
    x = &d->e[d->n];
    x->len = type_size(type) * count;
    x->data = calloc(x->len ? x->len : 1, 1);
    if (!x->data) {
        d->oom = 1;
        return NULL;
    }
    x->tag = tag;
    x->type = type;
    x->count = count;
    d->n++;
    return x->data;
}

static void add_short(ifd *d, unsigned tag, unsigned long v)
{
    unsigned char *p = add(d, tag, T_SHORT, 1);
    if (p) le16(p, v);
}

static void add_long(ifd *d, unsigned tag, unsigned long v)
{
    unsigned char *p = add(d, tag, T_LONG, 1);
    if (p) le32(p, v);
}

static void add_ascii(ifd *d, unsigned tag, const char *s)
{
    unsigned char *p = add(d, tag, T_ASCII, (unsigned long)strlen(s) + 1);
    if (p) memcpy(p, s, strlen(s));
}

static void add_bytes(ifd *d, unsigned tag, const unsigned char *b, int n)
{
    unsigned char *p = add(d, tag, T_BYTE, (unsigned long)n);
    if (p) memcpy(p, b, (size_t)n);
}

/* signed or unsigned rationals over a fixed denominator */
static void add_rationals(ifd *d, unsigned tag, unsigned type,
                          const double *v, int n, long den)
{
    unsigned char *p = add(d, tag, type, (unsigned long)n);
    int i;
    for (i = 0; p && i < n; i++) {
        long num = (long)floor(v[i] * den + 0.5);
        le32(p + 8 * i, (unsigned long)num);
        le32(p + 8 * i + 4, (unsigned long)den);
    }
}

static int by_tag(const void *a, const void *b)
{
    return (int)((const entry *)a)->tag - (int)((const entry *)b)->tag;
}

static void free_ifd(ifd *d)
{
    int i;
    for (i = 0; i < d->n; i++)
        free(d->e[i].data);
}

int ft_dng_write(const char *path, const ft_image *im, ft_err *e)
{
    static const unsigned char version[4] = { 1, 4, 0, 0 };
    static const unsigned char backward[4] = { 1, 1, 0, 0 };
    static const double neutral[3] = { 1, 1, 1 };
    ifd d;
    unsigned char *p, *buf = NULL, hdr[8];
    unsigned long rps, nstrips, row_bytes, ifd_len, pos, data0, s;
    char stamp[20] = "";
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    FILE *f = NULL;
    int i, rc = -1;
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
    if (tm)
        strftime(stamp, sizeof stamp, "%Y:%m:%d %H:%M:%S", tm);

    memset(&d, 0, sizeof d);
    add_long(&d, 254, 0);                         /* NewSubFileType: main */
    add_long(&d, 256, (unsigned long)im->w);
    add_long(&d, 257, (unsigned long)im->h);
    if ((p = add(&d, 258, T_SHORT, 3)) != NULL) { /* BitsPerSample */
        le16(p, 16); le16(p + 2, 16); le16(p + 4, 16);
    }
    add_short(&d, 259, 1);                        /* no compression */
    add_short(&d, 262, 34892);                    /* LinearRaw */
    if (im->desc[0])
        add_ascii(&d, 270, im->desc);
    add_ascii(&d, 271, im->make);
    add_ascii(&d, 272, im->model);
    add(&d, 273, T_LONG, nstrips);                /* StripOffsets: below */
    add_short(&d, 274, 1);                        /* Orientation: as stored */
    add_short(&d, 277, 3);
    add_long(&d, 278, rps);
    if ((p = add(&d, 279, T_LONG, nstrips)) != NULL)
        for (s = 0; s < nstrips; s++) {
            unsigned long rows = s + 1 < nstrips ? rps
                                 : (unsigned long)im->h - s * rps;
            le32(p + 4 * s, rows * row_bytes);
        }
    if (im->ppi > 0) {
        add_rationals(&d, 282, T_RATIONAL, &im->ppi, 1, 1000);
        add_rationals(&d, 283, T_RATIONAL, &im->ppi, 1, 1000);
    }
    add_short(&d, 284, 1);                        /* chunky RGB */
    if (im->ppi > 0)
        add_short(&d, 296, 2);                    /* inches */
    add_ascii(&d, 305, "flextight-shenanigans libft");
    if (stamp[0])
        add_ascii(&d, 306, stamp);
    add_bytes(&d, 50706, version, 4);
    add_bytes(&d, 50707, backward, 4);
    {
        char ucm[80];
        snprintf(ucm, sizeof ucm, "%s %s", im->make, im->model);
        add_ascii(&d, 50708, ucm);
    }
    if ((p = add(&d, 50717, T_LONG, 3)) != NULL) { /* WhiteLevel */
        le32(p, 65535); le32(p + 4, 65535); le32(p + 8, 65535);
    }
    add_rationals(&d, 50721, T_SRATIONAL, im->xyz_to_rgb, 9, 10000000L);
    add_rationals(&d, 50728, T_RATIONAL, neutral, 3, 1);
    add_short(&d, 50778, 21);                     /* D65 */
    if (d.oom) {
        ft_fail(e, "dng: out of memory");
        goto out;
    }
    qsort(d.e, (size_t)d.n, sizeof d.e[0], by_tag);

    /* layout: header, IFD, external values, strips */
    ifd_len = 2 + 12UL * d.n + 4;
    pos = 8 + ifd_len;
    for (i = 0; i < d.n; i++)
        if (d.e[i].len > 4)
            pos += (d.e[i].len + 1) & ~1UL;
    data0 = pos;
    for (i = 0; i < d.n; i++)
        if (d.e[i].tag == 273)
            for (s = 0; s < nstrips; s++)
                le32(d.e[i].data + 4 * s, data0 + s * rps * row_bytes);

    f = fopen(path, "wb");
    if (!f) {
        ft_fail(e, "dng: cannot write %s", path);
        goto out;
    }
    memcpy(hdr, "II*\0", 4);
    le32(hdr + 4, 8);
    fwrite(hdr, 1, 8, f);
    {
        unsigned char ent[12];
        unsigned long ext = 8 + ifd_len;
        le16(ent, (unsigned long)d.n);
        fwrite(ent, 1, 2, f);
        for (i = 0; i < d.n; i++) {
            memset(ent, 0, sizeof ent);
            le16(ent, d.e[i].tag);
            le16(ent + 2, d.e[i].type);
            le32(ent + 4, d.e[i].count);
            if (d.e[i].len <= 4)
                memcpy(ent + 8, d.e[i].data, d.e[i].len);
            else {
                le32(ent + 8, ext);
                ext += (d.e[i].len + 1) & ~1UL;
            }
            fwrite(ent, 1, 12, f);
        }
        memset(ent, 0, 4);
        fwrite(ent, 1, 4, f);                     /* no next IFD */
        for (i = 0; i < d.n; i++)
            if (d.e[i].len > 4) {
                fwrite(d.e[i].data, 1, d.e[i].len, f);
                if (d.e[i].len & 1)
                    fputc(0, f);
            }
    }
    buf = malloc(row_bytes);
    if (!buf) {
        ft_fail(e, "dng: out of memory");
        goto out;
    }
    for (r = 0; r < im->h; r++) {
        const float *src = im->rgb + (size_t)r * im->w * 3;
        unsigned long k;
        for (k = 0; k < (unsigned long)im->w * 3; k++) {
            float v = src[k];
            le16(buf + 2 * k, v <= 0 ? 0 : v >= 1 ? 65535
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
    free_ifd(&d);
    return rc;
}
