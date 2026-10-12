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
    return TEST_DONE();
}
