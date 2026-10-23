/*
 * The history: lanes of the graph, a commit's changes, revert and
 * cherry-pick, blame, the stash, a merge with a conflict resolved.
 */
#define _DEFAULT_SOURCE   /* mkdtemp, setenv */
#include "ccr_git.h"
#include "ccr_gitlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static char g_repo[600];

static void put(const char *name, const char *text)
{
    char p[800];
    snprintf(p, sizeof p, "%s/%s", g_repo, name);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static char *get(const char *name)
{
    static char buf[4096];
    char p[800];
    snprintf(p, sizeof p, "%s/%s", g_repo, name);
    FILE *f = fopen(p, "r");
    const size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[n] = '\0';
    if (f)
        fclose(f);
    return buf;
}

static void git(const char *a, const char *b, const char *c, const char *d)
{
    const char *args[] = { a, b, c, d, NULL };
    free(ccr_git_run(g_repo, args, NULL));
}

static void commit(const char *msg)
{
    git("add", "-A", NULL, NULL);
    git("commit", "-qm", msg, NULL);
}

static void test_lanes(void)
{
    /* d merges c into b; b and c both come from a:
     *   d
     *   |\
     *   b c
     *   |/
     *   a      */
    ccr_graph_row rows[4];
    memset(rows, 0, sizeof rows);
    const char *hashes[] = { "d", "b", "c", "a" };
    const char parents[4][400] = { "b c", "a", "a", "" };
    for (int i = 0; i < 4; ++i)
        snprintf(rows[i].hash, sizeof rows[i].hash, "%s", hashes[i]);
    ccr_git_graph_lanes(rows, parents, 4);
    CHECK(rows[0].lane == 0 && rows[1].lane == 0 && rows[2].lane == 1 && rows[3].lane == 0,
          "lanes: %d %d %d %d", rows[0].lane, rows[1].lane, rows[2].lane, rows[3].lane);
    bool to_c = false;
    for (int e = 0; e < rows[0].edge_count; ++e)
        to_c = to_c || (rows[0].edges[e].from == 0 && rows[0].edges[e].to == 1);
    CHECK(to_c, "the merge opens a lane for c");
    bool joins = false;
    for (int e = 0; e < rows[3].edge_count; ++e)
        joins = joins || (rows[3].edges[e].merge_in && rows[3].edges[e].from == 1 && rows[3].edges[e].to == 0);
    CHECK(joins, "c's lane joins a's at a");
    CHECK(rows[0].parents == 2 && rows[3].parents == 0, "parents counted");
}

int main(void)
{
    test_lanes();
    if (!ccr_git_available()) {
        printf("gitlog: no git, the rest skipped\n");
        return g_failures ? 1 : 0;
    }
    setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
    char tmpl[] = "/tmp/ccratch_gitlog_XXXXXX";
    if (!mkdtemp(tmpl))
        return 1;
    char home[700];
    snprintf(home, sizeof home, "%s/home", tmpl);
    mkdir(home, 0755);
    setenv("HOME", home, 1);
    snprintf(g_repo, sizeof g_repo, "%s/repo", tmpl);
    mkdir(g_repo, 0755);
    char err[512];
    CHECK(ccr_git_init(g_repo, err, (int)sizeof err), "init");
    git("config", "user.name", "Ann", NULL);
    git("config", "user.email", "ann@example.com", NULL);
    git("config", "commit.gpgsign", "false", NULL);
    git("symbolic-ref", "HEAD", "refs/heads/main", NULL);

    put("a.c", "int a = 1;\nint b = 2;\n");
    commit("First");
    put("a.c", "int a = 1;\nint b = 3;\n");
    commit("Bump b");

    /* blame: line 1 from First, line 2 from Bump b; a change not committed */
    put("a.c", "int a = 1;\nint b = 3;\nint c = 4;\n");
    char path[800];
    snprintf(path, sizeof path, "%s/a.c", g_repo);
    ccr_blame bl;
    CHECK(ccr_git_blame(path, &bl) && bl.line_count == 3, "blame: %d lines", bl.line_count);
    if (bl.line_count == 3) {
        CHECK(strcmp(bl.commits[bl.line_commit[0]].summary, "First") == 0 &&
              strcmp(bl.commits[bl.line_commit[1]].summary, "Bump b") == 0 &&
              strcmp(bl.commits[bl.line_commit[0]].author, "Ann") == 0, "who and why");
        CHECK(bl.commits[bl.line_commit[2]].uncommitted, "the new line: not committed yet");
    }
    ccr_git_blame_free(&bl);
    char ago[64];
    ccr_git_ago(0, ago, (int)sizeof ago);
    CHECK(strstr(ago, "year"), "long ago: %s", ago);

    /* stash it away and back */
    CHECK(ccr_git_stash_push(g_repo, "a third int", err, (int)sizeof err), "stash: %s", err);
    CHECK(!strstr(get("a.c"), "int c"), "stashed: gone from the file");
    ccr_stash_entry st[4];
    CHECK(ccr_git_stash_list(g_repo, st, 4) == 1 && strstr(st[0].what, "a third int"), "the list");
    CHECK(ccr_git_stash_apply(g_repo, 0, true, err, (int)sizeof err) && strstr(get("a.c"), "int c = 4;") &&
          ccr_git_stash_list(g_repo, st, 4) == 0, "popped: %s", err);
    put("a.c", "int a = 1;\nint b = 3;\n");   /* the third int goes in a file of its own */
    put("c.c", "int c = 4;\n");
    commit("Third");

    /* the graph, a commit's changes, revert */
    ccr_graph_row rows[16];
    int n = ccr_git_graph(g_repo, rows, 16);
    CHECK(n == 3 && strcmp(rows[0].subject, "Third") == 0 && rows[0].head && strstr(rows[0].refs, "main"),
          "graph: %d, %s [%s]", n, n ? rows[0].subject : "", n ? rows[0].refs : "");
    char *show = n >= 2 ? ccr_git_show(g_repo, rows[1].hash) : NULL;
    CHECK(show && strstr(show, "Bump b") && strstr(show, "-int b = 2;") && strstr(show, "+int b = 3;"), "show");
    free(show);
    CHECK(n >= 2 && ccr_git_revert(g_repo, rows[1].hash, err, (int)sizeof err) && strstr(get("a.c"), "int b = 2;"),
          "reverted: %s", err);

    /* a branch, a conflict, resolved both ways at once, merged */
    n = ccr_git_graph(g_repo, rows, 16);
    CHECK(ccr_git_branch_from(g_repo, "side", rows[1].hash, err, (int)sizeof err), "branch from: %s", err);
    put("c.c", "int c = 40;\n");
    commit("Side c");
    git("switch", "-q", "main", NULL);
    put("c.c", "int c = 400;\n");
    commit("Main c");
    bool conflicts = false;
    CHECK(!ccr_git_merge(g_repo, "side", &conflicts, err, (int)sizeof err) && conflicts && ccr_git_merging(g_repo),
          "a conflict: %s", err);
    ccr_git_status s;
    CHECK(ccr_git_status_read(g_repo, &s) && s.conflicts == 1, "status says so: %d", s.conflicts);
    ccr_git_status_free(&s);
    CHECK(ccr_git_resolve(g_repo, "c.c", CCR_RESOLVE_BOTH, err, (int)sizeof err), "resolve: %s", err);
    CHECK(strcmp(get("c.c"), "int c = 400;\nint c = 40;\n") == 0, "both:\n%s", get("c.c"));
    CHECK(ccr_git_merge_commit(g_repo, err, (int)sizeof err) && !ccr_git_merging(g_repo), "merged: %s", err);
    n = ccr_git_graph(g_repo, rows, 16);
    CHECK(n > 0 && rows[0].parents == 2, "the merge has two parents");

    /* the text alone */
    char *t = ccr_git_resolve_text("x\n<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> side\ny\n", CCR_RESOLVE_THEIRS);
    CHECK(t && strcmp(t, "x\ntheirs\ny\n") == 0, "theirs: [%s]", t ? t : "");
    free(t);
    t = ccr_git_resolve_text("<<<<<<< a\no\n||||||| base\nb\n=======\nt\n>>>>>>> c\n", CCR_RESOLVE_OURS);
    CHECK(t && strcmp(t, "o\n") == 0, "diff3 style, ours: [%s]", t ? t : "");
    free(t);

    /* cherry-pick from the side branch, and an abort */
    git("switch", "-q", "side", NULL);
    put("b.c", "int z;\n");
    commit("Side z");
    n = ccr_git_graph(g_repo, rows, 16);
    int z = -1;
    for (int i = 0; i < n; ++i)
        if (strcmp(rows[i].subject, "Side z") == 0)
            z = i;
    git("switch", "-q", "main", NULL);
    CHECK(z >= 0 && ccr_git_cherry_pick(g_repo, rows[z].hash, err, (int)sizeof err) && strstr(get("b.c"), "int z;"),
          "cherry-picked: %s", err);

    /* a cherry-pick that conflicts waits, and is given up */
    git("switch", "-q", "side", NULL);
    put("c.c", "int c = 7;\n");
    commit("Side seven");
    git("switch", "-q", "main", NULL);
    n = ccr_git_graph(g_repo, rows, 16);
    int seven = -1, main_c = -1;
    for (int i = 0; i < n; ++i) {
        if (strcmp(rows[i].subject, "Side seven") == 0)
            seven = i;
        if (strcmp(rows[i].subject, "Main c") == 0)
            main_c = i;
    }
    CHECK(seven >= 0 && !ccr_git_cherry_pick(g_repo, rows[seven].hash, err, (int)sizeof err) &&
          ccr_git_pending(g_repo) == CCR_GIT_OP_PICK, "a cherry-pick waits");
    CHECK(ccr_git_merge_abort(g_repo, err, (int)sizeof err) && ccr_git_pending(g_repo) == CCR_GIT_OP_NONE &&
          strcmp(get("c.c"), "int c = 400;\nint c = 40;\n") == 0, "given up: %s", err);

    /* a revert that conflicts: resolved, then committed */
    CHECK(main_c >= 0 && !ccr_git_revert(g_repo, rows[main_c].hash, err, (int)sizeof err) &&
          ccr_git_pending(g_repo) == CCR_GIT_OP_REVERT, "a revert waits");
    CHECK(ccr_git_resolve(g_repo, "c.c", CCR_RESOLVE_THEIRS, err, (int)sizeof err) &&
          ccr_git_merge_commit(g_repo, err, (int)sizeof err) && ccr_git_pending(g_repo) == CCR_GIT_OP_NONE,
          "reverted after all: %s", err);
    n = ccr_git_graph(g_repo, rows, 16);
    CHECK(n > 0 && strncmp(rows[0].subject, "Revert", 6) == 0, "the revert's own message: %s", n ? rows[0].subject : "");

    /* a commit made together: its co-authors in the graph and in blame */
    {
        const char people[3][160] = { "Bob Builder <bob@example.com>", "Claude <noreply@anthropic.com>",
                                      "Bob Builder <bob@example.com>" };
        char *msg = ccr_git_with_coauthors("Pair on d\n", people, 3);
        CHECK(msg && strcmp(msg, "Pair on d\n\nCo-authored-by: Bob Builder <bob@example.com>\n"
                                 "Co-authored-by: Claude <noreply@anthropic.com>") == 0, "the trailers: [%s]", msg);
        put("d.c", "int d = 5;\n");
        git("add", "-A", NULL, NULL);
        CHECK(msg && ccr_git_commit(g_repo, msg, false, false, err, (int)sizeof err), "commit together: %s", err);
        free(msg);
        n = ccr_git_graph(g_repo, rows, 16);
        CHECK(n > 0 && rows[0].coauthor_count == 2 && strcmp(rows[0].coauthors[0], "Bob Builder") == 0 &&
              strcmp(rows[0].coauthors[1], "Claude") == 0 && strcmp(rows[0].subject, "Pair on d") == 0,
              "graph: %d co-authors, %s", n ? rows[0].coauthor_count : -1, n ? rows[0].subject : "");
        CHECK(n > 1 && rows[1].coauthor_count == 0, "a commit of one: nobody else");
        snprintf(path, sizeof path, "%s/d.c", g_repo);
        CHECK(ccr_git_blame(path, &bl) && bl.line_count == 1 && bl.commits[bl.line_commit[0]].coauthor_count == 2 &&
              strcmp(bl.commits[bl.line_commit[0]].coauthors[1], "Claude") == 0, "blame knows them too");
        ccr_git_blame_free(&bl);
        char who[8][160];
        const int np = ccr_git_people(g_repo, who, 8);
        CHECK(np == 2 && strcmp(who[0], "Bob Builder <bob@example.com>") == 0, "people: %d, %s", np, np ? who[0] : "");
        /* any case, the author left out, repeats once, \x1f between */
        char co[CCR_GIT_COAUTHORS][64];
        const int nc = ccr_git_coauthors("x\n\nco-authored-by:  Ann <a@x>\nCO-AUTHORED-BY: Eve <e@x>\x1f"
                                         "Co-authored-by: Eve <e@x>\nCo-authored-by: <z@x>", "ann", co, CCR_GIT_COAUTHORS);
        CHECK(nc == 2 && strcmp(co[0], "Eve") == 0 && strcmp(co[1], "z@x") == 0, "parsed: %d", nc);
    }

    char rm[800];
    snprintf(rm, sizeof rm, "rm -rf '%s'", tmpl);
    if (system(rm) != 0)
        fprintf(stderr, "could not clean\n");
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("gitlog: all checks passed\n");
    return g_failures ? 1 : 0;
}
