/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include "ft_geom.h"
#include "ft_plan.h"

int ft_bins_for_ppi(double ppi, int bins[6])
{
    static const int all[] = { 12, 6, 4, 3, 2, 1 };
    int i, n = 0;
    if (!isfinite(ppi) || !(ppi >= 525))
        return 0;
    for (i = 0; i < 6; i++) {
        double scale = ppi * all[i];
        if (scale >= FT_SCALE_MIN - 1e-6 && scale <= FT_SCALE_MAX + 1e-6)
            bins[n++] = all[i];
    }
    return n;
}

int ft_bin_ok(double ppi, int bin)
{
    int bins[6], n = ft_bins_for_ppi(ppi, bins), i;
    for (i = 0; i < n; i++)
        if (bins[i] == bin)
            return 1;
    return 0;
}

double ft_light_rel(const ft_lens *l, double scale)
{
    double r = (1 + l->p * 6300.0) / (1 + l->p * scale);
    return r * r;
}

double ft_dmax_est(double light)
{
    double d = FT_DMAX_6300 + log10(light);
    return d < FT_DMAX_CEIL ? d : FT_DMAX_CEIL;
}

double ft_scan_seconds(double ppi, int bin, double len_mm)
{
    double lines = len_mm / 25.4 * ppi;
    return FT_SCAN_OVERHEAD + lines * (FT_CCD_PIXELS / (double)bin) /
                                  FT_OUT_PX_RATE;
}
