/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <string.h>
#include "ft_render_auto.h"

#define H_D0 -0.2               /* histogram: densities -0.2 .. 4.6 */
#define H_STEP 0.001
#define H_N 4800
#define MAX_SAMPLES 4000000.0
#define BLOCK 4                 /* statistics on 4x4 means: noise / 4 */
#define SEL_MEAN 4              /* channel code: mean of the 3 densities */
#define SEL_LUMA 3              /* channel code: luminance density */
#define SAME_D 0.15             /* highlight neighbour: same area within */

/* Density of channel ch (0..2, SEL_LUMA by w, SEL_MEAN) for the BLOCK x
 * BLOCK block at x, y: means of transmittance, because dense highlights
 * are a few DN above dark and one pixel's density is mostly noise there. */
static double block_density(const ft_image *m, int x, int y, int ch,
                            const float w[3])
{
    double t[3] = { 0, 0, 0 }, l;
    int i, j, c;
    for (j = 0; j < BLOCK; j++)
        for (i = 0; i < BLOCK; i++) {
            const float *p = m->rgb + 3 * ((size_t)(y + j) * m->w + x + i);
            for (c = 0; c < 3; c++)
                t[c] += p[c];
        }
    for (c = 0; c < 3; c++) {
        t[c] /= BLOCK * BLOCK;
        if (!(t[c] >= FT_T_FLOOR))
            t[c] = FT_T_FLOOR;                   /* also NaN */
    }
    if (ch < 3)
        return -log10(t[ch]);
    if (ch == SEL_LUMA) {
        l = w[0] * t[0] + w[1] * t[1] + w[2] * t[2];
        return -log10(l > FT_T_FLOOR ? l : FT_T_FLOOR);
    }
    return -(log10(t[0]) + log10(t[1]) + log10(t[2])) / 3;
}

/* The histogram of block densities of channel ch over every frame, 2 % in
 * from each edge, strided to <= MAX_SAMPLES blocks in all. With sel >= 0:
 * only where the block's density of channel sel is >= min, measuring the
 * block to its right. Same scene (highlights are areas), independent
 * noise: selecting a block for its own value would pick its noise.
 * Returns the count. */
static unsigned long histogram(const ft_image *const *im, int n, int ch,
                               const float w[3], int sel, double min,
                               unsigned *h)
{
    unsigned long total = 0;
    double npx = 0;
    int f, step;
    for (f = 0; f < n; f++)
        npx += (double)im[f]->w * im[f]->h;
    step = (int)ceil(sqrt(npx / MAX_SAMPLES));
    if (step < BLOCK)
        step = BLOCK;
    memset(h, 0, H_N * sizeof *h);
    for (f = 0; f < n; f++) {
        const ft_image *m = im[f];
        int x0 = m->w / 50, x1 = m->w - m->w / 50;
        int y0 = m->h / 50, y1 = m->h - m->h / 50, x, y;
        if (!m->rgb)
            continue;
        for (y = y0; y + BLOCK <= y1; y += step)
            for (x = x0; x + (sel >= 0 ? 2 : 1) * BLOCK <= x1; x += step) {
                int mx = x;
                long b;
                if (sel >= 0) {
                    if (block_density(m, x, y, sel, w) < min)
                        continue;
                    mx = x + BLOCK;
                    /* neighbour in other scene content: skip it. The
                     * margin is ~5x the block noise, so this hardly
                     * selects on noise */
                    if (block_density(m, mx, y, sel, w) < min - SAME_D)
                        continue;
                }
                b = (long)floor((block_density(m, mx, y, ch, w) - H_D0) /
                                H_STEP);
                if (b < 0)
                    b = 0;
                if (b >= H_N)
                    b = H_N - 1;
                h[b]++;
                total++;
            }
    }
    return total;
}

/* density at quantile q (0..1) of the histogram */
static double quantile(const unsigned *h, unsigned long total, double q)
{
    double want, cum = 0;
    int b;
    /* defensive: an empty histogram has no quantile (callers check) */
    if (total == 0)
        return H_D0;
    want = q * (double)(total - 1);
    for (b = 0; b < H_N; b++) {
        cum += h[b];
        if (cum > want)
            break;
    }
    if (b == H_N)
        b = H_N - 1;
    return H_D0 + (b + 0.5) * H_STEP;
}

int ft_render_base_from_frames(const ft_image *const *im, int n,
                               ft_render_mode m, float d[3], ft_err *e)
{
    unsigned h[H_N];
    int c;
    for (c = 0; c < 3; c++) {
        unsigned long tot = histogram(im, n, c, NULL, -1, 0, h);
        if (tot == 0)
            return ft_fail(e, "auto: no pixels for a film base estimate");
        d[c] = (float)quantile(h, tot, m == FT_RENDER_SLIDE ? 0.995 : 0.005);
    }
    return 0;
}

int ft_render_auto(const ft_image *const *im, int n, ft_recipe *r,
                   ft_err *e)
{
    unsigned h[H_N];
    unsigned long tot;
    double min, st, z, u;
    int c, sel;
    for (c = 0; c < 3; c++)
        r->tint[c] = 0;
    if (r->mode == FT_RENDER_SLIDE) {
        r->exposure = 0;
        return 0;
    }
    /* The brightest 0.5 % of the scene (densest film), picked once on the
     * mean density so every channel sees the same blocks; each channel's
     * range is its median there. A percentile per channel would pick each
     * channel's own noise tail, the blue most: a colour cast. */
    sel = r->mode == FT_RENDER_BW ? SEL_LUMA : SEL_MEAN;
    tot = histogram(im, n, sel, r->bw_weights, -1, 0, h);
    if (tot == 0)
        return ft_fail(e, "auto: no pixels to estimate from");
    min = quantile(h, tot, 0.995) - H_STEP / 2;
    for (c = 0; c < 3; c++) {
        int ch = r->mode == FT_RENDER_BW ? SEL_LUMA : c;
        double range;
        tot = histogram(im, n, ch, r->bw_weights, sel, min, h);
        if (tot == 0) {
            /* a thin highlight with no same-area neighbour to measure:
             * use the channel's own 99.5th percentile instead, slightly
             * noise-biased but better than nothing */
            tot = histogram(im, n, ch, r->bw_weights, -1, 0, h);
            range = quantile(h, tot, 0.995) - r->base[c];
        } else
            range = quantile(h, tot, 0.5) - r->base[c];
        r->range[c] = (float)(range > 0.05 ? range : 0.05);
    }
    /* the highlight (n = 1) at paper white + 0.03 D:
     * s = 0.03 / (black - white), z = logit(s) = 4 grade u,
     * u = -0.5 - lift / range[1], lift = exposure log10(2) */
    st = 0.03 / (r->paper_black - r->paper_white);
    z = log(st / (1 - st));
    u = z / (4 * r->grade);
    r->exposure = (float)(-(u + 0.5) * r->range[1] / log10(2));
    if (r->exposure > 6)
        r->exposure = 6;
    if (r->exposure < -6)
        r->exposure = -6;
    return 0;
}
