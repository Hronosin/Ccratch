/*
 * Block packs: reading them, finding a call's block, checking, drafting
 * one from a header. Run with CCRATCH_BLOCKS_DIR=tests/data/blocks.
 */
#include "ccr_lib.h"
#include "ccr_lang.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    CHECK(out && ccr_lib_make(argv[2], NULL, 0, out), "make-blocks failed");
    if (out) {
        char buf[4096];
        rewind(out);
        size_t n = fread(buf, 1, sizeof buf - 1, out);
        buf[n] = '\0';
        fclose(out);
        CHECK(strstr(buf, "  - call: draw_dot\n    text: \"draw dot {x} {y}\"\n    params: [x, y]\n"),
              "draw_dot draft:\n%s", buf);
        CHECK(strstr(buf, "  - call: is_ready\n    text: \"is ready\"\n    returns: \"bool\"\n"),
              "is_ready draft:\n%s", buf);
        CHECK(strstr(buf, "params: [fmt, more]"), "variadic draft:\n%s", buf);
        CHECK(strstr(buf, "header: toy.h"), "header");
    }
    ccr_lib_unload();
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("lib: all checks passed\n");
    return g_failures ? 1 : 0;
}
