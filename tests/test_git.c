/*
 * Git: status read right, stage / unstage / discard / commit, branches,
 * the history, changed lines for the marks on blocks, and pull / push
 * against a repository in a folder (no network).
 */
#define _DEFAULT_SOURCE   /* mkdtemp, nanosleep */
#include "ccr_git.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int g_failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
            ++g_failures;                                             \
        }                                                             \
    } while (0)

static void write_text(const char *dir, const char *name, const char *text)
{
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void git(const char *dir, const char *a, const char *b, const char *c)
{
    const char *args[] = { a, b, c, NULL };
    int code = 0;
    free(ccr_git_run(dir, args, &code));
}

static const ccr_git_file *file(const ccr_git_status *s, const char *path)
{
    for (int i = 0; i < s->count; ++i)
        if (strcmp(s->files[i].path, path) == 0)
            return &s->files[i];
    return NULL;
}

static void wait_net(ccr_git_job *j)
{
    for (int i = 0; i < 3000 && ccr_git_net_poll(j); ++i) {
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
}

static void identity(const char *dir)
{
    git(dir, "config", "user.name", "Test");
    git(dir, "config", "user.email", "test@example.com");
    git(dir, "config", "commit.gpgsign", "false");
}

int main(void)
{
    if (!ccr_git_available()) {
        printf("git: not installed, skipped\n");
        return 0;
    }
    /* no global config getting in the way */
    setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
    char tmpl[] = "/tmp/ccratch_git_XXXXXX";
    char *base = mkdtemp(tmpl);
    CHECK(base != NULL, "temp dir");
    if (!base)
        return 1;
    char home[1100], repo[1100], origin[1100], other[1100];
    snprintf(home, sizeof home, "%s/home", base);
    snprintf(repo, sizeof repo, "%s/repo", base);
    snprintf(origin, sizeof origin, "%s/origin.git", base);
    snprintf(other, sizeof other, "%s/other", base);
    mkdir(home, 0755);
    mkdir(repo, 0755);
    setenv("HOME", home, 1);

    ccr_git_status st;
    CHECK(ccr_git_status_read(repo, &st) && !st.repo, "not a repository yet");
    ccr_git_status_free(&st);
    char err[512];
    CHECK(ccr_git_init(repo, err, (int)sizeof err), "init: %s", err);
    identity(repo);
    git(repo, "checkout", "-q", "-b");   /* (no-op: the branch is named below) */
    git(repo, "symbolic-ref", "HEAD", "refs/heads/main");

    write_text(repo, "main.c", "int main(void)\n{\n    return 0;\n}\n");
    write_text(repo, "my file.c", "int x;\n");
    CHECK(ccr_git_status_read(repo, &st) && st.repo && st.no_commits && st.untracked == 2,
          "two new files: %d untracked, no commits %d", st.untracked, st.no_commits);
    CHECK(strcmp(st.branch, "main") == 0, "branch: %s", st.branch);
    CHECK(file(&st, "my file.c") && file(&st, "my file.c")->work == '?', "a name with a space");
    ccr_git_status_free(&st);

    char path[1200];
    snprintf(path, sizeof path, "%s/main.c", repo);
    ccr_git_hunk h[16];
    int n = ccr_git_file_hunks(path, h, 16);
    CHECK(n == 1 && h[0].kind == CCR_GIT_ADDED && h[0].first == 1 && h[0].count == 4, "new file: all added (%d)", n);

    CHECK(ccr_git_stage(repo, "main.c", err, (int)sizeof err), "stage: %s", err);
    CHECK(ccr_git_status_read(repo, &st) && st.staged == 1 && file(&st, "main.c")->index == 'A', "staged");
    ccr_git_status_free(&st);
    CHECK(ccr_git_unstage(repo, "main.c", true, err, (int)sizeof err), "unstage before any commit: %s", err);
    CHECK(ccr_git_status_read(repo, &st) && st.staged == 0 && st.untracked == 2, "unstaged");
    ccr_git_status_free(&st);
    CHECK(!ccr_git_commit(repo, "nothing", false, false, err, (int)sizeof err) && err[0],
          "nothing staged: refused (%s)", err);
    CHECK(ccr_git_stage_all(repo, err, (int)sizeof err) && ccr_git_commit(repo, "First", false, false, err, (int)sizeof err),
          "first commit: %s", err);
    CHECK(ccr_git_status_read(repo, &st) && st.count == 0 && !st.no_commits, "clean after it");
    ccr_git_status_free(&st);
    CHECK(ccr_git_file_hunks(path, h, 16) == 0, "committed: nothing changed");

    /* a change: modified and added lines, a deletion */
    write_text(repo, "main.c", "#include <stdio.h>\nint main(void)\n{\n    return 1;\n}\n");
    n = ccr_git_file_hunks(path, h, 16);
    CHECK(n == 2 && h[0].kind == CCR_GIT_ADDED && h[0].first == 1 && h[1].kind == CCR_GIT_CHANGED && h[1].first == 4,
          "hunks: %d (%d@%d, %d@%d)", n, n > 0 ? (int)h[0].kind : -1, n > 0 ? h[0].first : -1, n > 1 ? (int)h[1].kind : -1,
          n > 1 ? h[1].first : -1);
    char *d = ccr_git_diff(repo, "main.c", false, false);
    CHECK(d && strstr(d, "-    return 0;") && strstr(d, "+    return 1;"), "the diff:\n%s", d ? d : "");
    free(d);
    d = ccr_git_diff(repo, "main.c", true, false);
    CHECK(d && !strstr(d, "return"), "nothing staged in it");
    free(d);
    CHECK(ccr_git_discard(repo, "main.c", false, err, (int)sizeof err) && ccr_git_file_hunks(path, h, 16) == 0,
          "discarded: %s", err);
    write_text(repo, "junk.c", "x\n");
    CHECK(ccr_git_discard(repo, "junk.c", true, err, (int)sizeof err), "untracked discarded: %s", err);
    snprintf(path, sizeof path, "%s/junk.c", repo);
    CHECK(access(path, F_OK) != 0, "and gone");

    /* one hunk at a time: two changes far apart, staged one by one */
    {
        char big[2048] = "", changed[2048] = "";
        for (int i = 1; i <= 30; ++i) {
            char l[64];
            snprintf(l, sizeof l, "int line%d;\n", i);
            strcat(big, l);
            snprintf(l, sizeof l, i == 2 ? "int second;\n" : i == 25 ? "int twenty_fifth;\n" : "int line%d;\n", i);
            strcat(changed, l);
        }
        write_text(repo, "big.c", big);
        CHECK(ccr_git_stage(repo, "big.c", err, (int)sizeof err) &&
              ccr_git_commit(repo, "big", false, false, err, (int)sizeof err), "big: %s", err);
        write_text(repo, "big.c", changed);
        char *dw = ccr_git_diff(repo, "big.c", false, false);
        CHECK(dw && ccr_git_hunk_count(dw) == 2, "two hunks: %d", dw ? ccr_git_hunk_count(dw) : -1);
        CHECK(ccr_git_apply_hunk(repo, dw, 1, false, err, (int)sizeof err), "stage hunk 2: %s", err);
        free(dw);
        char *ds = ccr_git_diff(repo, "big.c", true, false);
        dw = ccr_git_diff(repo, "big.c", false, false);
        CHECK(ds && ccr_git_hunk_count(ds) == 1 && strstr(ds, "+int twenty_fifth;") && !strstr(ds, "second"),
              "staged: the second change only:\n%s", ds ? ds : "");
        CHECK(dw && ccr_git_hunk_count(dw) == 1 && strstr(dw, "+int second;"), "not staged: the first");
        CHECK(ccr_git_apply_hunk(repo, ds, 0, true, err, (int)sizeof err), "unstage it: %s", err);
        free(ds);
        free(dw);
        ds = ccr_git_diff(repo, "big.c", true, false);
        CHECK(ds && ccr_git_hunk_count(ds) == 0, "nothing staged again");
        free(ds);
        CHECK(!ccr_git_apply_hunk(repo, "", 0, false, err, (int)sizeof err), "no hunk: refused");
        CHECK(ccr_git_discard(repo, "big.c", false, err, (int)sizeof err), "big back");
    }

    /* Ccratch's backups stay out of it */
    write_text(repo, "main.c~", "old\n");
    ccr_git_exclude_backups(repo);
    ccr_git_exclude_backups(repo);   /* once is enough */
    CHECK(ccr_git_status_read(repo, &st) && st.count == 0, "a backup is no change: %d", st.count);
    ccr_git_status_free(&st);
    char exclude[1300];
    snprintf(exclude, sizeof exclude, "%s/.git/info/exclude", repo);
    FILE *ex = fopen(exclude, "r");
    int stars = 0;
    char exl[256];
    while (ex && fgets(exl, sizeof exl, ex))
        stars += strcmp(exl, "*~\n") == 0;
    if (ex)
        fclose(ex);
    CHECK(stars == 1, "*~ in the exclude list once: %d", stars);

    /* hunks of -U0 text */
    const char *u0 = "@@ -3,0 +4,2 @@\n+a\n+b\n@@ -10 +11 @@\n-x\n+y\n@@ -20,3 +20,0 @@\n-p\n";
    n = ccr_git_parse_hunks(u0, h, 16);
    CHECK(n == 3 && h[0].kind == CCR_GIT_ADDED && h[0].first == 4 && h[0].count == 2 && h[1].kind == CCR_GIT_CHANGED &&
          h[1].first == 11 && h[1].count == 1 && h[2].kind == CCR_GIT_DELETED && h[2].first == 20, "parsed hunks");

    /* branches */
    CHECK(ccr_git_switch(repo, "feature", true, err, (int)sizeof err), "new branch: %s", err);
    char names[8][256];
    n = ccr_git_branches(repo, names, 8);
    CHECK(n == 2 && strcmp(names[0], "feature") == 0, "branches: %d, first %s", n, n ? names[0] : "");
    CHECK(ccr_git_switch(repo, "main", false, err, (int)sizeof err), "back: %s", err);
    CHECK(ccr_git_status_read(repo, &st) && strcmp(st.branch, "main") == 0, "on main");
    ccr_git_status_free(&st);

    /* amend, the history */
    write_text(repo, "b.c", "int b;\n");
    CHECK(ccr_git_commit(repo, "Second", false, false, err, (int)sizeof err) == false, "untracked is not committed");
    CHECK(ccr_git_stage(repo, "b.c", err, (int)sizeof err) &&
          ccr_git_commit(repo, "Second, better", true, false, err, (int)sizeof err), "amend: %s", err);
    ccr_git_commit_info log[8];
    n = ccr_git_log(repo, log, 8);
    CHECK(n == 2 && strcmp(log[0].subject, "Second, better") == 0 && strcmp(log[1].subject, "First") == 0 && strcmp(log[0].author, "Test") == 0,
          "log: %d, %s", n, n ? log[0].subject : "");

    /* push to a remote in a folder, a clone commits, pull it back */
    const char *bare[] = { "init", "-q", "--bare", origin, NULL };
    free(ccr_git_run(base, bare, NULL));
    git(origin, "symbolic-ref", "HEAD", "refs/heads/main");
    git(repo, "remote", "add", "origin");   /* incomplete on purpose: a no-op */
    const char *add_remote[] = { "remote", "add", "origin", origin, NULL };
    free(ccr_git_run(repo, add_remote, NULL));
    CHECK(ccr_git_status_read(repo, &st) && st.upstream[0] == '\0', "no upstream yet");
    ccr_git_status_free(&st);
    ccr_git_job j;
    CHECK(ccr_git_net_start(&j, repo, CCR_GIT_NET_PUSH, true, err, (int)sizeof err), "push: %s", err);
    wait_net(&j);
    CHECK(j.done && j.ok, "pushed: %s", j.said);
    CHECK(ccr_git_status_read(repo, &st) && strcmp(st.upstream, "origin/main") == 0 && st.ahead == 0,
          "upstream: %s", st.upstream);
    ccr_git_status_free(&st);

    const char *clone[] = { "clone", "-q", origin, other, NULL };
    free(ccr_git_run(base, clone, NULL));
    identity(other);
    write_text(other, "c.c", "int c;\n");
    CHECK(ccr_git_stage_all(other, err, (int)sizeof err) && ccr_git_commit(other, "From the other", false, false, err, (int)sizeof err),
          "other commit: %s", err);
    CHECK(ccr_git_net_start(&j, other, CCR_GIT_NET_PUSH, false, err, (int)sizeof err), "push 2");
    wait_net(&j);
    CHECK(j.ok, "pushed from the clone: %s", j.said);

    CHECK(ccr_git_net_start(&j, repo, CCR_GIT_NET_FETCH, false, err, (int)sizeof err), "fetch");
    wait_net(&j);
    CHECK(j.ok && ccr_git_status_read(repo, &st) && st.behind == 1, "behind by one: %d (%s)", st.behind, j.said);
    ccr_git_status_free(&st);
    CHECK(ccr_git_net_start(&j, repo, CCR_GIT_NET_PULL, false, err, (int)sizeof err), "pull");
    wait_net(&j);
    snprintf(path, sizeof path, "%s/c.c", repo);
    CHECK(j.ok && access(path, F_OK) == 0, "pulled: %s", j.said);

    /* a push that cannot work says why, and does not hang */
    const char *bad[] = { "remote", "set-url", "origin", "https://127.0.0.1:1/nothing.git", NULL };
    free(ccr_git_run(repo, bad, NULL));
    CHECK(ccr_git_net_start(&j, repo, CCR_GIT_NET_PUSH, false, err, (int)sizeof err), "bad push");
    wait_net(&j);
    CHECK(j.done && !j.ok && j.said[0], "refused, with words: %s", j.said);

    char rm[1200];
    snprintf(rm, sizeof rm, "rm -rf '%s'", base);
    if (system(rm) != 0)
        fprintf(stderr, "could not clean %s\n", base);
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("git: all checks passed\n");
    return g_failures ? 1 : 0;
}
