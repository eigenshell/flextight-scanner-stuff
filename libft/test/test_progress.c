/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "ft_progress.h"
#include "test/ftest.h"

/* p -> text == want, and want parses back to the same fields */
static void both(const ft_progress *p, const char *want)
{
    char buf[400];
    ft_progress q;
    size_t n = ft_progress_fmt(p, buf, sizeof buf);
    CHECK(n == strlen(want) && strcmp(buf, want) == 0);
    CHECK(ft_progress_parse(want, strlen(want), &q) == 0);
    CHECK(q.type == p->type);
    switch (p->type) {
    case FT_PG_BEGIN:
        CHECK(q.kind == p->kind && q.n == p->n);
        break;
    case FT_PG_UNIT:
        CHECK(q.kind == p->kind && q.n == p->n && q.label == p->label);
        break;
    case FT_PG_LINES:
        CHECK(q.k == p->k && q.total == p->total);
        break;
    case FT_PG_FILE:
        CHECK(strcmp(q.path, p->path) == 0);
        break;
    case FT_PG_END:
        CHECK(q.status == p->status);
        break;
    }
}

static int bad(const char *s)
{
    ft_progress q;
    return ft_progress_parse(s, strlen(s), &q) == -1;
}

int main(void)
{
    ft_progress p;
    char buf[400];

    /* the wire format: pinned, the web server and page read it */
    memset(&p, 0, sizeof p);
    p.type = FT_PG_BEGIN; p.kind = FT_PG_WINDOW; p.n = 3;
    both(&p, "progress begin window 3\n");
    p.kind = FT_PG_PREVIEW; p.n = 1;
    both(&p, "progress begin preview 1\n");
    p.kind = FT_PG_FRAME; p.n = 12;
    both(&p, "progress begin frame 12\n");
    p.kind = FT_PG_PASS; p.n = 2;
    both(&p, "progress begin pass 2\n");
    p.type = FT_PG_UNIT; p.kind = FT_PG_WINDOW; p.n = 2; p.label = 5;
    both(&p, "progress unit window 2 5\n");
    p.type = FT_PG_LINES; p.k = 812; p.total = 1890;
    both(&p, "progress lines 812/1890\n");
    p.type = FT_PG_FILE;
    strcpy(p.path, "C:\\scans\\my strip\\strip_win05.raw");
    both(&p, "progress file C:\\scans\\my strip\\strip_win05.raw\n");
    p.type = FT_PG_END; p.status = FT_PG_OK;
    both(&p, "progress end ok\n");
    p.status = FT_PG_STOPPED;
    both(&p, "progress end stopped\n");
    p.status = FT_PG_FAILED;
    both(&p, "progress end failed\n");
    p.status = FT_PG_REFUSED;
    both(&p, "progress end refused\n");

    /* parse: without the '\n', a '\r\n' ending, exactly n bytes */
    {
        ft_progress q;
        CHECK(ft_progress_parse("progress lines 5/9", 18, &q) == 0 &&
              q.type == FT_PG_LINES && q.k == 5 && q.total == 9);
        CHECK(ft_progress_parse("progress lines 5/9\r\n", 20, &q) == 0 &&
              q.total == 9);
        CHECK(ft_progress_parse("progress lines 5/99", 18, &q) == 0 &&
              q.total == 9);
    }
    /* anything else is not a progress line */
    CHECK(bad("frame 3: REFUSED (reason above)\n"));
    CHECK(bad("progress\n"));
    CHECK(bad("progress begin\n"));
    CHECK(bad("progress begin window\n"));
    CHECK(bad("progress begin window -1\n"));
    CHECK(bad("progress begin window 3x\n"));
    CHECK(bad("progress begin film 3\n"));
    CHECK(bad("progress unit window 2\n"));
    CHECK(bad("progress unit window 0 5\n"));
    CHECK(bad("progress lines 5\n"));
    CHECK(bad("progress lines 5/\n"));
    CHECK(bad("progress lines 10/9\n"));
    CHECK(bad("progress lines 99999999999999999999/1\n"));
    CHECK(bad("progress file \n"));
    CHECK(bad("progress end maybe\n"));
    CHECK(bad("progress end ok now\n"));
    CHECK(bad("progress  end ok\n"));
    CHECK(bad("Progress end ok\n"));
    /* a path the line cannot carry: nothing written */
    memset(&p, 0, sizeof p);
    p.type = FT_PG_FILE;
    strcpy(p.path, "a\nb");
    CHECK(ft_progress_fmt(&p, buf, sizeof buf) == FT_PG_BAD && buf[0] == 0);
    p.path[0] = 0;
    CHECK(ft_progress_fmt(&p, buf, sizeof buf) == FT_PG_BAD);
    /* truncation: the length needed, NUL-terminated */
    p.type = FT_PG_END;
    p.status = FT_PG_OK;
    CHECK(ft_progress_fmt(&p, buf, 5) == 16 && strcmp(buf, "prog") == 0);
    return TEST_DONE();
}
