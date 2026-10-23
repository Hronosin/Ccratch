/*
 * Renaming across the project: the same thing in every file is renamed,
 * a different thing with the same spelling (a local, a field, a word in a
 * comment) is not; undone in one go.
 */
#define _DEFAULT_SOURCE   /* mkdtemp */
#include "ccr_rename.h"

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

static void put(const char *name, const char *text)
{
    char p[600];
    snprintf(p, sizeof p, "%s/%s", g_root, name);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static char *get(const char *name)
{
    static char buf[4096];
    char p[600];
    snprintf(p, sizeof p, "%s/%s", g_root, name);
    FILE *f = fopen(p, "r");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[n] = '\0';
    if (f)
        fclose(f);
    return buf;
}

static const char *UTIL_H = "#pragma once\ntypedef enum { RED, BLUE } shade;\nint add(int a, int b);\n"
                            "struct box { int add; };\n";
static const char *UTIL_C = "#include \"util.h\"\nint add(int a, int b) { return a + b; }\n"
                            "int twice(int v) { int add = v; return add * 2; }\n";
static const char *MAIN_C = "#include \"util.h\"\n/* add things */\nint main(void)\n{\n"
                            "    struct box b = { add(1, 2) };\n    shade s = RED;\n"
                            "    return add(b.add, -3) + (s == RED ? 0 : 1);\n}\n";

/* The byte where `what` is in file `name`, the `nth` time (from 0). */
static uint32_t at(const char *name, const char *what, int nth)
{
    const char *t = get(name), *p = t;
    for (int i = 0; p && i <= nth; ++i) {
        p = strstr(p, what);
        if (p && i < nth)
            ++p;
    }
    return p ? (uint32_t)(p - t) : 0;
}

int main(void)
{
    char tmpl[] = "/tmp/ccratch_rename_XXXXXX";
    if (!mkdtemp(tmpl))
        return 1;
    snprintf(g_root, sizeof g_root, "%s", tmpl);
    put("util.h", UTIL_H);
    put("util.c", UTIL_C);
    put("main.c", MAIN_C);
    char path[600], err[300];
    snprintf(path, sizeof path, "%s/main.c", g_root);

    /* everything called add: the function, the field, a local of twice() */
    {
        ccr_symbols ss;
        char e2[300];
        CHECK(ccr_symbols_named(g_root, "add", NULL, 0, &ss, e2, (int)sizeof e2), "by name: %s", e2);
        int fn = -1, field = -1, local = -1;
        for (int i = 0; i < ss.sym_count; ++i) {
            if (ss.syms[i].kind == CCR_SYM_FUNCTION)
                fn = i;
            if (ss.syms[i].kind == CCR_SYM_FIELD)
                field = i;
            if (ss.syms[i].kind == CCR_SYM_LOCAL)
                local = i;
        }
        CHECK(ss.sym_count == 3 && fn == 0 && field >= 0 && local >= 0, "three things called add: %d (fn %d)",
              ss.sym_count, fn);
        CHECK(fn >= 0 && ss.syms[fn].count == 4 && ss.syms[fn].own, "the function: %d places", fn >= 0 ? ss.syms[fn].count : 0);
        CHECK(field >= 0 && ss.syms[field].count == 2 && strcmp(ss.syms[field].owner, "box") == 0, "the field of box: %d, %s",
              field >= 0 ? ss.syms[field].count : 0, field >= 0 ? ss.syms[field].owner : "");
        CHECK(local >= 0 && ss.syms[local].count == 2 && strcmp(ss.syms[local].owner, "twice") == 0, "the local of twice");
        /* the field alone, renamed: the function and the local stay */
        ccr_rename fr;
        ccr_rename_undo fu;
        CHECK(field >= 0 && ccr_symbols_rename(&ss, field, &fr) && fr.place_count == 2 &&
              ccr_rename_apply(&fr, "total", &fu, e2, (int)sizeof e2), "the field renamed: %s", e2);
        CHECK(strstr(get("util.h"), "struct box { int total; };") && strstr(get("main.c"), "b.total") &&
              strstr(get("main.c"), "add(1, 2)") && strstr(get("util.c"), "int add = v;"), "only the field");
        CHECK(ccr_rename_undo_apply(&fu) == 2 && strstr(get("util.h"), "{ int add; }"), "and back");
        ccr_rename_undo_free(&fu);
        ccr_rename_free(&fr);
        /* all three at once: one rename, no place missed */
        CHECK(ccr_symbols_rename(&ss, -1, &fr) && fr.place_count == 8 && fr.file_count == 3 &&
              ccr_rename_apply(&fr, "sum", &fu, e2, (int)sizeof e2), "all of them: %d places, %s", fr.place_count, e2);
        CHECK(strstr(get("util.c"), "int sum = v; return sum * 2;") && strstr(get("util.h"), "{ int sum; }") &&
              strstr(get("main.c"), "return sum(b.sum, -3)"), "every add is sum:\n%s", get("main.c"));
        CHECK(ccr_rename_undo_apply(&fu) == 3, "and back");
        ccr_rename_undo_free(&fu);
        ccr_rename_free(&fr);
        ccr_symbols_free(&ss);
        /* a macro, and printf: not the project's own */
        put("more.c", "#include <stdio.h>\n#define LIMIT 3\nint lim(void) { printf(\"%d\", LIMIT); return LIMIT; }\n");
        CHECK(ccr_symbols_named(g_root, "LIMIT", NULL, 0, &ss, e2, (int)sizeof e2) && ss.sym_count == 1 &&
              ss.syms[0].kind == CCR_SYM_MACRO && ss.syms[0].count == 3, "the macro: %s %d", e2, ss.sym_count ? ss.syms[0].count : 0);
        ccr_symbols_free(&ss);
        CHECK(ccr_symbols_named(g_root, "printf", NULL, 0, &ss, e2, (int)sizeof e2) && ss.sym_count == 1 &&
              !ss.syms[0].own && ss.syms[0].kind == CCR_SYM_FUNCTION, "printf: found, not ours");
        ccr_symbols_free(&ss);
        CHECK(!ccr_symbols_named(g_root, "nothing_here", NULL, 0, &ss, e2, (int)sizeof e2) && e2[0], "nothing");
        char rm[700];
        snprintf(rm, sizeof rm, "%s/more.c", g_root);
        remove(rm);
    }

    /* add, from a call in main.c */
    ccr_rename r;
    CHECK(ccr_rename_find(g_root, path, at("main.c", "add(1", 0), NULL, 0, &r, err, (int)sizeof err),
          "find add: %s", err);
    CHECK(strcmp(r.old_name, "add") == 0 && r.file_count == 3 && r.place_count == 4,
          "add: %s in %d files, %d places (want 3, 4)", r.old_name, r.file_count, r.place_count);
    /* what each place is: the definition in util.c, the declaration in
     * util.h, two calls in main.c */
    int defs = 0, decls = 0, uses = 0;
    for (int i = 0; i < r.place_count; ++i) {
        defs += r.places[i].kind == CCR_PLACE_DEFINITION;
        decls += r.places[i].kind == CCR_PLACE_DECLARATION;
        uses += r.places[i].kind == CCR_PLACE_USE;
    }
    CHECK(defs == 1 && decls == 1 && uses == 2, "kinds: %d definitions, %d declarations, %d uses", defs, decls, uses);
    const int d = ccr_rename_definition(&r);
    CHECK(d >= 0 && strstr(r.files[r.places[d].file], "util.c") && r.places[d].line == 2,
          "the definition: %s:%d", d >= 0 ? r.files[r.places[d].file] : "-", d >= 0 ? r.places[d].line : 0);
    ccr_rename_undo undo;
    CHECK(!ccr_rename_apply(&r, "int", &undo, err, (int)sizeof err), "a keyword is refused");
    CHECK(ccr_rename_apply(&r, "sum", &undo, err, (int)sizeof err), "apply: %s", err);
    ccr_rename_free(&r);
    CHECK(strstr(get("util.h"), "int sum(int a, int b);\nstruct box { int add; };"), "util.h:\n%s", get("util.h"));
    CHECK(strstr(get("util.c"), "int sum(int a, int b)") && strstr(get("util.c"), "int add = v; return add * 2;"),
          "util.c (its local add stays):\n%s", get("util.c"));
    CHECK(strstr(get("main.c"), "/* add things */") && strstr(get("main.c"), "{ sum(1, 2) }") &&
          strstr(get("main.c"), "return sum(b.add, -3)"), "main.c (the field and the comment stay):\n%s",
          get("main.c"));

    /* undone: every file as it was */
    CHECK(ccr_rename_undo_apply(&undo) == 3, "undo");
    CHECK(strcmp(get("util.h"), UTIL_H) == 0 && strcmp(get("util.c"), UTIL_C) == 0 && strcmp(get("main.c"), MAIN_C) == 0,
          "back as they were");
    CHECK(ccr_rename_undo_apply(&undo) == -1, "a second undo: the files changed since, nothing done");
    ccr_rename_undo_free(&undo);

    /* an enum's value, from its declaration in the header */
    snprintf(path, sizeof path, "%s/util.h", g_root);
    CHECK(ccr_rename_find(g_root, path, at("util.h", "RED", 0), NULL, 0, &r, err, (int)sizeof err) &&
          r.place_count == 3, "RED: %d places (%s)", r.place_count, err);
    CHECK(ccr_rename_apply(&r, "CRIMSON", NULL, err, (int)sizeof err), "apply RED");
    ccr_rename_free(&r);
    CHECK(strstr(get("main.c"), "shade s = CRIMSON;") && strstr(get("main.c"), "s == CRIMSON"), "RED renamed");

    /* the field: only the field */
    snprintf(path, sizeof path, "%s/main.c", g_root);
    CHECK(ccr_rename_find(g_root, path, at("main.c", "b.add", 0) + 2, NULL, 0, &r, err, (int)sizeof err) &&
          r.place_count == 2, "the field: %d places (%s)", r.place_count, err);
    ccr_rename_free(&r);

    /* not the project's: printf is not renamed */
    put("main.c", "#include <stdio.h>\nint main(void) { printf(\"x\"); return 0; }\n");
    CHECK(!ccr_rename_find(g_root, path, at("main.c", "printf", 0), NULL, 0, &r, err, (int)sizeof err) && err[0],
          "printf refused: %s", err);
    ccr_rename_free(&r);
    CHECK(ccr_rename_valid("ok_name2") && !ccr_rename_valid("2bad") && !ccr_rename_valid("a-b"), "names");

    char rm[400];
    snprintf(rm, sizeof rm, "rm -rf '%s'", g_root);
    if (system(rm) != 0)
        fprintf(stderr, "could not clean\n");
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("rename: all checks passed\n");
    return g_failures ? 1 : 0;
}
