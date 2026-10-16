/*
 * The YAML reader: what block packs are written in, and the mistakes it
 * must report with a line number. No window needed.
 */
#include "ccr_yaml.h"

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

static int count(const ccr_yaml_node *n)
{
    int k = 0;
    for (const ccr_yaml_node *c = n ? n->first : NULL; c; c = c->next)
        ++k;
    return k;
}

static const ccr_yaml_node *nth(const ccr_yaml_node *n, int i)
{
    const ccr_yaml_node *c = n ? n->first : NULL;
    while (c && i-- > 0)
        c = c->next;
    return c;
}

static void test_pack(void)
{
    const char *text =
        "# a block pack\n"
        "name: raylib   # the library\n"
        "name.ru: \"рейлиб\"\n"
        "header: raylib.h\n"
        "color: '#E6544D'\n"
        "blocks:\n"
        "  - call: InitWindow\n"
        "    text: open window {width} x {height} titled {title}\n"
        "    params: [width, height, title]\n"
        "    defaults: {width: 800, height: 450, title: '\"Hello, raylib!\"'}\n"
        "\n"
        "  - call: WindowShouldClose\n"
        "    returns: bool\n"
        "    text: \"window wants to close\"\n"
        "  -\n"
        "    call: CloseWindow\n"
        "notes: |\n"
        "  line one\n"
        "    indented two\n"
        "list:\n"
        "- a\n"
        "- 'b # not a comment'\n"
        "empty:\n"
        "folded: >\n"
        "  one\n"
        "  two\n";
    ccr_yaml y;
    CHECK(ccr_yaml_parse(text, &y), "parse failed: %s", y.error);
    const ccr_yaml_node *r = y.root;
    CHECK(r && r->kind == CCR_YAML_MAP, "root is not a mapping");
    CHECK(strcmp(ccr_yaml_str(r, "name", ""), "raylib") == 0, "name: '%s'", ccr_yaml_str(r, "name", ""));
    CHECK(strcmp(ccr_yaml_str(r, "name.ru", ""), "рейлиб") == 0, "a quoted UTF-8 value");
    CHECK(strcmp(ccr_yaml_str(r, "color", ""), "#E6544D") == 0, "a # inside quotes is not a comment");

    const ccr_yaml_node *blocks = ccr_yaml_get(r, "blocks");
    CHECK(blocks && blocks->kind == CCR_YAML_SEQ && count(blocks) == 3, "blocks: %d items",
          count(blocks));
    const ccr_yaml_node *b0 = nth(blocks, 0);
    CHECK(strcmp(ccr_yaml_str(b0, "text", ""), "open window {width} x {height} titled {title}") == 0,
          "text with braces: '%s'", ccr_yaml_str(b0, "text", ""));
    const ccr_yaml_node *params = ccr_yaml_get(b0, "params");
    CHECK(params && params->kind == CCR_YAML_SEQ && count(params) == 3 &&
          strcmp(nth(params, 2)->value, "title") == 0, "flow sequence");
    const ccr_yaml_node *defs = ccr_yaml_get(b0, "defaults");
    CHECK(defs && defs->kind == CCR_YAML_MAP && count(defs) == 3, "flow mapping");
    CHECK(strcmp(ccr_yaml_str(defs, "title", ""), "\"Hello, raylib!\"") == 0,
          "a quoted value with a comma and quotes: '%s'", ccr_yaml_str(defs, "title", ""));
    CHECK(strcmp(ccr_yaml_str(defs, "height", ""), "450") == 0, "a plain value in flow");
    CHECK(strcmp(ccr_yaml_str(nth(blocks, 1), "returns", ""), "bool") == 0, "second item");
    CHECK(strcmp(ccr_yaml_str(nth(blocks, 2), "call", ""), "CloseWindow") == 0, "item under a bare dash");

    CHECK(strcmp(ccr_yaml_str(r, "notes", ""), "line one\n  indented two") == 0, "| block: '%s'",
          ccr_yaml_str(r, "notes", ""));
    CHECK(strcmp(ccr_yaml_str(r, "folded", ""), "one two") == 0, "> block");
    const ccr_yaml_node *list = ccr_yaml_get(r, "list");
    CHECK(list && list->kind == CCR_YAML_SEQ && count(list) == 2 &&
          strcmp(nth(list, 1)->value, "b # not a comment") == 0, "a sequence at the key's indent");
    CHECK(ccr_yaml_get(r, "empty") && ccr_yaml_get(r, "empty")->kind == CCR_YAML_NULL, "empty value");
    CHECK(ccr_yaml_get(r, "nope") == NULL && strcmp(ccr_yaml_str(r, "nope", "fb"), "fb") == 0,
          "missing key");
    ccr_yaml_free(&y);
}

static void bad(const char *text, int line, const char *what)
{
    ccr_yaml y;
    bool ok = ccr_yaml_parse(text, &y);
    char want[32];
    snprintf(want, sizeof want, "line %d:", line);
    CHECK(!ok && strncmp(y.error, want, strlen(want)) == 0, "%s: got '%s'", what, ok ? "(parsed)" : y.error);
    ccr_yaml_free(&y);
}

int main(void)
{
    test_pack();
    bad("a: 1\n\tb: 2\n", 2, "a tab");
    bad("a: [1, 2\n", 1, "an open bracket");
    bad("a: \"open\n", 1, "an open quote");
    bad("a: 1\nb\n", 2, "a line that is no key");
    bad("a: 1\n    b: 2\n", 2, "indented under a value");
    bad("a:\n  - x\n    y: 1\n", 3, "bad indentation in a list");
    ccr_yaml y;
    CHECK(ccr_yaml_parse("", &y) && y.root && y.root->kind == CCR_YAML_NULL, "an empty file");
    ccr_yaml_free(&y);
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("yaml: all checks passed\n");
    return g_failures ? 1 : 0;
}
