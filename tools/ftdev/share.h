/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ftdev's scan-to-share commands (share.c). */
#ifndef SHARE_H
#define SHARE_H

/* whole file + a NUL; NULL if unreadable (ftdev.c) */
void *ftdev_load(const char *path, long *size);
/* ftdev cut <window.raw> [-o base] [-d defects.txt] */
int ftdev_cut(int argc, char **argv);
/* ftdev export <frame.dng> [render flags] [--space ..] [--gamut ..]
 * [--crop x,y,w,h] [--long-edge px] [--quality q] [--raw-too] [-o ..] */
int ftdev_export(int argc, char **argv);

#endif
