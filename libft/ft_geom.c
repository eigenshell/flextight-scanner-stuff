/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ft_geom.h"

double ft_mm_to_ccd(double mm, double scale)
{
    return FT_CCD_AXIS +
           (mm / 25.4 * 6000.0 + FT_CCD_FIRST - FT_CCD_AXIS) * scale / 6000.0;
}

double ft_ccd_to_mm(double ccd, double scale)
{
    return ((ccd - FT_CCD_AXIS) * 6000.0 / scale + FT_CCD_AXIS -
            FT_CCD_FIRST) * 25.4 / 6000.0;
}

double ft_out_to_ccd(double x, double c, int bin)
{
    return (x + c) * bin + FT_CCD_FIRST;
}
