/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
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
    CHECK_NEAR(ft_out_to_ccd(98, 0, 6), 732, 1e-9);
    CHECK_NEAR(ft_out_to_ccd(2, 3, 12), 204, 1e-9);
    CHECK_NEAR(ft_out_to_ccd(5, 0, 1), 149, 1e-9);
    CHECK_NEAR(ft_ccd_to_mm(144, 6000), 0, 1e-9);
    CHECK_NEAR(ft_ccd_to_mm(4118, 6000), 16.8233, 1e-4);

    h = ft_holder_find("24x217");
    CHECK(h != NULL);
    CHECK_NEAR(h->scan_len_mm, 231.0, 1e-9);
    CHECK_NEAR(h->scan_width_mm, 32.0, 1e-9);
    CHECK(h->grid_frames == 6);
    CHECK(strcmp(h->name, "24x217") == 0);
    /* FlexColor 3f autoframe: 50, 943, 992 lines at 630 ppi */
    CHECK_NEAR(h->grid_first_mm, 50 / 630.0 * 25.4, 0.01);
    CHECK_NEAR(h->grid_pitch_mm, 943 / 630.0 * 25.4, 0.01);
    CHECK_NEAR(h->grid_frame_mm, 992 / 630.0 * 25.4, 0.01);
    /* the last grid frame overshoots the scan length; callers clip it */
    CHECK_NEAR(h->grid_first_mm + (h->grid_frames - 1) * h->grid_pitch_mm +
               h->grid_frame_mm, 232.12, 1e-9);
    CHECK(h->grid_first_mm + (h->grid_frames - 1) * h->grid_pitch_mm +
          h->grid_frame_mm > h->scan_len_mm);
    CHECK(ft_holder_find("60x180") == NULL);
    CHECK(ft_holder_find(NULL) == NULL);
    return TEST_DONE();
}
