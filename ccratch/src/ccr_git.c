#define _DEFAULT_SOURCE   /* setenv, nanosleep */
#include "ccr_git.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

bool ccr_git_available(void)
{
    return ccr_proc_in_path("git");
}

/* git must never sit waiting for a password nobody can type. */
static void quiet_git(void)
{
    static bool done;
    if (done)
        return;
    done = true;
    setenv("GIT_TERMINAL_PROMPT", "0", 1);
    setenv("GIT_OPTIONAL_LOCKS", "0", 1);   /* status must not fight an editor's git for the index */
    setenv("GIT_PAGER", "cat", 1);
}

/* ------------------------------------------------------------ running */

typedef struct buf {
    char  *s;
    size_t len, cap;
} buf;

static void put(buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *g = realloc(b->s, cap);
        if (!g)
            return;
        b->s = g;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void collect(void *ctx, const char *line)
{
    buf *b = ctx;
    put(b, line, strlen(line));
    put(b, "\n", 1);
}

char *ccr_git_run(const char *root, const char *const *args, int *exit_code)
{
    quiet_git();
    const char *argv[48];
    int n = 0;
    argv[n++] = "git";
    argv[n++] = "-c";
    argv[n++] = "core.quotePath=false";
    argv[n++] = "-c";
    argv[n++] = "color.ui=false";
    for (int i = 0; args[i] && n < 47; ++i)
        argv[n++] = args[i];
    argv[n] = NULL;
    ccr_proc p;
    char err[256];
    if (exit_code)
        *exit_code = -1;
    if (!ccr_proc_start(&p, argv, root, true, err, sizeof err))
        return NULL;
    buf b = { 0 };
    put(&b, "", 0);
    for (int waited = 0; ccr_proc_poll(&p, collect, &b); ++waited) {
        if (waited > 10000) {   /* 10 s: something is wrong */
            ccr_proc_kill_wait(&p);
            break;
        }
        struct timespec ts = { 0, 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (exit_code)
        *exit_code = p.exit_code;
    return b.s;
}

/* Runs, and says git's words in `err` when it fails. */
static bool run_ok(const char *root, const char *const *args, char *err, int cap)
{
    int code = 0;
    char *out = ccr_git_run(root, args, &code);
    if (cap)
        err[0] = '\0';
    if (!out) {
        snprintf(err, (size_t)cap, "git did not start");
        return false;
    }
    if (code != 0 && cap) {
        /* the last lines that say something: "error:", "fatal:", "hint:" aside */
        char *best = NULL;
        for (char *line = strtok(out, "\n"); line; line = strtok(NULL, "\n"))
            if (strncmp(line, "hint:", 5) != 0 && *line)
                if (!best || strncmp(line, "error:", 6) == 0 || strncmp(line, "fatal:", 6) == 0)
                    best = line;
        snprintf(err, (size_t)cap, "%s", best ? best : "git said no");
    }
    free(out);
    return code == 0;
}

/* ------------------------------------------------------------- status */

/* A path as porcelain prints it: plain, or "quoted with \\ escapes". */
static void unquote(const char *s, char *out, size_t cap)
{
    size_t k = 0;
    if (*s != '"') {
        snprintf(out, cap, "%s", s);
        return;
    }
    for (++s; *s && *s != '"' && k + 1 < cap; ++s) {
        char c = *s;
        if (c == '\\' && s[1]) {
            c = *++s;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c >= '0' && c <= '7') {
                int v = c - '0';
                for (int i = 0; i < 2 && s[1] >= '0' && s[1] <= '7'; ++i)
                    v = v * 8 + (*++s - '0');
                c = (char)v;
            }
        }
        out[k++] = c;
    }
    out[k] = '\0';
}

/* The rest of `line` after `n` fields separated by single spaces. */
static const char *after_fields(const char *line, int n)
{
    while (n-- > 0 && line) {
        line = strchr(line, ' ');
        if (line)
            ++line;
    }
    return line ? line : "";
}

static ccr_git_file *add_file(ccr_git_status *s)
{
    if ((s->count & 63) == 0) {
        ccr_git_file *v = realloc(s->files, (size_t)(s->count + 64) * sizeof *v);
        if (!v)
            return NULL;
        s->files = v;
    }
    ccr_git_file *f = &s->files[s->count++];
    memset(f, 0, sizeof *f);
    return f;
}

static bool toplevel(const char *dir, char *out, size_t cap)
{
    const char *args[] = { "rev-parse", "--show-toplevel", NULL };
    int code = 0;
    char *o = ccr_git_run(dir, args, &code);
    if (!o)
        return false;
    bool ok = code == 0 && o[0] == '/';
    if (ok) {
        size_t n = strcspn(o, "\n");
        snprintf(out, cap, "%.*s", (int)n, o);
    }
    free(o);
    return ok;
}

bool ccr_git_status_read(const char *dir, ccr_git_status *out)
{
    memset(out, 0, sizeof *out);
    struct stat st;
    if (!dir || stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    if (!toplevel(dir, out->root, sizeof out->root))
        return ccr_git_available();   /* git runs: just no repository here */
    out->repo = true;
    const char *args[] = { "status", "--porcelain=v2", "--branch", "--untracked-files=all", NULL };
    int code = 0;
    char *o = ccr_git_run(out->root, args, &code);
    if (!o || code != 0) {
        free(o);
        return false;
    }
    for (char *line = o, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        if (strncmp(line, "# branch.head ", 14) == 0) {
            snprintf(out->branch, sizeof out->branch, "%s", line + 14);
        } else if (strncmp(line, "# branch.oid ", 13) == 0) {
            out->no_commits = strcmp(line + 13, "(initial)") == 0;
        } else if (strncmp(line, "# branch.upstream ", 18) == 0) {
            snprintf(out->upstream, sizeof out->upstream, "%s", line + 18);
        } else if (strncmp(line, "# branch.ab ", 12) == 0) {
            sscanf(line + 12, "+%d -%d", &out->ahead, &out->behind);
        } else if ((line[0] == '1' || line[0] == '2' || line[0] == 'u') && line[1] == ' ') {
            ccr_git_file *f = add_file(out);
            if (!f)
                break;
            f->index = line[2];
            f->work = line[3];
            if (line[0] == 'u') {
                f->conflict = true;
                unquote(after_fields(line, 10), f->path, sizeof f->path);
            } else if (line[0] == '2') {
                char both[2 * CCR_GIT_PATH];
                snprintf(both, sizeof both, "%s", after_fields(line, 9));
                char *tab = strchr(both, '\t');
                if (tab) {
                    *tab = '\0';
                    unquote(tab + 1, f->orig, sizeof f->orig);
                }
                unquote(both, f->path, sizeof f->path);
            } else {
                unquote(after_fields(line, 8), f->path, sizeof f->path);
            }
        } else if (line[0] == '?' && line[1] == ' ') {
            ccr_git_file *f = add_file(out);
            if (!f)
                break;
            f->index = '.';
            f->work = '?';
            unquote(line + 2, f->path, sizeof f->path);
        }
    }
    free(o);
    for (int i = 0; i < out->count; ++i) {
        const ccr_git_file *f = &out->files[i];
        if (f->conflict)
            ++out->conflicts;
        else if (f->work == '?')
            ++out->untracked;
        else {
            out->staged += f->index != '.';
            out->changed += f->work != '.';
        }
    }
    return true;
}

void ccr_git_status_free(ccr_git_status *s)
{
    free(s->files);
    memset(s, 0, sizeof *s);
}

/* ---------------------------------------------------------- the diffs */

char *ccr_git_diff(const char *root, const char *path, bool staged, bool untracked)
{
    int code;
    if (untracked) {   /* all of it is new */
        const char *args[] = { "diff", "--no-index", "--no-ext-diff", "--", "/dev/null", path, NULL };
        return ccr_git_run(root, args, &code);   /* exit 1: they differ, as they do */
    }
    if (staged) {
        const char *args[] = { "diff", "--cached", "--no-ext-diff", "--", path, NULL };
        return ccr_git_run(root, args, &code);
    }
    const char *args[] = { "diff", "--no-ext-diff", "--", path, NULL };
    return ccr_git_run(root, args, &code);
}

/* "@@ -a,b +c,d @@": a hunk of -U0 */
int ccr_git_parse_hunks(const char *diff, ccr_git_hunk *out, int cap)
{
    int n = 0;
    for (const char *line = diff; line && *line;) {
        if (strncmp(line, "@@ -", 4) == 0) {
            long a = 0, b = 1, c = 0, d = 1;
            const char *p = line + 4;
            char *e;
            a = strtol(p, &e, 10);
            if (*e == ',')
                b = strtol(e + 1, &e, 10);
            if (strncmp(e, " +", 2) == 0) {
                c = strtol(e + 2, &e, 10);
                if (*e == ',')
                    d = strtol(e + 1, &e, 10);
                ccr_git_hunk h;
                if (d == 0)
                    h = (ccr_git_hunk){ CCR_GIT_DELETED, (int)c, 0 };
                else if (b == 0)
                    h = (ccr_git_hunk){ CCR_GIT_ADDED, (int)c, (int)d };
                else
                    h = (ccr_git_hunk){ CCR_GIT_CHANGED, (int)c, (int)d };
                if (n < cap)
                    out[n] = h;
                ++n;
            }
            (void)a;
        }
        line = strchr(line, '\n');
        if (line)
            ++line;
    }
    return n;
}

int ccr_git_file_hunks(const char *path, ccr_git_hunk *out, int cap)
{
    char dir[CCR_GIT_PATH], root[CCR_GIT_PATH];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = '\0';
    else
        snprintf(dir, sizeof dir, ".");
    if (!toplevel(dir, root, sizeof root))
        return -1;
    int code = 0;
    const char *known[] = { "ls-files", "--error-unmatch", "--", path, NULL };
    char *o = ccr_git_run(root, known, &code);
    free(o);
    const char *head[] = { "rev-parse", "--verify", "-q", "HEAD", NULL };
    int has_head = 0;
    o = ccr_git_run(root, head, &has_head);
    free(o);
    if (code != 0 || has_head != 0) {
        /* new to git (or git has nothing yet): every line is new, unless ignored */
        const char *ign[] = { "check-ignore", "-q", "--", path, NULL };
        int ignored = 1;
        o = ccr_git_run(root, ign, &ignored);
        free(o);
        if (ignored == 0)
            return 0;
        FILE *f = fopen(path, "rb");
        if (!f)
            return 0;
        int lines = 0, c, last = '\n';
        while ((c = fgetc(f)) != EOF) {
            lines += c == '\n';
            last = c;
        }
        fclose(f);
        lines += last != '\n';
        if (lines == 0)
            return 0;
        if (cap > 0)
            out[0] = (ccr_git_hunk){ CCR_GIT_ADDED, 1, lines };
        return 1;
    }
    const char *args[] = { "diff", "HEAD", "-U0", "--no-ext-diff", "--", path, NULL };
    o = ccr_git_run(root, args, &code);
    if (!o)
        return 0;
    const int n = ccr_git_parse_hunks(o, out, cap);
    free(o);
    return n;
}

/* ------------------------------------------------------ one hunk only */

/* Where hunk `index` of `diff` starts and ends; the header is [0, first). */
static bool hunk_range(const char *diff, int index, size_t *first, size_t *b, size_t *e)
{
    int seen = -1;
    *first = *b = *e = 0;
    bool any = false;
    for (const char *line = diff; line && *line;) {
        const char *next = strchr(line, '\n');
        next = next ? next + 1 : line + strlen(line);
        const size_t at = (size_t)(line - diff);
        if (strncmp(line, "@@", 2) == 0) {
            if (!any) {
                *first = at;
                any = true;
            }
            ++seen;
            if (seen == index)
                *b = at;
            else if (seen == index + 1) {
                *e = at;
                return true;
            }
        }
        line = next;
    }
    if (seen >= index && any) {
        *e = strlen(diff);
        return true;
    }
    return false;
}

int ccr_git_hunk_count(const char *diff)
{
    int n = 0;
    for (const char *line = diff; line && *line;) {
        n += strncmp(line, "@@", 2) == 0;
        line = strchr(line, '\n');
        if (line)
            ++line;
    }
    return n;
}

bool ccr_git_apply_hunk(const char *root, const char *diff, int index, bool unstage, char *err, int cap)
{
    size_t first, b, e;
    if (!diff || !hunk_range(diff, index, &first, &b, &e) || first == 0) {
        snprintf(err, (size_t)cap, "no such part");
        return false;
    }
    /* the file's header, then that one hunk: a patch git can apply alone */
    char path[] = "/tmp/ccratch_hunk_XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) {
        snprintf(err, (size_t)cap, "no temporary file");
        return false;
    }
    FILE *f = fdopen(fd, "w");
    bool ok = f && fwrite(diff, 1, first, f) == first && fwrite(diff + b, 1, e - b, f) == e - b;
    if (ok && e > b && diff[e - 1] != '\n')
        ok = fputc('\n', f) != EOF;
    if (f)
        ok = fclose(f) == 0 && ok;
    else
        close(fd);
    if (ok) {
        const char *args[] = { "apply", "--cached", "--whitespace=nowarn", unstage ? "--reverse" : path,
                               unstage ? path : NULL, NULL };
        ok = run_ok(root, args, err, cap);
    } else {
        snprintf(err, (size_t)cap, "could not write the patch");
    }
    remove(path);
    return ok;
}

/* ------------------------------------------------------------ actions */

bool ccr_git_stage(const char *root, const char *path, char *err, int cap)
{
    const char *args[] = { "add", "-A", "--", path, NULL };
    return run_ok(root, args, err, cap);
}

bool ccr_git_unstage(const char *root, const char *path, bool no_commits, char *err, int cap)
{
    if (no_commits) {   /* no HEAD to go back to: out of the index */
        const char *args[] = { "rm", "--cached", "-q", "-r", "--", path, NULL };
        return run_ok(root, args, err, cap);
    }
    const char *args[] = { "restore", "--staged", "--", path, NULL };
    return run_ok(root, args, err, cap);
}

bool ccr_git_discard(const char *root, const char *path, bool untracked, char *err, int cap)
{
    if (untracked) {
        const char *args[] = { "clean", "-f", "-q", "--", path, NULL };
        return run_ok(root, args, err, cap);
    }
    const char *args[] = { "restore", "--worktree", "--", path, NULL };
    return run_ok(root, args, err, cap);
}

bool ccr_git_stage_all(const char *root, char *err, int cap)
{
    const char *args[] = { "add", "-A", NULL };
    return run_ok(root, args, err, cap);
}

bool ccr_git_unstage_all(const char *root, bool no_commits, char *err, int cap)
{
    if (no_commits) {
        const char *args[] = { "rm", "--cached", "-q", "-r", ".", NULL };
        return run_ok(root, args, err, cap);
    }
    const char *args[] = { "reset", "-q", NULL };
    return run_ok(root, args, err, cap);
}

bool ccr_git_commit(const char *root, const char *message, bool amend, bool all, char *err, int cap)
{
    const char *args[8];
    int n = 0;
    args[n++] = "commit";
    args[n++] = "-q";
    if (all)
        args[n++] = "-a";
    if (amend)
        args[n++] = "--amend";
    args[n++] = "-m";
    args[n++] = message;
    args[n] = NULL;
    return run_ok(root, args, err, cap);
}

bool ccr_git_init(const char *dir, char *err, int cap)
{
    const char *args[] = { "init", "-q", NULL };
    return run_ok(dir, args, err, cap);
}

void ccr_git_exclude_backups(const char *root)
{
    const char *args[] = { "rev-parse", "--git-path", "info/exclude", NULL };
    int code = 0;
    char *o = ccr_git_run(root, args, &code);
    if (!o || code != 0) {
        free(o);
        return;
    }
    o[strcspn(o, "\n")] = '\0';
    char path[2 * CCR_GIT_PATH];
    if (o[0] == '/')
        snprintf(path, sizeof path, "%s", o);
    else
        snprintf(path, sizeof path, "%s/%s", root, o);
    free(o);
    FILE *f = fopen(path, "r");
    char line[512];
    bool there = false;
    while (f && !there && fgets(line, sizeof line, f))
        there = strcmp(line, "*~\n") == 0 || strcmp(line, "*~") == 0;
    if (f)
        fclose(f);
    if (there)
        return;
    char dir[2 * CCR_GIT_PATH];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755);   /* info/ may not be there */
    }
    f = fopen(path, "a");
    if (f) {
        fputs("# Ccratch's backups of files it saved (FILE~)\n*~\n", f);
        fclose(f);
    }
}

int ccr_git_branches(const char *root, char names[][256], int cap)
{
    const char *args[] = { "branch", "--format=%(HEAD)%(refname:short)", NULL };
    int code = 0;
    char *o = ccr_git_run(root, args, &code);
    if (!o)
        return 0;
    int n = 0;
    /* the current one first */
    for (int pass = 0; pass < 2; ++pass) {
        for (const char *line = o; *line;) {
            const size_t len = strcspn(line, "\n");
            const bool current = line[0] == '*';
            if (len > 1 && line[1] != '(' && current == (pass == 0) && n < cap)   /* (HEAD detached...) */
                snprintf(names[n++], 256, "%.*s", (int)len - 1, line + 1);
            line += len + (line[len] == '\n');
        }
    }
    free(o);
    return code == 0 ? n : 0;
}

bool ccr_git_switch(const char *root, const char *branch, bool create, char *err, int cap)
{
    const char *args[] = { "switch", create ? "-c" : branch, create ? branch : NULL, NULL };
    return run_ok(root, args, err, cap);
}

int ccr_git_log(const char *root, ccr_git_commit_info *out, int cap)
{
    char n[16];
    snprintf(n, sizeof n, "-%d", cap);
    const char *args[] = { "log", n, "--format=%h%x09%an%x09%ar%x09%s", NULL };
    int code = 0;
    char *o = ccr_git_run(root, args, &code);
    if (!o)
        return 0;
    int k = 0;
    for (char *line = o, *next; line && *line && k < cap; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        char *f[4] = { line, NULL, NULL, NULL };
        for (int i = 1; i < 4 && f[i - 1]; ++i) {
            f[i] = strchr(f[i - 1], '\t');
            if (f[i])
                *f[i]++ = '\0';
        }
        if (!f[3])
            continue;
        ccr_git_commit_info *c = &out[k++];
        snprintf(c->hash, sizeof c->hash, "%s", f[0]);
        snprintf(c->author, sizeof c->author, "%s", f[1]);
        snprintf(c->when, sizeof c->when, "%s", f[2]);
        snprintf(c->subject, sizeof c->subject, "%s", f[3]);
    }
    free(o);
    return code == 0 ? k : 0;
}

/* ---------------------------------------------------------- network */

static void net_line(void *ctx, const char *line)
{
    ccr_git_job *j = ctx;
    if (!*line)
        return;
    /* keep the last few lines: the end of the story */
    size_t len = strlen(j->said);
    const size_t need = strlen(line) + 1;
    while (len + need + 1 > sizeof j->said && len > 0) {
        char *nl = strchr(j->said, '\n');
        if (!nl) {
            j->said[0] = '\0';
            len = 0;
            break;
        }
        memmove(j->said, nl + 1, strlen(nl + 1) + 1);
        len = strlen(j->said);
    }
    snprintf(j->said + len, sizeof j->said - len, "%s%.*s", len ? "\n" : "", (int)(sizeof j->said - len - 2), line);
}

bool ccr_git_net_start(ccr_git_job *j, const char *root, ccr_git_net what, bool set_upstream, char *err, int cap)
{
    quiet_git();
    memset(j, 0, sizeof *j);
    j->proc.fd = j->proc.in_fd = -1;
    const char *argv[12];
    int n = 0;
    argv[n++] = "git";
    argv[n++] = "-c";
    argv[n++] = "color.ui=false";
    switch (what) {
    case CCR_GIT_NET_PULL:
        argv[n++] = "pull";
        argv[n++] = "--no-rebase";
        argv[n++] = "--no-edit";
        argv[n++] = "--autostash";   /* changes not committed: put aside, pulled, put back */
        break;
    case CCR_GIT_NET_PUSH:
        argv[n++] = "push";
        if (set_upstream) {
            argv[n++] = "-u";
            argv[n++] = "origin";
            argv[n++] = "HEAD";
        }
        break;
    case CCR_GIT_NET_FETCH:
        argv[n++] = "fetch";
        argv[n++] = "--prune";
        break;
    case CCR_GIT_NET_NONE:
        return false;
    }
    argv[n] = NULL;
    if (!ccr_proc_start(&j->proc, argv, root, true, err, (size_t)cap))
        return false;
    j->what = what;
    return true;
}

bool ccr_git_net_poll(ccr_git_job *j)
{
    if (j->what == CCR_GIT_NET_NONE || j->done)
        return false;
    if (ccr_proc_poll(&j->proc, net_line, j))
        return true;
    j->done = true;
    j->ok = j->proc.exit_code == 0;
    return false;
}

void ccr_git_net_stop(ccr_git_job *j)
{
    if (j->what != CCR_GIT_NET_NONE && !j->done)
        ccr_proc_kill_wait(&j->proc);
    j->done = true;
}
