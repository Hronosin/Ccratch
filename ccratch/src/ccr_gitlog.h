/*
 * ccr_gitlog - git's history and what goes with it: the graph of the
 * branches, a commit's changes (and revert, cherry-pick, a branch from
 * it), who last changed each line (blame), the stash, and merging with
 * its conflicts resolved file by file.
 *
 * Every function runs git and waits (ccr_git_run): these are quick.
 */
#ifndef CCR_GITLOG_H
#define CCR_GITLOG_H

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------ graph */

#define CCR_GRAPH_EDGES 12
/* The most co-authors kept for a commit (a long list: the first ones). */
#define CCR_GIT_COAUTHORS 6

/* A line of the graph between this row and the next: from lane `from`
 * (at this row) to lane `to` (at the next). `merge_in`: a lane that
 * ends at this row's commit, drawn from its lane at the row's top to the
 * commit. */
typedef struct ccr_graph_edge {
    int8_t from, to;
    bool   merge_in;
} ccr_graph_edge;

typedef struct ccr_graph_row {
    char  hash[41];
    char  short_hash[16];
    char  author[64];
    char  when[48];
    char  subject[200];
    char  refs[160];        /* "HEAD -> main, origin/main, tag: v1" */
    char  coauthors[CCR_GIT_COAUTHORS][64];   /* who made it with the author, by name */
    int   coauthor_count;
    int   parents;
    int   lane;             /* the commit's column */
    int   edge_count;
    ccr_graph_edge edges[CCR_GRAPH_EDGES];
    bool  head;             /* it is where HEAD is */
} ccr_graph_row;

/* The last `cap` commits of every branch, newest first, with their lanes.
 * How many. */
int  ccr_git_graph(const char *root, ccr_graph_row *out, int cap);
/* The lanes, for rows already read (hash and parents in `parents_of`,
 * space separated): for the tests. */
void ccr_git_graph_lanes(ccr_graph_row *rows, const char (*parents_of)[400], int n);

/* --------------------------------------------------------- commits */

/* `git show` of a commit: its message and its diff (malloc'd). */
char *ccr_git_show(const char *root, const char *hash);
bool ccr_git_revert(const char *root, const char *hash, char *err, int cap);
bool ccr_git_cherry_pick(const char *root, const char *hash, char *err, int cap);
bool ccr_git_branch_from(const char *root, const char *name, const char *hash, char *err, int cap);

/* ---------------------------------------------------- co-authors
 *
 * A commit several people made together names the others in its
 * message, one "Co-authored-by: Name <mail>" line each, at its end (as
 * GitHub and GitLab read it).
 */

/* The names a message's "Co-authored-by:" lines give (any case; lines
 * end with a newline or \x1f), without `author` (NULL: nobody) and
 * without repeats. How many, up to `cap`. */
int ccr_git_coauthors(const char *message, const char *author, char out[][64], int cap);
/* The message with a "Co-authored-by: ..." line for each of `people`
 * ("Name <mail>") after a blank line (malloc'd). Someone it names
 * already is not named again. */
char *ccr_git_with_coauthors(const char *message, const char (*people)[160], int n);
/* Who worked on the repository: the authors and co-authors of its last
 * 500 commits, as "Name <mail>", the most frequent first, without the
 * one who commits here (user.email). How many, up to `cap`. */
int ccr_git_people(const char *root, char out[][160], int cap);

/* ------------------------------------------------------------ blame */

typedef struct ccr_blame_commit {
    char    hash[41];
    char    author[64];
    int64_t time;           /* seconds since 1970 */
    char    summary[160];
    char    coauthors[CCR_GIT_COAUTHORS][64];
    int     coauthor_count;
    bool    uncommitted;    /* the lines are not committed yet */
} ccr_blame_commit;

typedef struct ccr_blame {
    ccr_blame_commit *commits;
    int               commit_count;
    int              *line_commit;   /* per line (from 0): an index in commits */
    int               line_count;
} ccr_blame;

/* Who last changed every line of `path` (the file as it is on disk). */
bool ccr_git_blame(const char *path, ccr_blame *out);
void ccr_git_blame_free(ccr_blame *b);
/* "2 hours ago", "3 days ago" in the interface language. */
void ccr_git_ago(int64_t when, char *out, int cap);
/* "18.10.2026 11:59", local time. */
void ccr_git_date(int64_t when, char *out, int cap);

/* ------------------------------------------------------------ stash */

typedef struct ccr_stash_entry {
    char what[200];         /* "On main: tried a thing" */
} ccr_stash_entry;

bool ccr_git_stash_push(const char *root, const char *message, char *err, int cap);
int  ccr_git_stash_list(const char *root, ccr_stash_entry *out, int cap);
/* Back into the files: `pop` also takes it off the list. */
bool ccr_git_stash_apply(const char *root, int index, bool pop, char *err, int cap);
bool ccr_git_stash_drop(const char *root, int index, char *err, int cap);

/* ------------------------------------------------------------ merge */

/* Merges `branch` into the current one. True if it went in (merged, or
 * fast-forwarded); false with `conflicts` set if files need resolving
 * (the merge waits for that), or false with git's words in `err`. */
bool ccr_git_merge(const char *root, const char *branch, bool *conflicts, char *err, int cap);
/* What waits for conflicts to be resolved and a commit: a merge, a
 * revert, a cherry-pick (or nothing). */
typedef enum ccr_git_op {
    CCR_GIT_OP_NONE,
    CCR_GIT_OP_MERGE,
    CCR_GIT_OP_REVERT,
    CCR_GIT_OP_PICK
} ccr_git_op;

ccr_git_op ccr_git_pending(const char *root);
/* Is a merge (or a revert, a cherry-pick) waiting? */
bool ccr_git_merging(const char *root);
/* Gives up whatever waits: the files as before it. */
bool ccr_git_merge_abort(const char *root, char *err, int cap);

typedef enum ccr_resolve {
    CCR_RESOLVE_OURS,       /* this branch's side */
    CCR_RESOLVE_THEIRS,     /* the merged branch's */
    CCR_RESOLVE_BOTH        /* ours, then theirs, in every conflict */
} ccr_resolve;

/* Resolves every conflict of `path` (from the root) one way, and stages it. */
bool ccr_git_resolve(const char *root, const char *path, ccr_resolve how, char *err, int cap);
/* The text with every <<<<<<< ======= >>>>>>> region resolved `how`
 * (malloc'd): for the tests and for BOTH. */
char *ccr_git_resolve_text(const char *text, ccr_resolve how);
/* Commits what waited (the merge, the revert...) with git's own message. */
bool ccr_git_merge_commit(const char *root, char *err, int cap);

#endif /* CCR_GITLOG_H */
