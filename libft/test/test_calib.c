/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <string.h>
#include "ft_calib.h"
#include "test/ftest.h"

int main(void)
{
    enum { PX = 4, CL = 250 };
    static unsigned short cal[CL * PX * 3];
    float dark[PX * 3], white[PX * 3];
    ft_err e;
    int l, i;

    /* the 3150 ppi case: 188 dark lines, 12 white, 2 transition, 48 white */
    for (l = 0; l < CL; l++)
        for (i = 0; i < PX * 3; i++)
            cal[l * PX * 3 + i] = (unsigned short)
                (l < 188 ? 300 + l % 3 : l < 200 ? 3600 : l < 202 ? 2000 : 3600);
    CHECK(ft_split_cal(cal, CL, PX, dark, white, &e) == 0);
    CHECK_NEAR(dark[0], 300.995, 0.01);
    CHECK_NEAR(white[5], 3600, 1e-3);

    for (i = 0; i < CL * PX * 3; i++)
        cal[i] = 300;
    CHECK(ft_split_cal(cal, CL, PX, dark, white, &e) == -1);

    /* flat-field: (raw - dark) / (white - dark), clipped to 0..1 */
    {
        unsigned short raw[3] = { 1950, 100, 5000 };
        float d[3] = { 300, 300, 300 }, w[3] = { 3600, 3600, 3600 }, t[3];
        ft_flatfield(raw, 1, 1, d, w, t);
        CHECK_NEAR(t[0], 0.5, 1e-6);
        CHECK_NEAR(t[1], 0.0, 1e-9);
        CHECK_NEAR(t[2], 1.0, 1e-9);
    }

    /* Faul Pixs: header, then per channel "100\t0" and "<pixel>\t+999" */
    {
        static const char txt[] =
            "Faul Pixs\0" "100\t0\n1234\t+999\n100\t0\n100\t0\n2000\t+999\n"
            "77\t-12\n";
        int pix[3][FT_FAUL_MAX], cnt[3];
        CHECK(ft_faul_parse(txt, sizeof txt - 1, pix, cnt) == 2);
        CHECK(cnt[0] == 1 && pix[0][0] == 1234);
        CHECK(cnt[1] == 0);
        CHECK(cnt[2] == 1 && pix[2][0] == 2000);
    }

    /* Faul fix: flagged output pixels interpolate from good neighbours */
    {
        float t[10 * 3];
        int pix[3][FT_FAUL_MAX], cnt[3] = { 2, 0, 0 };
        for (i = 0; i < 10; i++)
            t[i * 3] = i * 0.1f;
        t[4 * 3] = 9;
        t[5 * 3] = 9;
        pix[0][0] = 144 + 4;          /* x = 0, bin = 1 -> output pixel 4 */
        pix[0][1] = 144 + 5;
        ft_faul_fix(t, 1, 10, pix, cnt, 0, 1);
        CHECK_NEAR(t[4 * 3], 0.4, 1e-6);
        CHECK_NEAR(t[5 * 3], 0.5, 1e-6);
    }

    /* tri-linear alignment: R[l + 2k], G[l + k], B[l] form output line l */
    {
        float t[6 * 3];
        for (l = 0; l < 6; l++)
            for (i = 0; i < 3; i++)
                t[l * 3 + i] = (float)(l * 10 + i);
        CHECK(ft_align_rgb(t, 6, 1, 1) == 4);
        CHECK_NEAR(t[0], 20, 1e-9);
        CHECK_NEAR(t[1], 11, 1e-9);
        CHECK_NEAR(t[2], 2, 1e-9);
        CHECK_NEAR(t[3 * 3 + 0], 50, 1e-9);
    }
    return TEST_DONE();
}
