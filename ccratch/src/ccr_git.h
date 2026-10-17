/*
 * ccr_git - the project's git, as VS Code's Source Control shows it:
 * what changed, staged or not; the diff of a file; stage, unstage,
 * discard, commit; branches; pull, push, fetch; the history; and which
 * lines of a file changed since the last commit, for marks on blocks.
 *
 * Quick commands (status, add, commit...) run and are waited for; the
 * ones that talk to a server (pull, push, fetch) run in the background
 * and are polled, so the window never hangs on the network. git is never
 * left asking for a password in a terminal nobody sees
 * (GIT_TERMINAL_PROMPT=0): it says no, and the panel says why.
 */
#ifndef CCR_GIT_H
#define CCR_GIT_H

#include "ccr_proc.h"

#include <stdbool.h>
#include <stdint.h>

#define CCR_GIT_PATH 1024

typedef struct ccr_git_file {
    char path[CCR_GIT_PATH];   /* from the repository's root */
    char orig[CCR_GIT_PATH];   /* renamed: what it was */
    char index;                /* staged: 'M' 'A' 'D' 'R' 'C', '.' none */
    char work;                 /* in the folder: 'M' 'D', '?' untracked, '.' none */
    bool conflict;             /* a merge left it unresolved */
} ccr_git_file;

typedef struct ccr_git_status {
    bool          repo;           /* the folder is in a repository at all */
    char          root[CCR_GIT_PATH];
    char          branch[256];    /* "(detached)" when not on one */
    char          upstream[256];  /* "origin/main", or "" */
    int           ahead, behind;
    bool          no_commits;     /* a new repository: nothing committed yet */
    ccr_git_file *files;
    int           count;
    int           staged, changed, untracked, conflicts;
} ccr_git_status;

/* Is git installed? */
bool ccr_git_available(void);

/* Reads the status of the repository `dir` is in. False if git could
 * not run; out->repo false if `dir` is in none. */
bool ccr_git_status_read(const char *dir, ccr_git_status *out);
void ccr_git_status_free(ccr_git_status *s);

/* The output of `git ARGS...` run in `root` (malloc'd), and its exit
 * code. NULL if it could not run. Waits (10 s at most). */
char *ccr_git_run(const char *root, const char *const *args, int *exit_code);

/* What `files` (paths from the root) changed: their diff (staged: what
 * the next commit takes; else what is not staged yet; untracked: all of
 * it, added). malloc'd unified diff text. */
char *ccr_git_diff(const char *root, const char *path, bool staged, bool untracked);

/* Quick actions: false with git's words in `err`. */
bool ccr_git_stage(const char *root, const char *path, char *err, int cap);
bool ccr_git_unstage(const char *root, const char *path, bool no_commits, char *err, int cap);
/* Throws away what changed in `path` (an untracked file is deleted). */
bool ccr_git_discard(const char *root, const char *path, bool untracked, char *err, int cap);
bool ccr_git_stage_all(const char *root, char *err, int cap);
bool ccr_git_unstage_all(const char *root, bool no_commits, char *err, int cap);
bool ccr_git_commit(const char *root, const char *message, bool amend, bool all, char *err, int cap);
bool ccr_git_init(const char *dir, char *err, int cap);
/* Ccratch's own backups (FILE~) are no change to show or commit: `*~`
 * into the repository's local exclude list (.git/info/exclude, never
 * committed), once. */
void ccr_git_exclude_backups(const char *root);

/* Branches: the local ones (the current one first). */
int  ccr_git_branches(const char *root, char names[][256], int cap);
bool ccr_git_switch(const char *root, const char *branch, bool create, char *err, int cap);

/* The last `cap` commits. */
typedef struct ccr_git_commit_info {
    char hash[16];
    char author[64];
    char when[48];       /* "2 hours ago" */
    char subject[200];
} ccr_git_commit_info;
int  ccr_git_log(const char *root, ccr_git_commit_info *out, int cap);

/* Lines of a file that changed since the last commit (the file as it is
 * on disk), for marks: added, changed, and lines after which some were
 * deleted (`after`: 0 = before the first). */
typedef enum ccr_git_line_kind {
    CCR_GIT_ADDED,
    CCR_GIT_CHANGED,
    CCR_GIT_DELETED
} ccr_git_line_kind;

typedef struct ccr_git_hunk {
    ccr_git_line_kind kind;
    int first, count;    /* lines first..first+count-1; DELETED: after line `first`, count 0 */
} ccr_git_hunk;

/* Its hunks against HEAD (a new, untracked file: all of it added).
 * How many; up to `cap` in `out`. -1 if it is not in a repository. */
int  ccr_git_file_hunks(const char *path, ccr_git_hunk *out, int cap);
/* The same, read from `git diff -U0` text (for the tests). */
int  ccr_git_parse_hunks(const char *diff, ccr_git_hunk *out, int cap);

/* --------------------------------------------- talking to the server */

typedef enum ccr_git_net {
    CCR_GIT_NET_NONE,
    CCR_GIT_NET_PULL,
    CCR_GIT_NET_PUSH,
    CCR_GIT_NET_FETCH
} ccr_git_net;

typedef struct ccr_git_job {
    ccr_git_net what;
    ccr_proc    proc;
    bool        done, ok;
    char        said[1200];   /* git's last lines: what went wrong, or right */
} ccr_git_job;

/* Starts a pull (rebase off: a plain merge, fast-forward when it can),
 * push (its upstream set the first time), or fetch. */
bool ccr_git_net_start(ccr_git_job *j, const char *root, ccr_git_net what, bool set_upstream,
                       char *err, int cap);
/* True while it runs. */
bool ccr_git_net_poll(ccr_git_job *j);
void ccr_git_net_stop(ccr_git_job *j);

#endif /* CCR_GIT_H */
