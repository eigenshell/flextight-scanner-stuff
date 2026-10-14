/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Scan to share, after the scanner (ftpoc scan --job of a share job):
 *
 *   ftdev cut <window.raw> [-o base] [-d defects.txt]
 *     the window scan -> one working raw per frame: <base>_frameNN.dng
 *     (linear transmittance, 16-bit) and <base>_frameNN.dng.txt (the
 *     frame box, export scale, film base). base defaults to the scan path
 *     without ".raw" and a trailing "_winNN".
 *
 *   ftdev export <frame.dng> [render flags] [--space srgb|p3|adobe]
 *         [--gamut soft|clip] [--crop x,y,w,h] [--long-edge px]
 *         [--quality q] [--raw-too] [-o out.jpg]
 *     render (film and base from the sidecar unless given), crop (the
 *     frame box unless --crop, in DNG pixels), downscale in linear light
 *     to the share size (the sidecar's export scale; --long-edge px puts
 *     that many px on the frame's long side; never upscales), output
 *     colour (ft_output.h) and an 8-bit JPEG with its ICC profile.
 *     --raw-too also copies the DNG and its sidecar next to the JPEG.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft_dng.h"
#include "ft_icc.h"
#include "ft_jpeg.h"
#include "ft_nr.h"
#include "ft_output.h"
#include "ft_resize.h"
#include "ft_share_cut.h"
#include "ft_sharpen.h"
#include "render_flags.h"
#include "share.h"

static ft_render g_render;              /* ~380 KB */
static ft_output g_output;              /* ~330 KB */

#define PATH_CAP 4096

/* dst = a + b; -1 (message printed) if it does not fit */
static int path_cat(char *dst, size_t cap, const char *a, const char *b)
{
    int n = snprintf(dst, cap, "%s%s", a, b);
    if (n < 0 || (size_t)n >= cap) {
        fprintf(stderr, "ftdev: path too long: %.60s...\n", a);
        return -1;
    }
    return 0;
}

/* dst = src with its trailing from replaced by to (if src ends so) */
static int path_swap(char *dst, size_t cap, const char *src, const char *from,
                     const char *to)
{
    size_t n = strlen(src), f = strlen(from);
    if (n > f && strcmp(src + n - f, from) == 0) {
        char head[PATH_CAP];
        if (n - f >= sizeof head) {
            fprintf(stderr, "ftdev: path too long: %.60s...\n", src);
            return -1;
        }
        memcpy(head, src, n - f);
        head[n - f] = 0;
        return path_cat(dst, cap, head, to);
    }
    return path_cat(dst, cap, src, to);
}

/* path without ext (if it ends so) and then without "_winNN" */
static int base_of(const char *path, const char *ext, char *out, size_t cap)
{
    size_t n;
    if (path_swap(out, cap, path, ext, ""))
        return -1;
    n = strlen(out);
    if (n > 6 && strncmp(out + n - 6, "_win", 4) == 0 &&
        out[n - 2] >= '0' && out[n - 2] <= '9' && out[n - 1] >= '0' &&
        out[n - 1] <= '9')
        out[n - 6] = 0;
    return 0;
}

int ftdev_cut(int argc, char **argv)
{
    static ft_share_cut_result r;
    const char *scan = NULL, *base = NULL, *dpath = NULL;
    char basebuf[PATH_CAP], path[PATH_CAP], txt[PATH_CAP], *meta = NULL, *faul = NULL, *dt = NULL;
    unsigned short *raw = NULL, *cal = NULL;
    long nmeta = 0, nraw = 0, ncal = 0, nfaul = 0, nd = 0;
    ft_share_meta m;
    ft_defects defects;
    ft_err e;
    int i, rc = 1;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) base = argv[++i];
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) dpath = argv[++i];
        else if (argv[i][0] == '-' || scan) {
            fprintf(stderr, "usage: ftdev cut <window.raw> [-o base] "
                            "[-d defects.txt]\n");
            return 2;
        } else
            scan = argv[i];
    }
    if (!scan) {
        fprintf(stderr, "usage: ftdev cut <window.raw> [-o base] "
                        "[-d defects.txt]\n");
        return 2;
    }
    if (!base) {
        if (base_of(scan, ".raw", basebuf, sizeof basebuf))
            return 2;
        base = basebuf;
    }
    if (path_cat(path, sizeof path, scan, ".txt"))
        return 2;
    meta = ftdev_load(path, &nmeta);
    if (!meta) {
        fprintf(stderr, "ftdev: cannot read %s\n", path);
        goto out;
    }
    if (ft_share_meta_parse(meta, (size_t)nmeta, &m, &e)) {
        fprintf(stderr, "ftdev: %s: %s (not a share window scan?)\n", path,
                e.msg);
        goto out;
    }
    if (dpath) {
        dt = ftdev_load(dpath, &nd);
        if (!dt || ft_defects_parse(dt, (size_t)nd, &defects, &e)) {
            fprintf(stderr, "ftdev: %s: %s\n", dpath,
                    dt ? e.msg : "cannot read");
            goto out;
        }
    }
    raw = ftdev_load(scan, &nraw);
    if (path_cat(path, sizeof path, scan, ".cal")) {
        rc = 2;
        goto out;
    }
    cal = ftdev_load(path, &ncal);
    if (!raw || !cal) {
        fprintf(stderr, "ftdev: cannot read %s or its .cal\n", scan);
        goto out;
    }
    if (path_cat(path, sizeof path, scan, ".faul")) {
        rc = 2;
        goto out;
    }
    faul = ftdev_load(path, &nfaul);
    if (!faul)
        fprintf(stderr, "ftdev: note: no %s; Faul Pixs not interpolated\n",
                path);
    if (ft_share_cut(raw, (int)(nraw / ((long)m.px * 6)), cal,
                     (int)(ncal / ((long)m.px * 6)), faul,
                     (size_t)(faul ? nfaul : 0), dpath ? &defects : NULL, &m,
                     &r, &e)) {
        fprintf(stderr, "ftdev: %s: %s\n", scan, e.msg);
        goto out;
    }
    free(raw);
    raw = NULL;
    if (!r.detect_ok)
        fprintf(stderr, "ftdev: WARNING: frame detection failed on %s; "
                        "every frame cut at its preview box\n", scan);
    if (r.noise_rc)
        fprintf(stderr, "ftdev: note: %s\n", r.noise_err.msg);
    rc = 0;
    for (i = 0; i < r.n; i++) {
        ft_share_cut_frame *c = &r.f[i];
        FILE *f;
        int long_px = c->box[2] > c->box[3] ? c->box[2] : c->box[3];
        char num[32];
        snprintf(num, sizeof num, "_frame%02d.dng", c->index);
        if (path_cat(path, sizeof path, base, num)) {
            rc = 2;
            break;
        }
        if (ft_dng_write(path, &c->im, &e)) {
            fprintf(stderr, "ftdev: %s\n", e.msg);
            rc = 1;
            continue;
        }
        printf("%s: %dx%d, frame %dx%d, shift %+.2f mm%s, exports at %d px\n",
               path, c->im.w, c->im.h, c->box[2], c->box[3], c->shift_mm,
               c->estimated ? " (ESTIMATED: not detected)" : "",
               (int)floor(long_px * c->export_scale + 0.5));
        if (path_cat(txt, sizeof txt, path, ".txt")) {
            rc = 2;
            break;
        }
        f = fopen(txt, "w");
        if (!f || ft_share_sidecar_write(f, &m, c) || fclose(f)) {
            fprintf(stderr, "ftdev: cannot write %s\n", txt);
            rc = 1;
        }
    }
    ft_share_cut_free(&r);
out:
    free(meta);
    free(faul);
    free(dt);
    free(raw);
    free(cal);
    return rc;
}

/* copy a file byte for byte; -1 on failure */
static int copy_file(const char *from, const char *to)
{
    long n = 0;
    char *b = ftdev_load(from, &n);
    FILE *f;
    int rc = -1;
    if (!b)
        return -1;
    f = fopen(to, "wb");
    if (f) {
        rc = fwrite(b, 1, (size_t)n, f) == (size_t)n ? 0 : -1;
        if (fclose(f))
            rc = -1;
    }
    free(b);
    return rc;
}

static void export_usage(void)
{
    fprintf(stderr, "usage: ftdev export <frame.dng> [render flags] "
                    "[--space srgb|p3|adobe] [--gamut soft|clip]\n"
                    "         [--crop x,y,w,h] [--long-edge px] "
                    "[--quality q] [--raw-too] [-o out.jpg]\n"
                    "         [--nr-sensor 0..4 (1)] [--nr-chroma 0..8 (2)] "
                    "[--nr-grain 0..4 (0)] [--sharpen 0..3 (0.5)]\n");
}

/* an export strength flag: 1 parsed, 0 not this flag, -1 bad (printed) */
static int strength(const char *a, const char *v, const char *name,
                    double hi, double *out)
{
    char *end;
    double d;
    if (strcmp(a, name) != 0)
        return 0;
    d = strtod(v, &end);
    if (end == v || *end || !(d >= 0 && d <= hi)) {
        fprintf(stderr, "ftdev: %s is 0..%g\n", name, hi);
        return -1;
    }
    *out = d;
    return 1;
}

/* the frame's noise profile for sensor NR: the DNG's NoiseProfile, else
 * the sidecar's, else measured on the pixels (with a warning) */
static void export_profile(const ft_image *im, const ft_share_sidecar *sc,
                           const ft_image *cut, double prof[6],
                           void *scratch)
{
    int i;
    if (im->has_noise) {
        memcpy(prof, im->noise, 6 * sizeof *prof);
        if (sc && sc->has_noise)
            for (i = 0; i < 6; i++)
                if (fabs(sc->noise[i] - prof[i]) > 1e-6 * fabs(prof[i])) {
                    fprintf(stderr, "ftdev: warning: sidecar noise_profile "
                                    "disagrees with the DNG; using the "
                                    "DNG's\n");
                    break;
                }
    } else if (sc && sc->has_noise) {
        memcpy(prof, sc->noise, 6 * sizeof *prof);
    } else {
        ft_nr_estimate(cut->rgb, cut->w, cut->h, prof, scratch);
        fprintf(stderr, "ftdev: warning: no noise profile, estimated\n");
    }
}

int ftdev_export(int argc, char **argv)
{
    const char *dng = NULL, *outp = NULL;
    char outbuf[PATH_CAP], path[PATH_CAP], *side = NULL;
    int i, rc = 1, crop[4] = { 0, 0, 0, 0 }, has_crop = 0, raw_too = 0;
    int quality = FT_JPEG_QUALITY, ow, oh, has_side = 0;
    long nside = 0;
    double long_edge = 0, scale = 1, ref_long;
    ft_space space = FT_SPACE_SRGB;
    ft_gamut gamut = FT_GAMUT_SOFT;
    ft_share_sidecar sc;
    ft_recipe rcp;
    rflags rf;
    ft_image im, cut;
    float *small = NULL, *scratch = NULL;
    void *nrs = NULL;
    double nr_sensor = 1, nr_chroma = 2, nr_grain = 0, sharpen = 0.5;
    unsigned char *px8 = NULL, icc[FT_ICC_MAX];
    size_t icc_n;
    ft_err e;

    ft_image_init(&im);
    ft_image_init(&cut);
    rflags_init(&rf);
    for (i = 0; i < argc; i++) {
        const char *a = argv[i];
        int r = rflags_parse(&rf, argc, argv, &i);
        if (r < 0)
            return 2;
        if (r > 0)
            continue;
        if (i + 1 < argc &&
            ((r = strength(a, argv[i + 1], "--nr-sensor", 4, &nr_sensor)) ||
             (r = strength(a, argv[i + 1], "--nr-chroma", 8, &nr_chroma)) ||
             (r = strength(a, argv[i + 1], "--nr-grain", 4, &nr_grain)) ||
             (r = strength(a, argv[i + 1], "--sharpen", 3, &sharpen)))) {
            if (r < 0)
                return 2;
            i++;
            continue;
        }
        if (strcmp(a, "--space") == 0 && i + 1 < argc) {
            if (ft_space_find(argv[++i], &space) ||
                space == FT_SPACE_REC2020) {
                fprintf(stderr, "ftdev: --space is srgb, p3 or adobe "
                                "(8-bit Rec.2020 bands)\n");
                return 2;
            }
        } else if (strcmp(a, "--gamut") == 0 && i + 1 < argc) {
            if (ft_gamut_find(argv[++i], &gamut)) {
                fprintf(stderr, "ftdev: --gamut is soft or clip\n");
                return 2;
            }
        } else if (strcmp(a, "--crop") == 0 && i + 1 < argc) {
            char extra;
            if (sscanf(argv[++i], "%d,%d,%d,%d%c", &crop[0], &crop[1],
                       &crop[2], &crop[3], &extra) != 4 || crop[0] < 0 ||
                crop[1] < 0 || crop[2] < 1 || crop[3] < 1) {
                fprintf(stderr, "ftdev: --crop is x,y,w,h (DNG pixels)\n");
                return 2;
            }
            has_crop = 1;
        } else if (strcmp(a, "--long-edge") == 0 && i + 1 < argc) {
            char *end;
            long_edge = strtod(argv[++i], &end);
            if (*end || !(long_edge >= 16 && long_edge <= 65000)) {
                fprintf(stderr, "ftdev: --long-edge is px (16..65000)\n");
                return 2;
            }
        } else if (strcmp(a, "--quality") == 0 && i + 1 < argc) {
            char *end;
            long q = strtol(argv[++i], &end, 10);
            if (*end || end == argv[i] || q < 1 || q > 100) {
                fprintf(stderr, "ftdev: --quality is 1..100\n");
                return 2;
            }
            quality = (int)q;
        } else if (strcmp(a, "--raw-too") == 0) {
            raw_too = 1;
        } else if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            outp = argv[++i];
        } else if (a[0] == '-' || dng) {
            export_usage();
            return 2;
        } else {
            dng = a;
        }
    }
    if (!dng) {
        export_usage();
        return 2;
    }
    if (path_cat(path, sizeof path, dng, ".txt"))
        return 2;
    side = ftdev_load(path, &nside);
    if (side) {
        if (ft_share_sidecar_parse(side, (size_t)nside, &sc, &e)) {
            fprintf(stderr, "ftdev: %s: %s\n", path, e.msg);
            goto out;
        }
        has_side = 1;
        if (rf.film < 0)
            rf.film = sc.film;
    }
    if (rf.film < 0) {
        fprintf(stderr, "ftdev: export needs --film neg|slide|bw (no "
                        "sidecar %s)\n", path);
        rc = 2;
        goto out;
    }
    if (rflags_recipe(&rf, &rcp)) {
        rc = 2;
        goto out;
    }
    if (!outp) {
        if (path_swap(outbuf, sizeof outbuf, dng, ".dng", ".jpg")) {
            rc = 2;
            goto out;
        }
        outp = outbuf;
    }
    if (ft_dng_read(dng, &im, &e)) {
        fprintf(stderr, "ftdev: %s\n", e.msg);
        goto out;
    }
    /* the crop: --crop, else the frame box, else everything */
    if (!has_crop) {
        if (has_side)
            memcpy(crop, sc.box, sizeof crop);
        else {
            crop[2] = im.w;
            crop[3] = im.h;
        }
    }
    if (crop[2] > im.w || crop[3] > im.h || crop[0] > im.w - crop[2] ||
        crop[1] > im.h - crop[3]) {
        fprintf(stderr, "ftdev: %s %d,%d %dx%d outside the %dx%d image%s\n",
                has_crop ? "crop" : "sidecar box", crop[0], crop[1], crop[2],
                crop[3], im.w, im.h,
                has_crop ? "" : " (sidecar from another DNG?)");
        rc = 2;
        goto out;
    }
    /* the scale: px on the frame's long side, the same for any crop */
    ref_long = has_side ? (sc.box[2] > sc.box[3] ? sc.box[2] : sc.box[3])
                        : (crop[2] > crop[3] ? crop[2] : crop[3]);
    if (long_edge > 0)
        scale = long_edge / ref_long;
    else if (has_side)
        scale = sc.export_scale;
    if (scale > 1)
        scale = 1;
    if (!has_side && long_edge <= 0)
        fprintf(stderr, "ftdev: note: no sidecar; exporting at full size "
                        "(use --long-edge)\n");
    if (ft_image_alloc(&cut, crop[2], crop[3], &e)) {
        fprintf(stderr, "ftdev: %s\n", e.msg);
        goto out;
    }
    for (i = 0; i < crop[3]; i++)
        memcpy(cut.rgb + (size_t)i * crop[2] * 3,
               im.rgb + ((size_t)(crop[1] + i) * im.w + crop[0]) * 3,
               (size_t)crop[2] * 3 * sizeof *cut.rgb);
    cut.ppi = im.ppi;
    /* noise reduction on the transmittance, before the render */
    if (nr_sensor > 0 || nr_grain > 0) {
        nrs = malloc(ft_nr_scratch_bytes(cut.w, cut.h));
        if (!nrs) {
            fprintf(stderr, "ftdev: out of memory\n");
            goto out;
        }
    }
    if (nr_sensor > 0) {
        double prof[6];
        export_profile(&im, has_side ? &sc : NULL, &cut, prof, nrs);
        ft_nr_sensor(cut.rgb, cut.w, cut.h, prof, nr_sensor,
                     nr_sensor * nr_chroma, nrs);
    }
    if (nr_grain > 0)
        ft_nr_grain(cut.rgb, cut.w, cut.h, nr_grain, nrs);
    free(nrs);
    nrs = NULL;
    ft_image_free(&im);
    {
        int r = rflags_render(&rf, &rcp,
                              !rf.has_base && has_side && sc.has_film_base
                              ? sc.film_base : NULL, &cut, &g_render);
        if (r) {
            rc = r;
            goto out;
        }
    }
    ft_resize_dims(cut.w, cut.h, scale, &ow, &oh);
    small = malloc((size_t)ow * oh * 3 * sizeof *small);
    scratch = malloc(ft_resize_scratch_bytes(cut.w, cut.h, ow, oh));
    px8 = malloc((size_t)ow * oh * 3);
    if (!small || !scratch || !px8) {
        fprintf(stderr, "ftdev: out of memory\n");
        goto out;
    }
    if (ft_resize(cut.rgb, cut.w, cut.h, small, ow, oh, scratch, &e) ||
        ft_output_compile(space, gamut, &g_output, &e)) {
        fprintf(stderr, "ftdev: %s\n", e.msg);
        goto out;
    }
    /* output sharpening, after the resize: its scratch reuses the resize
     * scratch when that is big enough */
    if (sharpen > 0) {
        size_t need = ft_sharpen_scratch_bytes(ow, oh);
        if (need > ft_resize_scratch_bytes(cut.w, cut.h, ow, oh)) {
            float *s2 = realloc(scratch, need);
            if (!s2) {
                fprintf(stderr, "ftdev: out of memory\n");
                goto out;
            }
            scratch = s2;
        }
        ft_sharpen(small, ow, oh, sharpen, (double)ow / cut.w, scratch);
    }
    for (i = 0; i < oh; i++) {
        float *row = small + (size_t)i * ow * 3;
        ft_output_apply(&g_output, row, row, (size_t)ow);
        ft_output_quantise8(row, px8 + (size_t)i * ow * 3, (size_t)ow,
                            (size_t)i * ow);
    }
    icc_n = ft_icc_build(space, icc, sizeof icc);
    if (icc_n == 0) {
        fprintf(stderr, "ftdev: cannot build the ICC profile\n");
        goto out;
    }
    if (ft_jpeg_write(outp, px8, ow, oh, quality,
                      cut.ppi > 0 ? cut.ppi * ow / cut.w : 0, icc, icc_n,
                      &e)) {
        fprintf(stderr, "ftdev: %s\n", e.msg);
        goto out;
    }
    printf("%s: %dx%d, %s, gamut %s, q%d\n", outp, ow, oh,
           ft_space_what(space), gamut == FT_GAMUT_SOFT ? "soft" : "clip",
           quality);
    rc = 0;
    if (raw_too) {
        char to[PATH_CAP], to_txt[PATH_CAP];
        if (path_swap(to, sizeof to, outp, ".jpg", ".dng")) {
            rc = 2;
            goto out;
        }
        if (strcmp(to, dng) == 0) {
            printf("%s: the working raw is already there\n", to);
        } else if (copy_file(dng, to)) {
            fprintf(stderr, "ftdev: cannot copy %s to %s\n", dng, to);
            rc = 1;
        } else {
            printf("%s: the working raw\n", to);
            if (has_side) {
                if (path_cat(to_txt, sizeof to_txt, to, ".txt")) {
                    rc = 2;
                } else if (copy_file(path, to_txt)) {
                    fprintf(stderr, "ftdev: cannot copy %s\n", path);
                    rc = 1;
                }
            }
        }
    }
out:
    ft_image_free(&im);
    ft_image_free(&cut);
    free(small);
    free(scratch);
    free(nrs);
    free(px8);
    free(side);
    return rc;
}
