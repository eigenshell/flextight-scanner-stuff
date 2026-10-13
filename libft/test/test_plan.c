/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdio.h>
#include "ft_plan.h"
#include "test/ftest.h"

int main(void)
{
    int b[6], n;

    /* usable bins: optics ppi x bin within 1500..6300, largest first */
    n = ft_bins_for_ppi(2000, b);
    CHECK(n == 3 && b[0] == 3 && b[1] == 2 && b[2] == 1);
    n = ft_bins_for_ppi(1000, b);
    CHECK(n == 4 && b[0] == 6 && b[1] == 4 && b[2] == 3 && b[3] == 2);
    n = ft_bins_for_ppi(525, b);
    CHECK(n == 4 && b[0] == 12 && b[3] == 3);
    n = ft_bins_for_ppi(3150, b);
    CHECK(n == 2 && b[0] == 2 && b[1] == 1);
    n = ft_bins_for_ppi(6300, b);
    CHECK(n == 1 && b[0] == 1);
    CHECK(ft_bins_for_ppi(6301, b) == 0);
    CHECK(ft_bins_for_ppi(500, b) == 0);
    CHECK(ft_bins_for_ppi(NAN, b) == 0);
    /* the default (fastest) is ft_bin_for_ppi's */
    CHECK(ft_bins_for_ppi(2000, b) && b[0] == ft_bin_for_ppi(2000));
    CHECK(ft_bins_for_ppi(1200, b) && b[0] == ft_bin_for_ppi(1200));
    CHECK(ft_bin_ok(2000, 1) && ft_bin_ok(2000, 3));
    CHECK(!ft_bin_ok(2000, 6) && !ft_bin_ok(2000, 5) && !ft_bin_ok(1000, 1));

    /* light: 1 at 6300 optics; ~3.5x at 2000 and ~4.4x at 1500 */
    CHECK_NEAR(ft_light_rel(&ft_lens_default, 6300), 1.0, 1e-12);
    CHECK_NEAR(ft_light_rel(&ft_lens_default, 2000), 3.543, 0.01);
    CHECK_NEAR(ft_light_rel(&ft_lens_default, 1500), 4.400, 0.01);
    /* Dmax: 3.0 at 6300 (measured), ~3.55 at 2000 (3.60 measured), the
     * per-read ceiling caps it */
    CHECK_NEAR(ft_dmax_est(1.0), 3.0, 1e-12);
    CHECK_NEAR(ft_dmax_est(ft_light_rel(&ft_lens_default, 2000)), 3.55, 0.01);
    CHECK_NEAR(ft_dmax_est(100), FT_DMAX_CEIL, 1e-12);

    /* time: the measured line rates (21.1 l/s at bin 2, 10.5 at bin 1,
     * 127 at bin 12) over the hires/native/standard previews */
    CHECK_NEAR(ft_scan_seconds(1000, 2, 9133 / 1000.0 * 25.4) -
               FT_SCAN_OVERHEAD, 433.5, 5);
    CHECK_NEAR(ft_scan_seconds(1500, 1, 13700 / 1500.0 * 25.4) -
               FT_SCAN_OVERHEAD, 1300.7, 15);
    CHECK_NEAR(ft_scan_seconds(525, 12, 4795 / 525.0 * 25.4) -
               FT_SCAN_OVERHEAD, 37.9, 1);
    /* a 36 mm frame at 2000 ppi: bin 3 ~1.5 min, bin 1 ~4.6 min */
    CHECK_NEAR(ft_scan_seconds(2000, 3, 36), 15 + 2834.6 * 2664 / 84000.0,
               0.5);
    CHECK(ft_scan_seconds(2000, 1, 36) > 3 * (ft_scan_seconds(2000, 3, 36) -
                                              FT_SCAN_OVERHEAD));

    return TEST_DONE();
}
