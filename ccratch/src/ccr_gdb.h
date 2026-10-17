/*
 * ccr_gdb - the program built, run under gdb: stopped at breakpoints,
 * stepped a line at a time, its variables read at every stop.
 *
 * gdb is spoken to in its machine interface (ccr_mi), over a pipe; the
 * program gets a terminal of its own (a pty), so what it prints arrives
 * line by line, as it would in a terminal, not when a pipe's buffer
 * fills up.
 *
 * Knows nothing about blocks: breakpoints are file:line, and so is where
 * the program stopped. The app turns those into blocks and back.
 */
#ifndef CCR_GDB_H
#define CCR_GDB_H

#include "ccr_proc.h"

#include <stdbool.h>
#include <stdint.h>

#define CCR_GDB_MAX_BPS    256
#define CCR_GDB_MAX_FRAMES 64
#define CCR_GDB_MAX_VARS   128
#define CCR_GDB_OUT_LINES  2000

typedef enum ccr_gdb_state {
    CCR_GDB_OFF,        /* not started, or gone */
    CCR_GDB_STARTING,   /* gdb is reading the program */
    CCR_GDB_RUNNING,    /* the program runs */
    CCR_GDB_STOPPED,    /* stopped: frames and variables read */
    CCR_GDB_EXITED      /* the program finished (gdb is still there) */
} ccr_gdb_state;

typedef struct ccr_gdb_frame {
    char func[128];
    char file[1024];    /* the whole path; empty: no source (libc) */
    int  line;
} ccr_gdb_frame;

typedef struct ccr_gdb_var {
    char name[64];
    char value[256];
    bool changed;       /* not what it was at the stop before */
    bool arg;           /* a parameter of the function */
} ccr_gdb_var;

typedef struct ccr_gdb_bp {
    char file[1024];
    int  line;
    int  number;        /* gdb's, once set; 0: not (yet) */
    int  hits;
    bool pending;       /* gdb could not place it (no code there) */
    bool sent;          /* asked of gdb this run */
} ccr_gdb_bp;

typedef struct ccr_gdb_ask {
    long token;
    int  what, arg;
} ccr_gdb_ask;

typedef enum ccr_gdb_out_kind {
    CCR_GDB_OUT_PROGRAM,   /* what the program printed */
    CCR_GDB_OUT_INFO,      /* Ccratch's own notes: stopped at..., exited with... */
    CCR_GDB_OUT_ERROR      /* gdb said no */
} ccr_gdb_out_kind;

typedef struct ccr_gdb_out {
    char            *text;
    ccr_gdb_out_kind kind;
} ccr_gdb_out;

typedef struct ccr_gdb {
    ccr_gdb_state state;
    ccr_proc      proc;
    int           pty;               /* the program's terminal (master), -1 */
    char          pty_partial[1024];
    int           pty_partial_len;
    char          exe[1300];
    long          token;             /* the next command's */

    char          reason[64];        /* why it stopped: breakpoint-hit, end-stepping-range... */
    char          signal[64];        /* SIGSEGV..., when a signal stopped it */
    int           exit_code;         /* when EXITED */
    int           stopped_bp;        /* the breakpoint it stopped at (index), or -1 */

    ccr_gdb_frame frames[CCR_GDB_MAX_FRAMES];
    int           frame_count;
    int           frame;             /* the one whose variables are shown */
    ccr_gdb_var   vars[CCR_GDB_MAX_VARS];
    int           var_count;

    ccr_gdb_bp    bps[CCR_GDB_MAX_BPS];
    int           bp_count;

    ccr_gdb_out   out[CCR_GDB_OUT_LINES];
    int           out_count;
    uint32_t      generation;        /* bumps whenever any of the above changes */

    /* what the commands in flight were for, by token */
    ccr_gdb_ask   pending[64];
    int           pending_count;
    char          error[256];        /* gdb's last "^error,msg=" */
    char          finished_from[140];   /* the function Step out left */
} ccr_gdb;

void ccr_gdb_init(ccr_gdb *g);
/* Stops gdb and the program; frees the output. */
void ccr_gdb_free(ccr_gdb *g);

/* Is gdb installed? */
bool ccr_gdb_available(void);

/* Starts `exe` under gdb in folder `cwd`, with the breakpoints already
 * set by ccr_gdb_set_break (kept from one run to the next). */
bool ccr_gdb_start(ccr_gdb *g, const char *exe, const char *cwd, char *err, int err_cap);

/* Every frame: reads what gdb and the program said. */
void ccr_gdb_poll(ccr_gdb *g);

bool ccr_gdb_active(const ccr_gdb *g);   /* STARTING, RUNNING or STOPPED */

void ccr_gdb_continue(ccr_gdb *g);
void ccr_gdb_next(ccr_gdb *g);      /* a line, over calls */
void ccr_gdb_step(ccr_gdb *g);      /* a line, into calls */
void ccr_gdb_finish(ccr_gdb *g);    /* out of this function */
void ccr_gdb_pause(ccr_gdb *g);     /* stop it where it is */
void ccr_gdb_stop(ccr_gdb *g);      /* end it all */

/* The variables of frame `i` (0: where it stopped). */
void ccr_gdb_select_frame(ccr_gdb *g, int i);

/* A breakpoint at file:line on or off; set in gdb at once if it runs. */
void ccr_gdb_set_break(ccr_gdb *g, const char *file, int line, bool on);
/* Is there one at file:line? Its index, or -1. */
int  ccr_gdb_find_break(const ccr_gdb *g, const char *file, int line);
/* All of `file`'s go (its lines changed: the app sets them again). */
void ccr_gdb_clear_breaks(ccr_gdb *g, const char *file);

/* For the tests: feed gdb's (or the program's) output by hand. */
void ccr_gdb_feed(ccr_gdb *g, const char *line);

#endif /* CCR_GDB_H */
