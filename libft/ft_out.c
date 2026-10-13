/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ft_dng.h"
#include "ft_out.h"
#include "ft_png.h"

static const ft_writer WRITERS[] = {
    { "dng", ".dng", "linear 16-bit DNG (darktable, RawTherapee, Lightroom)",
      ft_dng_write },
    { "png", ".png", "8-bit preview, gamma 2.2, not inverted",
      ft_png_write_preview },
    { NULL, NULL, NULL, NULL },
};

const ft_writer *ft_writers(void)
{
    return WRITERS;
}

const ft_writer *ft_writer_find(const char *name)
{
    const ft_writer *w;
    for (w = WRITERS; w->name; w++)
        if (strcmp(w->name, name) == 0)
            return w;
    return NULL;
}

int ft_png_write_preview(const char *path, const ft_image *im, ft_err *e)
{
    size_t n, i;
    unsigned char *rgb;
    int rc;
    if (!im->rgb || im->w < 1 || im->h < 1)
        return ft_fail(e, "png: empty image");
    n = (size_t)im->w * im->h * 3;
    rgb = malloc(n);
    if (!rgb)
        return ft_fail(e, "png: out of memory");
    for (i = 0; i < n; i++) {
        float v = im->rgb[i];
        v = v <= 0 ? 0 : v >= 1 ? 1 : (float)pow(v, 1 / 2.2);
        rgb[i] = (unsigned char)(v * 255 + 0.5f);
    }
    rc = ft_png_write_rgb8(path, rgb, im->w, im->h, e);
    free(rgb);
    return rc;
}
