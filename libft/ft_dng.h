/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Linear DNG writer: uncompressed 16-bit LinearRaw RGB (DNG 1.4), the
 * image's transmittance scaled to 0..65535, with ColorMatrix1 from the
 * image (D65) and an as-shot neutral of 1,1,1: the open beam is white,
 * since the flat-field already divided the lamp out. */
#ifndef FT_DNG_H
#define FT_DNG_H
#include "ft.h"
#include "ft_image.h"

int ft_dng_write(const char *path, const ft_image *im, ft_err *e);

#endif
