/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A developed image: what every pipeline stage after the scan works on and
 * what every output writer takes. Pixels are linear transmittance, 0 = no
 * light through the film, 1 = the open-beam white of the scan's own
 * calibration, RGB interleaved, rows top to bottom. */
#ifndef FT_IMAGE_H
#define FT_IMAGE_H
#include "ft.h"

typedef struct {
    int w, h;
    float *rgb;              /* h rows of w RGB floats, 0..1 */
    double ppi;              /* output resolution; 0 = unknown */
    int film;                /* ft_film, or -1 = unknown */
    int frame;               /* job frame number; 0 = none */
    /* XYZ (D65) -> scanner RGB, row-major (the DNG ColorMatrix1). The
     * default treats scanner RGB as linear sRGB until the scanner is
     * profiled from an IT8 target. */
    double xyz_to_rgb[9];
    char make[32], model[32];
    char desc[256];          /* free text: scan settings, job, frame */
} ft_image;

/* metadata defaults, no pixels (rgb NULL, w = h = 0) */
void ft_image_init(ft_image *im);
/* allocates w x h pixels (zeroed); -1 with e filled on failure */
int ft_image_alloc(ft_image *im, int w, int h, ft_err *e);
void ft_image_free(ft_image *im);

#endif
