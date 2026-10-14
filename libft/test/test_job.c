/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ft_focus.h"
#include "ft_job.h"
#include "test/ftest.h"

static FILE *text(const char *s)
{
    FILE *f = tmpfile();
    fputs(s, f);
    rewind(f);
    return f;
}

int main(void)
{
    ft_job a, b;
    ft_err e;
    FILE *f;

    memset(&a, 0, sizeof a);
    strcpy(a.holder, "24x217");
    a.film = FT_FILM_SLIDE;
    a.focus = FT_FOCUS_PER_FRAME;
    a.focus_offset = 2400;
    a.nframes = 2;
    a.f[0].index = 1; a.f[0].type = FT_FRAME_FULL; a.f[0].include = 1;
    a.f[0].ppi = 3150; a.f[0].y0 = 4.1; a.f[0].x0 = 1.6;
    a.f[0].len = 36.2; a.f[0].width = 24.4; a.f[0].tilt_deg = 0.12;
    a.f[1].index = 2; a.f[1].type = FT_FRAME_PARTIAL; a.f[1].include = 0;
    a.f[1].ppi = 2000; a.f[1].y0 = 42; a.f[1].x0 = 4.1;
    a.f[1].len = 20; a.f[1].width = 24; a.f[1].sprockets = 1;
    a.f[1].straighten = 1; a.f[1].confidence_low = 1;
    a.f[1].focus_offset = -800; a.f[1].done = 1;

    /* round trip */
    f = tmpfile();
    CHECK(ft_job_write(&a, f) == 0);
    rewind(f);
    CHECK(ft_job_read(&b, f, &e) == 0);
    fclose(f);
    CHECK(strcmp(b.holder, "24x217") == 0);
    CHECK(b.film == FT_FILM_SLIDE && b.focus == FT_FOCUS_PER_FRAME);
    CHECK(b.focus_offset == 2400 && b.nframes == 2);
    CHECK(b.f[0].index == 1 && b.f[0].type == FT_FRAME_FULL && b.f[0].include);
    CHECK_NEAR(b.f[0].ppi, 3150, 1e-9);
    CHECK_NEAR(b.f[0].len, 36.2, 0.006);
    CHECK_NEAR(b.f[0].tilt_deg, 0.12, 0.006);
    CHECK(b.f[1].type == FT_FRAME_PARTIAL && !b.f[1].include);
    CHECK(b.f[1].sprockets && b.f[1].straighten && b.f[1].confidence_low);
    CHECK(b.f[1].focus_offset == -800 && b.f[1].done);
    CHECK(b.f[0].line > 0 && b.f[1].line > b.f[0].line);
    CHECK(b.f[0].bin == 0 && ft_job_bin(&b.f[0]) == 2);   /* fastest */

    /* bin: per-frame optics; written, read back, options listed */
    {
        char buf[4096];
        size_t n;
        ft_job c = a;
        c.f[1].bin = 1;
        f = tmpfile();
        CHECK(ft_job_write(&c, f) == 0);
        rewind(f);
        n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        CHECK(strstr(buf, "bin = 0") != NULL && strstr(buf, "bin = 1") != NULL);
        /* 3150: bins 2 and 1; 2000: 3, 2 and 1 */
        CHECK(strstr(buf, "# bin options at 3150 ppi") != NULL);
        CHECK(strstr(buf, "# bin options at 2000 ppi (time, light, Dmax~): "
                          "3: ") != NULL);
        rewind(f);
        CHECK(ft_job_read(&b, f, &e) == 0);
        fclose(f);
        CHECK(b.f[0].bin == 0 && b.f[1].bin == 1 && ft_job_bin(&b.f[1]) == 1);
    }
    /* detailed scans: strip keys and the frame's pass, written only for a
     * detail job (older jobs read and write as before) */
    {
        ft_job c = a, d2;
        int k;
        f = tmpfile();
        CHECK(ft_job_write(&c, f) == 0);
        rewind(f);
        {
            char buf[4096];
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            CHECK(strstr(buf, "detail") == NULL && strstr(buf, "pass") == NULL);
        }
        fclose(f);
        c.film = FT_FILM_NEGATIVE;
        c.detail = FT_DETAIL_POS;
        c.detail_fmt = FT_DFMT_TIFF;
        c.detail_bw = FT_BW_MONO;
        c.keep_raw = 1;
        c.ring_mb = 256;
        c.f[0].pass = 1;
        c.f[1].pass = 2;
        f = tmpfile();
        CHECK(ft_job_write(&c, f) == 0);
        rewind(f);
        CHECK(ft_job_read(&d2, f, &e) == 0);
        fclose(f);
        CHECK(d2.detail == FT_DETAIL_POS && d2.detail_fmt == FT_DFMT_TIFF);
        CHECK(d2.detail_bw == FT_BW_MONO && d2.keep_raw && d2.ring_mb == 256);
        CHECK(d2.f[0].pass == 1 && d2.f[1].pass == 2);

        f = text("[strip]\nholder = 24x217\n");
        CHECK(ft_job_read(&d2, f, &e) == 0);
        fclose(f);
        CHECK(d2.detail == FT_DETAIL_NONE && d2.ring_mb == FT_RING_MB_DEFAULT);
        {
            static const char *bad[] = {
                "detail = both\n", "detail = none\n", "detail_fmt = jpg\n",
                "detail_bw = grey\n", "keep_raw = 1\n", "ring_mb = 4\n",
                "ring_mb = 5000\n", NULL };
            for (k = 0; bad[k]; k++) {
                char t2[128];
                snprintf(t2, sizeof t2, "[strip]\nholder = 24x217\n%s", bad[k]);
                f = text(t2);
                CHECK(ft_job_read(&d2, f, &e) == -1);
                fclose(f);
            }
        }
        f = text("[strip]\nholder = 24x217\n[frame 1]\nppi = 3150\n"
                 "window = 4 4 36 24\npass = -1\n");
        CHECK(ft_job_read(&d2, f, &e) == -1);
        fclose(f);

        /* validation */
        c.has_film_base = 1;
        c.film_base[0] = 0.3; c.film_base[1] = 0.6; c.film_base[2] = 0.9;
        c.f[0].bin = 1;
        c.f[1].include = 0;
        c.focus = FT_FOCUS_SHIFTED;
        c.focus_offset = 0;
        CHECK(ft_job_validate(&c, &ft_lens_default, &e) == 0);
        d2 = c;
        d2.film = FT_FILM_SLIDE;
        CHECK(ft_job_validate(&d2, &ft_lens_default, &e) == -1);
        CHECK(strstr(e.msg, "negative") != NULL);
        d2 = c;
        d2.detail_fmt = FT_DFMT_DNG;
        CHECK(ft_job_validate(&d2, &ft_lens_default, &e) == -1);
        d2 = c;
        d2.detail = FT_DETAIL_NEG;
        CHECK(ft_job_validate(&d2, &ft_lens_default, &e) == -1);   /* bw */
        d2.detail_bw = FT_BW_OFF;
        d2.film = FT_FILM_SLIDE;
        CHECK(ft_job_validate(&d2, &ft_lens_default, &e) == 0);
        d2 = c;
        d2.has_film_base = 0;
        CHECK(ft_job_validate(&d2, &ft_lens_default, &e) == -1);
        d2 = c;
        d2.f[0].pass = 0;
        CHECK(ft_job_validate(&d2, &ft_lens_default, &e) == -1);
        d2 = c;
        d2.f[0].bin = 2;                    /* 3150 x 2 = 6300 optics */
        CHECK(ft_job_validate(&d2, &ft_lens_default, &e) == -1);
        CHECK(strstr(e.msg, "bin 1") != NULL);
        d2 = c;
        d2.f[1].include = 1;
        d2.f[1].pass = 1;
        d2.f[1].ppi = 6300;
        d2.f[1].bin = 1;
        d2.f[1].type = FT_FRAME_FULL;
        CHECK(ft_job_validate(&d2, &ft_lens_default, &e) == -1);
        CHECK(strstr(e.msg, "mixes") != NULL);
    }

    /* film_base: written with the strip, read back; absent = not set */
    {
        ft_job c = a;
        c.has_film_base = 1;
        c.film_base[0] = 0.2512; c.film_base[1] = 0.6;
        c.film_base[2] = 0.9177;
        f = tmpfile();
        CHECK(ft_job_write(&c, f) == 0);
        rewind(f);
        CHECK(ft_job_read(&b, f, &e) == 0);
        fclose(f);
        CHECK(b.has_film_base);
        CHECK_NEAR(b.film_base[0], 0.2512, 1e-9);
        CHECK_NEAR(b.film_base[2], 0.9177, 1e-9);
        f = tmpfile();
        CHECK(ft_job_write(&a, f) == 0);           /* a has none */
        rewind(f);
        CHECK(ft_job_read(&b, f, &e) == 0);
        fclose(f);
        CHECK(!b.has_film_base);
        f = text("[strip]\nholder = 24x217\nfilm_base = 0.2 0.5\n");
        CHECK(ft_job_read(&b, f, &e) == -1);       /* two values */
        fclose(f);
        f = text("[strip]\nholder = 24x217\nfilm_base = 0.2 0.5 5\n");
        CHECK(ft_job_read(&b, f, &e) == -1);       /* above 4.5 D */
        fclose(f);
        f = text("[strip]\nholder = 24x217\nfilm_base = 0.2 0.5 0.9 1\n");
        CHECK(ft_job_read(&b, f, &e) == -1);       /* four values */
        fclose(f);
        f = text("[strip]\nholder = 24x217\nfilm_base = 0.2 0.5 0.9   # comment\n");
        CHECK(ft_job_read(&b, f, &e) == 0 && b.has_film_base);
        fclose(f);
        f = text("[strip]\nholder = 24x217\nfilm_base = -0.1 0.5 0.9\n");
        CHECK(ft_job_read(&b, f, &e) == -1);       /* negative D */
        fclose(f);
    }
    /* share: strip keys and per-frame groups round trip; groups are
     * written only in a share job */
    {
        ft_job c;
        char buf[4096];
        size_t n;
        memset(&c, 0, sizeof c);
        strcpy(c.holder, "24x217");
        c.share = 2000;
        c.hf_half_res = 1;
        c.nframes = 3;
        for (n = 0; n < 3; n++) {
            c.f[n].index = (int)n + 1;
            c.f[n].type = FT_FRAME_HALF;
            c.f[n].include = n != 1;
            c.f[n].ppi = 1500;
            c.f[n].bin = 1;
            c.f[n].y0 = 3 + 19.0 * n;
            c.f[n].x0 = 4;
            c.f[n].len = 18;
            c.f[n].width = 24;
            c.f[n].group = n == 0 ? 1 : n == 2 ? 2 : 0;
        }
        f = tmpfile();
        CHECK(ft_job_write(&c, f) == 0);
        rewind(f);
        n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        CHECK(strstr(buf, "share = 2000") && strstr(buf, "hf_half_res = yes"));
        CHECK(strstr(buf, "group = 2") != NULL);
        rewind(f);
        CHECK(ft_job_read(&b, f, &e) == 0);
        fclose(f);
        CHECK(b.share == 2000 && b.hf_half_res == 1);
        CHECK(b.f[0].group == 1 && b.f[1].group == 0 && b.f[2].group == 2);
        CHECK(ft_job_validate(&b, &ft_lens_default, &e) == 0);
        /* an ordinary job writes no share keys and no groups */
        f = tmpfile();
        CHECK(ft_job_write(&a, f) == 0);
        rewind(f);
        n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
        CHECK(!strstr(buf, "share") && !strstr(buf, "group"));
        /* share rules */
        b.f[2].group = 0;                     /* included, no group */
        CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
        CHECK(strstr(e.msg, "without a group") != NULL);
        b.f[2].group = 2;
        b.f[2].bin = 2;                       /* own group: fine */
        CHECK(ft_job_validate(&b, &ft_lens_default, &e) == 0);
        b.f[2].group = 1;                     /* same group, other bin */
        CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
        CHECK(strstr(e.msg, "differ") != NULL);
        b.f[2].bin = 1;
        CHECK(ft_job_validate(&b, &ft_lens_default, &e) == 0);
        b.f[1].include = 1;                   /* 1, 2, 1: split */
        b.f[1].group = 2;
        CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
        CHECK(strstr(e.msg, "split") != NULL);
        b.f[1].include = 0;
        b.f[0].y0 = 50;                       /* out of strip order */
        CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
        CHECK(strstr(e.msg, "follow the strip") != NULL);
        b.f[0].y0 = 3;
        b.share = 0;                          /* groups without share */
        CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
        CHECK(strstr(e.msg, "group without share") != NULL);
        /* reader limits */
        f = text("[strip]\nholder = 24x217\nshare = 50\n");
        CHECK(ft_job_read(&b, f, &e) == -1);
        fclose(f);
        f = text("[strip]\nholder = 24x217\nhf_half_res = maybe\n");
        CHECK(ft_job_read(&b, f, &e) == -1);
        fclose(f);
        f = text("[strip]\nholder = 24x217\n[frame 1]\nppi = 1500\n"
                 "window = 50 4 36 24\ngroup = -1\n");
        CHECK(ft_job_read(&b, f, &e) == -1);
        fclose(f);
    }
    /* a job without bin keys (older files) scans with the fastest bin */
    f = text("[strip]\nholder = 24x217\n[frame 1]\nppi = 2000\n"
             "window = 50 4 36 24\n");
    CHECK(ft_job_read(&b, f, &e) == 0);
    fclose(f);
    CHECK(b.f[0].bin == 0 && ft_job_bin(&b.f[0]) == 3);
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == 0);
    b.f[0].bin = 1;                           /* optics 2000: usable */
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == 0);
    b.f[0].bin = 6;                           /* optics 12000: no */
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.msg, "bin 6") != NULL);
    f = text("[strip]\nholder = 24x217\n[frame 1]\nppi = 2000\nbin = 13\n"
             "window = 50 4 36 24\n");
    CHECK(ft_job_read(&b, f, &e) == -1);
    fclose(f);

    /* parse errors carry the line number */
    f = text("[strip]\nholder = 24x217\nbogus = 1\n");
    CHECK(ft_job_read(&b, f, &e) == -1);
    CHECK(strstr(e.msg, "line 3") != NULL);
    fclose(f);
    f = text("[strip]\n[frame 1]\nppi = fast\n");
    CHECK(ft_job_read(&b, f, &e) == -1);
    CHECK(strstr(e.msg, "line 3") != NULL);
    fclose(f);
    f = text("ppi = 2000\n");
    CHECK(ft_job_read(&b, f, &e) == -1);
    fclose(f);

    /* validation: holder, ppi, window, optics */
    CHECK(ft_job_validate(&a, &ft_lens_default, &e) == 0);
    b = a;
    b.f[0].ppi = 6400;
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.msg, "frame 1") != NULL);
    b = a;
    b.f[0].y0 = 200;                          /* runs past 231 mm */
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
    b = a;
    b.f[1].ppi = 6400;                        /* excluded frames are skipped */
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == 0);
    b = a;
    strcpy(b.holder, "60x180");
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
    b = a;
    b.focus = FT_FOCUS_SHIFTED;
    b.focus_offset = 900000;                  /* CCD/lens past their range */
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
    /* hardening */
    f = text("[strip]\r\nholder = 24x217\r\n[frame 1]\r\nppi = 2000\r\n"
             "window = 1 2 3 4\r\n");
    CHECK(ft_job_read(&b, f, &e) == 0 && b.nframes == 1);
    fclose(f);
    {
        char big[400] = "[strip]\nholder = 24x217\n# ";
        memset(big + 26, 'x', 300);
        strcpy(big + 326, "\n");
        f = text(big);
        CHECK(ft_job_read(&b, f, &e) == -1);
        CHECK(strstr(e.msg, "line 3") && strstr(e.msg, "too long"));
        fclose(f);
    }
#define GOOD "[strip]\nholder = 24x217\n"
#define FR "[frame 1]\nppi = 2000\nwindow = 1 2 3 4\n"
    f = text(GOOD FR "[frame 1]\nppi = 2000\nwindow = 1 2 3 4\n");
    CHECK(ft_job_read(&b, f, &e) == -1 && strstr(e.msg, "repeated"));
    fclose(f);
    f = text(GOOD "[frame 1 junk]\nppi = 2000\nwindow = 1 2 3 4\n");
    CHECK(ft_job_read(&b, f, &e) == -1);
    fclose(f);
    f = text(GOOD "[frame 0]\nppi = 2000\nwindow = 1 2 3 4\n");
    CHECK(ft_job_read(&b, f, &e) == -1);
    fclose(f);
    f = text(GOOD "[frame 1]\nppi = 2000\nwindow = nan 2 3 4\n");
    CHECK(ft_job_read(&b, f, &e) == -1 && strstr(e.msg, "line 5"));
    fclose(f);
    f = text(GOOD "[frame 1]\nppi = inf\nwindow = 1 2 3 4\n");
    CHECK(ft_job_read(&b, f, &e) == -1);
    fclose(f);
    f = text(GOOD "[frame 1]\nppi = 2000\nwindow = 1 2 3 4 5\n");
    CHECK(ft_job_read(&b, f, &e) == -1);
    fclose(f);
    f = text(GOOD "[frame 1]\nppi = 2000\n");
    CHECK(ft_job_read(&b, f, &e) == -1 && strstr(e.msg, "no window"));
    fclose(f);
    f = text(GOOD "[frame 1]\nwindow = 1 2 3 4\n");
    CHECK(ft_job_read(&b, f, &e) == -1 && strstr(e.msg, "no ppi"));
    fclose(f);
    f = text("[strip]\n" FR);
    CHECK(ft_job_read(&b, f, &e) == -1 && strstr(e.msg, "no holder"));
    fclose(f);
    f = text(FR);
    CHECK(ft_job_read(&b, f, &e) == -1);
    fclose(f);
    f = text("");
    CHECK(ft_job_read(&b, f, &e) == -1 && strstr(e.msg, "[strip]"));
    fclose(f);
    f = text(GOOD "[strip]\n");
    CHECK(ft_job_read(&b, f, &e) == -1 && strstr(e.msg, "line 3"));
    fclose(f);
    f = text(GOOD "film = slide\nfilm = slide\n");
    CHECK(ft_job_read(&b, f, &e) == -1 && strstr(e.msg, "duplicate key"));
    fclose(f);
    f = text(GOOD "focus_offset = 99999999999999999999\n");
    CHECK(ft_job_read(&b, f, &e) == -1);
    fclose(f);
    f = text(GOOD "film = slide  # comment\n" FR);
    CHECK(ft_job_read(&b, f, &e) == 0 && b.film == FT_FILM_SLIDE);
    fclose(f);

    /* validation hardening */
    b = a;
    b.focus = FT_FOCUS_PER_FRAME;
    b.f[0].focus_offset = 900000;
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
    b = a;
    b.f[0].focus_offset = -2000000000L;
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
    b = a;
    b.f[0].tilt_deg = 60;
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
    b = a;
    b.f[0].len = NAN;
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
    b = a;
    b.nframes = 0;
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1 &&
          strstr(e.msg, "no frames"));
    return TEST_DONE();
}
