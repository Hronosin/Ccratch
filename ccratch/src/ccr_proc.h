/*
 * ccr_proc - a program run in the background, its output read line by
 * line without ever blocking the window.
 *
 * stdout and stderr come through one pipe, in the order they were
 * written. The program gets a process group of its own, so stopping it
 * stops whatever it started too (a compiler under ninja under cmake).
 */
#ifndef CCR_PROC_H
#define CCR_PROC_H

#include <stdbool.h>
#include <stddef.h>

typedef struct ccr_proc {
    int    pid;          /* 0: nothing running */
    int    fd;           /* the read end, -1 once closed */
    bool   finished;
    int    exit_code;    /* when finished: its code, or 128 + the signal */
    double stop_at;      /* asked to stop then (SIGTERM); SIGKILL a second later */
    char   partial[4096];
    size_t partial_len;
} ccr_proc;

typedef void (*ccr_proc_line_fn)(void *ctx, const char *line);

/* Starts argv[0] (looked up in PATH) in folder `cwd`. `c_locale`: with
 * LC_ALL=C, so compilers speak the English the build panel reads. */
bool ccr_proc_start(ccr_proc *p, const char *const *argv, const char *cwd, bool c_locale,
                    char *err, size_t err_cap);

/* Reads what is there, calls `fn` for each whole line. True while it runs;
 * false once it has finished and everything it wrote has been read. */
bool ccr_proc_poll(ccr_proc *p, ccr_proc_line_fn fn, void *ctx);

/* SIGTERM to its group now; ccr_proc_poll sends SIGKILL if it lingers. */
void ccr_proc_stop(ccr_proc *p);

/* Stops it (if running) and waits: for quitting. */
void ccr_proc_kill_wait(ccr_proc *p);

/* Is `name` a program in PATH? */
bool ccr_proc_in_path(const char *name);

#endif /* CCR_PROC_H */
