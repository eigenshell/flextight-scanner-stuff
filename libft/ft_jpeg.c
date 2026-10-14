/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <setjmp.h>
#include <stdio.h>
#include <jpeglib.h>
#include "ft_jpeg.h"

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
    char msg[JMSG_LENGTH_MAX];
} err_mgr;

static void on_error(j_common_ptr c)
{
    err_mgr *m = (err_mgr *)c->err;
    (*c->err->format_message)(c, m->msg);
    longjmp(m->jump, 1);
}

int ft_jpeg_write(const char *path, const unsigned char *rgb, int w, int h,
                  int quality, double ppi, const unsigned char *icc,
                  size_t icc_n, ft_err *e)
{
    struct jpeg_compress_struct c;
    err_mgr em;
    FILE *volatile f = NULL;
    int rc = -1;

    if (!rgb || w < 1 || h < 1 || w > 65500 || h > 65500)
        return ft_fail(e, "jpeg: bad size %dx%d", w, h);
    if (quality < 1 || quality > 100)
        return ft_fail(e, "jpeg: quality %d (1..100)", quality);
    if (icc && icc_n > 65519L * 255)
        return ft_fail(e, "jpeg: ICC profile too large");
    f = fopen(path, "wb");
    if (!f)
        return ft_fail(e, "jpeg: cannot write %s", path);
    c.err = jpeg_std_error(&em.pub);
    em.pub.error_exit = on_error;
    if (setjmp(em.jump)) {
        ft_fail(e, "jpeg: %s: %s", path, em.msg);
        jpeg_destroy_compress(&c);
        fclose(f);
        remove(path);
        return -1;
    }
    jpeg_create_compress(&c);
    jpeg_stdio_dest(&c, f);
    c.image_width = (JDIMENSION)w;
    c.image_height = (JDIMENSION)h;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, quality, TRUE);
    c.optimize_coding = TRUE;
    c.comp_info[0].h_samp_factor = c.comp_info[0].v_samp_factor = 1;
    c.comp_info[1].h_samp_factor = c.comp_info[1].v_samp_factor = 1;
    c.comp_info[2].h_samp_factor = c.comp_info[2].v_samp_factor = 1;
    if (ppi > 0) {
        long d = (long)(ppi + 0.5);
        c.density_unit = 1;                     /* dots per inch */
        c.X_density = c.Y_density = (UINT16)(d < 1 ? 1 : d > 65535 ? 65535
                                                                  : d);
    }
    jpeg_start_compress(&c, TRUE);
    if (icc && icc_n)
        jpeg_write_icc_profile(&c, icc, (unsigned int)icc_n);
    while (c.next_scanline < c.image_height) {
        JSAMPROW row = (JSAMPROW)(rgb + (size_t)c.next_scanline * w * 3);
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    rc = 0;
    if (fclose(f) != 0) {
        remove(path);
        rc = ft_fail(e, "jpeg: write error on %s", path);
    }
    return rc;
}
