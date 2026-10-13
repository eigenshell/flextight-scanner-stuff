/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Whole-holder previews. "standard" is FlexColor's captured 525 ppi
 * preview (6300 optics, bin 12: the stock firmware keeps one CCD pixel in
 * 12). The others are synthesized at a lower magnification, which puts
 * ~1/(1+m)^2 more light on every sample (2026-10-13):
 *   hires   1000 ppi, optics 2000, bin 2  (~3.5x the light)
 *   native  1500 ppi, optics 1500, bin 1  (3.9x measured; every CCD px)
 * All cover the standard preview's window. Frame detection runs on a copy
 * box-averaged by `down` to ~500 ppi, near the 525 ppi it is tuned on. */
#ifndef FT_PREVIEW_H
#define FT_PREVIEW_H
#include "ft_detect.h"

#define FT_PREVIEW_LINES_525 4795L  /* holder length at 525 ppi (~232 mm) */
#define FT_PREVIEW_X_OUT     2      /* standard preview: first output px */
#define FT_PREVIEW_PX        666    /* standard preview: px per line */

/* Across the film the curve's optics at scale ppi x bin land slightly
 * off the nominal magnification and the ft_geom axis: scale_eff and axis
 * map CCD pixels to holder mm (ft_geom's mm, the 6000-scale frame crops
 * use). Measured 2026-10-13 against the standard preview on four physical
 * edges (holder, hole rows) of one strip, residuals <= 0.03 mm. */
typedef struct {
    const char *name;
    double ppi;        /* output pitch along (and nominally across) */
    int bin;           /* CCD pixels per output pixel across */
    int down;          /* box factor to the detection grid */
    double scale_eff;  /* CCD px per inch of film actually reached */
    double axis;       /* CCD pixel on the optical axis */
} ft_preview;

const ft_preview *ft_preview_find(const char *name);   /* NULL: unknown */
/* window in holder mm: across x0..x0+w (the standard preview's span),
 * along 0..len */
void ft_preview_window(double *x0, double *w, double *len);
long ft_preview_lines(const ft_preview *m);
/* tri-linear row offset in lines at the scan's resolution (12 / bin) */
int ft_preview_rgb_k(const ft_preview *m);
/* holder mm across <-> CCD pixel for this mode's optics */
double ft_preview_mm_to_ccd(const ft_preview *m, double mm);
double ft_preview_ccd_to_mm(const ft_preview *m, double ccd);
/* axes of the detection image of a scan whose first output pixel is
 * x_out: box-averaged by m->down, then RGB-aligned (k / down lines) */
void ft_preview_axes(const ft_preview *m, long x_out, ft_axes *ax);
/* box average f x f: in lines x px x 3 -> out (lines/f) x (px/f) x 3;
 * leftover lines and columns are dropped */
void ft_box_down(const float *in, int lines, int px, int f, float *out);

#endif
