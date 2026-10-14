/* SPDX-License-Identifier: GPL-2.0-or-later */
/* JPEG writer (libjpeg-turbo). Not part of libft.a: libft has no
 * dependencies, so only the programs that write JPEGs (tools/ftdev) build
 * ft_jpeg.c and link libjpeg. Errors come back through ft_err, never as
 * libjpeg's exit(). */
#ifndef FT_JPEG_H
#define FT_JPEG_H
#include <stddef.h>
#include "ft.h"

#define FT_JPEG_QUALITY 90

/* rgb: h rows of w pixels, 3 bytes each (encoded, ft_output_quantise8).
 * quality 1..100; 4:4:4 (no chroma subsampling: film grain and fine
 * colour survive), optimised Huffman tables; the ICC profile (icc, icc_n
 * bytes; NULL for none) in APP2; JFIF density ppi dots per inch (0:
 * none). */
int ft_jpeg_write(const char *path, const unsigned char *rgb, int w, int h,
                  int quality, double ppi, const unsigned char *icc,
                  size_t icc_n, ft_err *e);

#endif
