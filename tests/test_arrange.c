/*
 * Arrangements of your own, as text: read, written back the same, and
 * what is wrong with a bad one said by line.
 */
#include "ccr_arrange.h"

#include <stdio.h>
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

int main(void)
{
    ccr_arrangement a, b;
    char err[200], text[2000], again[2000];
    const char *src = "# mine\n@name = Kernel\n@name.ru = Ядро\n"
                      "column = includes macros\n"
                      "column = types, globals protos | order=name\n"
                      "column = main funcs | order=calls wrap=1600\n";
    CHECK(ccr_arrangement_parse(src, &a, err, sizeof err), "parse: %s", err);
    CHECK(strcmp(a.name, "Kernel") == 0 && a.ncols == 3, "name, columns: %s %d", a.name, a.ncols);
    CHECK(a.cols[0].kinds == (CCR_AK_INCLUDES | CCR_AK_MACROS) && a.cols[0].order == CCR_AO_FILE, "column 1");
    CHECK(a.cols[1].kinds == (CCR_AK_TYPES | CCR_AK_GLOBALS | CCR_AK_PROTOS) && a.cols[1].order == CCR_AO_NAME,
          "column 2");
    CHECK(a.cols[2].order == CCR_AO_CALLS && a.cols[2].wrap == 1600.0f, "column 3");
    ccr_arrangement_write(&a, text, sizeof text);
    CHECK(ccr_arrangement_parse(text, &b, err, sizeof err) && memcmp(a.cols, b.cols, sizeof a.cols) == 0 &&
          a.ncols == b.ncols && strcmp(b.name, "Kernel") == 0, "written back:\n%s", text);
    ccr_arrangement_write(&b, again, sizeof again);
    CHECK(strcmp(text, again) == 0, "the same text twice");

    CHECK(!ccr_arrangement_parse("column = includes spaghetti\n", &a, err, sizeof err) && strstr(err, "line 1") &&
          strstr(err, "spaghetti"), "unknown kind: %s", err);
    CHECK(!ccr_arrangement_parse("@name = x\ncolumn = main | order=vibes\n", &a, err, sizeof err) &&
          strstr(err, "line 2"), "unknown order: %s", err);
    CHECK(!ccr_arrangement_parse("@name = empty\n", &a, err, sizeof err), "no columns");

    char code[64];
    ccr_arrangement_code("Kernel style!", code, sizeof code);
    CHECK(strcmp(code, "kernel_style") == 0, "code: %s", code);
    ccr_arrangement_code("Моя", code, sizeof code);
    CHECK(strncmp(code, "arrangement_", 12) == 0, "code of Cyrillic: %s", code);

    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("arrange: all checks passed\n");
    return g_failures ? 1 : 0;
}
