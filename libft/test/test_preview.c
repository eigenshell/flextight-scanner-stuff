/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdio.h>
#include "ft_focus.h"
#include "ft_geom.h"
#include "ft_preview.h"
#include "test/ftest.h"

int main(void)
{
    const ft_preview *s = ft_preview_find("standard");
    const ft_preview *h = ft_preview_find("hires");
    const ft_preview *n = ft_preview_find("native");
    double x0, w, len;
    long c, o;
    ft_axes ax;

    CHECK(s && h && n);
    CHECK(!ft_preview_find("preview") && !ft_preview_find(NULL));
    CHECK(s->ppi == 525 && s->bin == 12 && s->down == 1);
    CHECK(h->ppi == 1000 && h->bin == 2 && h->down == 2);
    CHECK(n->ppi == 1500 && n->bin == 1 && n->down == 3);

    /* the standard preview's window: 0.898..33.120 mm, 4795 lines */
    ft_preview_window(&x0, &w, &len);
    CHECK_NEAR(x0, 0.8979, 1e-3);
    CHECK_NEAR(x0 + w, 33.1196, 1e-3);
    CHECK_NEAR(len, 231.98, 1e-2);
    CHECK(ft_preview_lines(s) == 4795);
    CHECK(ft_preview_lines(h) == 9133);
    CHECK(ft_preview_lines(n) == 13700);

    /* RGB row offset divides by the detection box: exact alignment */
    CHECK(ft_preview_rgb_k(s) == 1 && ft_preview_rgb_k(h) == 6 &&
          ft_preview_rgb_k(n) == 12);
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

    /* native: 3x3 boxes of 1500 ppi pixels = 500 ppi; x_out 3034 is where
     * the window's 0.898 mm lands at scale 1500 */
    CHECK(lround((ft_preview_mm_to_ccd(n, x0) - FT_CCD_FIRST) / n->bin) ==
          3050);
    ft_preview_axes(n, 3050, &ax);
    CHECK_NEAR(ax.y_mm_per_line, 25.4 / 500, 1e-12);
    CHECK_NEAR(ax.y_mm0, 4 * 25.4 / 500, 1e-12);
    CHECK_NEAR(ax.x_mm_per_px, 3 * 25.4 / 1491.5, 1e-9);
    CHECK_NEAR(ax.x_mm0, x0 + 25.4 / 1491.5, 0.01);  /* centre of 3 px */
    /* hires: 2x2 boxes of 1000 ppi pixels = 500 ppi along */
    ft_preview_axes(h, 0, &ax);
    CHECK_NEAR(ax.y_mm_per_line, 25.4 / 500, 1e-12);
    CHECK_NEAR(ax.y_mm0, 3 * 25.4 / 500, 1e-12);
    CHECK_NEAR(ax.x_mm_per_px, 4 * 25.4 / 1992.4, 1e-9);

    /* the hardware fit (2026-10-13): edges at the strip start as CCD
     * pixels in a hires and a native scan, each taken back to back with a
     * standard preview that put them at these holder mm */
    {
        const double hc[5] = { 5061.3, 2913.3, 3009.7, 4967.4, 5063.3 };
        const double hm[5] = { 28.663, 1.276, 2.499, 27.456, 28.681 };
        const double nc[5] = { 4822.9, 3214.4, 3287.1, 4752.3, 4824.1 };
        const double nm[5] = { 28.637, 1.248, 2.481, 27.431, 28.663 };
        int i;
        for (i = 0; i < 5; i++) {
            CHECK_NEAR(ft_preview_ccd_to_mm(h, hc[i]), hm[i], 0.01);
            CHECK_NEAR(ft_preview_ccd_to_mm(n, nc[i]), nm[i], 0.01);
            /* the nominal model was ~0.19 mm high */
            CHECK(ft_ccd_to_mm(hc[i], 2000) - hm[i] > 0.1);
        }
        /* standard keeps ft_geom's model exactly */
        CHECK_NEAR(ft_preview_ccd_to_mm(s, 3000), ft_ccd_to_mm(3000, 6300),
                   1e-12);
        CHECK_NEAR(ft_preview_mm_to_ccd(s, 10), ft_mm_to_ccd(10, 6300),
                   1e-9);
    }

    /* both synthesized previews are in focus and inside the limits */
    CHECK(ft_optics_for(&ft_lens_default, h->ppi * h->bin, &c, &o) == 0);
    CHECK(ft_positions_ok(c, o) && o - c > 270000);
    CHECK(ft_optics_for(&ft_lens_default, n->ppi * n->bin, &c, &o) == 0);
    CHECK(ft_positions_ok(c, o) && o - c > 215000);

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
