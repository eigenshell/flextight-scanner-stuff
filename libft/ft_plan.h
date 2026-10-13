/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optics options for a scan at a given ppi, with estimates.
 *
 * Any bin dividing 12 whose optical scale ppi x bin is in focus range
 * (FT_SCALE_MIN..FT_SCALE_MAX) gives the same output ppi. A larger bin
 * is faster: the stock firmware costs ~FT_OUT_PX_RATE output px/s for the
 * full line (FT_CCD_PIXELS / bin px) whatever the window. A smaller bin is
 * a lower magnification m and puts ~(1+m)^-2 more light on every sample
 * (and keeps more of the CCD's pixels: the firmware keeps one per bin).
 * Measured 2026-10-13: line rate x 7992/bin = 84k px/s at bin 12, 2, 1;
 * per-read Dmax (R) ~3.0 at 6300 optics, 3.60 at 2000, 3.58 at 1500
 * (white near the ADC top there: ~3.73 is the per-read ceiling). */
#ifndef FT_PLAN_H
#define FT_PLAN_H
#include "ft_focus.h"

#define FT_OUT_PX_RATE    84000.0  /* output px/s over the full line */
#define FT_SCAN_OVERHEAD  15.0     /* s per scan: hold, calibration, moves */
#define FT_DMAX_6300      3.0      /* per-read Dmax (R) at 6300 optics */
#define FT_DMAX_CEIL      3.73     /* log10((16383 - 310) / 3 DN) */

/* bins usable at ppi, largest (fastest) first; returns how many (0..6) */
int ft_bins_for_ppi(double ppi, int bins[6]);
/* 1 if bin is one of ft_bins_for_ppi's */
int ft_bin_ok(double ppi, int bin);
/* light per sample relative to the 6300 optics */
double ft_light_rel(const ft_lens *l, double scale);
/* per-read Dmax (R) estimate for that light */
double ft_dmax_est(double light);
/* seconds for a scan len_mm along the strip */
double ft_scan_seconds(double ppi, int bin, double len_mm);

#endif
