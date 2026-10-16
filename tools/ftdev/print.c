/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Scan to share as a print (libft/ft_print.h):
 *
 *   ftdev print <frame.dng>... --emulsion <id> [--cmyd c,m,y,d] [-o out.jpg]
 *   ftdev print <frame.dng>... --roll <roll.print> [--job <file.job>]
 *   ftdev emulsions
 *
 * The frames given are the roll: the auto balance compares them (on stats
 * proxies, FT_PRINT_STATS_EDGE px), so print a roll's frames together.
 * Each frame (ftdev cut's <base>_frameNN.dng and its sidecar) is cropped
 * to its box, noise-reduced, printed through the emulsion with its clicks
 * (--cmyd for every frame, or the .print file's per frame), resized to the
 * share size and written as <frame>.jpg (sRGB) with how it was made in
 * <frame>.jpg.txt and the JPEG comment. -o: one frame only.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft_dng.h"
#include "ft_emulsion.h"
#include "ft_job.h"
#include "ft_jpeg.h"
#include "ft_pm.h"
#include "ft_print.h"
#include "ft_print_file.h"
#include "ft_render_auto.h"
#include "ft_resize.h"
#include "render_flags.h"
#include "share.h"

#define PATH_CAP 4096

static ft_print_render g_print;         /* ~380 KB */
static ft_print_file g_pf;

typedef struct {
    const char *dng;
    int frame;                          /* -1: unknown */
    int has_side;
    ft_share_sidecar sc;
    ft_image proxy;
} pframe;

static void print_usage(void)
{
    fprintf(stderr, "usage: ftdev print <frame.dng>... --emulsion <id> "
                    "[--cmyd c,m,y,d] [-o out.jpg]\n"
                    "       ftdev print <frame.dng>... --roll <roll.print> "
                    "[--job <file.job>]\n"
                    "       ftdev emulsions\n");
}

int ftdev_emulsions(int argc, char **argv)
{
    static const char *MODE[3] = { "negative", "slide", "b&w" };
    const ft_emulsion *em;
    int i;
    (void)argv;
    if (argc) {
        print_usage();
        return 2;
    }
    printf("emulsions (science revision %d):\n", FT_EMULSION_REV);
    for (i = 0; (em = ft_emulsion_at(i)) != NULL; i++)
        printf("  %-16s %-9s %s\n", em->id, MODE[em->mode], em->name);
    return 0;
}

static void list_ids(void)
{
    const ft_emulsion *em;
    int i;
    for (i = 0; (em = ft_emulsion_at(i)) != NULL; i++)
        fprintf(stderr, "%s%s", i ? " " : "", em->id);
}

/* the frame number in "..._frameNN.dng"; -1 if none */
static int frame_of_name(const char *path)
{
    const char *p = strstr(path, "_frame");
    const char *last = NULL;
    while (p) {
        last = p;
        p = strstr(p + 1, "_frame");
    }
    if (last) {
        char *end;
        long v = strtol(last + 6, &end, 10);
        if (end != last + 6 && strcmp(end, ".dng") == 0 && v >= 0 &&
            v <= 9999)
            return (int)v;
    }
    return -1;
}

/* the frame's DNG cropped to its box (the sidecar's, else all of it) */
static int load_cut(const pframe *f, ft_image *im, ft_image *cut)
{
    int box[4];
    ft_err e;
    if (ft_dng_read(f->dng, im, &e)) {
        fprintf(stderr, "ftdev: %s\n", e.msg);
        return 1;
    }
    if (f->has_side) {
        memcpy(box, f->sc.box, sizeof box);
    } else {
        box[0] = box[1] = 0;
        box[2] = im->w;
        box[3] = im->h;
    }
    if (ftdev_crop(im, box, cut, &e)) {
        fprintf(stderr, "ftdev: %s: %s (sidecar from another DNG?)\n",
                f->dng, e.msg);
        return 1;
    }
    return 0;
}

/* how the print was made: the .txt next to the JPEG and its comment */
static int print_text(char *buf, size_t cap, const ft_emulsion *em,
                      int frame, ft_cmyd k, const ft_print_auto *a,
                      const ft_recipe *r, const ft_look_detail *d)
{
    char s[3][32];
    size_t n = 0;
    int w;
#define APP(...) do { \
        w = snprintf(buf + (n < cap ? n : cap), n < cap ? cap - n : 0, \
                     __VA_ARGS__); \
        if (w > 0) \
            n += (size_t)w; \
    } while (0)
#define F3(v) fmtf((v)[0], s[0], 32), fmtf((v)[1], s[1], 32), \
              fmtf((v)[2], s[2], 32)
    APP("emulsion=%s\n", em->id);
    APP("science=%d\n", FT_EMULSION_REV);
    APP("chain=%s %s %s %s\n", em->film->name, em->balance->name,
        em->print->name, em->output->name);
    if (frame >= 0)
        APP("frame=%d\n", frame);
    APP("cmyd=%d %d %d %d\n", k.c, k.m, k.y, k.d);
    APP("auto_exposure=%s\n", fmtf(a->exposure, s[0], 32));
    APP("auto_tint=%s %s %s\n", F3(a->tint));
    APP("film=%s\n", r->mode == FT_RENDER_BW ? "bw"
                     : r->mode == FT_RENDER_SLIDE ? "slide" : "neg");
    APP("base=%s %s %s\n", F3(r->base));
    APP("range=%s %s %s\n", F3(r->range));
    APP("exposure=%s\n", fmtf(r->exposure, s[0], 32));
    APP("tint=%s %s %s\n", F3(r->tint));
    APP("grade=%s\n", fmtf(r->grade, s[0], 32));
    APP("paper_black=%s\n", fmtf(r->paper_black, s[0], 32));
    APP("paper_white=%s\n", fmtf(r->paper_white, s[0], 32));
    APP("nr_sensor=%g\nnr_chroma=%g\nnr_grain=%g\nsharpen=%g\n",
        d->nr_sensor, d->nr_chroma, d->nr_grain, d->sharpen);
    APP("space=srgb\ngamut=soft\n");
#undef F3
#undef APP
    return (int)n;
}

int ftdev_print(int argc, char **argv)
{
    static pframe fs[FT_MAX_FRAMES];
    static ft_print_frame pfr[FT_MAX_FRAMES];
    static const ft_look_detail DET = { 1, 2, 0, 0.5 };  /* share's */
    const char *emulsion = NULL, *cmyd = NULL, *roll_path = NULL;
    const char *job_path = NULL, *outp = NULL;
    const ft_emulsion *em;
    const ft_image *proxies[FT_MAX_FRAMES];
    ft_cmyd k1 = { 0, 0, 0, 0 };
    ft_print_roll roll;
    float base[3];
    int i, n = 0, rc = 1, nbase = 0;
    double bsum[3][FT_MAX_FRAMES];
    ft_err e;

    for (i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--emulsion") == 0 && i + 1 < argc)
            emulsion = argv[++i];
        else if (strcmp(a, "--cmyd") == 0 && i + 1 < argc)
            cmyd = argv[++i];
        else if (strcmp(a, "--roll") == 0 && i + 1 < argc)
            roll_path = argv[++i];
        else if (strcmp(a, "--job") == 0 && i + 1 < argc)
            job_path = argv[++i];
        else if (strcmp(a, "-o") == 0 && i + 1 < argc)
            outp = argv[++i];
        else if (a[0] == '-') {
            print_usage();
            return 2;
        } else {
            if (n == FT_MAX_FRAMES) {
                fprintf(stderr, "ftdev: more than %d frames\n",
                        FT_MAX_FRAMES);
                return 2;
            }
            memset(&fs[n], 0, sizeof fs[n]);
            fs[n].dng = a;
            ft_image_init(&fs[n].proxy);
            n++;
        }
    }
    if (n == 0) {
        print_usage();
        return 2;
    }
    if (roll_path && (emulsion || cmyd)) {
        fprintf(stderr, "ftdev: with --roll the emulsion and the clicks come "
                        "from the .print file\n");
        return 2;
    }
    if (!roll_path && !emulsion) {
        fprintf(stderr, "ftdev: print needs --emulsion <id> or --roll "
                        "<roll.print>\n");
        return 2;
    }
    if (job_path && !roll_path) {
        fprintf(stderr, "ftdev: --job needs --roll\n");
        return 2;
    }
    if (outp && n != 1) {
        fprintf(stderr, "ftdev: -o needs exactly one frame\n");
        return 2;
    }
    if (roll_path) {
        long nt = 0;
        char *txt = ftdev_load(roll_path, &nt);
        if (!txt) {
            fprintf(stderr, "ftdev: cannot read %s\n", roll_path);
            return 1;
        }
        if (ft_print_file_parse(txt, (size_t)nt, &g_pf, &e)) {
            fprintf(stderr, "ftdev: %s: %s\n", roll_path, e.msg);
            free(txt);
            return 2;
        }
        free(txt);
        emulsion = g_pf.emulsion;
    }
    em = ft_emulsion_find(emulsion);
    if (!em) {
        fprintf(stderr, "ftdev: --emulsion: '%s' is not an emulsion (",
                emulsion);
        list_ids();
        fprintf(stderr, ")\n");
        return 2;
    }
    if (cmyd) {
        char extra;
        if (sscanf(cmyd, "%d,%d,%d,%d%c", &k1.c, &k1.m, &k1.y, &k1.d,
                   &extra) != 4 || ft_print_clicks_ok(em, k1, &e)) {
            fprintf(stderr, "ftdev: --cmyd is c,m,y,d (whole numbers, "
                            "-%d..%d; b&w: 0,0,0,d)\n", FT_PRINT_CLICKS_MAX,
                    FT_PRINT_CLICKS_MAX);
            return 2;
        }
    }

    /* each frame: its sidecar, frame number and stats proxy */
    for (i = 0; i < n; i++) {
        pframe *f = &fs[i];
        char path[PATH_CAP];
        long ns = 0;
        char *side;
        ft_image im, cut;
        int pw, ph;
        void *scratch;
        if (snprintf(path, sizeof path, "%s.txt", f->dng) >= (int)sizeof path) {
            fprintf(stderr, "ftdev: path too long: %.60s...\n", f->dng);
            rc = 2;
            goto out;
        }
        side = ftdev_load(path, &ns);
        if (side) {
            if (ft_share_sidecar_parse(side, (size_t)ns, &f->sc, &e)) {
                fprintf(stderr, "ftdev: %s: %s\n", path, e.msg);
                free(side);
                goto out;
            }
            free(side);
            f->has_side = 1;
        }
        f->frame = f->has_side && f->sc.frame > 0 ? f->sc.frame
                                                  : frame_of_name(f->dng);
        if (roll_path && f->frame < 0) {
            fprintf(stderr, "ftdev: %s: no frame number (no sidecar, no "
                            "_frameNN in the name)\n", f->dng);
            rc = 2;
            goto out;
        }
        if (f->has_side &&
            (f->sc.film == FT_FILM_SLIDE) != (em->mode == FT_RENDER_SLIDE)) {
            fprintf(stderr, "ftdev: %s is a %s; %s prints %s\n", f->dng,
                    f->sc.film == FT_FILM_SLIDE ? "slide" : "negative",
                    em->id, em->mode == FT_RENDER_SLIDE ? "slides"
                                                        : "negatives");
            rc = 2;
            goto out;
        }
        if (f->has_side && f->sc.has_film_base) {
            int c;
            for (c = 0; c < 3; c++)
                bsum[c][nbase] = f->sc.film_base[c];
            nbase++;
        }
        ft_image_init(&im);
        ft_image_init(&cut);
        if (load_cut(f, &im, &cut)) {
            ft_image_free(&im);
            ft_image_free(&cut);
            goto out;
        }
        ft_image_free(&im);
        ft_print_proxy_dims(cut.w, cut.h, &pw, &ph);
        scratch = malloc(ft_resize_scratch_bytes(cut.w, cut.h, pw, ph));
        if (!scratch || ft_image_alloc(&f->proxy, pw, ph, &e) ||
            ft_resize(cut.rgb, cut.w, cut.h, f->proxy.rgb, pw, ph, scratch,
                      &e)) {
            fprintf(stderr, "ftdev: %s\n", scratch ? e.msg : "out of memory");
            free(scratch);
            ft_image_free(&cut);
            goto out;
        }
        free(scratch);
        ft_image_free(&cut);
        proxies[i] = &f->proxy;
    }

    /* the roll: base, [frame N] check, the balance across the frames */
    if (nbase) {
        int c;
        for (c = 0; c < 3; c++)
            base[c] = (float)ft_print_median(bsum[c], nbase);
    } else {
        if (ft_render_base_from_frames(proxies, n, em->mode, base, &e)) {
            fprintf(stderr, "ftdev: %s\n", e.msg);
            goto out;
        }
        fprintf(stderr, "ftdev: note: no film_base in the sidecars; "
                        "estimated from the frames\n");
    }
    if (roll_path) {
        int idx[FT_MAX_FRAMES], ni = 0;
        if (job_path) {
            static ft_job job;
            FILE *jf = fopen(job_path, "r");
            if (!jf || ft_job_read(&job, jf, &e)) {
                fprintf(stderr, "ftdev: %s: %s\n", job_path,
                        jf ? e.msg : "cannot read");
                if (jf)
                    fclose(jf);
                rc = 2;
                goto out;
            }
            fclose(jf);
            for (i = 0; i < job.nframes; i++)
                idx[ni++] = job.f[i].index;
        } else {
            for (i = 0; i < n; i++)
                idx[ni++] = fs[i].frame;
        }
        if (ft_print_file_unknown(&g_pf, idx, ni, &e))
            fprintf(stderr, "ftdev: warning: %s: %s\n", roll_path, e.msg);
    }
    if (ft_print_roll_measure(em, base, proxies, n, pfr, &roll, &e)) {
        fprintf(stderr, "ftdev: %s\n", e.msg);
        goto out;
    }
    if (n < 6 && em->balance == &ft_pm_balance_frontier)
        fprintf(stderr, "ftdev: note: %d frame%s: print a roll's frames "
                        "together for the roll's colour balance\n", n,
                n == 1 ? "" : "s");

    /* each frame: the print */
    for (i = 0; i < n; i++) {
        pframe *f = &fs[i];
        ft_cmyd k = roll_path ? ft_print_file_clicks(&g_pf, f->frame) : k1;
        ft_print_auto a;
        ft_recipe r;
        ft_image im, cut;
        char out[PATH_CAP], text[4096];
        int clamped, t, r2;
        double scale;
        if (ft_print_recipe(em, &roll, &pfr[i], k, &r, &a, &clamped, &e)) {
            fprintf(stderr, "ftdev: %s: %s\n", f->dng, e.msg);
            rc = 2;
            goto out;
        }
        if (clamped)
            fprintf(stderr, "ftdev: warning: %s: the filtration is at its "
                            "limit (50 CC, 6 stops)\n", f->dng);
        if (ft_print_compile(em, &r, &g_print, &e)) {
            fprintf(stderr, "ftdev: %s: %s\n", f->dng, e.msg);
            goto out;
        }
        ft_image_init(&im);
        ft_image_init(&cut);
        if (load_cut(f, &im, &cut)) {
            ft_image_free(&im);
            ft_image_free(&cut);
            goto out;
        }
        if (ftdev_denoise(&im, f->has_side ? &f->sc : NULL, &cut, &DET)) {
            ft_image_free(&im);
            ft_image_free(&cut);
            goto out;
        }
        ft_image_free(&im);
        ft_print_apply(&g_print, cut.rgb, cut.rgb, (size_t)cut.w * cut.h);
        if (outp) {
            snprintf(out, sizeof out, "%s", outp);
        } else {
            size_t l = strlen(f->dng);
            if (l > 4 && strcmp(f->dng + l - 4, ".dng") == 0)
                l -= 4;
            if (l + 5 > sizeof out) {
                fprintf(stderr, "ftdev: path too long: %.60s...\n", f->dng);
                ft_image_free(&cut);
                rc = 2;
                goto out;
            }
            memcpy(out, f->dng, l);
            memcpy(out + l, ".jpg", 5);
        }
        scale = f->has_side ? f->sc.export_scale : 1;
        if (!f->has_side)
            fprintf(stderr, "ftdev: note: no sidecar for %s; printing at "
                            "full size\n", f->dng);
        t = print_text(text, sizeof text, em, f->frame, k, &a, &r, &DET);
        if (t >= (int)sizeof text) {
            fprintf(stderr, "ftdev: print text too long\n");
            ft_image_free(&cut);
            goto out;
        }
        printf("print: %s frame %d, %s, cmyd %d %d %d %d\n", f->dng,
               f->frame, em->id, k.c, k.m, k.y, k.d);
        r2 = ftdev_finish(&cut, scale > 1 ? 1 : scale, DET.sharpen,
                          FT_SPACE_SRGB, FT_GAMUT_SOFT, FT_JPEG_QUALITY,
                          text, out);
        ft_image_free(&cut);
        if (r2) {
            rc = r2;
            goto out;
        }
    }
    rc = 0;
out:
    for (i = 0; i < n; i++)
        ft_image_free(&fs[i].proxy);
    return rc;
}
