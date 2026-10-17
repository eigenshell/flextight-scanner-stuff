/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ftwebd: the scanner box's web server. Serves the page, the strip
 * folders and their files, runs whitelisted ftpoc tasks and streams their
 * output (SSE). LAN only, single user, no auth.
 *
 *   ftwebd [--port 8080] [--strips strips] [--www www] [--ftpoc ftpoc.exe]
 *
 * Run it from ftpoc's own directory: ftpoc reads focus.txt from its
 * current directory, and so does the planner here.
 *
 *   GET  /                                index.html (from --www)
 *   GET  /api/strips                      [{name, job, png}]
 *   POST /api/strips  name=               create the folder
 *   GET  /api/strips/{s}/frames           frame boxes in strip.png pixels,
 *                                         each frame's size now
 *   POST /api/strips/{s}/plan  f<N>=px    re-plan strip.job from the
 *                                         preview's job; absent = skip
 *   GET  /api/strips/{s}/files            [{name, size}]
 *   GET  /api/strips/{s}/files/{f}        one file
 *   GET|PUT /api/strips/{s}/strip.job     whole file (PUT: validated)
 *   POST /api/tasks  op=preview|scan&strip=&film=neg|slide
 *                                         202 {id} | 409 busy
 *   GET  /api/tasks/current, /api/tasks/{id}
 *   GET  /api/tasks/{id}/events?since=k   SSE, id = line number
 *   POST /api/tasks/{id}/stop
 *
 * Interim until the page's wasm engine (web UI plan 2): the share-size
 * planner runs here, in libft's pure planner, from strip.preview.job (the
 * job the last good preview wrote; the planner excludes frames, so every
 * re-plan starts from it). */
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "ft_focus.h"
#include "ft_holder.h"
#include "ft_job.h"
#include "ft_preview.h"
#include "ft_share.h"
#include "http.h"
#include "proc.h"
#include "task.h"

static const char *g_strips = "strips", *g_www = "www";
static ft_lens g_lens;
static pthread_mutex_t g_plan_mu = PTHREAD_MUTEX_INITIALIZER;

static void sock_close(sock_t s)
{
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

/* strip and file names: [A-Za-z0-9._-], 1..64, no leading '.' */
static int name_ok(const char *s)
{
    size_t i, n = strlen(s);
    if (n < 1 || n > 64 || s[0] == '.')
        return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode) ? (long)st.st_size : -1;
}

static int make_dir(const char *p)
{
#ifdef _WIN32
    return _mkdir(p) && errno != EEXIST ? -1 : 0;
#else
    return mkdir(p, 0777) && errno != EEXIST ? -1 : 0;
#endif
}

/* path's whole text (malloc'd, NUL-terminated); NULL if unreadable */
static char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    char *b = NULL;
    long k;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (k = ftell(f)) >= 0 &&
        k <= HTTP_BODY_MAX && fseek(f, 0, SEEK_SET) == 0 &&
        (b = malloc((size_t)k + 1)) != NULL) {
        if (fread(b, 1, (size_t)k, f) != (size_t)k) {
            free(b);
            b = NULL;
        } else {
            b[k] = 0;
            *n = (size_t)k;
        }
    }
    fclose(f);
    return b;
}

static int load_job(const char *path, ft_job *j, ft_err *e)
{
    size_t n;
    char *t = slurp(path, &n);
    int rc;
    if (!t) {
        snprintf(e->msg, sizeof e->msg, "cannot read %s", path);
        return -1;
    }
    rc = ft_job_parse(j, t, n, e);
    free(t);
    return rc;
}

static int save_job(const char *path, const ft_job *j)
{
    size_t n = ft_job_write_buf(j, NULL, 0);
    char *b;
    FILE *f;
    int rc = -1;
    if (n == FT_JOB_BAD || !(b = malloc(n + 1)))
        return -1;
    ft_job_write_buf(j, b, n + 1);
    f = fopen(path, "wb");
    if (f) {
        rc = fwrite(b, 1, n, f) != n;
        rc |= fclose(f) != 0;
    }
    free(b);
    return rc ? -1 : 0;
}

/* "x=" in the preview's metadata (its first output pixel); -1 if absent */
static long preview_x(const char *dir)
{
    char path[400], line[128];
    long v = -1;
    FILE *f;
    snprintf(path, sizeof path, "%s/strip.raw.txt", dir);
    f = fopen(path, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "x=", 2) == 0)
            v = atol(line + 2);
    fclose(f);
    return v;
}

static void api_strips_list(sock_t s)
{
    char *b = malloc(HTTP_BODY_MAX), p[700], nm[160];
    size_t k = 0;
    DIR *d = opendir(g_strips);
    struct dirent *de;
    if (!b) {
        http_error(s, 500, "out of memory");
        if (d)
            closedir(d);
        return;
    }
    b[k++] = '[';
    while (d && (de = readdir(d)) != NULL) {
        if (!name_ok(de->d_name))
            continue;
        snprintf(p, sizeof p, "%s/%s", g_strips, de->d_name);
        if (!is_dir(p) || k + 400 > HTTP_BODY_MAX)
            continue;
        json_esc(nm, sizeof nm, de->d_name);
        snprintf(p, sizeof p, "%s/%s/strip.job", g_strips, de->d_name);
        k += (size_t)snprintf(b + k, HTTP_BODY_MAX - k,
                              "%s{\"name\":%s,\"job\":%s", k > 1 ? "," : "",
                              nm, file_size(p) >= 0 ? "true" : "false");
        snprintf(p, sizeof p, "%s/%s/strip.png", g_strips, de->d_name);
        k += (size_t)snprintf(b + k, HTTP_BODY_MAX - k, ",\"png\":%s}",
                              file_size(p) >= 0 ? "true" : "false");
    }
    if (d)
        closedir(d);
    b[k++] = ']';
    http_send(s, 200, "application/json", b, k);
    free(b);
}

static void api_strips_create(sock_t s, const http_req *r)
{
    char name[80], p[400], nm[160];
    if (http_param(r->body ? r->body : "", "name", name, sizeof name) ||
        !name_ok(name)) {
        http_error(s, 422, "a strip name is 1..64 of A-Z a-z 0-9 . _ - "
                           "(not starting with .)");
        return;
    }
    snprintf(p, sizeof p, "%s/%s", g_strips, name);
    if (make_dir(p)) {
        http_error(s, 500, "cannot create the folder");
        return;
    }
    json_esc(nm, sizeof nm, name);
    http_json(s, 200, "{\"name\":%s}", nm);
}

static void api_files(sock_t s, const char *dir)
{
    char *b = malloc(HTTP_BODY_MAX), p[700], nm[160];
    size_t k = 0;
    DIR *d = opendir(dir);
    struct dirent *de;
    if (!b || !d) {
        free(b);
        if (d)
            closedir(d);
        http_error(s, 404, "no such strip");
        return;
    }
    b[k++] = '[';
    while ((de = readdir(d)) != NULL) {
        long n;
        if (!name_ok(de->d_name) || k + 200 > HTTP_BODY_MAX)
            continue;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if ((n = file_size(p)) < 0)
            continue;
        json_esc(nm, sizeof nm, de->d_name);
        k += (size_t)snprintf(b + k, HTTP_BODY_MAX - k,
                              "%s{\"name\":%s,\"size\":%ld}",
                              k > 1 ? "," : "", nm, n);
    }
    closedir(d);
    b[k++] = ']';
    http_send(s, 200, "application/json", b, k);
    free(b);
}

static const char *type_name(ft_frame_type t)
{
    switch (t) {
    case FT_FRAME_FULL: return "full";
    case FT_FRAME_HALF: return "half";
    case FT_FRAME_PANORAMA: return "panorama";
    case FT_FRAME_PARTIAL: return "partial";
    default: return "other";
    }
}

/* the preview's job (every frame, as detected), else strip.job */
static int load_preview_job(const char *dir, ft_job *j, ft_err *e)
{
    char p[400];
    snprintf(p, sizeof p, "%s/strip.preview.job", dir);
    if (file_size(p) >= 0)
        return load_job(p, j, e);
    snprintf(p, sizeof p, "%s/strip.job", dir);
    return load_job(p, j, e);
}

static void api_frames(sock_t s, const char *dir)
{
    static ft_job pv, cur;
    char p[400], *b;
    size_t k = 0;
    const ft_preview *pm = ft_preview_find("standard");
    ft_axes ax;
    ft_err e;
    long x;
    int i, g, have_cur;

    pthread_mutex_lock(&g_plan_mu);
    if (load_preview_job(dir, &pv, &e)) {
        pthread_mutex_unlock(&g_plan_mu);
        http_error(s, 404, e.msg);
        return;
    }
    snprintf(p, sizeof p, "%s/strip.job", dir);
    have_cur = load_job(p, &cur, &e) == 0;
    x = preview_x(dir);
    ft_preview_axes(pm, x >= 0 ? x : FT_PREVIEW_X_OUT, &ax);
    b = malloc(HTTP_BODY_MAX);
    if (!b) {
        pthread_mutex_unlock(&g_plan_mu);
        http_error(s, 500, "out of memory");
        return;
    }
    k += (size_t)snprintf(b + k, HTTP_BODY_MAX - k,
                          "{\"film\":\"%s\",\"frames\":[",
                          pv.film == FT_FILM_SLIDE ? "slide" : "neg");
    for (i = 0; i < pv.nframes; i++) {
        const ft_job_frame *f = &pv.f[i], *c = NULL;
        double size = 0;
        int done = 0, grp = 0;
        for (g = 0; have_cur && g < cur.nframes; g++)
            if (cur.f[g].index == f->index)
                c = &cur.f[g];
        if (c && c->include && cur.share > 0) {
            size = ft_job_share_px(&cur, c);
            done = c->done;
            grp = c->group;
        }
        k += (size_t)snprintf(b + k, HTTP_BODY_MAX - k,
            "%s{\"n\":%d,\"type\":\"%s\",\"low\":%s,"
            "\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f,"
            "\"size\":%g,\"group\":%d,\"done\":%s}",
            i ? "," : "", f->index, type_name(f->type),
            f->confidence_low ? "true" : "false",
            (f->y0 - ax.y_mm0) / ax.y_mm_per_line,
            (f->x0 - ax.x_mm0) / ax.x_mm_per_px,
            f->len / ax.y_mm_per_line, f->width / ax.x_mm_per_px,
            size, grp, done ? "true" : "false");
    }
    pthread_mutex_unlock(&g_plan_mu);
    k += (size_t)snprintf(b + k, HTTP_BODY_MAX - k, "]}");
    http_send(s, 200, "application/json", b, k);
    free(b);
}

static void api_plan(sock_t s, const char *dir, const http_req *r)
{
    static ft_job j;
    double tg[FT_MAX_FRAMES];
    const ft_holder *h;
    char p[400], key[16], v[32], *b;
    size_t k = 0;
    int i, g, ng, any = 0;
    ft_err e;

    pthread_mutex_lock(&g_plan_mu);
    if (load_preview_job(dir, &j, &e)) {
        pthread_mutex_unlock(&g_plan_mu);
        http_error(s, 404, e.msg);
        return;
    }
    h = ft_holder_find(j.holder);
    for (i = 0; i < j.nframes; i++) {
        char *end;
        tg[i] = 0;
        snprintf(key, sizeof key, "f%d", j.f[i].index);
        if (http_param(r->body ? r->body : "", key, v, sizeof v) == 0) {
            tg[i] = strtod(v, &end);
            if (*end || !(tg[i] >= 0 && tg[i] <= 20000)) {
                pthread_mutex_unlock(&g_plan_mu);
                http_error(s, 422, "a size is px, 0..20000");
                return;
            }
            any |= tg[i] > 0;
        }
    }
    if (!h || !any) {
        pthread_mutex_unlock(&g_plan_mu);
        http_error(s, 422, h ? "no frame chosen" : "unknown holder");
        return;
    }
    ng = ft_share_plan_sizes(&j, tg, j.hf_half_res, &g_lens, h->scan_len_mm,
                             &e);
    if (ng < 0 || ft_job_validate(&j, &g_lens, &e)) {
        pthread_mutex_unlock(&g_plan_mu);
        http_error(s, 422, e.msg);
        return;
    }
    snprintf(p, sizeof p, "%s/strip.job", dir);
    if (save_job(p, &j)) {
        pthread_mutex_unlock(&g_plan_mu);
        http_error(s, 500, "cannot write strip.job");
        return;
    }
    b = malloc(HTTP_BODY_MAX);
    if (!b) {
        pthread_mutex_unlock(&g_plan_mu);
        http_error(s, 500, "out of memory");
        return;
    }
    k += (size_t)snprintf(b + k, HTTP_BODY_MAX - k, "{\"windows\":[");
    for (g = 1; g <= ng; g++) {
        const ft_job_frame *first = NULL;
        int nf = 0;
        k += (size_t)snprintf(b + k, HTTP_BODY_MAX - k,
                              "%s{\"n\":%d,\"frames\":[", g > 1 ? "," : "", g);
        for (i = 0; i < j.nframes; i++)
            if (j.f[i].include && j.f[i].group == g) {
                k += (size_t)snprintf(b + k, HTTP_BODY_MAX - k, "%s%d",
                                      nf++ ? "," : "", j.f[i].index);
                if (!first)
                    first = &j.f[i];
            }
        if (first)
            k += (size_t)snprintf(b + k, HTTP_BODY_MAX - k,
                                  "],\"px\":%g,\"ppi\":%g,\"bin\":%d}",
                                  ft_job_share_px(&j, first), first->ppi,
                                  ft_job_bin(first));
        else
            k += (size_t)snprintf(b + k, HTTP_BODY_MAX - k, "]}");
    }
    pthread_mutex_unlock(&g_plan_mu);
    k += (size_t)snprintf(b + k, HTTP_BODY_MAX - k, "]}");
    http_send(s, 200, "application/json", b, k);
    free(b);
}

static void api_job_put(sock_t s, const char *dir, const http_req *r)
{
    static ft_job j;
    char p[400];
    ft_err e;
    FILE *f;
    int rc;
    pthread_mutex_lock(&g_plan_mu);
    if (ft_job_parse(&j, r->body ? r->body : "", r->nbody, &e) ||
        ft_job_validate(&j, &g_lens, &e)) {
        pthread_mutex_unlock(&g_plan_mu);
        http_error(s, 422, e.msg);
        return;
    }
    snprintf(p, sizeof p, "%s/strip.job", dir);
    f = fopen(p, "wb");
    rc = !f;
    if (f) {
        rc = fwrite(r->body, 1, r->nbody, f) != r->nbody;
        rc |= fclose(f) != 0;
    }
    pthread_mutex_unlock(&g_plan_mu);
    if (rc)
        http_error(s, 500, "cannot write strip.job");
    else
        http_json(s, 200, "{\"ok\":true}");
}

static void api_task_start(sock_t s, const http_req *r)
{
    char op[16], strip[80], film[16], err[300], p[400];
    const char *body = r->body ? r->body : "";
    int id, rc;
    if (http_param(body, "op", op, sizeof op) ||
        http_param(body, "strip", strip, sizeof strip) || !name_ok(strip)) {
        http_error(s, 422, "op and a valid strip name are required");
        return;
    }
    snprintf(p, sizeof p, "%s/%s", g_strips, strip);
    if (!is_dir(p)) {
        http_error(s, 404, "no such strip");
        return;
    }
    if (http_param(body, "film", film, sizeof film))
        film[0] = 0;
    rc = task_start(op, strip, film[0] ? film : NULL, &id, err, sizeof err);
    if (rc == 1)
        http_error(s, 409, err);
    else if (rc)
        http_error(s, 422, err);
    else
        http_json(s, 202, "{\"id\":%d}", id);
}

static void api_events(sock_t s, int id, const http_req *r)
{
    char v[24], *buf;
    long since = 0;
    int ended = 0;
    if (http_param(r->query, "since", v, sizeof v) == 0)
        since = atol(v);
    if (r->last_event_id[0])
        since = atol(r->last_event_id);
    if (since < 0)
        since = 0;
    buf = malloc(256 * 1024);
    if (!buf) {
        http_error(s, 500, "out of memory");
        return;
    }
    if (http_sse_head(s) == 0)
        for (;;) {
            long n = task_events(id, &since, buf, 256 * 1024, 15, &ended);
            if (n < 0) {
                static const char gone[] = "event: gone\ndata: \n\n";
                http_send_all(s, gone, sizeof gone - 1);
                break;
            }
            if (n == 0 && !ended) {
                static const char ka[] = ": keepalive\n\n";
                if (http_send_all(s, ka, sizeof ka - 1))
                    break;
                continue;
            }
            if (n > 0 && http_send_all(s, buf, (size_t)n))
                break;
            if (ended) {
                static const char end[] = "event: end\ndata: \n\n";
                http_send_all(s, end, sizeof end - 1);
                break;
            }
        }
    free(buf);
}

/* path split on '/': seg[0..n) point into a copy */
static int split(const char *path, char *copy, size_t cap, char **seg,
                 int max)
{
    int n = 0;
    char *p;
    snprintf(copy, cap, "%s", path);
    for (p = strtok(copy, "/"); p && n < max; p = strtok(NULL, "/"))
        seg[n++] = p;
    return p ? -1 : n;
}

static void route(http_req *r)
{
    sock_t s = r->s;
    char copy[512], *seg[8], dir[300], p[600], js[600];
    int n = split(r->path, copy, sizeof copy, seg, 8);
    int get = strcmp(r->method, "GET") == 0;
    int post = strcmp(r->method, "POST") == 0;
    int put = strcmp(r->method, "PUT") == 0;

    if (n == 0 || (n == 1 && strcmp(seg[0], "index.html") == 0)) {
        snprintf(p, sizeof p, "%s/index.html", g_www);
        if (get)
            http_file(s, p);
        else
            http_error(s, 405, "GET only");
        return;
    }
    if (n == 1 && name_ok(seg[0]) && get) {     /* ftweb.wasm etc. later */
        snprintf(p, sizeof p, "%s/%s", g_www, seg[0]);
        http_file(s, p);
        return;
    }
    if (n < 2 || strcmp(seg[0], "api") != 0) {
        http_error(s, 404, "not found");
        return;
    }
    if (strcmp(seg[1], "strips") == 0) {
        if (n == 2 && get) {
            api_strips_list(s);
            return;
        }
        if (n == 2 && post) {
            api_strips_create(s, r);
            return;
        }
        if (n < 4 || !name_ok(seg[2])) {
            http_error(s, 404, "not found");
            return;
        }
        snprintf(dir, sizeof dir, "%s/%s", g_strips, seg[2]);
        if (!is_dir(dir)) {
            http_error(s, 404, "no such strip");
            return;
        }
        if (n == 4 && strcmp(seg[3], "frames") == 0 && get)
            api_frames(s, dir);
        else if (n == 4 && strcmp(seg[3], "plan") == 0 && post)
            api_plan(s, dir, r);
        else if (n == 4 && strcmp(seg[3], "files") == 0 && get)
            api_files(s, dir);
        else if (n == 5 && strcmp(seg[3], "files") == 0 && get &&
                 name_ok(seg[4])) {
            snprintf(p, sizeof p, "%s/%s", dir, seg[4]);
            http_file(s, p);
        } else if (n == 4 && strcmp(seg[3], "strip.job") == 0 && get) {
            snprintf(p, sizeof p, "%s/strip.job", dir);
            http_file(s, p);
        } else if (n == 4 && strcmp(seg[3], "strip.job") == 0 && put)
            api_job_put(s, dir, r);
        else
            http_error(s, 404, "not found");
        return;
    }
    if (strcmp(seg[1], "tasks") == 0) {
        int id;
        if (n == 2 && post) {
            api_task_start(s, r);
            return;
        }
        if (n < 3) {
            http_error(s, 404, "not found");
            return;
        }
        id = strcmp(seg[2], "current") == 0 ? 0 : atoi(seg[2]);
        if (n == 3 && get) {
            task_json(id, js, sizeof js);
            http_send(s, 200, "application/json", js, strlen(js));
        } else if (n == 4 && strcmp(seg[3], "events") == 0 && get && id > 0) {
            api_events(s, id, r);
        } else if (n == 4 && strcmp(seg[3], "stop") == 0 && post && id > 0) {
            int rc = task_stop(id);
            if (rc == -2)
                http_error(s, 404, "no such task");
            else if (rc)
                http_error(s, 409, "not running");
            else
                http_json(s, 200, "{\"ok\":true}");
        } else {
            http_error(s, 404, "not found");
        }
        return;
    }
    http_error(s, 404, "not found");
}

static void *serve(void *arg)
{
    http_req r;
    int code;
    memset(&r, 0, sizeof r);
    r.s = *(sock_t *)arg;
    free(arg);
    code = http_read(&r);
    if (code)
        http_error(r.s, code, "bad request");
    else
        route(&r);
    http_free(&r);
    sock_close(r.s);
    return NULL;
}

static void usage(void)
{
    fprintf(stderr, "usage: ftwebd [--port 8080] [--strips strips] "
                    "[--www www] [--ftpoc ftpoc]\n");
}

static void load_lens(void)
{
    FILE *f = fopen("focus.txt", "r");
    ft_err e;
    g_lens = ft_lens_default;
    if (!f)
        return;
    if (ft_lens_read(&g_lens, f, &e)) {
        fprintf(stderr, "focus.txt: %s; using the built-in curve\n", e.msg);
        g_lens = ft_lens_default;
    } else {
        printf("focus.txt: the planner uses this scanner's fit\n");
    }
    fclose(f);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    const char *ftpoc = "ftpoc.exe";
    WSADATA wsa;
#else
    const char *ftpoc = "./ftpoc";
#endif
    static char ftpoc_abs[1024];
    struct sockaddr_in a;
    task_cfg cfg;
    sock_t ls;
    int i, port = 8080, one = 1;

    for (i = 1; i < argc; i++) {
        if (i + 1 >= argc) {
            usage();
            return 2;
        }
        if (strcmp(argv[i], "--port") == 0)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--strips") == 0)
            g_strips = argv[++i];
        else if (strcmp(argv[i], "--www") == 0)
            g_www = argv[++i];
        else if (strcmp(argv[i], "--ftpoc") == 0)
            ftpoc = argv[++i];
        else {
            usage();
            return 2;
        }
    }
    if (port < 1 || port > 65535) {
        usage();
        return 2;
    }
    if (proc_abspath(ftpoc, ftpoc_abs, sizeof ftpoc_abs)) {
        fprintf(stderr, "ftwebd: no ftpoc at %s (--ftpoc)\n", ftpoc);
        return 1;
    }
    if (make_dir(g_strips)) {
        fprintf(stderr, "ftwebd: cannot create %s\n", g_strips);
        return 1;
    }
    load_lens();
    cfg.ftpoc = ftpoc_abs;
    cfg.strips = g_strips;
    task_init(&cfg);

#ifdef _WIN32
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
        fprintf(stderr, "ftwebd: WSAStartup failed\n");
        return 1;
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    ls = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 16)) {
        fprintf(stderr, "ftwebd: cannot listen on port %d\n", port);
        return 1;
    }
    printf("ftwebd: http://<this machine>:%d/  (ftpoc %s, strips in %s)\n",
           port, ftpoc_abs, g_strips);
    fflush(stdout);
    for (;;) {
        pthread_t th;
        sock_t *c = malloc(sizeof *c);
        if (!c)
            continue;
        *c = accept(ls, NULL, NULL);
#ifdef _WIN32
        if (*c == INVALID_SOCKET) {
#else
        if (*c < 0) {
#endif
            free(c);
            continue;
        }
        if (pthread_create(&th, NULL, serve, c)) {
            sock_close(*c);
            free(c);
            continue;
        }
        pthread_detach(th);
    }
}
