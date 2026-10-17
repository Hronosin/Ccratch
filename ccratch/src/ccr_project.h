/*
 * ccr_project - the project's own functions, as blocks for every file.
 *
 * Every .c and .h of the project is read with clang (function bodies
 * skipped), for what each file itself declares: its functions that are
 * not static, and its enums. A function a header declares is offered
 * with that header, so a block dropped into another file brings its
 * #include along; one only a .c file has is offered as it is.
 *
 * Reading takes a moment on a big project, so it runs in a thread of its
 * own; files that did not change since the last time are not read again.
 */
#ifndef CCR_PROJECT_H
#define CCR_PROJECT_H

#include "ccr_lib.h"

#include <stdbool.h>

typedef struct ccr_project_enum {
    char  name[64];
    int   count;
    char  (*values)[64];
} ccr_project_enum;

typedef struct ccr_project_result {
    ccr_lib_block   *blocks;
    int              count;
    ccr_project_enum *enums;
    int              enum_count;
    int              files;        /* how many were read (or known) */
} ccr_project_result;

/* Reads the project in `root` (blocking): for the tests and the thread. */
bool ccr_project_read(const char *root, const char *const *clang_args, int nargs, ccr_project_result *out);
void ccr_project_result_free(ccr_project_result *r);

/* In the background: started (a scan already going is let finish and
 * started again after), and polled every frame. */
void ccr_project_scan_start(const char *root, const char *const *clang_args, int nargs);
/* True once, when a scan has finished: `out` is its result (owned by the
 * caller now). */
bool ccr_project_scan_take(ccr_project_result *out);

/* Waits for a scan going on, and lets go of everything (quitting). */
void ccr_project_shutdown(void);

/* Puts a result into the block registry (ccr_lib_set_project and the
 * enums), the pack named `name`. */
void ccr_project_apply(const ccr_project_result *r, const char *name);

#endif /* CCR_PROJECT_H */
