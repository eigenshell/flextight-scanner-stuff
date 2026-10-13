/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "ft_geom.h"
#include "ft_preview.h"

static const ft_preview MODES[] = {
    { "standard", 525.0, 12, 1, 6300.0, FT_CCD_AXIS },
    { "hires", 1000.0, 2, 2, 1992.4, 4140.5 },     /* nominal 2000 */
    { "native", 1500.0, 1, 3, 1491.5, 4132.9 },    /* nominal 1500 */
};

const ft_preview *ft_preview_find(const char *name)
{
    size_t i;
    if (!name)
        return NULL;
    for (i = 0; i < sizeof MODES / sizeof MODES[0]; i++)
        if (strcmp(MODES[i].name, name) == 0)
            return &MODES[i];
    return NULL;
}

void ft_preview_window(double *x0, double *w, double *len)
{
    double a = ft_ccd_to_mm(ft_out_to_ccd(FT_PREVIEW_X_OUT, 0, 12), 6300);
    double b = ft_ccd_to_mm(ft_out_to_ccd(FT_PREVIEW_X_OUT, FT_PREVIEW_PX, 12),
                            6300);
    *x0 = a;
    *w = b - a;
    *len = FT_PREVIEW_LINES_525 * 25.4 / 525;
}

/* ft_geom's mapping with this mode's scale and axis */
double ft_preview_mm_to_ccd(const ft_preview *m, double mm)
{
    return m->axis +
           (mm / 25.4 * 6000.0 + FT_CCD_FIRST - m->axis) * m->scale_eff /
               6000.0;
}

double ft_preview_ccd_to_mm(const ft_preview *m, double ccd)
{
    return ((ccd - m->axis) * 6000.0 / m->scale_eff + m->axis -
            FT_CCD_FIRST) * 25.4 / 6000.0;
}

long ft_preview_lines(const ft_preview *m)
{
    /* the same rounding as ftpoc's holder-end check */
    return (long)((double)FT_PREVIEW_LINES_525 * m->ppi / 525.0);
}

int ft_preview_rgb_k(const ft_preview *m)
{
    return 12 / m->bin;
}

void ft_preview_axes(const ft_preview *m, long x_out, ft_axes *ax)
{
    int f = m->down;
    /* centre of a detection pixel: f output pixels of bin CCD pixels */
    double half = (f * m->bin - 1) / 2.0;
    ax->y_mm_per_line = 25.4 / m->ppi * f;
    ax->y_mm0 = (ft_preview_rgb_k(m) / f) * ax->y_mm_per_line;
    ax->x_mm0 =
        ft_preview_ccd_to_mm(m, ft_out_to_ccd(x_out, 0, m->bin) + half);
    ax->x_mm_per_px =
        ft_preview_ccd_to_mm(m, ft_out_to_ccd(x_out, f, m->bin) + half) -
        ax->x_mm0;
}

void ft_box_down(const float *in, int lines, int px, int f, float *out)
{
    int lo = lines / f, po = px / f, l, c, i, j, k;
    float norm = 1.0f / (f * f);
    for (l = 0; l < lo; l++)
        for (c = 0; c < po; c++)
            for (k = 0; k < 3; k++) {
                float s = 0;
                for (i = 0; i < f; i++)
                    for (j = 0; j < f; j++)
                        s += in[((size_t)(l * f + i) * px + c * f + j) * 3 + k];
                out[((size_t)l * po + c) * 3 + k] = s * norm;
            }
}
