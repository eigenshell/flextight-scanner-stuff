/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <string.h>
#include "ft_image.h"

/* IEC 61966-2-1 (sRGB), XYZ D65 -> linear sRGB */
static const double SRGB_FROM_XYZ[9] = {
     3.2404542, -1.5371385, -0.4985314,
    -0.9692660,  1.8760108,  0.0415560,
     0.0556434, -0.2040259,  1.0572252,
};

void ft_image_init(ft_image *im)
{
    memset(im, 0, sizeof *im);
    im->film = -1;
    memcpy(im->xyz_to_rgb, SRGB_FROM_XYZ, sizeof im->xyz_to_rgb);
    strcpy(im->make, "Imacon");
    strcpy(im->model, "Flextight Precision II");
}

int ft_image_alloc(ft_image *im, int w, int h, ft_err *e)
{
    if (w < 1 || h < 1)
        return ft_fail(e, "image: bad size %dx%d", w, h);
    if ((double)w * h * 3 > (double)((size_t)-1 / sizeof(float)))
        return ft_fail(e, "image: %dx%d is too large", w, h);
    im->rgb = calloc((size_t)w * h * 3, sizeof *im->rgb);
    if (!im->rgb)
        return ft_fail(e, "image: out of memory for %dx%d", w, h);
    im->w = w;
    im->h = h;
    return 0;
}

void ft_image_free(ft_image *im)
{
    free(im->rgb);
    im->rgb = NULL;
    im->w = im->h = 0;
}
