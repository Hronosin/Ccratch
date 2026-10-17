/*
 * Slots that take an enum or a bool: what their list holds.
 * Run with CCRATCH_BLOCKS_DIR=tests/data/blocks (the toy pack).
 */
#include "ccr_choice.h"
#include "ccr_lib.h"
#include "ccr_parse.h"

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

static ccr_ast g_ast;
static ccr_choices g_c;

/* The n-th node (from 0) of `kind` named `name` ("" for any). */
static ccr_node_id nth(ccr_node_kind kind, const char *name, int n)
{
    for (ccr_node_id i = 0; i < g_ast.node_count; ++i)
        if (g_ast.nodes[i].kind == kind && (!*name || strcmp(ccr_ast_str(&g_ast, g_ast.nodes[i].name), name) == 0) &&
            n-- == 0)
            return i;
    return CCR_NONE;
}

static const char *list(void)
{
    static char buf[512];
    size_t k = 0;
    buf[0] = '\0';
    for (int i = 0; i < g_c.n && k + 64 < sizeof buf; ++i)
        k += (size_t)snprintf(buf + k, sizeof buf - k, "%s%s", i ? "," : "", g_c.v[i]);
    return buf;
}

static void want(ccr_node_id slot, const char *values, const char *title, int line)
{
    const bool got = slot != CCR_NONE && ccr_slot_choices(&g_ast, slot, &g_c);
    if (!values) {
        if (got) {
            fprintf(stderr, "FAIL line %d: no list wanted, got [%s]\n", line, list());
            ++g_failures;
        }
        return;
    }
    if (!got || strcmp(list(), values) != 0 || (title && strcmp(g_c.title, title) != 0)) {
        fprintf(stderr, "FAIL line %d: want [%s] (%s), got [%s] (%s)\n", line, values, title ? title : "-",
                got ? list() : "nothing", g_c.title);
        ++g_failures;
    }
}
#define WANT(slot, values, title) want(slot, values, title, __LINE__)

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s ENUMS_C LIB_C\n", argv[0]);
        return 2;
    }
    ccr_lib_load(stderr);
    CHECK(ccr_ast_init(&g_ast) && ccr_parse_file(argv[1], NULL, 0, &g_ast), "could not read %s", argv[1]);

    const ccr_node_id call = nth(CCR_NODE_CALL, "go", 0);
    const ccr_node_id a0 = g_ast.nodes[call].first_child;
    const ccr_node_id a1 = g_ast.nodes[a0].next_sibling;
    const ccr_node_id a2 = g_ast.nodes[a1].next_sibling;
    WANT(a0, "UP,DOWN,LEFT,RIGHT", "enum dir");              /* go(enum dir d, ...) */
    WANT(a1, "RED,GREEN", "colour");                         /* a typedef of an unnamed enum */
    WANT(a2, "true,false", "bool");
    WANT(nth(CCR_NODE_REF, "DOWN", 0), "UP,DOWN,LEFT,RIGHT", NULL);    /* d == DOWN */
    WANT(nth(CCR_NODE_REF, "LEFT", 0), "UP,DOWN,LEFT,RIGHT", NULL);    /* d = LEFT */
    WANT(nth(CCR_NODE_REF, "d", 2), NULL, NULL);             /* d = ...: the variable is no value */
    WANT(nth(CCR_NODE_REF, "UP", 0), "UP,DOWN,LEFT,RIGHT", NULL);      /* enum dir d = UP */
    WANT(nth(CCR_NODE_REF, "CIRCLE", 0), "CIRCLE,SQUARE", NULL);       /* shape s = CIRCLE */
    WANT(nth(CCR_NODE_REF, "CIRCLE", 1), "CIRCLE,SQUARE", NULL);       /* case CIRCLE: (switch on s) */
    WANT(nth(CCR_NODE_REF, "RED", 0), "RED,GREEN", NULL);              /* return in colour pick() */
    WANT(nth(CCR_NODE_CALL, "pick", 0), "RED,GREEN", NULL);            /* t.c = pick() */
    WANT(nth(CCR_NODE_INT_LIT, "", 0), NULL, NULL);          /* return 0 from int main */
    ccr_ast_free(&g_ast);

    /* a library's block: the pack's lists */
    CHECK(ccr_ast_init(&g_ast) && ccr_parse_file(argv[2], NULL, 0, &g_ast), "could not read %s", argv[2]);
    const ccr_node_id dot = nth(CCR_NODE_CALL, "draw_dot", 0);
    const ccr_node_id x = dot == CCR_NONE ? CCR_NONE : g_ast.nodes[dot].first_child;
    WANT(x, "1,2,3", "x");
    WANT(x == CCR_NONE ? CCR_NONE : g_ast.nodes[x].next_sibling, "TOP,MIDDLE,BOTTOM", "Rows");
    ccr_ast_free(&g_ast);
    ccr_lib_unload();

    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("choice: all checks passed\n");
    return g_failures ? 1 : 0;
}
