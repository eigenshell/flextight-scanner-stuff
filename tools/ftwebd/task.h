/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ftpoc tasks: the client names an op, ftwebd builds the argv (never the
 * client). One task at a time (the scanner has one holder). Its output is
 * a ring of numbered lines (1..): line 1 is the built command, the last
 * is "ftwebd exit <code>". Events resume with since = the last number
 * seen. */
#ifndef FTWEBD_TASK_H
#define FTWEBD_TASK_H
#include <stddef.h>

typedef struct {
    const char *ftpoc;          /* absolute path */
    const char *strips;         /* strip folders, relative to the cwd */
} task_cfg;

void task_init(const task_cfg *cfg);
/* op "preview" (film "neg" or "slide") or "scan" on strip s (a checked
 * name). 0 with *id set; 1 busy (err names the running task); -1 bad
 * request or spawn failure (err) */
int task_start(const char *op, const char *strip, const char *film, int *id,
               char *err, size_t errn);
/* 0 delivered, -1 not running or not delivered, -2 no such task */
int task_stop(int id);
/* {"id":..,"op":..,"strip":..,"state":"running|done","exit":..,
 * "lines":..} for id, or the latest task when id is 0. 0, or -1 if there
 * is none (out: "null") */
int task_json(int id, char *out, size_t n);
/* SSE text for the lines after `since` ("id: k\ndata: line\n\n" each),
 * waiting up to wait_s for one. Returns the bytes written (0: none yet),
 * -1 no such task. *since moves past what was written; *ended = the task
 * has exited and every line was written. */
long task_events(int id, long *since, char *out, size_t n, int wait_s,
                 int *ended);

#endif
