/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <math.h>
#include "ft_focus.h"
#include "test/ftest.h"

int main(void)
{
    long c, o, shift;
    double pk;

    /* bin: largest divisor of 12 with ppi x bin <= 6300 */
    CHECK(ft_bin_for_ppi(525) == 12);
    CHECK(ft_bin_for_ppi(1000) == 6);
    CHECK(ft_bin_for_ppi(1075) == 4);
    CHECK(ft_bin_for_ppi(1200) == 4);
    CHECK(ft_bin_for_ppi(2000) == 3);
    CHECK(ft_bin_for_ppi(2400) == 2);
    CHECK(ft_bin_for_ppi(3150) == 2);
    CHECK(ft_bin_for_ppi(4000) == 1);
    CHECK(ft_bin_for_ppi(6300) == 1);
    CHECK(ft_bin_for_ppi(6301) == -1);

    /* supported: 525, 634..3150, 3800..6300 */
    CHECK(ft_ppi_supported(525));
    CHECK(!ft_ppi_supported(600));
    CHECK(ft_ppi_supported(634));
    CHECK(ft_ppi_supported(3150));
    CHECK(!ft_ppi_supported(3500));
    CHECK(ft_ppi_supported(3800));
    CHECK(!ft_ppi_supported(500));

    /* thin-lens optics, this scanner's curve (values from the 2026-10-12 fit) */
    CHECK(ft_optics_for(&ft_lens_default, 3807, &c, &o) == 0);
    CHECK(c == 440053 && o == 910852);
    CHECK(ft_optics_for(&ft_lens_default, 6000, &c, &o) == 0);
    CHECK(c == 285187 && o == 998540);
    CHECK(ft_optics_for(&ft_lens_default, 6300, &c, &o) == 0);
    CHECK(c == 259254 && o == 1005789);
    CHECK(ft_optics_for(&ft_lens_default, 3700, &c, &o) == -1);
    CHECK(ft_optics_for(&ft_lens_default, 6400, &c, &o) == -1);

    CHECK(ft_positions_ok(285187, 998540));
    CHECK(!ft_positions_ok(1020000, 997296));   /* CCD past the lens */
    CHECK(!ft_positions_ok(200000, 998540));    /* below FlexColor's range */

    /* parabola peak: the 4800-scale bracket measured on hardware */
    {
        long x[5] = { 378816, 379816, 380816, 381816, 382816 };
        double y[5] = { .0927, .1006, .1027, .0977, .0890 };
        CHECK(ft_peak(x, y, 5, &pk) == 0);
        CHECK_NEAR(pk, 380612, 1);
    }
    {
        long x[3] = { 0, 1000, 2000 };
        double y[3] = { .1, .2, .3 };            /* still rising: edge */
        CHECK(ft_peak(x, y, 3, &pk) == 1);
        CHECK_NEAR(pk, 2000, 1e-9);
        CHECK(ft_peak(x, y, 2, &pk) == -1);
    }

    /* 3-point decision: all within the margin of their mean -> shift */
    {
        long off[3] = { 2000, 2600, 3100 };
        CHECK(ft_focus_decide(off, &shift) == FT_FOCUS_SHIFTED);
        CHECK(shift == 2567);
    }
    {
        long off[3] = { 0, 500, 4500 };          /* bowed strip */
        CHECK(ft_focus_decide(off, &shift) == FT_FOCUS_PER_FRAME);
    }
    {
        long off[3] = { -3000, -3200, -2900 };
        CHECK(ft_focus_decide(off, &shift) == FT_FOCUS_SHIFTED);
        CHECK(shift == -3033);
    }
    /* sharpness: a crisp pattern beats a blurred one, flat is zero */
    {
        float crisp[64], soft[64], flat[64];
        int i;
        for (i = 0; i < 64; i++) {
            crisp[i] = (i / 4) % 2 ? 0.8f : 0.2f;
            soft[i] = 0.5f + 0.15f * (float)sin(i * 3.14159 / 4);
            flat[i] = 0.5f;
        }
        CHECK(ft_sharpness(crisp, 16, 4) > ft_sharpness(soft, 16, 4));
        CHECK_NEAR(ft_sharpness(flat, 16, 4), 0, 1e-12);
    }

    /* focus file round trip */
    {
        ft_lens a = ft_lens_default, b;
        ft_err e;
        FILE *f = tmpfile();
        CHECK(f != NULL);
        CHECK(ft_lens_write(&a, f) == 0);
        rewind(f);
        CHECK(ft_lens_read(&b, f, &e) == 0);
        CHECK_NEAR(b.film, a.film, 0.05);
        CHECK_NEAR(b.p, a.p, 1e-9);
        fclose(f);
        f = tmpfile();
        fputs("film = 1\nw = 2\n", f);
        rewind(f);
        CHECK(ft_lens_read(&b, f, &e) == -1);   /* fl and p missing */
        fclose(f);
    }

    /* lens fit recovers the curve from the eight measured focus peaks */
    {
        double M[8] = { 3807, 4000, 4309, 4787, 5384, 5678, 5947, 6230 };
        long C[8] = { 440254, 429917, 412759, 380637, 335765, 312131,
                      289987, 265226 };
        long L[8] = { 911050, 922360, 938639, 959966, 981011, 989872,
                      997296, 1004246 };
        ft_lens l = { 1460000.0, 270000.0, 320000.0, 3.5e-4 };
        double rc, rl;
        CHECK(ft_lens_fit(M, C, L, 8, &l, &rc, &rl) == 0);
        CHECK(rc < 240 && rl < 130);
        CHECK_NEAR(l.film, ft_lens_default.film, 3000);
        CHECK_NEAR(l.p, ft_lens_default.p, 2e-6);
        CHECK(ft_lens_fit(M, C, L, 3, &l, &rc, &rl) == -1);
    }
    return TEST_DONE();
}
