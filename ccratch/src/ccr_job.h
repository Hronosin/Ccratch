/*
 * ccr_job - the Build button: CMake configures and builds the project in
 * the background, the program runs after, and what the compiler, the
 * linker and CMake say is read for errors that point at code.
 *
 * The build goes into PROJECT/.ccratch/build, out of the way of any build
 * folder of the project's own (Debug, compile_commands.json for tools).
 * Steps: configure (only when that folder has no build yet), build, and,
 * if asked, run.
 *
 * Knows nothing about the window: the app polls it every frame.
 */
#ifndef CCR_JOB_H
#define CCR_JOB_H

#include "ccr_proc.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum ccr_job_step {
    CCR_JOB_IDLE,
    CCR_JOB_CONFIGURE,
    CCR_JOB_BUILD,
    CCR_JOB_RUN
} ccr_job_step;

typedef enum ccr_diag_kind {
    CCR_BD_ERROR,
    CCR_BD_WARNING,
    CCR_BD_NOTE
} ccr_diag_kind;

/* Something the tools said about a place in the code. */
typedef struct ccr_build_diag {
    ccr_diag_kind kind;
    char     file[1024];     /* as written; ccr_job makes it absolute */
    uint32_t line, col;      /* 0: not known */
    char     message[400];
    char     symbol[128];    /* "undefined reference to `foo'": foo */
    bool     from_cmake;     /* CMake's own (CMakeLists.txt:12) */
    int      log_line;       /* the line of the log it came from */
} ccr_build_diag;

/* How a log line is drawn. */
typedef enum ccr_log_kind {
    CCR_LOG_TEXT,
    CCR_LOG_COMMAND,    /* what Ccratch ran */
    CCR_LOG_ERROR,
    CCR_LOG_WARNING,
    CCR_LOG_NOTE,
    CCR_LOG_QUIET,      /* progress, the code under a message, ^~~~ */
    CCR_LOG_GOOD,       /* built, it ran */
    CCR_LOG_BAD         /* the step failed */
} ccr_log_kind;

typedef struct ccr_log_line {
    char        *text;
    ccr_log_kind kind;
    int          diag;      /* -1, or the diagnostic it is */
} ccr_log_line;

typedef enum ccr_job_result {
    CCR_JOB_RUNNING,
    CCR_JOB_BUILT,          /* built (and maybe ran) */
    CCR_JOB_FAILED,         /* configure or build said no */
    CCR_JOB_STOPPED,        /* the Stop button */
    CCR_JOB_NO_CMAKE        /* cmake is not installed */
} ccr_job_result;

/* How the program is built (Settings > Building). Each has its own build
 * folder in .ccratch, so switching does not throw away the other. */
typedef enum ccr_build_mode {
    CCR_BUILD_DEBUG,      /* -g, no optimisation: .ccratch/build */
    CCR_BUILD_RELEASE,    /* -O2: .ccratch/build-release */
    CCR_BUILD_SANITIZE,   /* -g with AddressSanitizer and UBSan: .ccratch/build-sanitize */
    CCR_BUILD_MODE_COUNT
} ccr_build_mode;

/* What a sanitizer is in the middle of saying (see ccr_san_parse_line). */
typedef struct ccr_san_state {
    char what[200];          /* "heap-buffer-overflow", "Direct leak of 16 byte(s)..." */
    bool want_place;         /* the first frame in the project is where */
} ccr_san_state;

typedef struct ccr_job {
    ccr_job_step   step;
    ccr_build_mode mode;             /* set before ccr_job_build */
    ccr_job_result result;
    ccr_proc       proc;
    bool           run_after;
    bool           ran;              /* the program was started */
    int            run_exit;         /* and finished with this */
    char           project[1024];
    char           build_dir[1100];
    char           exe[1300];        /* the program built, once found */
    double         started, ended;   /* seconds, ccr_job_now() */

    ccr_log_line  *lines;
    int            line_count, line_cap;
    int            dropped;          /* lines dropped from the top (a long log) */
    ccr_build_diag *diags;
    int            diag_count, diag_cap;
    int            errors, warnings;
    int            cmake_open;       /* a CMake message still being read, or -1 */
    bool           cmake_said;       /* and its first paragraph is over */
    ccr_san_state  san;              /* the program's output, under Bug hunt */
    uint32_t       generation;       /* bumps when diags change */
} ccr_job;

double ccr_job_now(void);

void ccr_job_init(ccr_job *j);
void ccr_job_free(ccr_job *j);

/* Configures if needed and builds `project` (which has a CMakeLists.txt);
 * then runs the program if `run_after`. False if it could not start. */
bool ccr_job_build(ccr_job *j, const char *project, bool run_after);
/* Runs the program the last build made, from the project's folder. */
bool ccr_job_run(ccr_job *j);
/* Every frame: reads output, moves to the next step. */
void ccr_job_poll(ccr_job *j);
void ccr_job_stop(ccr_job *j);
bool ccr_job_busy(const ccr_job *j);

/* One line of compiler / linker / CMake output: is it about a place in
 * the code? Pure, for the tests. */
bool ccr_build_parse_line(const char *line, ccr_build_diag *out);

/* One line of a program's own output, built with sanitizers: AddressSanitizer
 * and LeakSanitizer say what, then a stack, whose first frame in `project`
 * is the place; UBSan says "file:line:col: runtime error: ...". True when
 * the line puts the report on a place: *out is it. Pure, for the tests. */
bool ccr_san_parse_line(ccr_san_state *st, const char *line, const char *project, ccr_build_diag *out);

/* The newest program (an ELF executable) under `build_dir`. */
bool ccr_build_find_exe(const char *build_dir, char *out, int cap);

#endif /* CCR_JOB_H */
