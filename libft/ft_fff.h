/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FlexColor 3F (.fff) writer, byte for byte the layout of
 * src/flextight/fff.py (see there for the tags and why they are ordered
 * as they are): a TIFF that LibRaw reads as Imacon FFF, 16-bit RGB, the
 * image's transmittance scaled to 0..65535. Verified against LibRaw;
 * whether FlexColor itself opens it is not yet tested. */
#ifndef FT_FFF_H
#define FT_FFF_H
#include "ft.h"
#include "ft_image.h"

int ft_fff_write(const char *path, const ft_image *im, ft_err *e);

#endif
