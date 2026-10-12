/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "test/synth.h"

#define PI 3.14159265358979323846

void synth_default(synth_spec *s, ft_film film)
{
    memset(s, 0, sizeof *s);
    s->px = 666;
    s->lines = 4795;
    s->y_mm_per_line = 25.4 / 525;
    s->x_mm_per_px = 25.4 * 12 / 6300;
    s->film = film;
    s->film_y0 = 3.0;
    s->film_y1 = 231.5;
    s->film_x0 = -1.4;
    s->film_x1 = 33.6;
    s->img_x0 = 4.1;
    s->img_x1 = 28.1;
    s->seed = 1;
}

void synth_add(synth_spec *s, double y0, double len, int blank)
{
    s->fr[s->nframes].y0 = y0;
    s->fr[s->nframes].len = len;
    s->fr[s->nframes].blank = blank;
    s->nframes++;
}

static int in_hole(const synth_spec *s, double x, double yf)
{
    double ph;
    int across = (x >= s->film_x0 + 2.0 && x < s->film_x0 + 4.8) ||
                 (x >= s->film_x1 - 4.8 && x < s->film_x1 - 2.0);
    if (!across)
        return 0;
    ph = fmod(yf - s->film_y0 - 1.0, 4.75);
    if (ph < 0)
        ph += 4.75;
    return ph < 1.98;
}

static double content(double x, double y, int ch, unsigned seed)
{
    double ph = seed * 0.37 + ch * 1.1;
    double v = 0.5 + 0.25 * sin(x * 1.3 + ph) * cos(y * 0.7 + 2 * ph) +
               0.25 * sin((x + y) * 2.1 + 3 * ph);
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static int base_like_area(const synth_spec *s, double x, double yf)
{
    int i;
    for (i = 0; i < s->npatch; i++)
        if (yf >= s->patch[i].y0 && yf < s->patch[i].y1 &&
            x >= s->patch[i].x0 && x < s->patch[i].x1)
            return 1;
    return s->blob && (x - s->blob_x) * (x - s->blob_x) +
                      (yf - s->blob_y) * (yf - s->blob_y) < s->blob_r * s->blob_r;
}

float *synth_preview(const synth_spec *s)
{
    static const float base_neg[3] = { 0.62f, 0.40f, 0.25f };
    static const float base_slide[3] = { 0.003f, 0.003f, 0.003f };
    const float *base = s->film == FT_FILM_SLIDE ? base_slide : base_neg;
    float *t = malloc((size_t)s->lines * s->px * 3 * sizeof *t);
    double tn = tan(s->tilt_deg * PI / 180), xc = 0.5 * (s->img_x0 + s->img_x1);
    unsigned rng = s->seed * 2654435761u + 1;
    int l, c, k, i;

    if (!t)
        return NULL;
    for (l = 0; l < s->lines; l++)
        for (c = 0; c < s->px; c++) {
            double y = l * s->y_mm_per_line, x = c * s->x_mm_per_px;
            double yf = y - (x - xc) * tn;            /* tilted strip */
            float *p = t + ((size_t)l * s->px + c) * 3;
            int fr = -1;
            if (y < s->film_y0 || y >= s->film_y1) {  /* holder metal */
                p[0] = p[1] = p[2] = 0.0005f;
                continue;
            }
            if (x < s->film_x0 || x >= s->film_x1 || in_hole(s, x, yf)) {
                p[0] = p[1] = p[2] = 1.0f;            /* open beam */
                continue;
            }
            for (i = 0; i < s->nframes; i++)
                if (yf >= s->fr[i].y0 && yf < s->fr[i].y0 + s->fr[i].len)
                    fr = i;
            for (k = 0; k < 3; k++) {
                double v = base[k];
                if (fr >= 0 && !s->fr[fr].blank && x >= s->img_x0 &&
                    x < s->img_x1 && !base_like_area(s, x, yf)) {
                    double n = content(x, yf, k, s->seed);
                    v = s->film == FT_FILM_NEGATIVE
                            ? base[k] * pow(10, -(0.3 + 0.9 * n) * (1 - 0.1 * k))
                            : 0.03 + 0.6 * n;
                }
                rng = rng * 1103515245u + 12345u;     /* +-0.4 % noise */
                v *= 1 + 0.004 * (((rng >> 16) & 1023) / 511.5 - 1);
                p[k] = (float)(v < 0 ? 0 : v > 1 ? 1 : v);
            }
        }
    return t;
}
