/* SPDX-License-Identifier: GPL-2.0-or-later */
/* task.h: one ftpoc child at a time, a reader thread splitting its output
 * into numbered lines (on '\n' or '\r': ftpoc's human counters use '\r'),
 * and waiters woken per line. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "http.h"
#include "proc.h"
#include "task.h"

#define RING 4096               /* lines kept; older ones are dropped */
#define LINE 320

static struct {
    task_cfg cfg;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int id, running, exit_code, stopped;
    char op[16], strip[80];
    proc p;
    long nlines;                /* lines numbered 1..nlines so far */
    char lines[RING][LINE];
    char part[LINE];            /* the line being read */
    size_t npart;
} T = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

void task_init(const task_cfg *cfg)
{
    T.cfg = *cfg;
}

/* under T.mu */
static void add_line(const char *s, size_t n)
{
    char *d;
    if (n >= LINE)
        n = LINE - 1;
    T.nlines++;
    d = T.lines[(T.nlines - 1) % RING];
    memcpy(d, s, n);
    d[n] = 0;
    pthread_cond_broadcast(&T.cv);
}

/* under T.mu: bytes from the child, split into lines */
static void feed(const char *b, long n)
{
    long i;
    for (i = 0; i < n; i++) {
        if (b[i] == '\n' || b[i] == '\r') {
            if (T.npart)
                add_line(T.part, T.npart);
            T.npart = 0;
        } else if (T.npart < LINE - 1) {
            T.part[T.npart++] = b[i];
        }
    }
}

/* a good preview: keep its job as strip.preview.job, the planner's
 * starting point (re-plans exclude frames from strip.job) */
static void keep_preview_job(void)
{
    char a[300], b[300], buf[4096];
    FILE *in, *out;
    size_t n;
    snprintf(a, sizeof a, "%s/%s/strip.job", T.cfg.strips, T.strip);
    snprintf(b, sizeof b, "%s/%s/strip.preview.job", T.cfg.strips, T.strip);
    in = fopen(a, "rb");
    if (!in)
        return;
    out = fopen(b, "wb");
    if (out) {
        while ((n = fread(buf, 1, sizeof buf, in)) > 0)
            fwrite(buf, 1, n, out);
        fclose(out);
    }
    fclose(in);
}

static void *reader(void *arg)
{
    char buf[4096], end[48];
    long n;
    int code;
    (void)arg;
    while ((n = proc_read(&T.p, buf, sizeof buf)) > 0) {
        pthread_mutex_lock(&T.mu);
        feed(buf, n);
        pthread_mutex_unlock(&T.mu);
    }
    code = proc_wait(&T.p);
    pthread_mutex_lock(&T.mu);
    if (T.npart)
        add_line(T.part, T.npart);
    T.npart = 0;
    snprintf(end, sizeof end, "ftwebd exit %d", code);
    T.exit_code = code;
    if (code == 0 && strcmp(T.op, "preview") == 0)
        keep_preview_job();
    T.running = 0;
    add_line(end, strlen(end));
    pthread_mutex_unlock(&T.mu);
    return NULL;
}

int task_start(const char *op, const char *strip, const char *film, int *id,
               char *err, size_t errn)
{
    char base[300], job[310], cmd[LINE];
    const char *argv[16];
    pthread_t th;
    size_t k = 0;
    int i;

    snprintf(base, sizeof base, "%s/%s/strip", T.cfg.strips, strip);
    snprintf(job, sizeof job, "%s.job", base);
    argv[0] = T.cfg.ftpoc;
    if (strcmp(op, "preview") == 0) {
        /* preview + detection, every whole frame planned at 4MP (no
         * 3-point focus check; ~40 s); the page re-plans sizes */
        if (!film || (strcmp(film, "neg") && strcmp(film, "slide"))) {
            snprintf(err, errn, "film must be neg or slide");
            return -1;
        }
        argv[1] = "frames";
        argv[2] = "--film";
        argv[3] = film;
        argv[4] = "--share";
        argv[5] = "2450";
        argv[6] = "--progress";
        argv[7] = "-o";
        argv[8] = base;
        argv[9] = NULL;
    } else if (strcmp(op, "scan") == 0) {
        argv[1] = "scan";
        argv[2] = "--job";
        argv[3] = job;
        argv[4] = "--progress";
        argv[5] = NULL;
    } else {
        snprintf(err, errn, "unknown op '%s'", op);
        return -1;
    }

    pthread_mutex_lock(&T.mu);
    if (T.running) {
        snprintf(err, errn, "scanner busy: %s %s (task %d)", T.op, T.strip,
                 T.id);
        pthread_mutex_unlock(&T.mu);
        return 1;
    }
    if (proc_spawn(&T.p, argv, ".", err, errn)) {
        pthread_mutex_unlock(&T.mu);
        return -1;
    }
    T.id++;
    T.running = 1;
    T.stopped = 0;
    T.exit_code = -1;
    T.nlines = 0;
    T.npart = 0;
    snprintf(T.op, sizeof T.op, "%s", op);
    snprintf(T.strip, sizeof T.strip, "%s", strip);
    /* line 1: the command, to copy to a terminal */
    for (i = 0; argv[i] && k + 2 < sizeof cmd; i++)
        k += (size_t)snprintf(cmd + k, sizeof cmd - k, "%s%s", i ? " " : "",
                              argv[i]);
    add_line(cmd, strlen(cmd));
    *id = T.id;
    if (pthread_create(&th, NULL, reader, NULL)) {
        /* cannot happen in practice; the child runs on unread */
        snprintf(err, errn, "cannot start the reader thread");
        pthread_mutex_unlock(&T.mu);
        return -1;
    }
    pthread_detach(th);
    pthread_mutex_unlock(&T.mu);
    return 0;
}

int task_stop(int id)
{
    int rc = -1;
    pthread_mutex_lock(&T.mu);
    if (id != T.id || !T.id)
        rc = -2;
    else if (T.running && proc_stop(&T.p) == 0) {
        T.stopped = 1;
        rc = 0;
    }
    pthread_mutex_unlock(&T.mu);
    return rc;
}

int task_json(int id, char *out, size_t n)
{
    char op[40], strip[200];
    pthread_mutex_lock(&T.mu);
    if (!T.id || (id && id != T.id)) {
        pthread_mutex_unlock(&T.mu);
        snprintf(out, n, "null");
        return -1;
    }
    json_esc(op, sizeof op, T.op);
    json_esc(strip, sizeof strip, T.strip);
    snprintf(out, n, "{\"id\":%d,\"op\":%s,\"strip\":%s,\"state\":\"%s\","
             "\"exit\":%d,\"stopped\":%s,\"lines\":%ld}", T.id, op, strip,
             T.running ? "running" : "done", T.exit_code,
             T.stopped ? "true" : "false", T.nlines);
    pthread_mutex_unlock(&T.mu);
    return 0;
}

long task_events(int id, long *since, char *out, size_t n, int wait_s,
                 int *ended)
{
    struct timespec ts;
    size_t k = 0;
    long i;
    pthread_mutex_lock(&T.mu);
    if (!T.id || id != T.id) {
        pthread_mutex_unlock(&T.mu);
        return -1;
    }
    ts.tv_sec = time(NULL) + wait_s;
    ts.tv_nsec = 0;
    while (*since >= T.nlines && T.running && id == T.id)
        if (pthread_cond_timedwait(&T.cv, &T.mu, &ts))
            break;
    if (id != T.id) {               /* a new task started meanwhile */
        pthread_mutex_unlock(&T.mu);
        return -1;
    }
    if (*since < T.nlines - RING)
        *since = T.nlines - RING;
    if (*since < 0)
        *since = 0;
    for (i = *since + 1; i <= T.nlines; i++) {
        int m = snprintf(out + k, n - k, "id: %ld\ndata: %s\n\n", i,
                         T.lines[(i - 1) % RING]);
        if (m < 0 || (size_t)m >= n - k)
            break;
        k += (size_t)m;
        *since = i;
    }
    if (k < n)
        out[k] = 0;
    *ended = !T.running && *since >= T.nlines;
    pthread_mutex_unlock(&T.mu);
    return (long)k;
}
