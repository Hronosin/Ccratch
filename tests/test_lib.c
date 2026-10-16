/*
 * Block packs: reading them, finding a call's block, checking, drafting
 * one from a header. Run with CCRATCH_BLOCKS_DIR=tests/data/blocks.
 */
#define _DEFAULT_SOURCE
#include "ccr_api.h"
#include "ccr_lib.h"
#include "ccr_lang.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* a pack written to memory */
static char *pack_of(const ccr_api *api, const char *name, const char *include, const char *link)
{
    static char buf[16384];
    FILE *f = tmpfile();
    if (!f)
        return strcpy(buf, "");
    ccr_api_write_pack(api, name, include, link, f);
    rewind(f);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static const ccr_api_func *func(const ccr_api *api, const char *name)
{
    for (int i = 0; i < api->func_count; ++i)
        if (strcmp(api->funcs[i].name, name) == 0)
            return &api->funcs[i];
    return NULL;
}

/* The API (clang reading headers) and the ABI (the .so / .a) */
static void test_api(const char *toy_h, const char *annotated_h, const char *so, const char *ar)
{
    char err[256];
    ccr_api api;
    CHECK(ccr_api_scan(toy_h, NULL, 0, &api, err, (int)sizeof err), "scan: %s", err);
    CHECK(api.func_count == 3, "%d functions in toy.h, want 3", api.func_count);
    const ccr_api_func *dot = func(&api, "draw_dot"), *pf = func(&api, "toy_printf");
    CHECK(dot && dot->nparams == 2 && strcmp(dot->params[1].name, "y") == 0 &&
          strcmp(dot->params[1].type, "int") == 0 && strcmp(dot->ret, "void") == 0, "draw_dot");
    CHECK(pf && pf->variadic && pf->nparams == 1, "toy_printf is variadic");
    CHECK(func(&api, "is_ready") && strcmp(func(&api, "is_ready")->ret, "bool") == 0, "bool");

    /* the binaries: what is not exported goes */
    const char *bins[2] = { so, ar };
    for (int b = 0; b < 2; ++b) {
        if (!bins[b])
            continue;
        ccr_api a2;
        CHECK(ccr_api_scan(toy_h, NULL, 0, &a2, err, (int)sizeof err), "scan again");
        const int exported = ccr_api_check_binary(&a2, bins[b]);
        CHECK(exported >= 2, "%s: %d symbols", bins[b], exported);
        CHECK(func(&a2, "draw_dot") && func(&a2, "is_ready") && !func(&a2, "toy_printf"),
              "%s: toy_printf is not in the binary, the others are", bins[b]);
        ccr_api_free(&a2);
    }
    if (so) {   /* found by name, from the project */
        char dir[1024], found[1024];
        snprintf(dir, sizeof dir, "%s", so);
        *strrchr(dir, '/') = '\0';
        CHECK(ccr_api_find_binary("toy", dir, found, (int)sizeof found) && strstr(found, "libtoy."),
              "libtoy not found in %s", dir);
        CHECK(!ccr_api_find_binary("no_such_library_zz", dir, found, (int)sizeof found), "a library out of nowhere");
    }
    CHECK(ccr_api_check_binary(&api, toy_h) == -1, "a header is no binary");
    ccr_api_free(&api);

    /* @block: the library picks its blocks and their words */
    if (annotated_h) {
        CHECK(ccr_api_scan(annotated_h, NULL, 0, &api, err, (int)sizeof err), "scan: %s", err);
        CHECK(!func(&api, "_beeper_hidden"), "_names are not public");
        CHECK(func(&api, "beeper_twice") && func(&api, "beeper_twice")->inline_def, "static inline kept");
        CHECK(api.enum_count == 1 && strcmp(api.enums[0].name, "beep_kind") == 0 && api.enums[0].count == 2,
              "the typedef'd enum");
        const char *p = pack_of(&api, "beeper", "\"annotated.h\"", "");
        CHECK(strstr(p, "text: \"beep {times} times\"\n    text.ru: \"пискнуть {times} раз\""),
              "@block texts:\n%s", p);
        CHECK(strstr(p, "set pitch to {hz} Hz") && strstr(p, "the beeper is busy"), "/** @block */:\n%s", p);
        CHECK(!strstr(p, "beeper_internal_thing") && !strstr(p, "beeper_twice"), "unmarked ones:\n%s", p);
        CHECK(strstr(p, "BEEP_SHORT") && strstr(p, "BEEP_LONG"), "enums:\n%s", p);
        ccr_api_free(&api);
    }

    /* where a header is, and how a file includes it */
    char where[1024], inc[256], dir[1024];
    snprintf(dir, sizeof dir, "%s", toy_h);
    *strrchr(dir, '/') = '\0';
    char from[1100];
    snprintf(from, sizeof from, "%s/somewhere.c", dir);
    CHECK(ccr_lib_find_header("toy", from, NULL, NULL, 0, where, (int)sizeof where) &&
          strstr(where, "/toy.h"), "\"toy\" next to the file");
    CHECK(ccr_lib_find_header("\"toy.h\"", NULL, dir, NULL, 0, where, (int)sizeof where), "in the project");
    const char *iargs[] = { "-I", dir };
    CHECK(ccr_lib_find_header("<toy.h>", NULL, NULL, iargs, 2, where, (int)sizeof where), "from -I");
    CHECK(!ccr_lib_find_header("no_such_header_zz.h", from, dir, NULL, 0, where, (int)sizeof where),
          "a header out of nowhere");
    if (ccr_lib_find_header("<stdio.h>", NULL, NULL, NULL, 0, where, (int)sizeof where)) {
        ccr_lib_include_for(where, dir, inc, (int)sizeof inc);
        CHECK(strcmp(inc, "<stdio.h>") == 0, "a system header: %s", inc);
    }
    char up[1100];   /* tests/ as the project: data/toy.h */
    snprintf(up, sizeof up, "%s/..", dir);
    ccr_lib_include_for(toy_h, up, inc, (int)sizeof inc);
    CHECK(strcmp(inc, "\"data/toy.h\"") == 0, "a project header: %s", inc);
    ccr_lib_include_for(toy_h, NULL, inc, (int)sizeof inc);
    CHECK(strcmp(inc, "\"toy.h\"") == 0, "any other header: %s", inc);

    /* a pack made into a project and loaded from there; made once */
    char proj[1024], pack[1200];
    snprintf(proj, sizeof proj, "/tmp/ccratch_test_lib_%d", (int)getpid());
    char packs[1100];
    ccr_lib_project_dir(proj, packs, (int)sizeof packs);
    int n = -1;
    CHECK(ccr_lib_create_pack(toy_h, "\"toy.h\"", packs, NULL, 0, NULL, pack, (int)sizeof pack, &n, err,
                              (int)sizeof err) && n == 3, "create_pack: %s (%d)", err, n);
    CHECK(ccr_lib_create_pack(toy_h, "\"toy.h\"", packs, NULL, 0, NULL, pack, (int)sizeof pack, &n, err,
                              (int)sizeof err) && n == 0, "made again: kept");
    ccr_lib_load_project(proj, NULL);
    const ccr_lib_block *b = ccr_lib_find("draw_dot", 2);
    CHECK(b && strstr(ccr_lib_pack_at(b->pack)->path, proj) && strcmp(b->text, "draw dot {x} y {y}") == 0,
          "the project's pack comes first");
    CHECK(ccr_lib_check(pack, NULL) == 0, "the made pack reads clean");
    remove(pack);
    rmdir(packs);
    snprintf(packs, sizeof packs, "%s/.ccratch", proj);
    rmdir(packs);
    rmdir(proj);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s BROKEN_PACK TOY_HEADER\n", argv[0]);
        return 2;
    }
    ccr_lib_load(stderr);
    CHECK(ccr_lib_pack_count() >= 1, "no pack found (CCRATCH_BLOCKS_DIR?)");
    const ccr_lib_pack *toy = NULL;
    for (int i = 0; i < ccr_lib_pack_count(); ++i)
        if (strcmp(ccr_lib_pack_at(i)->code, "toy") == 0)
            toy = ccr_lib_pack_at(i);
    CHECK(toy && toy->count == 3, "the toy pack");
    if (toy) {
        CHECK(strcmp(toy->name, "toy") == 0, "name: %s", toy->name);
        CHECK(ccr_lib_pack_matches(toy, "toy.h") && ccr_lib_pack_matches(toy, "include/toy.h") &&
              !ccr_lib_pack_matches(toy, "mytoy.h") && !ccr_lib_pack_matches(toy, "stdio.h"),
              "which includes bring the pack");
    }

    const ccr_lib_block *dot = ccr_lib_find("draw_dot", 2);
    CHECK(dot && strcmp(dot->text, "dot at {x} {y}") == 0, "draw_dot");
    CHECK(dot && strcmp(dot->defaults[1], "2") == 0, "defaults");
    CHECK(ccr_lib_find("draw_dot", 3) == NULL, "a call with other arguments is not that block");
    CHECK(ccr_lib_find("nope", 0) == NULL, "a block out of nowhere");
    const ccr_lib_block *rd = ccr_lib_find("is_ready", 0);
    CHECK(rd && strcmp(rd->returns, "bool") == 0, "returns");
    const ccr_lib_block *sc = ccr_lib_find("scale", 1);
    CHECK(sc && strcmp(sc->text, "scale by {k}") == 0, "a parameter left out goes at the end: %s",
          sc ? sc->text : "");
    CHECK(ccr_lib_index(dot) >= 0 && ccr_lib_block_at(ccr_lib_index(dot)) == dot, "index");

    /* texts in the interface language */
    ccr_lang_use("ru", NULL);
    ccr_lib_load(NULL);
    dot = ccr_lib_find("draw_dot", 2);
    CHECK(dot && strcmp(dot->text, "точка в {x} {y}") == 0, "text.ru");
    ccr_lang_reset();

    /* a broken pack: every problem told */
    FILE *sink = tmpfile();
    const int problems = ccr_lib_check(argv[1], sink);
    CHECK(problems == 4, "%d problems in the broken pack, want 4", problems);
    if (sink)
        fclose(sink);

    /* a draft from a header: its functions, bool as bool, ... as more */
    FILE *out = tmpfile();
    CHECK(out && ccr_lib_make(argv[2], NULL, 0, NULL, out), "make-blocks failed");
    if (out) {
        char buf[4096];
        rewind(out);
        size_t n = fread(buf, 1, sizeof buf - 1, out);
        buf[n] = '\0';
        fclose(out);
        CHECK(strstr(buf, "  - call: draw_dot\n    text: \"draw dot {x} y {y}\"\n    params: [x, y]\n"),
              "draw_dot draft:\n%s", buf);
        CHECK(strstr(buf, "  - call: is_ready\n    text: \"is ready\"\n    returns: \"bool\"\n"),
              "is_ready draft:\n%s", buf);
        CHECK(strstr(buf, "params: [fmt, more]"), "variadic draft:\n%s", buf);
        CHECK(strstr(buf, "header: \"toy.h\"") && strstr(buf, "include: \"\\\"toy.h\\\"\""),
              "header:\n%s", buf);
    }
    test_api(argv[2], argc > 3 ? argv[3] : NULL, argc > 5 ? argv[4] : NULL, argc > 5 ? argv[5] : NULL);
    ccr_lib_unload();
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("lib: all checks passed\n");
    return g_failures ? 1 : 0;
}
