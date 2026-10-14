/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The job file: what `ftpoc frames` found and `ftpoc scan --job` scans.
 * Plain key = value text; see tools/poc/README.md. '#' always starts a
 * comment, even after a value. */
#ifndef FT_JOB_H
#define FT_JOB_H
#include <stdio.h>
#include "ft.h"
#include "ft_focus.h"

typedef struct {
    int index, line;              /* line: where [frame N] was read */
    ft_frame_type type;
    int include;
    double ppi;
    int bin;                      /* CCD px per output px; 0 = the fastest
                                     (ft_bin_for_ppi). Smaller = lower
                                     magnification, more light, slower */
    double y0, x0, len, width;    /* window, holder mm */
    double tilt_deg;
    int sprockets, straighten, confidence_low, done;
    long focus_offset;            /* per-frame mode: measured offset */
    int group;                    /* share job: the window it scans in
                                     (1..); 0 = not in a share job */
} ft_job_frame;

typedef struct {
    char holder[16];
    ft_film film;
    ft_focus_mode focus;
    long focus_offset;            /* shifted mode: CCD steps on the curve */
    int has_film_base;
    double film_base[3];          /* D of unexposed film per channel, from
                                     the preview (ft_layout.base): the
                                     negative's base or the slide's black */
    double share;                 /* scan to share: target px on each
                                     frame's long side (ft_share.h); 0 =
                                     an ordinary job, one scan per frame */
    int hf_half_res;              /* share: half frames at the FF's ppi */
    int nframes;
    ft_job_frame f[FT_MAX_FRAMES];
} ft_job;

int ft_job_write(const ft_job *j, FILE *out);
int ft_job_read(ft_job *j, FILE *in, ft_err *e);
int ft_job_validate(const ft_job *j, const ft_lens *lens, ft_err *e);
/* the bin a frame scans with: its own, or the fastest for its ppi */
int ft_job_bin(const ft_job_frame *f);

#endif
