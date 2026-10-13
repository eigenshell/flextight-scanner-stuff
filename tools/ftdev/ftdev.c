/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ftdev: develop an ftpoc scan into image files, on any OS (no scanner).
 *
 *   ftdev <scan.raw> [-f dng,png,positive] [-o base] [-d defects.txt]
 *         [--no-crop] [--film neg|slide|bw] [--base r,g,b] [--exposure s]
 *         [--tint r,g,b] [--grade g] [--curve preset|"x,y x,y ..."]
 *
 * Reads <scan.raw> with its .cal, .faul and .txt (ftpoc's output), runs
 * the libft pipeline (ft_develop.h) and writes one file per format to
 * <base><ext>; base defaults to the scan path without ".raw". A scan from
 * `scan --job` carries its frame's crop (crop_* in the .txt), which is
 * applied unless --no-crop. -d adds defective CCD pixels the scanner's
 * Faul Pixs table misses (format in ft_develop.h).
 *
 * -f positive renders the frame (ft_render.h; needs --film): the film base
 * from --base, else the .txt's film_base (from the frames preview), else
 * an estimate from the frame itself; range and exposure automatic; then
 * the overrides. The recipe used is printed as flags to paste back.
 *
 * Build: make (Linux/macOS), or with mingw:
 *   gcc -std=c99 -O2 -I..\..\libft -o ftdev.exe ftdev.c <every libft .c> -lm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ft_develop.h"
#include "ft_image.h"
#include "ft_out.h"
#include "ft_render.h"
#include "ft_render_auto.h"

#define MAX_FORMATS 8

static ft_render g_render;              /* ~380 KB of LUTs: not on the stack */

/* "a,b,c" -> v; -1 if malformed or not finite */
static int parse3(const char *s, float v[3])
{
    char extra;
    if (sscanf(s, "%f,%f,%f%c", &v[0], &v[1], &v[2], &extra) != 3)
        return -1;
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]) ? 0 : -1;
}

/* a finite number and nothing else -> v; -1 otherwise */
static int parse1(const char *s, float *v)
{
    char *end;
    double d = strtod(s, &end);
    if (end == s || *end || !isfinite(d))
        return -1;
    *v = (float)d;
    return 0;
}

/* the shortest %g text of v that reads back as the same float */
static const char *fmtf(float v, char *buf, size_t cap)
{
    int p;
    for (p = 6; p < 9; p++) {
        snprintf(buf, cap, "%.*g", p, (double)v);
        if (strtof(buf, NULL) == v)
            return buf;
    }
    snprintf(buf, cap, "%.9g", (double)v);
    return buf;
}

static void *load_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    long n;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 &&
        fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)n + 1)) != NULL) {
        if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        } else {
            buf[n] = 0;
            *size = n;
        }
    }
    fclose(f);
    return buf;
}

/* value of key in a key=value text, or def */
static long meta_long(const char *meta, const char *key, long def)
{
    size_t k = strlen(key);
    const char *p = meta;
    while (p && *p) {
        if (strncmp(p, key, k) == 0 && p[k] == '=')
            return strtol(p + k + 1, NULL, 10);
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return def;
}

static int meta_str(const char *meta, const char *key, char *out, size_t cap)
{
    size_t k = strlen(key), n = 0;
    const char *p = meta;
    while (p && *p) {
        if (strncmp(p, key, k) == 0 && p[k] == '=') {
            p += k + 1;
            while (p[n] && p[n] != '\n' && p[n] != '\r' && n + 1 < cap) {
                out[n] = p[n];
                n++;
            }
            out[n] = 0;
            return 1;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return 0;
}

static void usage(void)
{
    const ft_writer *w;
    int i;
    fprintf(stderr, "usage: ftdev <scan.raw> [-f fmt[,fmt..]] [-o base] "
                    "[-d defects.txt] [--no-crop]\n"
                    "             [--film neg|slide|bw] [--base r,g,b] "
                    "[--exposure stops] [--tint r,g,b]\n"
                    "             [--grade g] [--curve preset|\"x,y x,y ...\"]\n"
                    "formats (default dng):\n");
    for (w = ft_writers(); w->name; w++)
        fprintf(stderr, "  %-8s %s\n", w->name, w->what);
    fprintf(stderr, "curve presets:");
    for (i = 0; ft_curve_preset_name(i); i++)
        fprintf(stderr, " %s", ft_curve_preset_name(i));
    fprintf(stderr, "\n");
}

int main(int argc, char **argv)
{
    const char *scan = NULL, *base = NULL, *fmts = "dng", *dpath = NULL;
    const ft_writer *wr[MAX_FORMATS];
    int nwr = 0, no_crop = 0, film = -1, i, rc = 1;
    /* render: -f positive, with the recipe overrides below */
    const char *curve_arg = NULL;
    int bw = 0, render = 0, has_base = 0, has_exp = 0, has_tint = 0;
    int has_grade = 0;
    float ubase[3], utint[3], uexp = 0, ugrade = 0;
    ft_recipe rcp;
    char path[1024], *meta = NULL, *faul = NULL, *dtext = NULL;
    char basebuf[1024], preset[32] = "";
    unsigned short *raw = NULL, *cal = NULL;
    long nmeta = 0, nraw = 0, ncal = 0, nfaul = 0, nd = 0, px, lines, cl;
    ft_defects defects;
    ft_develop_opts o;
    ft_image im;
    ft_err e;

    ft_image_init(&im);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-f") == 0 && i + 1 < argc) fmts = argv[++i];
        else if (strcmp(a, "-o") == 0 && i + 1 < argc) base = argv[++i];
        else if (strcmp(a, "-d") == 0 && i + 1 < argc) dpath = argv[++i];
        else if (strcmp(a, "--no-crop") == 0) no_crop = 1;
        else if (strcmp(a, "--film") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "neg") == 0 || strcmp(v, "negative") == 0)
                film = FT_FILM_NEGATIVE;
            else if (strcmp(v, "slide") == 0)
                film = FT_FILM_SLIDE;
            else if (strcmp(v, "bw") == 0) {
                film = FT_FILM_NEGATIVE;
                bw = 1;
            } else {
                fprintf(stderr, "ftdev: --film is neg, slide or bw\n");
                return 2;
            }
        } else if (strcmp(a, "--base") == 0 && i + 1 < argc) {
            if (parse3(argv[++i], ubase)) {
                fprintf(stderr, "ftdev: --base is r,g,b (densities)\n");
                return 2;
            }
            has_base = 1;
        } else if (strcmp(a, "--tint") == 0 && i + 1 < argc) {
            if (parse3(argv[++i], utint)) {
                fprintf(stderr, "ftdev: --tint is r,g,b (CC)\n");
                return 2;
            }
            has_tint = 1;
        } else if (strcmp(a, "--exposure") == 0 && i + 1 < argc) {
            if (parse1(argv[++i], &uexp)) {
                fprintf(stderr, "ftdev: --exposure is stops\n");
                return 2;
            }
            has_exp = 1;
        } else if (strcmp(a, "--grade") == 0 && i + 1 < argc) {
            if (parse1(argv[++i], &ugrade)) {
                fprintf(stderr, "ftdev: --grade is a number\n");
                return 2;
            }
            has_grade = 1;
        } else if (strcmp(a, "--curve") == 0 && i + 1 < argc) {
            curve_arg = argv[++i];
        } else if (a[0] == '-' || scan) {
            usage();
            return 2;
        } else
            scan = a;
    }
    if (!scan) {
        usage();
        return 2;
    }
    {
        char list[256], *tok;
        snprintf(list, sizeof list, "%s", fmts);
        for (tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
            const ft_writer *w = ft_writer_find(tok);
            if (!w) {
                fprintf(stderr, "ftdev: unknown format '%s'\n", tok);
                usage();
                return 2;
            }
            if (nwr == MAX_FORMATS)
                break;
            wr[nwr++] = w;
            render |= w->write == ft_png_write_positive;
        }
    }
    if (render) {
        if (film < 0) {
            fprintf(stderr, "ftdev: -f positive needs --film neg|slide|bw\n");
            return 2;
        }
        ft_recipe_default(&rcp, bw ? FT_RENDER_BW
                                : film == FT_FILM_SLIDE ? FT_RENDER_SLIDE
                                                        : FT_RENDER_NEG);
        if (curve_arg && ft_curve_preset(curve_arg, &rcp.master) != 0 &&
            ft_curve_parse(curve_arg, &rcp.master, &e) != 0) {
            const char *m = e.msg;
            int k;
            fprintf(stderr, "ftdev: --curve: not a preset (");
            for (k = 0; ft_curve_preset_name(k); k++)
                fprintf(stderr, "%s%s", k ? " " : "", ft_curve_preset_name(k));
            fprintf(stderr, ") and not points: %s\n",
                    strncmp(m, "curve: ", 7) == 0 ? m + 7 : m);
            return 2;
        }
        if (has_grade)
            rcp.grade = ugrade;     /* before auto: exposure depends on it */
        if (has_exp)
            rcp.exposure = uexp;
        if (has_tint)
            memcpy(rcp.tint, utint, sizeof rcp.tint);
        if (has_base)                   /* auto never changes the base */
            ft_recipe_set_base(&rcp, ubase);
        if (ft_recipe_validate(&rcp, &e)) {     /* before any file is written */
            fprintf(stderr, "ftdev: %s\n", e.msg);
            return 2;
        }
    } else if (has_base || has_exp || has_tint || has_grade || curve_arg)
        fprintf(stderr, "ftdev: note: render flags ignored without "
                        "-f positive\n");
    if (!base) {
        size_t n = strlen(scan);
        snprintf(basebuf, sizeof basebuf, "%s", scan);
        if (n > 4 && strcmp(basebuf + n - 4, ".raw") == 0)
            basebuf[n - 4] = 0;
        base = basebuf;
    }

    snprintf(path, sizeof path, "%s.txt", scan);
    meta = load_file(path, &nmeta);
    if (!meta) {
        fprintf(stderr, "ftdev: cannot read %s\n", path);
        goto out;
    }
    memset(&o, 0, sizeof o);
    px = meta_long(meta, "px", 0);
    o.x = (int)meta_long(meta, "x", -1);
    o.bin = (int)meta_long(meta, "bin", 0);
    meta_str(meta, "preset", preset, sizeof preset);
    if (o.bin < 1)              /* .txt from before ftpoc recorded bin */
        o.bin = strcmp(preset, "preview") == 0 ? 12
              : strcmp(preset, "2000") == 0 ? 3 : 6;
    if (px < 1 || px > 20000 || o.x < 0) {
        fprintf(stderr, "ftdev: %s lacks px/x\n", path);
        goto out;
    }
    if (!no_crop && meta_long(meta, "crop_w", 0) > 0) {
        o.crop_x = (int)meta_long(meta, "crop_x", 0);
        o.crop_y = (int)meta_long(meta, "crop_y", 0);
        o.crop_w = (int)meta_long(meta, "crop_w", 0);
        o.crop_h = (int)meta_long(meta, "crop_h", 0);
    }
    if (dpath) {
        dtext = load_file(dpath, &nd);
        if (!dtext) {
            fprintf(stderr, "ftdev: cannot read %s\n", dpath);
            goto out;
        }
        if (ft_defects_parse(dtext, (size_t)nd, &defects, &e)) {
            fprintf(stderr, "ftdev: %s: %s\n", dpath, e.msg);
            goto out;
        }
        o.defects = &defects;
    }

    raw = load_file(scan, &nraw);
    snprintf(path, sizeof path, "%s.cal", scan);
    cal = load_file(path, &ncal);
    if (!raw || !cal) {
        fprintf(stderr, "ftdev: cannot read %s or its .cal\n", scan);
        goto out;
    }
    lines = nraw / (px * 6);
    cl = ncal / (px * 6);
    snprintf(path, sizeof path, "%s.faul", scan);
    faul = load_file(path, &nfaul);
    if (!faul)
        fprintf(stderr, "ftdev: note: no %s; Faul Pixs not interpolated\n",
                path);

    im.ppi = (double)meta_long(meta, "ppi", 0);
    im.film = film;
    snprintf(im.desc, sizeof im.desc,
             "%s; preset %s, %ld ppi, bin %d, x %d, %ld px x %ld lines; "
             "crop %d,%d %dx%d; film %s",
             scan, preset[0] ? preset : "?", meta_long(meta, "ppi", 0), o.bin,
             o.x, px, lines, o.crop_x, o.crop_y, o.crop_w, o.crop_h,
             film == FT_FILM_NEGATIVE ? "negative"
             : film == FT_FILM_SLIDE ? "slide" : "unknown");
    if (ft_develop(raw, (int)lines, (int)px, cal, (int)cl, faul,
                   (size_t)(faul ? nfaul : 0), &o, &im, &e)) {
        fprintf(stderr, "ftdev: %s: %s\n", scan, e.msg);
        goto out;
    }
    free(raw);
    raw = NULL;
    rc = 0;
    /* the developed image first (DNG, PNG preview), then the render */
    for (i = 0; i < nwr; i++) {
        if (wr[i]->write == ft_png_write_positive)
            continue;
        snprintf(path, sizeof path, "%s%s", base, wr[i]->ext);
        if (wr[i]->write(path, &im, &e)) {
            fprintf(stderr, "ftdev: %s\n", e.msg);
            rc = 1;
        } else
            printf("%s: %dx%d, %.0f ppi\n", path, im.w, im.h, im.ppi);
    }
    if (render) {
        const ft_image *one[1];
        char fb[128];
        float d[3];
        one[0] = &im;
        if (has_base) {
            memcpy(d, ubase, sizeof d);
        } else if (meta_str(meta, "film_base", fb, sizeof fb) &&
                   sscanf(fb, "%f %f %f", &d[0], &d[1], &d[2]) == 3 &&
                   isfinite(d[0]) && isfinite(d[1]) && isfinite(d[2])) {
            /* from the frames preview, via the job */
        } else {
            if (ft_render_base_from_frames(one, 1, rcp.mode, d, &e)) {
                fprintf(stderr, "ftdev: %s\n", e.msg);
                rc = 1;
                goto out;
            }
            fprintf(stderr, "ftdev: note: no usable film_base in the .txt; "
                            "film base estimated from the frame itself\n");
        }
        ft_recipe_set_base(&rcp, d);
        if (ft_render_auto(one, 1, &rcp, &e)) {
            fprintf(stderr, "ftdev: %s\n", e.msg);
            rc = 1;
            goto out;
        }
        if (has_exp)
            rcp.exposure = uexp;
        if (has_tint)
            memcpy(rcp.tint, utint, sizeof rcp.tint);
        if (ft_render_compile(&rcp, &g_render, &e)) {
            fprintf(stderr, "ftdev: %s\n", e.msg);
            rc = 2;
            goto out;
        }
        ft_render_image(&g_render, &im);
        {
            char f[10][32];
            printf("recipe: --film %s --base %s,%s,%s --exposure %s "
                   "--tint %s,%s,%s --grade %s --curve \"%s\"\n",
                   bw ? "bw" : film == FT_FILM_SLIDE ? "slide" : "neg",
                   fmtf(rcp.base[0], f[0], 32), fmtf(rcp.base[1], f[1], 32),
                   fmtf(rcp.base[2], f[2], 32), fmtf(rcp.exposure, f[3], 32),
                   fmtf(rcp.tint[0], f[4], 32), fmtf(rcp.tint[1], f[5], 32),
                   fmtf(rcp.tint[2], f[6], 32), fmtf(rcp.grade, f[7], 32),
                   curve_arg ? curve_arg : "linear");
        }
        snprintf(path, sizeof path, "%s%s", base,
                 ft_writer_find("positive")->ext);
        if (ft_png_write_positive(path, &im, &e)) {
            fprintf(stderr, "ftdev: %s\n", e.msg);
            rc = 1;
        } else
            printf("%s: %dx%d, %.0f ppi\n", path, im.w, im.h, im.ppi);
    }
out:
    ft_image_free(&im);
    free(meta);
    free(faul);
    free(dtext);
    free(raw);
    free(cal);
    return rc;
}
