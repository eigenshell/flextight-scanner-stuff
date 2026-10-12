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
    double y0, x0, len, width;    /* window, holder mm */
    double tilt_deg;
    int sprockets, straighten, confidence_low, done;
    long focus_offset;            /* per-frame mode: measured offset */
} ft_job_frame;

typedef struct {
    char holder[16];
    ft_film film;
    ft_focus_mode focus;
    long focus_offset;            /* shifted mode: CCD steps on the curve */
    int nframes;
    ft_job_frame f[FT_MAX_FRAMES];
} ft_job;

int ft_job_write(const ft_job *j, FILE *out);
int ft_job_read(ft_job *j, FILE *in, ft_err *e);
int ft_job_validate(const ft_job *j, const ft_lens *lens, ft_err *e);

#endif
