/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ftweb: the web page's engine (ftweb.wasm). Everything the page does
 * with pixels and files is here, in C on libft's browser tier; the page's
 * JS only moves bytes between the box (GET/PUT), this engine and the DOM.
 *
 * One strip at a time (one ftw): its strip.job and strip.print, and the
 * share frames cut from its window scans. Cutting and printing go
 * through the same libft calls as ftdev cut + ftdev print --roll
 * (ft_share_cut, ft_share_print_frame), and each frame is kept as the
 * DNG would keep it (16-bit samples, ppi and noise profile as stored,
 * the sidecar through its text), so the JPEG bytes are the CLI's
 * (tests/test_ftweb.py).
 *
 * Calls return 0 (or a count) or -1 with ftw_error() set. Results that
 * are bytes (JSON, file texts, JPEG, zip) go to the out buffer:
 * ftw_out() / ftw_out_len(), valid until the next call that fills it. */
#ifndef FTWEB_H
#define FTWEB_H
#include <stddef.h>

typedef struct ftw ftw;

ftw *ftw_new(void);
void ftw_free(ftw *w);
const char *ftw_error(const ftw *w);
const unsigned char *ftw_out(const ftw *w);
size_t ftw_out_len(const ftw *w);
/* the last print text (ftw_jpeg): the JPEG's .txt and comment */
const char *ftw_text(const ftw *w);

/* the compiled-in emulsions -> out: [{"id","name","mode"}...] */
int ftw_emulsions(ftw *w);

/* strip.job: load, describe (-> out JSON: film, holder, share, frames
 * [{index,type,include,group,share_px,y0,len,x0,width}]), re-plan with a
 * size per frame (px[i] for the job's i-th frame: 0 skip,
 * FT_SHARE_4MP, FT_SHARE_6MP, ...) and write (-> out, the file text) */
int ftw_job_load(ftw *w, const char *text, size_t n);
int ftw_job_json(ftw *w);
int ftw_plan(ftw *w, const double *px, int n);
int ftw_job_text(ftw *w);

/* strip.print: load (or start one: ftw_print_new), the emulsion, one
 * frame's clicks, and write (-> out, the file text) */
int ftw_print_load(ftw *w, const char *text, size_t n);
int ftw_print_new(ftw *w, const char *emulsion);
int ftw_print_clicks(ftw *w, int frame, int c, int m, int y, int d);
int ftw_print_text(ftw *w);

/* one window scan (ftpoc scan --job): its raw lines, the .cal, the .faul
 * (NULL/0: none), the .txt and a defects file (NULL/0: none) -> its
 * frames, cut, replacing frames of the same number. Returns the number
 * of frames cut; out: JSON {"detect_ok","noise_note","frames":[{"frame",
 * "estimated","shift_mm","w","h"}]}. */
int ftw_cut(ftw *w, const unsigned short *raw, size_t raw_n,
            const unsigned short *cal, size_t cal_n, const char *faul,
            size_t faul_n, const char *meta, size_t meta_n,
            const char *defects, size_t defects_n);
/* the landed frames' numbers, ascending, into frames (cap); the count */
int ftw_frames(const ftw *w, int *frames, int cap);

/* the strip's colour balance over every landed frame (ftdev print
 * --roll over them): rerun as frames land. -> out: notes, one per line
 * (the "[frame N] not in the job" warning, few frames, estimated base) */
int ftw_measure(ftw *w);
/* one frame's share JPEG (-> out) and its text (ftw_text) with its
 * clicks from strip.print */
int ftw_jpeg(ftw *w, int frame);
/* the screen copy: the frame printed at long_edge px (never up), sRGB,
 * no NR or sharpening -> out: RGBA rows; *pw x *ph */
int ftw_display(ftw *w, int frame, int long_edge, int *pw, int *ph);
/* every landed frame's JPEG and text, <base>_frameNN.jpg(.txt), in a
 * store-only zip -> out */
int ftw_zip(ftw *w, const char *base);

/* the page's heap */
void *ftw_alloc(size_t n);
void ftw_release(void *p);

#endif
