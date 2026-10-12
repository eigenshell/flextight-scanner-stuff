/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "ft_holder.h"
#include "ft_job.h"

static const char *TYPES[] = { "full", "half", "panorama", "partial" };

static const char *yn(int v) { return v ? "yes" : "no"; }

int ft_job_write(const ft_job *j, FILE *out)
{
    int i;
    fprintf(out, "[strip]\nholder = %s\nfilm = %s\nfocus = %s\n"
                 "focus_offset = %ld\n", j->holder,
            j->film == FT_FILM_SLIDE ? "slide" : "negative",
            j->focus == FT_FOCUS_PER_FRAME ? "per-frame" : "shifted",
            j->focus_offset);
    for (i = 0; i < j->nframes; i++) {
        const ft_job_frame *f = &j->f[i];
        fprintf(out, "\n[frame %d]\ntype = %s\ninclude = %s\nppi = %g\n"
                     "window = %.2f %.2f %.2f %.2f   # y0 x0 length width, mm\n"
                     "tilt = %.2f\nsprockets = %s\nstraighten = %s\n"
                     "confidence = %s\nfocus_offset = %ld\ndone = %s\n",
                f->index, TYPES[f->type], yn(f->include), f->ppi,
                f->y0, f->x0, f->len, f->width, f->tilt_deg,
                yn(f->sprockets), yn(f->straighten),
                f->confidence_low ? "low" : "high", f->focus_offset,
                yn(f->done));
    }
    return ferror(out) ? -1 : 0;
}

static char *trim(char *s)
{
    char *end;
    while (isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = 0;
    return s;
}

static int yesno(const char *v, int *out)
{
    if (strcmp(v, "yes") == 0) { *out = 1; return 0; }
    if (strcmp(v, "no") == 0)  { *out = 0; return 0; }
    return -1;
}

static int num(const char *v, double *out)
{
    char *end;
    *out = strtod(v, &end);
    return end != v && *trim(end) == 0 ? 0 : -1;
}

static int lnum(const char *v, long *out)
{
    char *end;
    *out = strtol(v, &end, 10);
    return end != v && *trim(end) == 0 ? 0 : -1;
}

int ft_job_read(ft_job *j, FILE *in, ft_err *e)
{
    char line[256];
    int ln = 0, in_strip = 0;
    ft_job_frame *f = NULL;

    memset(j, 0, sizeof *j);
    while (fgets(line, sizeof line, in)) {
        char *s, *k, *v, *hash;
        int bad = 0;
        ln++;
        if ((hash = strchr(line, '#')) != NULL)
            *hash = 0;
        s = trim(line);
        if (!*s)
            continue;
        if (*s == '[') {
            int n;
            if (strcmp(s, "[strip]") == 0) {
                in_strip = 1;
                f = NULL;
                continue;
            }
            if (sscanf(s, "[frame %d]", &n) == 1) {
                if (j->nframes == FT_MAX_FRAMES)
                    return ft_fail(e, "line %d: more than %d frames", ln,
                                   FT_MAX_FRAMES);
                f = &j->f[j->nframes++];
                f->index = n;
                f->line = ln;
                f->include = 1;
                in_strip = 0;
                continue;
            }
            return ft_fail(e, "line %d: unknown section %s", ln, s);
        }
        v = strchr(s, '=');
        if (!v)
            return ft_fail(e, "line %d: expected key = value", ln);
        *v++ = 0;
        k = trim(s);
        v = trim(v);
        if (in_strip) {
            if (strcmp(k, "holder") == 0) {
                if (strlen(v) >= sizeof j->holder)
                    bad = 1;
                else
                    strcpy(j->holder, v);
            } else if (strcmp(k, "film") == 0) {
                if (strcmp(v, "negative") == 0) j->film = FT_FILM_NEGATIVE;
                else if (strcmp(v, "slide") == 0) j->film = FT_FILM_SLIDE;
                else bad = 1;
            } else if (strcmp(k, "focus") == 0) {
                if (strcmp(v, "shifted") == 0) j->focus = FT_FOCUS_SHIFTED;
                else if (strcmp(v, "per-frame") == 0) j->focus = FT_FOCUS_PER_FRAME;
                else bad = 1;
            } else if (strcmp(k, "focus_offset") == 0) {
                bad = lnum(v, &j->focus_offset);
            } else {
                return ft_fail(e, "line %d: unknown key %s", ln, k);
            }
        } else if (f) {
            if (strcmp(k, "type") == 0) {
                int t;
                for (t = 0; t < 4 && strcmp(v, TYPES[t]) != 0; t++)
                    ;
                if (t == 4) bad = 1; else f->type = (ft_frame_type)t;
            } else if (strcmp(k, "include") == 0) {
                bad = yesno(v, &f->include);
            } else if (strcmp(k, "ppi") == 0) {
                bad = num(v, &f->ppi);
            } else if (strcmp(k, "window") == 0) {
                char extra;
                bad = sscanf(v, "%lf %lf %lf %lf %c", &f->y0, &f->x0, &f->len,
                             &f->width, &extra) != 4;
            } else if (strcmp(k, "tilt") == 0) {
                bad = num(v, &f->tilt_deg);
            } else if (strcmp(k, "sprockets") == 0) {
                bad = yesno(v, &f->sprockets);
            } else if (strcmp(k, "straighten") == 0) {
                bad = yesno(v, &f->straighten);
            } else if (strcmp(k, "confidence") == 0) {
                if (strcmp(v, "low") == 0) f->confidence_low = 1;
                else if (strcmp(v, "high") == 0) f->confidence_low = 0;
                else bad = 1;
            } else if (strcmp(k, "focus_offset") == 0) {
                bad = lnum(v, &f->focus_offset);
            } else if (strcmp(k, "done") == 0) {
                bad = yesno(v, &f->done);
            } else {
                return ft_fail(e, "line %d: unknown key %s", ln, k);
            }
        } else {
            return ft_fail(e, "line %d: %s outside a section", ln, k);
        }
        if (bad)
            return ft_fail(e, "line %d: bad value for %s: %s", ln, k, v);
    }
    return 0;
}

int ft_job_validate(const ft_job *j, const ft_lens *lens, ft_err *e)
{
    const ft_holder *h = ft_holder_find(j->holder);
    int i;
    if (!h)
        return ft_fail(e, "unknown holder '%s'", j->holder);
    for (i = 0; i < j->nframes; i++) {
        const ft_job_frame *f = &j->f[i];
        long ccd, optic, off;
        if (!f->include)
            continue;
        if (!ft_ppi_supported(f->ppi))
            return ft_fail(e, "line %d (frame %d): ppi %g not supported "
                              "(525, 634..3150, 3800..6300)", f->line,
                           f->index, f->ppi);
        if (f->len <= 0 || f->width <= 0 || f->y0 < 0 || f->x0 < 0 ||
            f->y0 + f->len > h->scan_len_mm ||
            f->x0 + f->width > h->scan_width_mm)
            return ft_fail(e, "line %d (frame %d): window outside the "
                              "holder (%.0f x %.0f mm)", f->line, f->index,
                           h->scan_len_mm, h->scan_width_mm);
        off = j->focus == FT_FOCUS_PER_FRAME ? f->focus_offset : j->focus_offset;
        if (ft_optics_for(lens, f->ppi * ft_bin_for_ppi(f->ppi), &ccd, &optic) ||
            !ft_positions_ok(ccd + off, optic))
            return ft_fail(e, "line %d (frame %d): focus offset %ld puts "
                              "the CCD outside FlexColor's range", f->line,
                           f->index, off);
    }
    return 0;
}
