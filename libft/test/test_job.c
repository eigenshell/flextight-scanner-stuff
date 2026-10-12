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
    b.f[0].ppi = 3500;
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
    CHECK(strstr(e.msg, "frame 1") != NULL);
    b = a;
    b.f[0].y0 = 200;                          /* runs past 231 mm */
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
    b = a;
    b.f[1].ppi = 3500;                        /* excluded frames are skipped */
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == 0);
    b = a;
    strcpy(b.holder, "60x180");
    CHECK(ft_job_validate(&b, &ft_lens_default, &e) == -1);
    b = a;
    b.focus = FT_FOCUS_SHIFTED;
    b.focus_offset = 900000;                  /* CCD would pass the lens */
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
