/*
 * Finding and replacing text: where a match is (code, string, comment),
 * any case, whole words, the case kept, literals kept whole.
 */
#include "ccr_search.h"

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

static const char SRC[] =
    "#include <stdio.h>\n"
    "/* say Hello */\n"
    "void hello_world(void)\n"
    "{\n"
    "    printf(\"Hello, world!\\n\");   // HELLO back\n"
    "    puts(\"hello \\\"there\\\"\");\n"
    "    char c = 'h';\n"
    "    int big = 1'000;\n"
    "}\n";

static char *replaced(const char *q, const ccr_search_opts *o, const char *with, bool keep)
{
    ccr_search_hit h[32];
    const int n = ccr_search_text(SRC, strlen(SRC), q, o, h, 32);
    return ccr_search_replace(SRC, strlen(SRC), h, n, with, keep, NULL);
}

int main(void)
{
    const size_t len = strlen(SRC);
    ccr_search_hit h[32];
    ccr_search_opts any = { false, false, 0 };

    int n = ccr_search_text(SRC, len, "hello", &any, h, 32);
    CHECK(n == 5, "hello anywhere, any case: %d", n);
    CHECK(n == 5 && h[0].line == 2 && h[0].where == CCR_IN_COMMENT && h[1].line == 3 && h[1].where == CCR_IN_CODE &&
          h[2].where == CCR_IN_STRING && h[3].where == CCR_IN_COMMENT && h[4].line == 6, "where and lines");

    ccr_search_opts strings = { false, false, CCR_IN_STRING };
    n = ccr_search_text(SRC, len, "hello", &strings, h, 32);
    CHECK(n == 2, "in strings only: %d", n);
    ccr_search_opts exact = { true, false, 0 };
    CHECK(ccr_search_text(SRC, len, "Hello", &exact, h, 32) == 2, "case matters");
    ccr_search_opts word = { false, true, 0 };
    CHECK(ccr_search_text(SRC, len, "hello", &word, h, 32) == 4, "whole word: not hello_world");
    ccr_search_opts code = { false, false, CCR_IN_CODE };
    CHECK(ccr_search_text(SRC, len, "000", &code, h, 32) == 1, "a digit separator is no character literal");
    CHECK(ccr_search_text(SRC, len, "h", &(ccr_search_opts){ true, true, CCR_IN_STRING }, h, 32) == 1, "'h'");

    /* Hello -> Hi in strings, the case kept: Hello, hello */
    char *r = replaced("hello", &strings, "hi", true);
    CHECK(r && strstr(r, "printf(\"Hi, world!\\n\")") && strstr(r, "puts(\"hi \\\"there") &&
          strstr(r, "hello_world") && strstr(r, "say Hello"), "replaced in strings: %s", r);
    free(r);
    r = replaced("hello", &any, "hi", true);
    CHECK(r && strstr(r, "say Hi") && strstr(r, "hi_world") && strstr(r, "// HI back"), "everywhere, case kept: %s", r);
    free(r);
    /* a quote in the new text: escaped inside the literal, not outside */
    r = replaced("world", &strings, "\"big\" world", false);
    CHECK(r && strstr(r, "\"Hello, \\\"big\\\" world!\\n\""), "escaped: %s", r);
    free(r);

    /* Cyrillic: any case, and the case kept */
    const char *ru = "puts(\"Привет, мир\"); // ПРИВЕТ\nint привет_число;";
    ccr_search_hit hr[8];
    n = ccr_search_text(ru, strlen(ru), "привет", &any, hr, 8);
    CHECK(n == 3, "привет: %d", n);
    char *rr = ccr_search_replace(ru, strlen(ru), hr, n, "здравствуй", true, NULL);
    CHECK(rr && strstr(rr, "\"Здравствуй, мир\"") && strstr(rr, "// ЗДРАВСТВУЙ") && strstr(rr, "здравствуй_число"),
          "кириллица: %s", rr);
    free(rr);
    const char *uk = "Їжак і їжак";
    CHECK(ccr_search_text(uk, strlen(uk), "ЇЖАК", &any, hr, 8) == 2, "ї and Ї");

    /* the line shown for a match */
    char line[64];
    int at = -1;
    n = ccr_search_text(SRC, len, "world", &strings, h, 32);
    ccr_search_line(SRC, len, h[0].offset, line, sizeof line, &at);
    CHECK(strncmp(line, "printf(", 7) == 0 && at >= 0 && strncmp(line + at, "world", 5) == 0, "line: [%s] %d", line, at);

    /* raw strings and block comments */
    const char *raw = "auto s = R\"x(say \"hi\" )x\"; /* hi\n hi */ int hi;";
    CHECK(ccr_search_text(raw, strlen(raw), "hi", &strings, h, 32) == 1 &&
          ccr_search_text(raw, strlen(raw), "hi", &(ccr_search_opts){ false, false, CCR_IN_COMMENT }, h, 32) == 2 &&
          ccr_search_text(raw, strlen(raw), "hi", &code, h, 32) == 1, "raw string, comment, code");

    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("search: all checks passed\n");
    return g_failures ? 1 : 0;
}
