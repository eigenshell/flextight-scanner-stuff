/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The focus curve file (focus.txt) through a stream: ft_focus.c also runs
 * in the browser, these two are for the CLIs. */
#include <math.h>
#include <string.h>
#include "ft_focus.h"

int ft_lens_write(const ft_lens *l, FILE *f)
{
    fprintf(f, "# ftpoc focus curve (thin lens, motor steps)\n"
               "film = %.1f\nw = %.1f\nfl = %.1f\np = %.6e\n",
            l->film, l->w, l->fl, l->p);
    return ferror(f) ? -1 : 0;
}

int ft_lens_read(ft_lens *l, FILE *f, ft_err *e)
{
    char line[160];
    ft_lens t = { 0, 0, 0, 0 };
    int ln = 0, seen = 0;
    while (fgets(line, sizeof line, f)) {
        char key[16];
        double v;
        int used = 0, over = !strchr(line, '\n') && !feof(f);
        const char *p = line;
        ln++;
        if (over) {                      /* drop the rest of a long line */
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n')
                ;
        }
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\0' || *p == '\n' || *p == '\r')
            continue;
        if (over)
            return ft_fail(e, "focus file line %d: too long", ln);
        if (sscanf(p, "%15[a-z] = %lf %n", key, &v, &used) != 2 ||
            p[used] != '\0')
            return ft_fail(e, "focus file line %d: expected key = number", ln);
        if (!isfinite(v))
            return ft_fail(e, "focus file line %d: value not finite", ln);
        if (strcmp(key, "film") == 0)    { t.film = v; seen |= 1; }
        else if (strcmp(key, "w") == 0)  { t.w = v;    seen |= 2; }
        else if (strcmp(key, "fl") == 0) { t.fl = v;   seen |= 4; }
        else if (strcmp(key, "p") == 0)  { t.p = v;    seen |= 8; }
        else return ft_fail(e, "focus file line %d: unknown key %s", ln, key);
    }
    if (seen != 15)
        return ft_fail(e, "focus file: needs film, w, fl and p");
    if (!(t.fl > 0) || !(t.p > 0) || !isfinite(t.film))
        return ft_fail(e, "focus file: fl and p must be positive");
    *l = t;
    return 0;
}
