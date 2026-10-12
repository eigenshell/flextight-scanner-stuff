/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Focus: thin lens on one rail, motor steps. Film at `film`, lens at L,
 * CCD at C: u = film - L, v = L - C + w, 1/u + 1/v = 1/fl, and
 * v/u = p * scale (scale = ppi x bin = CCD px per inch of film; p = CCD
 * pixel pitch in inches). ft_lens_default is this scanner's fit to eight
 * focus brackets (2026-10-12, rms 223 CCD / 114 lens steps).
 */
#ifndef FT_FOCUS_H
#define FT_FOCUS_H
#include <stdio.h>
#include "ft.h"

typedef struct { double film, w, fl, p; } ft_lens;
extern const ft_lens ft_lens_default;

#define FT_SCALE_MIN     3800.0
#define FT_SCALE_MAX     6300.0
#define FT_FOCUS_MARGIN  1200L      /* CCD steps: ~5 % sharpness loss */
#define FT_BRACKET_MAX   12000L     /* never bracket further from the curve */
/* every value FlexColor sent; the CCD must also stay below the lens.
 * Exception: the CCD may go to 240000, ~14k steps under FlexColor's
 * 254123: at 3150 ppi negative focus was still rising at 254270
 * (hardware 2026-10-12). Both counters grow towards the film below; a
 * lower CCD value moves the CCD up, away from the lens. Stepped to 241813
 * with steady motor times (user approved). */
#define FT_CCD_MIN    240000L
#define FT_CCD_MAX    1020000L
#define FT_OPTIC_MIN  895204L
#define FT_OPTIC_MAX  1021000L

int ft_bin_for_ppi(double ppi);          /* -1 if ppi > 6300 */
int ft_ppi_supported(double ppi);        /* 1 = crop can scan it in focus */
/* 0 ok; -1 (outputs not written) for scale outside range / NaN or an
 * implausible lens or positions that overflow */
int ft_optics_for(const ft_lens *l, double scale, long *ccd, long *optic);
int ft_positions_ok(long ccd, long optic);

/* rms log-gradient along rows of a w x h single-channel image */
double ft_sharpness(const float *g, int w, int h);
/* x must be ascending; -1 also if any y is not finite.
 * parabola peak through the maximum and its neighbours: 0 ok, 1 maximum
 * at an end (*peak = that x), -1 fewer than 3 points */
int ft_peak(const long *x, const double *y, int n, double *peak);
/* offsets are averaged in double; per-frame if any is > margin from mean */
ft_focus_mode ft_focus_decide(const long off[3], long *shift);

int ft_lens_read(ft_lens *l, FILE *f, ft_err *e);
int ft_lens_write(const ft_lens *l, FILE *f);
/* least-squares fit of l (start value in, result out) to n >= 4 in-focus
 * points (scale M[i], CCD C[i], lens L[i]); rms residuals out */
int ft_lens_fit(const double *M, const long *C, const long *L, int n,
                ft_lens *l, double *rms_ccd, double *rms_lens);

#endif
