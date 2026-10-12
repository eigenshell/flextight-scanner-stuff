/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_geom.h"
#include "ft_holder.h"
#include "test/ftest.h"

int main(void)
{
    const ft_holder *h;

    /* at the 6000 scale, mm map straight onto CCD pixels from 144 */
    CHECK_NEAR(ft_mm_to_ccd(10.0, 6000), 10.0 / 25.4 * 6000 + 144, 1e-9);
    /* the optical axis is the fixed point of every scale */
    CHECK_NEAR(ft_mm_to_ccd(ft_ccd_to_mm(FT_CCD_AXIS, 6000), 3800),
               FT_CCD_AXIS, 1e-9);
    CHECK_NEAR(ft_ccd_to_mm(ft_mm_to_ccd(13.0, 4800), 4800), 13.0, 1e-9);
    /* the hardware run: 2400 ppi (bin 2) crop at 13 mm started at x=1626 */
    CHECK_NEAR((ft_mm_to_ccd(13.0, 4800) - FT_CCD_FIRST) / 2, 1626, 0.6);
    CHECK_NEAR(ft_out_to_ccd(98, 0, 6), 98 * 6 + 144, 1e-9);
    CHECK_NEAR(ft_out_to_ccd(2, 3, 12), (2 + 3) * 12 + 144, 1e-9);

    h = ft_holder_find("24x217");
    CHECK(h != NULL);
    CHECK_NEAR(h->scan_len_mm, 231.0, 1e-9);
    CHECK_NEAR(h->scan_width_mm, 32.0, 1e-9);
    CHECK(h->grid_frames == 6);
    CHECK(ft_holder_find("60x180") == NULL);
    return TEST_DONE();
}
