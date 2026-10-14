/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ft_dng.h"
#include "ft_out.h"
#include "ft_png.h"
#include "ft_render.h"

static const ft_writer WRITERS[] = {
    { "dng", ".dng", "linear 16-bit DNG (darktable, RawTherapee, Lightroom)",
      ft_dng_write },
    { "png", ".png", "8-bit preview, gamma 2.2, not inverted",
      ft_png_write_preview },
    { "positive", "_positive.png",
      "rendered positive, 16-bit sRGB PNG (needs a render: --film)",
      ft_png_write_positive },
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

/* linear Rec.2020 -> linear sRGB (the inverse of ITU-R BT.2087) */
static const double SRGB_FROM_2020[9] = {
     1.6604910021, -0.5876411388, -0.0728498633,
    -0.1245504745,  1.1328998971, -0.0083494226,
    -0.0181507634, -0.1005788980,  1.1187296614,
};

int ft_png_write_positive(const char *path, const ft_image *im, ft_err *e)
{
    size_t n, i;
    unsigned short *o;
    int c, rc;
    if (!im->rgb || im->w < 1 || im->h < 1)
        return ft_fail(e, "positive: empty image");
    n = (size_t)im->w * im->h;
    o = malloc(n * 3 * sizeof *o);
    if (!o)
        return ft_fail(e, "positive: out of memory");
    for (i = 0; i < n; i++) {
        const float *p = im->rgb + 3 * i;
        for (c = 0; c < 3; c++) {
            double v = SRGB_FROM_2020[3 * c] * p[0] +
                       SRGB_FROM_2020[3 * c + 1] * p[1] +
                       SRGB_FROM_2020[3 * c + 2] * p[2];
            /* hard clip for now; gamut mapping comes with the output stage */
            o[3 * i + c] = (unsigned short)(ft_srgb_encode(v) * 65535 + 0.5);
        }
    }
    rc = ft_png_write_rgb16(path, o, im->w, im->h, e);
    free(o);
    return rc;
}
