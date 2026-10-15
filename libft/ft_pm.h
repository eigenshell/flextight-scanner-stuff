/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Print modules (ft_print.h): the science behind the emulsions. One file
 * per module (ft_pm_*.c); an emulsion (ft_emulsion.c) picks one per slot
 * and gives it its parameters. To add science: a new module file with its
 * parameter block here, then an emulsion entry that uses it. Every module
 * is pure C99 (libft/test/purity.sh). */
#ifndef FT_PM_H
#define FT_PM_H
#include "ft_print.h"

/* film: classic = the frame's own auto range (ft_render_auto: contrast
 * stretched per frame, today's ftdev export); stock = one range for the
 * roll, scale x ratio (no per-frame stretch) */
extern const ft_pm_film ft_pm_film_classic;
extern const ft_pm_film ft_pm_film_stock;
typedef struct {
    float scale;            /* green range, D: the emulsion's contrast */
    int has_ratio;          /* else the roll's median r/g and b/g ratio */
    float ratio[3];         /* range[c] / range[green]; green = 1 */
} ft_pm_stock_par;

/* balance: classic = ft_render_auto's exposure (highlight at paper
 * white), no tint */
extern const ft_pm_balance ft_pm_balance_classic;

/* print: classic = ft_render's sigmoid paper */
extern const ft_pm_print ft_pm_print_classic;
typedef struct {
    float grade, black, white;      /* ft_recipe's grade, paper_black and
                                       paper_white */
} ft_pm_paper_par;                  /* NULL par: ft_recipe_default's */

/* output: classic = the recipe's matrix (par: 9 floats, NULL = the
 * default sRGB primaries -> Rec.2020) */
extern const ft_pm_output ft_pm_output_classic;

/* median of v[0..n) (n >= 1); sorts v */
double ft_print_median(double *v, int n);

#endif
