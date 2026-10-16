/*
 * The project tree: which files show, in what order, which folders open.
 * No window needed.
 */
#define _DEFAULT_SOURCE   /* mkdtemp */
#include "ccr_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

static char g_root[256];

static void touch(const char *rel)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs("int x;\n", f);
        fclose(f);
    }
}

static void dir(const char *rel)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_root, rel);
    mkdir(path, 0755);
}

static int find(const ccr_files *f, const char *name)
{
    for (int i = 0; i < f->count; ++i)
        if (strcmp(f->v[i].name, name) == 0)
            return i;
    return -1;
}

int main(void)
{
    snprintf(g_root, sizeof g_root, "/tmp/ccratch-files-XXXXXX");
    if (!mkdtemp(g_root))
        return 2;
    dir("src");
    dir("src/deep");
    dir("include");
    dir("build");
    dir(".git");
    dir("docs");               /* no code in it: left out */
    touch("main.c");
    touch("README.md");        /* not code */
    touch("src/b.c");
    touch("src/a.c");
    touch("src/deep/x.h");
    touch("include/api.h");
    touch("build/gen.c");      /* build trees are not the project */
    touch(".git/hook.c");
    touch("docs/notes.txt");

    ccr_files f = { 0 };
    char focus[512];
    snprintf(focus, sizeof focus, "%s/src/deep/x.h", g_root);
    CHECK(ccr_files_scan(&f, g_root, focus), "scan failed");

    /* folders first, then files, by name, depth first */
    const char *want[] = { "include", "api.h", "src", "deep", "x.h", "a.c", "b.c", "main.c" };
    CHECK(f.count == 8, "%d entries, want 8", f.count);
    for (int i = 0; i < 8 && i < f.count; ++i)
        CHECK(strcmp(f.v[i].name, want[i]) == 0, "entry %d is %s, want %s", i, f.v[i].name, want[i]);
    CHECK(find(&f, "gen.c") < 0 && find(&f, "hook.c") < 0 && find(&f, "docs") < 0 &&
          find(&f, "README.md") < 0, "something that is not the project showed up");

    /* the folders down to the focused file are open, the rest closed */
    const int src = find(&f, "src"), deep = find(&f, "deep"), inc = find(&f, "include");
    CHECK(src >= 0 && f.v[src].expanded && deep >= 0 && f.v[deep].expanded, "focus folders closed");
    CHECK(inc >= 0 && !f.v[inc].expanded, "include opened by itself");
    CHECK(!ccr_files_visible(&f, find(&f, "api.h")), "a file in a closed folder shows");
    CHECK(ccr_files_visible(&f, find(&f, "x.h")) && ccr_files_visible(&f, find(&f, "main.c")),
          "files that should show do not");

    /* a rescan keeps what was opened by hand, and finds new files */
    f.v[inc].expanded = true;
    touch("src/c.c");
    ccr_files_scan(&f, g_root, NULL);
    CHECK(find(&f, "c.c") >= 0, "a new file was not found");
    CHECK(f.v[find(&f, "include")].expanded && f.v[find(&f, "src")].expanded,
          "a rescan closed folders");

    /* paths */
    char p1[512], p2[512], d[1024];
    snprintf(p1, sizeof p1, "%s/src/a.c", g_root);
    snprintf(p2, sizeof p2, "%s/src/deep/../a.c", g_root);
    CHECK(ccr_files_same(p1, p2), "the same file spelled two ways");
    CHECK(ccr_files_inside(f.root, p1), "a.c is not inside its project");
    CHECK(!ccr_files_inside(f.root, "/etc/hostname"), "/etc is inside the project");
    ccr_files_dir_of(p1, d, (int)sizeof d);
    CHECK(strstr(d, "/src") && !strstr(d, "a.c"), "folder of a.c: %s", d);

    ccr_files_free(&f);
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_root);
    if (system(cmd) != 0)
        fprintf(stderr, "could not clean %s\n", g_root);
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("files: all checks passed\n");
    return g_failures ? 1 : 0;
}
