/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdio.h>
#include "ft_focus.h"
#include "ft_geom.h"
#include "ft_plan.h"
#include "ft_preview.h"
#include "test/ftest.h"

int main(void)
{
    const ft_preview *s = ft_preview_find("standard");
    const ft_preview *h = ft_preview_find("1000");
    const ft_preview *n = ft_preview_find("1500");
    double x0, w, len;
    long c, o;
    ft_axes ax;

    CHECK(s && h && n);
    CHECK(ft_preview_find("525") == s);
    CHECK(!ft_preview_find("preview") && !ft_preview_find("hires") &&
          !ft_preview_find(NULL));
    CHECK(s->ppi == 525 && s->bin == 12 && s->down == 1);
    CHECK(h->ppi == 1000 && h->bin == 6 && h->down == 2);
    CHECK(n->ppi == 1500 && n->bin == 4 && n->down == 3);
    /* the synthesized previews use FlexColor's crop optics, 6000 */
    CHECK(h->ppi * h->bin == 6000 && n->ppi * n->bin == 6000);

    /* the standard preview's window: 0.898..33.120 mm, 4795 lines */
    ft_preview_window(&x0, &w, &len);
    CHECK_NEAR(x0, 0.8979, 1e-3);
    CHECK_NEAR(x0 + w, 33.1196, 1e-3);
    CHECK_NEAR(len, 231.98, 1e-2);
    CHECK(ft_preview_lines(s) == 4795);
    CHECK(ft_preview_lines(h) == 9133);
    CHECK(ft_preview_lines(n) == 13700);

    /* RGB row offset divides by the detection box: exact alignment */
    CHECK(ft_preview_rgb_k(s) == 1 && ft_preview_rgb_k(h) == 2 &&
          ft_preview_rgb_k(n) == 3);
    CHECK(ft_preview_rgb_k(h) % h->down == 0);
    CHECK(ft_preview_rgb_k(n) % n->down == 0);

    /* standard axes = the formula frames used before (ftpoc preview_axes) */
    ft_preview_axes(s, 2, &ax);
    CHECK_NEAR(ax.y_mm_per_line, 25.4 / 525, 1e-12);
    CHECK_NEAR(ax.y_mm0, 25.4 / 525, 1e-12);
    CHECK_NEAR(ax.x_mm0, ft_ccd_to_mm(ft_out_to_ccd(2, 0, 12) + 5.5, 6300),
               1e-12);
    CHECK_NEAR(ax.x_mm_per_px,
               ft_ccd_to_mm(ft_out_to_ccd(2, 1, 12) + 5.5, 6300) - ax.x_mm0,
               1e-12);

    /* 1500: 3x3 boxes of 1500 ppi pixels = 500 ppi; the window's 0.898 mm
     * lands at output pixel 53 (CCD pixel 356 at scale 6000) */
    CHECK(lround((ft_mm_to_ccd(x0, 6000) - FT_CCD_FIRST) / n->bin) == 53);
    CHECK((53 + 1903) * n->bin <= FT_CCD_PIXELS);
    ft_preview_axes(n, 53, &ax);
    CHECK_NEAR(ax.y_mm_per_line, 25.4 / 500, 1e-12);
    CHECK_NEAR(ax.y_mm0, 25.4 / 500, 1e-12);
    CHECK_NEAR(ax.x_mm_per_px, 25.4 / 500, 1e-9);
    CHECK_NEAR(ax.x_mm0, x0 + 5.5 / 6000 * 25.4, 0.01);  /* centre of 12 */
    /* 1000: 2x2 boxes of 1000 ppi pixels = 500 ppi */
    CHECK(lround((ft_mm_to_ccd(x0, 6000) - FT_CCD_FIRST) / h->bin) == 35);
    CHECK((35 + 1269) * h->bin <= FT_CCD_PIXELS);
    ft_preview_axes(h, 35, &ax);
    CHECK_NEAR(ax.y_mm_per_line, 25.4 / 500, 1e-12);
    CHECK_NEAR(ax.y_mm0, 25.4 / 500, 1e-12);
    CHECK_NEAR(ax.x_mm_per_px, 25.4 / 500, 1e-9);

    /* in focus and inside the limits: the 6000 crop pair */
    CHECK(ft_optics_for(&ft_lens_default, 6000, &c, &o) == 0);
    CHECK(ft_positions_ok(c, o));

    /* fast: ~2.4 min (1000) and ~5.4 min (1500) for the whole holder,
     * against 7.2 / 21.7 min measured at the low-magnification optics */
    CHECK_NEAR(ft_line_seconds(ft_preview_lines(h), h->bin), 145, 2);
    CHECK_NEAR(ft_line_seconds(ft_preview_lines(n), n->bin), 326, 2);

    /* box average: 4 x 5 -> 2 x 2, last line and column dropped */
    {
        float in[4 * 5 * 3], out[2 * 2 * 3];
        int l, p, k;
        for (l = 0; l < 4; l++)
            for (p = 0; p < 5; p++)
                for (k = 0; k < 3; k++)
                    in[(l * 5 + p) * 3 + k] = (float)(l * 10 + p + k * 100);
        ft_box_down(in, 4, 5, 2, out);
        CHECK_NEAR(out[0], 5.5, 1e-6);                 /* (0,1,10,11) */
        CHECK_NEAR(out[1], 105.5, 1e-6);
        CHECK_NEAR(out[(0 * 2 + 1) * 3], 7.5, 1e-6);   /* (2,3,12,13) */
        CHECK_NEAR(out[(1 * 2 + 1) * 3 + 2], 227.5, 1e-6);
    }

    return TEST_DONE();
}
