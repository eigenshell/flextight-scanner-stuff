/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <string.h>
#include "ft_detect.h"
#include "ft_holder.h"
#include "test/ftest.h"
#include "test/synth.h"

static ft_layout L;

static int run(const synth_spec *s)
{
    ft_axes ax;
    ft_err e;
    float *t = synth_preview(s);
    int rc;
    ax.y_mm0 = 0;
    ax.y_mm_per_line = s->y_mm_per_line;
    ax.x_mm0 = 0;
    ax.x_mm_per_px = s->x_mm_per_px;
    CHECK(t != NULL);
    rc = ft_detect(t, s->lines, s->px, &ax, s->film, ft_holder_find("24x217"),
                   &L, &e);
    free(t);
    return rc;
}

static void ff6(synth_spec *s, ft_film film)
{
    int i;
    synth_default(s, film);
    for (i = 0; i < 6; i++)
        synth_add(s, 4 + 38.0 * i, 36, 0);
}

static void expect_types(const char *want)    /* F H P p per frame */
{
    int i;
    CHECK(L.nframes == (int)strlen(want));
    for (i = 0; want[i]; i++) {
        ft_frame_type t = want[i] == 'F' ? FT_FRAME_FULL :
                          want[i] == 'H' ? FT_FRAME_HALF :
                          want[i] == 'P' ? FT_FRAME_PANORAMA : FT_FRAME_PARTIAL;
        CHECK(L.f[i].type == t);
        CHECK(L.f[i].index == i + 1);
    }
}

int main(void)
{
    synth_spec s;
    int i;

    /* six full frames, colour negative */
    ff6(&s, FT_FILM_NEGATIVE);
    CHECK(run(&s) == 0);
    expect_types("FFFFFF");
    for (i = 0; i < 6; i++) {
        CHECK_NEAR(L.f[i].y0, 4 + 38.0 * i, 0.3);
        CHECK_NEAR(L.f[i].y1, 40 + 38.0 * i, 0.3);
        CHECK_NEAR(L.f[i].x0, 4.1, 0.3);
        CHECK_NEAR(L.f[i].x1, 28.1, 0.3);
        CHECK(!L.f[i].confidence_low);
    }
    CHECK(L.has_holes);
    CHECK_NEAR(L.holes_x0, 0.6, 0.3);
    CHECK_NEAR(L.holes_x1, 31.6, 0.3);
    CHECK_NEAR(L.film_y0, 3.0, 0.1);
    CHECK_NEAR(L.base[0], 0.62, 0.01);
    CHECK(L.nblank == 0 && !L.fallback);

    /* job: sprockets widen the window across to the hole rows */
    {
        ft_job j;
        ft_layout_to_job(&L, "24x217", FT_FILM_NEGATIVE, 2000, 1, &j);
        CHECK(j.nframes == 6 && j.f[0].sprockets && j.f[0].include);
        CHECK_NEAR(j.f[0].x0, L.holes_x0, 1e-9);
        CHECK_NEAR(j.f[0].width, L.holes_x1 - L.holes_x0, 1e-9);
        CHECK_NEAR(j.f[0].len, L.f[0].y1 - L.f[0].y0, 1e-9);
        ft_layout_to_job(&L, "24x217", FT_FILM_NEGATIVE, 2000, 0, &j);
        CHECK_NEAR(j.f[0].x0, L.f[0].x0, 1e-9);
        CHECK(j.focus == FT_FOCUS_SHIFTED && j.f[0].ppi == 2000);
    }

    /* twelve half frames */
    synth_default(&s, FT_FILM_NEGATIVE);
    for (i = 0; i < 12; i++)
        synth_add(&s, 4 + 19.0 * i, 18, 0);
    CHECK(run(&s) == 0);
    expect_types("HHHHHHHHHHHH");
    for (i = 0; i < 12; i++)
        CHECK_NEAR(L.f[i].y0, 4 + 19.0 * i, 0.3);

    /* Autoreflex: full/half switching mid-roll, wider switch gaps */
    synth_default(&s, FT_FILM_NEGATIVE);
    synth_add(&s, 4, 36, 0);
    synth_add(&s, 42, 36, 0);
    synth_add(&s, 81, 18, 0);
    synth_add(&s, 100, 18, 0);
    synth_add(&s, 119, 18, 0);
    synth_add(&s, 140, 36, 0);
    CHECK(run(&s) == 0);
    expect_types("FFHHHF");
    CHECK_NEAR(L.f[2].y0, 81, 0.3);
    CHECK_NEAR(L.f[5].y0, 140, 0.3);

    /* panorama */
    synth_default(&s, FT_FILM_NEGATIVE);
    synth_add(&s, 4, 100, 0);
    synth_add(&s, 106, 36, 0);
    CHECK(run(&s) == 0);
    expect_types("PF");

    /* partial: the film starts inside frame 1 */
    ff6(&s, FT_FILM_NEGATIVE);
    s.film_y0 = 20;
    CHECK(run(&s) == 0);
    expect_types("pFFFFF");

    /* blank frame 3 is dropped and counted */
    ff6(&s, FT_FILM_NEGATIVE);
    s.fr[2].blank = 1;
    CHECK(run(&s) == 0);
    expect_types("FFFFF");
    CHECK(L.nblank == 1);
    CHECK_NEAR(L.f[2].y0, 118, 0.3);

    /* tilted strip */
    ff6(&s, FT_FILM_NEGATIVE);
    s.tilt_deg = 0.3;
    CHECK(run(&s) == 0);
    expect_types("FFFFFF");
    for (i = 0; i < 6; i++) {
        CHECK_NEAR(L.f[i].tilt_deg, 0.3, 0.05);
        CHECK_NEAR(L.f[i].y0, 4 + 38.0 * i, 0.3);
    }

    /* slide film: gaps are the darkest film */
    ff6(&s, FT_FILM_SLIDE);
    CHECK(run(&s) == 0);
    expect_types("FFFFFF");
    CHECK_NEAR(L.f[3].y0, 4 + 38.0 * 3, 0.3);

    /* a round shadow spanning the image width is not a frame edge */
    synth_default(&s, FT_FILM_NEGATIVE);
    synth_add(&s, 4, 100, 0);
    synth_add(&s, 106, 36, 0);
    s.blob = 1;
    s.blob_y = 54;
    s.blob_x = 16.1;
    s.blob_r = 14;
    CHECK(run(&s) == 0);
    expect_types("PF");

    /* dark scene content against one edge of a real gap */
    ff6(&s, FT_FILM_NEGATIVE);
    s.npatch = 1;
    s.patch[0].y0 = 42;
    s.patch[0].y1 = 45;
    s.patch[0].x0 = 4.1;
    s.patch[0].x1 = 10.1;
    CHECK(run(&s) == 0);
    expect_types("FFFFFF");
    CHECK_NEAR(L.f[1].y0, 42, 0.3);

    /* frames touching each other: no edge anywhere -> grid, flagged */
    synth_default(&s, FT_FILM_NEGATIVE);
    s.film_y1 = 231.0;
    for (i = 0; i < 6; i++)
        synth_add(&s, 3 + 38.0 * i, 38, 0);
    CHECK(run(&s) == 0);
    CHECK(L.fallback && L.nframes == 6);
    CHECK(L.f[0].confidence_low && L.f[5].confidence_low);

    /* empty holder */
    synth_default(&s, FT_FILM_NEGATIVE);
    s.film_y0 = s.film_y1 = 0;
    CHECK(run(&s) == -1);
    return TEST_DONE();
}
