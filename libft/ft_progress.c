/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "ft_progress.h"

static const char *KIND[] = { "preview", "window", "frame", "pass" };
static const char *STATUS[] = { "ok", "stopped", "failed", "refused" };

size_t ft_progress_fmt(const ft_progress *p, char *buf, size_t cap)
{
    int k = -1;
    if (cap)
        buf[0] = 0;
    switch (p->type) {
    case FT_PG_BEGIN:
        if ((unsigned)p->kind <= FT_PG_PASS && p->n >= 0)
            k = snprintf(buf, cap, "progress begin %s %d\n", KIND[p->kind],
                         p->n);
        break;
    case FT_PG_UNIT:
        if ((unsigned)p->kind <= FT_PG_PASS && p->n >= 1 && p->label >= 0)
            k = snprintf(buf, cap, "progress unit %s %d %d\n", KIND[p->kind],
                         p->n, p->label);
        break;
    case FT_PG_LINES:
        if (p->k >= 0 && p->k <= p->total)
            k = snprintf(buf, cap, "progress lines %ld/%ld\n", p->k,
                         p->total);
        break;
    case FT_PG_FILE: {
        size_t n = 0;
        while (n < sizeof p->path && p->path[n])
            n++;
        if (n > 0 && n < sizeof p->path && !memchr(p->path, '\n', n) &&
            !memchr(p->path, '\r', n))
            k = snprintf(buf, cap, "progress file %s\n", p->path);
        break;
    }
    case FT_PG_END:
        if ((unsigned)p->status <= FT_PG_REFUSED)
            k = snprintf(buf, cap, "progress end %s\n", STATUS[p->status]);
        break;
    case FT_PG_FOCUS:
        if (p->k >= 1)
            k = snprintf(buf, cap, "progress focus %ld\n", p->k);
        break;
    }
    if (k < 0) {
        if (cap)
            buf[0] = 0;
        return FT_PG_BAD;
    }
    return (size_t)k;
}

/* the word at *s up to a space or the end; *s moves past one space */
static int word(const char **s, const char *end, const char *const *tab,
                int n)
{
    const char *a = *s, *b = a;
    int i;
    while (b < end && *b != ' ')
        b++;
    for (i = 0; i < n; i++)
        if ((size_t)(b - a) == strlen(tab[i]) &&
            memcmp(a, tab[i], (size_t)(b - a)) == 0) {
            *s = b < end ? b + 1 : b;
            return b < end && b + 1 == end ? -1 : i;   /* no trailing ' ' */
        }
    return -1;
}

/* digits at *s (at least one) up to stop or the end; -1 if none or too
 * big */
static long digits(const char **s, const char *end, char stop)
{
    long v = 0;
    const char *a = *s;
    while (*s < end && **s >= '0' && **s <= '9') {
        if (v > (LONG_MAX - 9) / 10)
            return -1;
        v = v * 10 + (**s - '0');
        (*s)++;
    }
    if (*s == a || (*s < end && **s != stop))
        return -1;
    if (*s < end)
        (*s)++;
    return v;
}

int ft_progress_parse(const char *line, size_t n, ft_progress *p)
{
    static const char *const TYPE[] = { "begin", "unit", "lines", "file",
                                        "end", "focus" };
    const char *s = line, *end = line + n;
    long a, b, c;
    int t;

    if (n && end[-1] == '\n')
        end--;
    if (end > line && end[-1] == '\r')
        end--;
    if (end - s < 9 || memcmp(s, "progress ", 9) != 0)
        return -1;
    s += 9;
    memset(p, 0, sizeof *p);
    t = word(&s, end, TYPE, 6);
    if (t < 0 || s >= end)
        return -1;
    p->type = (ft_pg_type)t;
    switch (p->type) {
    case FT_PG_BEGIN:
    case FT_PG_UNIT:
        if ((t = word(&s, end, KIND, 4)) < 0)
            return -1;
        p->kind = (ft_pg_kind)t;
        a = digits(&s, end, ' ');
        if (a < 0 || a > INT_MAX)
            return -1;
        p->n = (int)a;
        if (p->type == FT_PG_BEGIN)
            return s == end && end[-1] != ' ' ? 0 : -1;
        b = digits(&s, end, ' ');
        if (a < 1 || b < 0 || b > INT_MAX || s != end || end[-1] == ' ')
            return -1;
        p->label = (int)b;
        return 0;
    case FT_PG_LINES:
        a = digits(&s, end, '/');
        if (a < 0 || s >= end || s[-1] != '/')
            return -1;
        c = digits(&s, end, ' ');
        if (c < 0 || s != end || a > c)
            return -1;
        p->k = a;
        p->total = c;
        return 0;
    case FT_PG_FILE:
        if ((size_t)(end - s) >= sizeof p->path || memchr(s, '\r', end - s) ||
            memchr(s, 0, end - s))
            return -1;
        memcpy(p->path, s, (size_t)(end - s));
        return 0;
    case FT_PG_END:
        if ((t = word(&s, end, STATUS, 4)) < 0 || s != end)
            return -1;
        p->status = (ft_pg_status)t;
        return 0;
    case FT_PG_FOCUS:
        a = digits(&s, end, ' ');
        if (a < 1 || s != end)
            return -1;
        p->k = a;
        return 0;
    }
    return -1;
}
