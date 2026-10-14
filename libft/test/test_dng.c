/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft_dng.h"
#include "test/ftest.h"

static unsigned char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long m;
    CHECK(f != NULL);
    fseek(f, 0, SEEK_END);
    m = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)m);
    CHECK(b && fread(b, 1, (size_t)m, f) == (size_t)m);
    fclose(f);
    *n = (size_t)m;
    return b;
}

/* offset of tag's 12-byte IFD entry in a buffer from ft_dng_write */
static size_t entry_of(const unsigned char *b, unsigned tag)
{
    size_t ifd = b[4] | b[5] << 8 | (size_t)b[6] << 16 | (size_t)b[7] << 24;
    unsigned cnt = b[ifd] | b[ifd + 1] << 8, i;
    for (i = 0; i < cnt; i++) {
        size_t p = ifd + 2 + 12 * i;
        if ((unsigned)(b[p] | b[p + 1] << 8) == tag)
            return p;
    }
    return 0;
}

int main(void)
{
    const char *path = "test/dng_test_out.dng";
    ft_image a, b;
    ft_err e;
    unsigned char *buf, *bad;
    size_t n, k, p;
    int i;

    /* 300 x 400: several strips (262144 B each) */
    ft_image_init(&a);
    CHECK(ft_image_alloc(&a, 300, 400, &e) == 0);
    for (k = 0; k < (size_t)300 * 400 * 3; k++)
        a.rgb[k] = (float)((k * 7919) % 70001) / 70000.0f;
    a.rgb[0] = -0.5f;                       /* clamps to 0 */
    a.rgb[1] = 1.7f;                        /* clamps to 1 */
    a.ppi = 1500;
    a.xyz_to_rgb[4] = 1.8760108;
    strcpy(a.desc, "share frame 4, 1500 ppi, bin 1");
    CHECK(ft_dng_write(path, &a, &e) == 0);

    ft_image_init(&b);
    strcpy(b.make, "x");
    CHECK(ft_dng_read(path, &b, &e) == 0);
    CHECK(b.w == 300 && b.h == 400);
    /* the same 16-bit code back (codes, not floats: x87 excess precision) */
    for (k = 0; k < (size_t)300 * 400 * 3; k++) {
        float v = a.rgb[k];
        long want = v <= 0 ? 0 : v >= 1 ? 65535 : (long)(v * 65535.0f + 0.5f);
        long got = (long)floor(b.rgb[k] * 65535.0 + 0.5);
        if (got != want)
            CHECK(got == want);
    }
    CHECK(b.rgb[0] == 0 && b.rgb[1] == 1);
    CHECK_NEAR(b.ppi, 1500, 1e-9);
    CHECK_NEAR(b.xyz_to_rgb[4], 1.8760108, 1e-7);
    CHECK_NEAR(b.xyz_to_rgb[0], a.xyz_to_rgb[0], 1e-7);
    CHECK(strcmp(b.desc, a.desc) == 0);
    CHECK(strcmp(b.make, "Imacon") == 0);
    CHECK(strcmp(b.model, "Flextight Precision II") == 0);
    /* write -> read -> write is byte-identical in the pixels */
    {
        ft_image c;
        ft_dng_write("test/dng_test_out2.dng", &b, &e);
        ft_image_init(&c);
        CHECK(ft_dng_read("test/dng_test_out2.dng", &c, &e) == 0);
        CHECK(memcmp(b.rgb, c.rgb, (size_t)300 * 400 * 3 * sizeof *b.rgb)
              == 0);
        ft_image_free(&c);
        remove("test/dng_test_out2.dng");
    }
    ft_image_free(&b);

    /* refusals: each with the reason, never a crash */
    buf = slurp(path, &n);
    bad = malloc(n);
    CHECK(bad != NULL);
    CHECK(ft_dng_parse(buf, n, &b, &e) == 0);
    ft_image_free(&b);
    CHECK(ft_dng_parse(buf, 7, &b, &e) == -1);
    CHECK(ft_dng_parse(buf, n - 1, &b, &e) == -1);     /* last strip short */
    CHECK(strstr(e.msg, "strip") != NULL);
    CHECK(b.rgb == NULL);
    CHECK(ft_dng_parse(buf, n / 2, &b, &e) == -1);
    memcpy(bad, buf, n);
    bad[0] = 'M';                                       /* big-endian */
    CHECK(ft_dng_parse(bad, n, &b, &e) == -1);
    CHECK(strstr(e.msg, "little-endian") != NULL);
    memcpy(bad, buf, n);
    p = entry_of(bad, 259);
    CHECK(p);
    bad[p + 8] = 7;                                     /* JPEG compressed */
    CHECK(ft_dng_parse(bad, n, &b, &e) == -1);
    CHECK(strstr(e.msg, "compressed") != NULL);
    memcpy(bad, buf, n);
    p = entry_of(bad, 262);
    bad[p + 8] = 0x23;                                  /* 32803: CFA */
    bad[p + 9] = 0x80;
    CHECK(ft_dng_parse(bad, n, &b, &e) == -1);
    CHECK(strstr(e.msg, "LinearRaw") != NULL);
    memcpy(bad, buf, n);
    p = entry_of(bad, 50706);
    bad[p] = 0x13;                                      /* tag 50707 + ... */
    bad[p + 1] = 0x00;                                  /* -> tag 19 */
    CHECK(ft_dng_parse(bad, n, &b, &e) == -1);
    CHECK(strstr(e.msg, "not a DNG") != NULL);
    /* random corruption of the header and IFD: no crash, no leak of a
     * half-filled image */
    srand(5);
    for (i = 0; i < 3000; i++) {
        size_t hdr = entry_of(buf, 50721) + 12;
        int j;
        memcpy(bad, buf, n);
        for (j = 0; j < 1 + i % 4; j++)
            bad[(size_t)rand() % hdr] = (unsigned char)rand();
        ft_image_init(&b);
        if (ft_dng_parse(bad, n, &b, &e) == 0) {
            CHECK(b.rgb != NULL && b.w >= 1 && b.h >= 1);
            ft_image_free(&b);
        } else {
            CHECK(b.rgb == NULL);
        }
    }
    CHECK(ft_dng_read("test/no_such_file.dng", &b, &e) == -1);
    CHECK(strstr(e.msg, "no_such_file") != NULL);
    free(buf);
    free(bad);
    ft_image_free(&a);
    remove(path);
    return TEST_DONE();
}
