/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ftdev's scan-to-share commands (share.c) and stock learning (stock.c). */
#ifndef SHARE_H
#define SHARE_H
#include "ft_image.h"
#include "ft_look.h"
#include "ft_share_cut.h"

/* whole file + a NUL; NULL if unreadable (ftdev.c) */
void *ftdev_load(const char *path, long *size);
/* the box x,y,w,h of im as a new image (allocated in cut); -1 with e
 * filled if it does not fit */
int ftdev_crop(const ft_image *im, const int box[4], ft_image *cut,
               ft_err *e);
/* noise reduction on cut (transmittance, before the render) as export
 * does it: sensor NR with the profile of im (the whole frame's DNG), else
 * the sidecar's (sc NULL: none), else estimated; then grain NR. 0 ok, 1
 * out of memory (printed). */
int ftdev_denoise(const ft_image *im, const ft_share_sidecar *sc,
                  ft_image *cut, const ft_look_detail *d);
/* ftdev cut <window.raw> [-o base] [-d defects.txt] */
int ftdev_cut(int argc, char **argv);
/* ftdev export <frame.dng> [render flags] [--space ..] [--gamut ..]
 * [--crop x,y,w,h] [--long-edge px] [--quality q] [--raw-too] [-o ..]
 * [--edit roll.edit [--frame N]] */
int ftdev_export(int argc, char **argv);
/* ftdev stock learn <roll.edit> <frame.dng>... -o <name.look> [--replace] */
int ftdev_stock(int argc, char **argv);

#endif
