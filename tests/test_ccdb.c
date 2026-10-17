/*
 * The flags a project's build uses, read from compile_commands.json; and
 * which folder is the project when subfolders have CMakeLists.txt too.
 */
#define _DEFAULT_SOURCE
#include "ccr_ccdb.h"
#include "ccr_files.h"

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

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static int find_arg(const ccr_ccdb_args *a, const char *what)
{
    for (int i = 0; i < a->n; ++i)
        if (strcmp(a->v[i], what) == 0)
            return i;
    return -1;
}

static void dump(const ccr_ccdb_args *a)
{
    for (int i = 0; i < a->n; ++i)
        fprintf(stderr, "  [%s]\n", a->v[i]);
}

int main(void)
{
    char root[256], p[600], real_root[4096];
    snprintf(root, sizeof root, "/tmp/ccratch_test_ccdb_%d", (int)getpid());
    const char *dirs[] = { "", "/src", "/include", "/gui", "/gui/src", "/build", "/build/debug", "/.git" };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
        snprintf(p, sizeof p, "%s%s", root, dirs[i]);
        mkdir(p, 0755);
    }
    if (!realpath(root, real_root))
        snprintf(real_root, sizeof real_root, "%s", root);
    snprintf(p, sizeof p, "%s/CMakeLists.txt", root);
    write_text(p, "project(x)\nadd_subdirectory(gui)\n");
    snprintf(p, sizeof p, "%s/gui/CMakeLists.txt", root);
    write_text(p, "add_library(gui src/gui.c)\n");
    const char *files[] = { "/src/main.c", "/gui/src/gui.c", "/include/x.h" };
    for (size_t i = 0; i < 3; ++i) {
        snprintf(p, sizeof p, "%s%s", root, files[i]);
        write_text(p, "int x;\n");
    }

    /* a CMake-made database: one entry as a command line, one as arguments */
    char db[4096];
    snprintf(db, sizeof db,
             "[\n"
             "{\n  \"directory\": \"%s/build/debug\",\n"
             "  \"command\": \"/usr/bin/cc -DGAME=\\\\\\\"tetris\\\\\\\" -I%s/include -I../../third\\\\ party "
             "-isystem /opt/raylib/include -O2 -Wall -std=gnu11 -o x.o -c %s/src/main.c\",\n"
             "  \"file\": \"%s/src/main.c\",\n  \"output\": \"x.o\"\n},\n"
             "{\n  \"directory\": \"%s/build/debug/gui\",\n"
             "  \"arguments\": [\"cc\", \"-I\", \"../_deps/raylib-src/src\", \"-D\", \"PLATFORM_DESKTOP\","
             " \"-fsanitize=address\", \"-c\", \"../../../gui/src/gui.c\"],\n"
             "  \"file\": \"../../../gui/src/gui.c\", \"extra\": {\"nested\": [1, 2, {\"a\": null}]}\n}\n"
             "]\n",
             root, root, root, root, root);
    snprintf(p, sizeof p, "%s/build/debug/compile_commands.json", root);
    write_text(p, db);

    char found[1024];
    CHECK(ccr_ccdb_find(root, found, (int)sizeof found) && strstr(found, "/build/debug/compile_commands.json"),
          "not found: %s", found);

    ccr_ccdb_args a;
    snprintf(p, sizeof p, "%s/src/main.c", root);
    CHECK(ccr_ccdb_args_for(found, p, &a) && a.exact, "main.c: no exact entry");
    char want[700];
    snprintf(want, sizeof want, "%s/include", root);
    int k = find_arg(&a, "-I");
    CHECK(k >= 0 && k + 1 < a.n && strcmp(a.v[k + 1], want) == 0, "-I include");
    snprintf(want, sizeof want, "%s/build/debug/../../third party", root);
    bool spaced = false;
    for (int i = 0; i + 1 < a.n; ++i)
        spaced = spaced || (strcmp(a.v[i], "-I") == 0 && strcmp(a.v[i + 1], want) == 0);
    CHECK(spaced, "a relative -I with a space, made whole: %s", want);
    k = find_arg(&a, "-isystem");
    CHECK(k >= 0 && strcmp(a.v[k + 1], "/opt/raylib/include") == 0, "-isystem");
    CHECK(find_arg(&a, "-DGAME=\"tetris\"") >= 0, "a quoted define");
    CHECK(find_arg(&a, "-std=gnu11") >= 0, "-std");
    CHECK(find_arg(&a, "-O2") < 0 && find_arg(&a, "-Wall") < 0 && find_arg(&a, "-c") < 0 && find_arg(&a, "-o") < 0,
          "flags that do not change how code reads are left out");
    if (g_failures)
        dump(&a);
    ccr_ccdb_args_free(&a);

    /* "arguments", a relative file */
    snprintf(p, sizeof p, "%s/gui/src/gui.c", root);
    CHECK(ccr_ccdb_args_for(found, p, &a) && a.exact, "gui.c: no exact entry");
    snprintf(want, sizeof want, "%s/build/debug/gui/../_deps/raylib-src/src", root);
    k = find_arg(&a, "-I");
    CHECK(k >= 0 && strcmp(a.v[k + 1], want) == 0, "-I from arguments: %s", k >= 0 ? a.v[k + 1] : "");
    CHECK(find_arg(&a, "-DPLATFORM_DESKTOP") >= 0 && find_arg(&a, "-fsanitize=address") < 0, "-D NAME");
    ccr_ccdb_args_free(&a);

    /* a header: the nearest file's flags */
    snprintf(p, sizeof p, "%s/include/x.h", root);
    CHECK(ccr_ccdb_args_for(found, p, &a) && !a.exact && strstr(a.from, "/src/main.c"), "x.h: from %s", a.from);
    ccr_ccdb_args_free(&a);

    /* a broken database reads as nothing, not as garbage */
    snprintf(p, sizeof p, "%s/bad.json", root);
    write_text(p, "[ {\"directory\": \"/x\", \"file\": ");
    snprintf(want, sizeof want, "%s/src/main.c", root);
    CHECK(!ccr_ccdb_args_for(p, want, &a), "a cut-off database gave flags");
    ccr_ccdb_args_free(&a);

    /* gui/ has a CMakeLists.txt, but it is part of the project above */
    char proj[1024];
    snprintf(p, sizeof p, "%s/gui/src/gui.c", root);
    ccr_files_project_of(p, proj, (int)sizeof proj);
    CHECK(strcmp(proj, real_root) == 0, "gui.c belongs to %s, want %s", proj, real_root);
    /* and from a relative path */
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd) && chdir(root) == 0) {
        ccr_files_project_of("gui/src/gui.c", proj, (int)sizeof proj);
        CHECK(strcmp(proj, real_root) == 0, "relative: %s", proj);
        if (chdir(cwd) != 0)
            fprintf(stderr, "could not go back to %s\n", cwd);
    }

    snprintf(p, sizeof p, "rm -rf '%s'", root);
    if (system(p) != 0)
        fprintf(stderr, "could not clean %s\n", root);
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("ccdb: all checks passed\n");
    return g_failures ? 1 : 0;
}
