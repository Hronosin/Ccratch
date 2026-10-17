/*
 * The project's own functions as blocks: what is read from which file,
 * statics and main left out, headers' declarations first, enums for the
 * slots; and the same in the background.
 */
#define _DEFAULT_SOURCE   /* mkdtemp, nanosleep */
#include "ccr_lib.h"
#include "ccr_project.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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

static void put(const char *dir, const char *name, const char *text)
{
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static const ccr_lib_block *block(const ccr_project_result *r, const char *name)
{
    for (int i = 0; i < r->count; ++i)
        if (strcmp(r->blocks[i].call, name) == 0)
            return &r->blocks[i];
    return NULL;
}

int main(void)
{
    char tmpl[] = "/tmp/ccratch_project_XXXXXX";
    char *root = mkdtemp(tmpl);
    if (!root)
        return 1;
    char d[1200];
    snprintf(d, sizeof d, "%s/include", root);
    mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/src", root);
    mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/build", root);
    mkdir(d, 0755);
    put(root, "include/util.h", "#pragma once\ntypedef enum { RED, BLUE } color;\n"
                                "int add(int a, int b);\nvoid paint(color c);\n");
    put(root, "src/util.c", "#include \"util.h\"\nstatic int helper(void) { return 1; }\n"
                            "int add(int a, int b) { return a + b + helper() - 1; }\nvoid paint(color c) { (void)c; }\n");
    put(root, "src/other.c", "void lonely(float x, const char *__name) { (void)x; (void)__name; }\n");
    put(root, "main.c", "#include \"util.h\"\nint main(void) { return add(1, 2) - 3; }\n");
    put(root, "build/junk.c", "void from_the_build(void) {}\n");

    ccr_project_result r;
    CHECK(ccr_project_read(root, NULL, 0, &r), "read");
    CHECK(r.count == 3, "%d functions, want add, paint, lonely", r.count);
    const ccr_lib_block *add = block(&r, "add"), *paint = block(&r, "paint"), *lonely = block(&r, "lonely");
    CHECK(add && strcmp(add->include, "\"util.h\"") == 0 && add->nparams == 2 && strcmp(add->returns, "int") == 0,
          "add, from its header: %s", add ? add->include : "-");
    CHECK(add && strcmp(add->text, "add ( {a} , {b} )") == 0, "it reads as its call: [%s]", add ? add->text : "");
    CHECK(paint && strcmp(paint->types[0], "color") == 0, "paint(color)");
    CHECK(lonely && lonely->include[0] == '\0' && strcmp(lonely->params[1], "name") == 0 &&
          strcmp(lonely->returns, "void") == 0, "lonely: only a .c has it");
    CHECK(!block(&r, "helper") && !block(&r, "main") && !block(&r, "from_the_build"),
          "static, main and the build folder left out");
    CHECK(r.enum_count == 1 && strcmp(r.enums[0].name, "color") == 0 && r.enums[0].count == 2, "the enum");

    /* into the registry: found by name, its slot picks from the enum */
    ccr_lib_load(NULL);
    const int packs = ccr_lib_pack_count();
    ccr_project_apply(&r, "This project");
    CHECK(ccr_lib_pack_count() == packs + 1 && ccr_lib_project_pack() == packs, "a pack of its own");
    const ccr_lib_block *found = ccr_lib_find("paint", 1);
    const char *vals[8];
    char lname[64];
    CHECK(found && ccr_lib_param_choices(found, 0, vals, 8, lname, (int)sizeof lname) == 2 &&
          strcmp(vals[1], "BLUE") == 0, "paint's slot: RED, BLUE");
    ccr_project_apply(&r, "This project");   /* again: replaced, not added */
    CHECK(ccr_lib_pack_count() == packs + 1 && ccr_lib_find("add", 2), "applied twice: one pack");
    ccr_project_result_free(&r);

    /* in the background; a file changed is read again */
    put(root, "src/other.c", "void lonely(float x) { (void)x; }\nint fresh(void) { return 0; }\n");
    ccr_project_scan_start(root, NULL, 0);
    bool done = false;
    for (int i = 0; i < 1000 && !done; ++i) {
        done = ccr_project_scan_take(&r);
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    CHECK(done && r.count == 4 && block(&r, "fresh") && block(&r, "lonely")->nparams == 1,
          "the thread: %d functions", done ? r.count : -1);
    if (done)
        ccr_project_result_free(&r);
    ccr_project_shutdown();
    ccr_lib_unload();

    char rm[1200];
    snprintf(rm, sizeof rm, "rm -rf '%s'", root);
    if (system(rm) != 0)
        fprintf(stderr, "could not clean %s\n", root);
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("project: all checks passed\n");
    return g_failures ? 1 : 0;
}
