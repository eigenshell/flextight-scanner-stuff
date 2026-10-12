/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ft_focus.h"

const ft_lens ft_lens_default = { 1468601.0, 268105.0, 317836.0, 3.4799e-4 };

int ft_bin_for_ppi(double ppi)
{
    static const int bins[] = { 12, 6, 4, 3, 2, 1 };
    int i;
    for (i = 0; i < 6; i++)
        if (ppi * bins[i] <= 6300 + 1e-6)
            return bins[i];
    return -1;
}

int ft_ppi_supported(double ppi)
{
    int bin = ft_bin_for_ppi(ppi);
    double scale;
    if (ppi < 525 || bin < 0)
        return 0;
    scale = ppi * bin;
    return scale >= FT_SCALE_MIN - 1e-6 && scale <= FT_SCALE_MAX + 1e-6;
}

int ft_optics_for(const ft_lens *l, double scale, long *ccd, long *optic)
{
    double m, u, v, lens;
    if (scale < FT_SCALE_MIN - 1e-6 || scale > FT_SCALE_MAX + 1e-6)
        return -1;
    m = l->p * scale;
    u = l->fl * (1 + 1 / m);
    v = l->fl * (1 + m);
    lens = l->film - u;
    *optic = (long)(lens + 0.5);
    *ccd = (long)(lens + l->w - v + 0.5);
    return 0;
}

int ft_positions_ok(long ccd, long optic)
{
    return ccd >= FT_CCD_MIN && ccd <= FT_CCD_MAX &&
           optic >= FT_OPTIC_MIN && optic <= FT_OPTIC_MAX && ccd < optic;
}

double ft_sharpness(const float *g, int w, int h)
{
    double s = 0;
    long n = 0;
    int x, y;
    for (y = 0; y < h; y++) {
        const float *r = g + (long)y * w;
        for (x = 1; x < w; x++) {
            double a = log(r[x - 1] > 1e-3f ? r[x - 1] : 1e-3);
            double b = log(r[x] > 1e-3f ? r[x] : 1e-3);
            s += (b - a) * (b - a);
            n++;
        }
    }
    return n ? sqrt(s / n) : 0;
}

int ft_peak(const long *x, const double *y, int n, double *peak)
{
    int i, m = 0;
    double x0, x1, x2, y0, y1, y2, num, den;
    if (n < 3)
        return -1;
    for (i = 1; i < n; i++)
        if (y[i] > y[m])
            m = i;
    if (m == 0 || m == n - 1) {
        *peak = (double)x[m];
        return 1;
    }
    x0 = x[m - 1]; x1 = x[m]; x2 = x[m + 1];
    y0 = y[m - 1]; y1 = y[m]; y2 = y[m + 1];
    num = (x1 - x0) * (x1 - x0) * (y1 - y2) - (x1 - x2) * (x1 - x2) * (y1 - y0);
    den = (x1 - x0) * (y1 - y2) - (x1 - x2) * (y1 - y0);
    *peak = den != 0 ? x1 - 0.5 * num / den : x1;
    return 0;
}

ft_focus_mode ft_focus_decide(const long off[3], long *shift)
{
    double m = (off[0] + off[1] + off[2]) / 3.0;
    int i;
    *shift = (long)(m < 0 ? m - 0.5 : m + 0.5);
    for (i = 0; i < 3; i++)
        if (fabs(off[i] - m) > FT_FOCUS_MARGIN)
            return FT_FOCUS_PER_FRAME;
    return FT_FOCUS_SHIFTED;
}
