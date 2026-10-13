/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Auto-estimates that fill in a render recipe (ft_render.h). They never
 * render anything: every value they set is visible and can be
 * overridden. Statistics come from the developed frame(s), 2 % in from
 * each edge, on at most ~4 M sampled pixels, as density histograms
 * (0.001 D bins), so no allocation. Several frames pool their statistics
 * (one setting for a roll). The film base normally comes from the preview
 * (ft_layout.base, job key film_base); ft_render_base_from_frames is the
 * fallback when a scan has none. */
#ifndef FT_RENDER_AUTO_H
#define FT_RENDER_AUTO_H
#include "ft_image.h"
#include "ft_render.h"

/* per-channel film base estimate from the frames themselves: neg/b&w the
 * 0.5th percentile of density (the thinnest film), slide the 99.5th (the
 * densest). -1 if the frames have no pixels. */
int ft_render_base_from_frames(const ft_image *const *im, int n,
                               ft_render_mode m, float d[3], ft_err *e);
/* neg/b&w: range = 99.5th percentile density above base (>= 0.05) and the
 * exposure that puts that highlight at paper white + 0.03 D; slide:
 * exposure 0. Tint is set to 0. r->mode and r->base must be set. */
int ft_render_auto(const ft_image *const *im, int n, ft_recipe *r,
                   ft_err *e);

#endif
