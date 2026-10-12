/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft_detect.h"

#define OPEN_T      0.85f   /* all channels above: open beam */
#define FILM_T      0.002f  /* all channels below: holder metal */
#define BASE_DENS   0.08    /* log10 density within this of the base */
#define GAP_FRAC    0.97    /* image columns base-like on a gap line */
#define NEAR_GAP    0.85
#define EDGE_WIN_MM 3.0
#define EDGE_FOUND  0.80
#define EDGE_RMS_MM 0.25
#define MIN_GAP_MM  0.2
#define MIN_RUN_MM  6.0
#define END_SLIVER  3.0
#define BLANK_MM    12.0
#define FALLBACK_MM 45.0
#define HIST        16384

typedef struct {
    int a, b;                          /* gap lines [a, b) */
    int lead_ok, trail_ok, ok;
    double lead_pos, trail_pos;        /* edge line at the image centre */
    double lead_slope, trail_slope;    /* mm along per mm across */
} gap_t;

static const float *pix(const float *t, int px, int l, int c)
{
    return t + ((long)l * px + c) * 3;
}

static int is_open(const float *p)
{
    return p[0] > OPEN_T && p[1] > OPEN_T && p[2] > OPEN_T;
}

static int is_film(const float *p)
{
    return !is_open(p) && (p[0] > FILM_T || p[1] > FILM_T || p[2] > FILM_T);
}

/* widest run of i in [0, n) with lo <= v[i] < hi, as [*r0, *r1) */
static int widest_run(const double *v, int n, double lo, double hi,
                      int *r0, int *r1)
{
    int i, s = -1, best = 0;
    *r0 = *r1 = 0;
    for (i = 0; i <= n; i++) {
        int in = i < n && v[i] >= lo && v[i] < hi;
        if (in && s < 0)
            s = i;
        if (!in && s >= 0) {
            if (i - s > best) {
                best = i - s;
                *r0 = s;
                *r1 = i;
            }
            s = -1;
        }
    }
    return best;
}

/* walk from `from` in direction dir while lo <= v < hi; first index outside */
static int grow(const double *v, int n, int from, int dir, double lo, double hi)
{
    int i = from;
    while (i >= 0 && i < n && v[i] >= lo && v[i] < hi)
        i += dir;
    return i;
}

static int base_level(const float *t, int px, int f0, int f1, int c0, int c1,
                      ft_film film, float base[3])
{
    unsigned long *hist = calloc(3 * HIST, sizeof *hist);
    unsigned long n = 0, want, acc;
    int l, c, k, b;
    if (!hist)
        return -1;
    for (l = f0; l < f1; l++)
        for (c = c0; c < c1; c++) {
            const float *p = pix(t, px, l, c);
            if (!is_film(p))
                continue;
            n++;
            for (k = 0; k < 3; k++) {
                b = (int)(p[k] * (HIST - 1) + 0.5f);
                hist[k * HIST + (b < 0 ? 0 : b >= HIST ? HIST - 1 : b)]++;
            }
        }
    /* negative: 99.5th percentile (brightest film); slide: 0.5th */
    want = (unsigned long)(n * (film == FT_FILM_NEGATIVE ? 0.995 : 0.005));
    for (k = 0; k < 3; k++) {
        acc = 0;
        for (b = 0; b < HIST - 1; b++) {
            acc += hist[k * HIST + b];
            if (acc > want)
                break;
        }
        base[k] = (float)b / (HIST - 1);
        if (base[k] < 1e-4f)
            base[k] = 1e-4f;
    }
    free(hist);
    return 0;
}

/* A frame edge near line `at`: per image column, the first line in
 * [at - win, at + win] where the column switches (dir +1: base-like to
 * exposed, a frame starts; dir -1: exposed to base-like, a frame ends) and
 * stays switched for 3 lines. Accepted when found in EDGE_FOUND of the
 * columns and straight (rms residual of a line fit <= EDGE_RMS_MM). */
static int fit_edge(const unsigned char *bl, int px, int f0, int f1, int c0,
                    int c1, int at, int dir, int win, double ym, double xm,
                    double *pos, double *slope)
{
    int want = dir > 0 ? 0 : 1, c, l, k, n = 0, ok = 0;
    int lo = at - win < f0 + 1 ? f0 + 1 : at - win;
    int hi = at + win > f1 - 3 ? f1 - 3 : at + win;
    double *fx = malloc((c1 - c0) * sizeof *fx);
    double *fy = malloc((c1 - c0) * sizeof *fy);
    double sx = 0, sy = 0, sxx = 0, sxy = 0, a, b, r = 0;

    if (!fx || !fy) {
        free(fx);
        free(fy);
        return 0;
    }
    for (c = c0; c < c1; c++)
        for (l = lo; l <= hi; l++) {
            const unsigned char *p = bl + c;
            if (p[(long)(l - 1) * px] != want && p[(long)l * px] == want &&
                p[(long)(l + 1) * px] == want && p[(long)(l + 2) * px] == want) {
                fx[n] = c;
                fy[n] = l;
                n++;
                break;
            }
        }
    if (n >= 3 && n >= EDGE_FOUND * (c1 - c0)) {
        for (k = 0; k < n; k++) {
            sx += fx[k];
            sy += fy[k];
            sxx += fx[k] * fx[k];
            sxy += fx[k] * fy[k];
        }
        b = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        a = (sy - b * sx) / n;
        for (k = 0; k < n; k++) {
            double d = fy[k] - (a + b * fx[k]);
            r += d * d;
        }
        if (sqrt(r / n) * ym <= EDGE_RMS_MM) {
            *pos = a + b * 0.5 * (c0 + c1);
            *slope = b * ym / xm;
            ok = 1;
        }
    }
    free(fx);
    free(fy);
    return ok;
}

static void classify(double len, int at_end, ft_frame_type *type, int *low,
                     int *keep)
{
    *low = 0;
    *keep = 1;
    if (at_end) {
        if (len < END_SLIVER) *keep = 0;
        else if (fabs(len - 36) <= 1.5) *type = FT_FRAME_FULL;
        else if (fabs(len - 18) <= 1.5) *type = FT_FRAME_HALF;
        else *type = FT_FRAME_PARTIAL;
        return;
    }
    if (len >= 15.5 && len <= 21.0) { *type = FT_FRAME_HALF; return; }
    if (len >= 33.0 && len <= 39.5) { *type = FT_FRAME_FULL; return; }
    if (len > 39.5) { *type = FT_FRAME_PANORAMA; return; }
    if (len < MIN_RUN_MM) { *keep = 0; return; }
    *low = 1;
    if (len < 15.5) *type = FT_FRAME_PARTIAL;
    else *type = len < 27.0 ? FT_FRAME_HALF : FT_FRAME_FULL;
}

static void add_run(ft_layout *L, const unsigned char *bl, const double *g,
                    int px, int ic0, int ic1, int s, int e, const gap_t *before,
                    const gap_t *after, const ft_axes *ax)
{
    double ym = ax->y_mm_per_line, xm = ax->x_mm_per_px;
    double y0 = before && before->trail_ok ? before->trail_pos : s;
    double y1 = after && after->lead_ok ? after->lead_pos : e;
    double slope = 0, near = 0;
    int nslope = 0, c, l, c0 = -1, c1 = -1, low, keep;
    ft_frame_type type;
    ft_det_frame *f;

    classify((y1 - y0) * ym, !before || !after, &type, &low, &keep);
    if (!keep || L->nframes == FT_MAX_FRAMES)
        return;
    for (c = ic0; c < ic1; c++) {
        long n = 0;
        for (l = s; l < e; l++)
            n += !bl[(long)l * px + c];
        if (n >= 0.3 * (e - s)) {
            if (c0 < 0) c0 = c;
            c1 = c + 1;
        }
    }
    if (c0 < 0) {
        c0 = ic0;
        c1 = ic1;
    }
    if (before && before->trail_ok) { slope += before->trail_slope; nslope++; }
    if (after && after->lead_ok)    { slope += after->lead_slope;   nslope++; }
    if (type == FT_FRAME_PANORAMA) {
        for (l = s; l < e; l++)
            if (g[l] >= NEAR_GAP && g[l] < GAP_FRAC)
                near += ym;
        if (near >= 0.3)
            low = 1;            /* maybe a gap we did not trust */
    }
    f = &L->f[L->nframes++];
    f->index = L->nframes;
    f->type = type;
    f->y0 = ax->y_mm0 + y0 * ym;
    f->y1 = ax->y_mm0 + y1 * ym;
    f->x0 = ax->x_mm0 + c0 * xm;
    f->x1 = ax->x_mm0 + c1 * xm;
    f->tilt_deg = nslope ? atan(slope / nslope) * 180 / 3.14159265358979 : 0;
    f->confidence_low = low;
}

int ft_detect(const float *t, int lines, int px, const ft_axes *ax,
              ft_film film, const ft_holder *h, ft_layout *L, ft_err *e)
{
    double ym = ax->y_mm_per_line, xm = ax->x_mm_per_px;
    double *col = calloc(px, sizeof *col), *g = calloc(lines, sizeof *g);
    unsigned char *bl = calloc((size_t)lines * px, 1);
    gap_t *gaps = calloc(lines / 2 + 1, sizeof *gaps);
    int c, l, b0, b1, f0 = -1, f1 = -1, ic0, ic1, ng = 0, nok = 0, i, rc = -1;

    memset(L, 0, sizeof *L);
    if (!col || !g || !bl || !gaps) {
        ft_fail(e, "detect: out of memory");
        goto out;
    }

    /* 1. open fraction per column -> image band and hole rows */
    for (c = 0; c < px; c++) {
        long n = 0;
        for (l = 0; l < lines; l++)
            n += is_open(pix(t, px, l, c));
        col[c] = (double)n / lines;
    }
    if (widest_run(col, px, -1, 0.25, &b0, &b1) * xm < 10) {
        ft_fail(e, "detect: no film band found");
        goto out;
    }
    {
        int left = grow(col, px, b0 - 1, -1, 0.25, 0.9) + 1;
        int right = grow(col, px, b1, +1, 0.25, 0.9);
        L->has_holes = (b0 - left) * xm >= 1.0 && (right - b1) * xm >= 1.0;
        L->band_x0 = ax->x_mm0 + b0 * xm;
        L->band_x1 = ax->x_mm0 + b1 * xm;
        L->holes_x0 = L->has_holes ? ax->x_mm0 + left * xm : L->band_x0;
        L->holes_x1 = L->has_holes ? ax->x_mm0 + right * xm : L->band_x1;
    }

    /* 2. film lines: most band pixels are film */
    for (l = 0; l < lines; l++) {
        long n = 0;
        for (c = b0; c < b1; c++)
            n += is_film(pix(t, px, l, c));
        if (n * 2 > b1 - b0) {
            if (f0 < 0)
                f0 = l;
            f1 = l + 1;
        }
    }
    if (f0 < 0) {
        ft_fail(e, "detect: no film in the holder");
        goto out;
    }
    L->film_y0 = ax->y_mm0 + f0 * ym;
    L->film_y1 = ax->y_mm0 + f1 * ym;

    /* 3. film base */
    if (base_level(t, px, f0, f1, b0, b1, film, L->base)) {
        ft_fail(e, "detect: out of memory");
        goto out;
    }

    /* 4. base-like map, then the image columns (ever exposed) */
    for (l = f0; l < f1; l++)
        for (c = b0; c < b1; c++) {
            const float *p = pix(t, px, l, c);
            double ex = -1e9;
            int k;
            if (is_open(p))
                continue;
            for (k = 0; k < 3; k++) {
                double v = p[k] > 1e-4f ? p[k] : 1e-4;
                double d = film == FT_FILM_NEGATIVE ? log10(L->base[k] / v)
                                                    : log10(v / L->base[k]);
                if (d > ex)
                    ex = d;
            }
            bl[(long)l * px + c] = ex < BASE_DENS;
        }
    for (c = 0; c < px; c++)
        col[c] = 0;
    for (c = b0; c < b1; c++) {
        long n = 0;
        for (l = f0; l < f1; l++)
            n += !bl[(long)l * px + c];
        col[c] = (double)n / (f1 - f0);
    }
    if (widest_run(col, px, 0.05, 2.0, &ic0, &ic1) * xm < 5) {
        L->nblank = (int)((f1 - f0) * ym / 38 + 0.5);   /* nothing exposed */
        rc = 0;
        goto out;
    }
    for (l = f0; l < f1; l++) {
        long n = 0;
        for (c = ic0; c < ic1; c++)
            n += bl[(long)l * px + c];
        g[l] = (double)n / (ic1 - ic0);
    }

    /* 5. gap runs and their edges */
    {
        int minl = (int)ceil(MIN_GAP_MM / ym), s = -1;
        int win = (int)(EDGE_WIN_MM / ym + 0.5);
        for (l = f0; l <= f1; l++) {
            int in = l < f1 && g[l] >= GAP_FRAC;
            if (in && s < 0)
                s = l;
            if (!in && s >= 0) {
                if (l - s >= minl) {
                    gaps[ng].a = s;
                    gaps[ng].b = l;
                    ng++;
                }
                s = -1;
            }
        }
        for (i = 0; i < ng; i++) {
            gap_t *q = &gaps[i];
            int edges = 0;
            if (q->a > f0) {
                edges++;
                q->lead_ok = fit_edge(bl, px, f0, f1, ic0, ic1, q->a, -1, win,
                                      ym, xm, &q->lead_pos, &q->lead_slope);
            }
            if (q->b < f1) {
                edges++;
                q->trail_ok = fit_edge(bl, px, f0, f1, ic0, ic1, q->b, +1, win,
                                       ym, xm, &q->trail_pos, &q->trail_slope);
            }
            q->ok = edges == 0 || q->lead_ok || q->trail_ok;
            nok += q->ok;
        }
    }

    /* 6. no edge at all on a long strip: fixed grid, flagged */
    if (nok == 0 && (f1 - f0) * ym > FALLBACK_MM) {
        double fy0 = L->film_y0, fy1 = L->film_y1;
        L->fallback = 1;
        for (i = 0; i < h->grid_frames && L->nframes < FT_MAX_FRAMES; i++) {
            double y0 = h->grid_first_mm + i * h->grid_pitch_mm;
            double y1 = y0 + h->grid_frame_mm;
            ft_det_frame *f;
            if (y0 < fy0) y0 = fy0;
            if (y1 > fy1) y1 = fy1;
            if (y1 - y0 < MIN_RUN_MM)
                continue;
            f = &L->f[L->nframes++];
            f->index = L->nframes;
            f->type = FT_FRAME_FULL;
            f->y0 = y0;
            f->y1 = y1;
            f->x0 = ax->x_mm0 + ic0 * xm;
            f->x1 = ax->x_mm0 + ic1 * xm;
            f->confidence_low = 1;
        }
        rc = 0;
        goto out;
    }

    /* 7. exposed runs between trusted gaps; long gaps are blank frames */
    {
        int s = f0;
        const gap_t *before = NULL;
        for (i = 0; i <= ng; i++) {
            const gap_t *q = i < ng ? &gaps[i] : NULL;
            if (q && !q->ok)
                continue;               /* untrusted gaps stay in the run */
            if ((q ? q->a : f1) > s)
                add_run(L, bl, g, px, ic0, ic1, s, q ? q->a : f1, before, q, ax);
            if (q) {
                if (q->a > f0 && q->b < f1 && (q->b - q->a) * ym > BLANK_MM) {
                    int n = (int)((q->b - q->a) * ym / 38 + 0.5);
                    L->nblank += n < 1 ? 1 : n;
                }
                s = q->b;
                before = q;
            }
        }
    }
    rc = 0;
out:
    free(col);
    free(g);
    free(bl);
    free(gaps);
    return rc;
}

void ft_layout_to_job(const ft_layout *L, const char *holder, ft_film film,
                      double ppi, int sprockets, ft_job *j)
{
    int i;
    memset(j, 0, sizeof *j);
    snprintf(j->holder, sizeof j->holder, "%s", holder);
    j->film = film;
    j->focus = FT_FOCUS_SHIFTED;
    for (i = 0; i < L->nframes && i < FT_MAX_FRAMES; i++) {
        const ft_det_frame *d = &L->f[i];
        ft_job_frame *f = &j->f[j->nframes++];
        f->index = d->index;
        f->type = d->type;
        f->include = 1;
        f->ppi = ppi;
        f->y0 = d->y0;
        f->len = d->y1 - d->y0;
        f->x0 = sprockets ? L->holes_x0 : d->x0;
        f->width = (sprockets ? L->holes_x1 : d->x1) - f->x0;
        f->tilt_deg = d->tilt_deg;
        f->sprockets = sprockets;
        f->confidence_low = d->confidence_low;
    }
}
