/*
 * goto arrows: every goto finds its label in its own function; a label
 * dragged out, or deleted, breaks exactly the gotos that need it.
 *
 *   test_jumps examples/goto.c      (needs a window: run under xvfb)
 */
#include "ccr_ast.h"
#include "ccr_editor.h"
#include "ccr_gui.h"
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

static ccr_node_id find(const ccr_editor *ed, ccr_node_kind kind, const char *name)
{
    for (ccr_node_id i = 0; i < ed->ast.node_count; ++i)
        if (ed->ast.nodes[i].kind == kind &&
            strcmp(ccr_ast_str(&ed->ast, ed->ast.nodes[i].name), name) == 0)
            return i;
    fprintf(stderr, "no %s %s\n", ccr_node_kind_name(kind), name);
    ++g_failures;
    return CCR_NONE;
}

static void frame(ccr_editor *ed, ccr_vec2 m, bool pressed, bool down, bool released)
{
    ccr_editor_input in = { m, pressed, down, released, 1.0f, false };
    ccr_editor_update(ed, &in);
}

static ccr_jump_state jump(const ccr_editor *ed, const char *go_name)
{
    ccr_node_id label;
    return ccr_editor_jump(ed, find(ed, CCR_NODE_GOTO, go_name), &label);
}

int main(int argc, char **argv)
{
    if (argc < 2 || !ccr_gui_init(1280, 800, "ccratch test"))
        return 2;
    ccr_ast ast;
    ccr_ast_init(&ast);
    if (!ccr_parse_file(argv[1], NULL, 0, &ast))
        return 2;
    ccr_editor ed;
    if (!ccr_editor_init(&ed, &ast))
        return 2;

    const char *gotos[] = { "out", "close_file", "free_buf", "retry" };
    for (int i = 0; i < 4; ++i) {
        ccr_node_id label;
        ccr_jump_state st = ccr_editor_jump(&ed, find(&ed, CCR_NODE_GOTO, gotos[i]), &label);
        CHECK(st == CCR_JUMP_OK && label == find(&ed, CCR_NODE_LABEL, gotos[i]),
              "goto %s does not find its label", gotos[i]);
    }

    /* `out:` dragged onto the canvas: goto out breaks, the others do not */
    ccr_rect r;
    ccr_node_id out = find(&ed, CCR_NODE_LABEL, "out");
    CHECK(ccr_editor_node_rect(&ed, out, &r), "out: is not drawn");
    ccr_vec2 from = { r.x + 6.0f, r.y + 6.0f }, to = { 4000.0f, 3000.0f };
    frame(&ed, from, true, true, false);
    for (int i = 1; i <= 6; ++i)
        frame(&ed, (ccr_vec2){ from.x + (to.x - from.x) * i / 6.0f, from.y + (to.y - from.y) * i / 6.0f },
              false, true, false);
    frame(&ed, to, false, false, true);
    CHECK(jump(&ed, "out") == CCR_JUMP_ELSEWHERE, "goto out still thinks its label is home");
    CHECK(jump(&ed, "close_file") == CCR_JUMP_OK && jump(&ed, "retry") == CCR_JUMP_OK,
          "other gotos broke too");

    /* thrown away: no such label at all */
    CHECK(ccr_editor_delete(&ed, out), "delete refused");
    CHECK(jump(&ed, "out") == CCR_JUMP_MISSING, "a deleted label is still found");

    /* undo twice: home again */
    CHECK(ccr_editor_undo(&ed) && ccr_editor_undo(&ed), "undo failed");
    CHECK(jump(&ed, "out") == CCR_JUMP_OK, "undo did not bring the label home");

    /* labels belong to their function: retry: is in main, not in load */
    CHECK(jump(&ed, "retry") == CCR_JUMP_OK, "retry lost its label");

    ccr_editor_free(&ed);
    ccr_gui_shutdown();
    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("jumps: all checks passed\n");
    return g_failures ? 1 : 0;
}
