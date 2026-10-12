/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft_png.h"
#include "test/ftest.h"

int main(void)
{
    static unsigned char rgb[300 * 300 * 3];   /* > 64 KiB: several blocks */
    unsigned char head[24];
    const char *path = "test/png_test_out.png";
    ft_err e;
    FILE *f;
    long size, raw = 300L * (300 * 3 + 1);
    long blocks = (raw + 65534) / 65535;

    memset(rgb, 128, sizeof rgb);
    CHECK(ft_png_write_rgb8(path, rgb, 300, 300, &e) == 0);
    f = fopen(path, "rb");
    CHECK(f != NULL);
    CHECK(fread(head, 1, 24, f) == 24);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fclose(f);
    remove(path);
    CHECK(memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0);
    CHECK(memcmp(head + 12, "IHDR", 4) == 0);
    CHECK(head[16] == 0 && head[17] == 0 && head[18] == 1 && head[19] == 44);
    /* signature + IHDR + IDAT(zlib header, stored blocks, adler) + IEND */
    CHECK(size == 8 + 25 + 12 + 2 + raw + 5 * blocks + 4 + 12);
    CHECK(ft_png_write_rgb8(path, rgb, 0, 3, &e) == -1);
    return TEST_DONE();
}
