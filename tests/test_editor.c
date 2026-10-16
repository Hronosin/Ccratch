/*
 * Drives the editor with fake mouse input and checks that dragging blocks
 * edits the program the way Scratch would.
 *
 *   test_editor FILE.c [SCREENSHOT.png]
 *
 * Needs a window (text is measured with the GUI font): run under xvfb.
 */
#include "ccr_ast.h"
#include "ccr_editor.h"
#include "ccr_gui.h"
#include "ccr_parse.h"

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

/* ----------------------------------------------------------- helpers */

static const ccr_node *N(const ccr_editor *ed, ccr_node_id id) { return &ed->ast.nodes[id]; }

static const char *name_of(const ccr_editor *ed, ccr_node_id id)
{
    return ccr_ast_str(&ed->ast, N(ed, id)->name);
}

/* A statement: node of `kind` named `name` whose parent is a Block. */
static ccr_node_id find_stmt(const ccr_editor *ed, ccr_node_kind kind, const char *name)
{
    for (ccr_node_id i = 0; i < ed->ast.node_count; ++i) {
        const ccr_node *n = N(ed, i);
        if (n->kind != kind || n->parent == CCR_NONE)
            continue;
        if (N(ed, n->parent)->kind != CCR_NODE_BLOCK)
            continue;
        if (!name || strcmp(name_of(ed, i), name) == 0)
            return i;
    }
    return CCR_NONE;
}

static ccr_node_id find_function(const ccr_editor *ed, const char *name)
{
    for (ccr_node_id i = 0; i < ed->ast.node_count; ++i)
        if (N(ed, i)->kind == CCR_NODE_FUNCTION && (N(ed, i)->flags & CCR_FLAG_DEFINITION) &&
            strcmp(name_of(ed, i), name) == 0)
            return i;
    return CCR_NONE;
}

static ccr_node_id body_of(const ccr_editor *ed, ccr_node_id fn)
{
    for (ccr_node_id c = N(ed, fn)->first_child; c != CCR_NONE; c = N(ed, c)->next_sibling)
        if (N(ed, c)->kind == CCR_NODE_BLOCK)
            return c;
    return CCR_NONE;
}

/* "Var countdown, Call free, ..." for the children of a Block */
static void describe(const ccr_editor *ed, ccr_node_id block, char *out, size_t cap)
{
    size_t len = 0;
    out[0] = '\0';
    for (ccr_node_id c = N(ed, block)->first_child; c != CCR_NONE; c = N(ed, c)->next_sibling) {
        int w = snprintf(out + len, cap - len, "%s%s%s%s", len ? ", " : "",
                         ccr_node_kind_name(N(ed, c)->kind),
                         *name_of(ed, c) ? " " : "", name_of(ed, c));
        if (w < 0 || (size_t)w >= cap - len)
            break;
        len += (size_t)w;
    }
}

static ccr_vec2 top_left(const ccr_editor *ed, ccr_node_id node)
{
    ccr_rect r = { 0 };
    bool ok = ccr_editor_node_rect(ed, node, &r);
    CHECK(ok, "node %u has no block on screen", node);
    return (ccr_vec2){ r.x, r.y };
}

static float g_dt;   /* 0: no animation, like most tests want */

static void frame(ccr_editor *ed, ccr_vec2 m, bool pressed, bool down, bool released)
{
    ccr_editor_input in = { m, pressed, down, released, 1.0f, false, g_dt };
    ccr_editor_update(ed, &in);
}

/* Press at `from`, move in steps, release at `to`. All world coordinates. */
static void drag(ccr_editor *ed, ccr_vec2 from, ccr_vec2 to)
{
    frame(ed, from, true, true, false);
    for (int i = 1; i <= 8; ++i) {
        float t = (float)i / 8.0f;
        ccr_vec2 m = { from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t };
        frame(ed, m, false, true, false);
    }
    frame(ed, to, false, false, true);
}

/* Grab a block 6 units in from its corner, so the grab is inside it. */
static ccr_vec2 grip(ccr_vec2 corner) { return (ccr_vec2){ corner.x + 6.0f, corner.y + 6.0f }; }

/* ------------------------------------------------------------- tests */

static void test_tear_off_tail(ccr_editor *ed)
{
    ccr_node_id fn = find_function(ed, "main");
    ccr_node_id body = body_of(ed, fn);
    ccr_node_id free_call = find_stmt(ed, CCR_NODE_CALL, "free");
    CHECK(free_call != CCR_NONE, "free(p) statement not found");

    ccr_vec2 at = top_left(ed, free_call);
    drag(ed, grip(at), grip((ccr_vec2){ at.x + 3000.0f, at.y }));

    CHECK(ccr_editor_loose_count(ed) == 1, "expected 1 loose stack, got %u",
          ccr_editor_loose_count(ed));

    /* free(p), goto done and the label all went with it */
    ccr_node_id loose = N(ed, free_call)->parent;
    CHECK(loose != body, "free(p) is still in main");
    CHECK(N(ed, loose)->flags & CCR_FLAG_LOOSE, "free(p) is not in a loose stack");
    char got[512];
    describe(ed, loose, got, sizeof got);
    CHECK(strcmp(got, "Call free, Goto done, Label done") == 0, "loose stack is: %s", got);

    describe(ed, body, got, sizeof got);
    CHECK(strstr(got, "Call free") == NULL && strstr(got, "Label") == NULL,
          "main still has the tail: %s", got);
}

static void test_snap_back(ccr_editor *ed)
{
    ccr_node_id fn = find_function(ed, "main");
    ccr_node_id body = body_of(ed, fn);
    ccr_node_id free_call = find_stmt(ed, CCR_NODE_CALL, "free");
    ccr_node_id countdown = find_stmt(ed, CCR_NODE_VAR, "countdown");

    ccr_rect target;
    ccr_editor_node_rect(ed, countdown, &target);
    ccr_vec2 at = top_left(ed, free_call);
    /* top-left of the stack lands right under `int countdown = 3` */
    drag(ed, grip(at), grip((ccr_vec2){ target.x + 3.0f, target.y + target.h + 4.0f }));

    CHECK(ccr_editor_loose_count(ed) == 0, "stack did not snap, %u loose",
          ccr_editor_loose_count(ed));
    char got[512];
    describe(ed, body, got, sizeof got);
    CHECK(strstr(got, "Var countdown, Call free, Goto done, Label done, While") != NULL,
          "main is: %s", got);
}

/* The same round trip with animations on: the dropped stack glides in
 * for a few frames and ends up exactly where the instant drop puts it. */
static void test_animated_drop(ccr_editor *ed)
{
    ed->animate = true;
    g_dt = 1.0f / 60.0f;
    ccr_node_id fn = find_function(ed, "main");
    ccr_node_id body = body_of(ed, fn);
    ccr_node_id free_call = find_stmt(ed, CCR_NODE_CALL, "free");
    ccr_node_id countdown = find_stmt(ed, CCR_NODE_VAR, "countdown");

    ccr_vec2 at = top_left(ed, free_call);
    drag(ed, grip(at), grip((ccr_vec2){ at.x + 3000.0f, at.y }));
    CHECK(ed->drag == CCR_DRAG_IDLE, "a drop on empty canvas should not glide");
    CHECK(ccr_editor_loose_count(ed) == 1, "tear-off: %u loose", ccr_editor_loose_count(ed));

    ccr_rect target;
    ccr_editor_node_rect(ed, countdown, &target);
    at = top_left(ed, free_call);
    drag(ed, grip(at), grip((ccr_vec2){ target.x + 9.0f, target.y + target.h + 10.0f }));
    CHECK(ed->drag == CCR_DRAG_SETTLING, "the drop did not start gliding");
    CHECK(ccr_editor_loose_count(ed) == 1, "snapped in before the glide was over");
    int frames = 0;
    while (ed->drag == CCR_DRAG_SETTLING && frames < 100) {
        frame(ed, (ccr_vec2){ 0, 0 }, false, false, false);
        ++frames;
    }
    CHECK(frames >= 3 && frames <= 20, "glide took %d frames", frames);
    CHECK(ccr_editor_loose_count(ed) == 0, "stack did not snap, %u loose",
          ccr_editor_loose_count(ed));
    char got[512];
    describe(ed, body, got, sizeof got);
    CHECK(strstr(got, "Var countdown, Call free, Goto done, Label done, While") != NULL,
          "main is: %s", got);

    /* deleting throws bits about, and they are gone a second later */
    ed->particle_count = 0;
    ccr_editor_delete(ed, find_stmt(ed, CCR_NODE_CALL, "free"));
    CHECK(ed->particle_count > 0, "deleting made no bits");
    for (int i = 0; i < 60; ++i)
        frame(ed, (ccr_vec2){ 0, 0 }, false, false, false);
    CHECK(ed->particle_count == 0, "%u bits still flying after a second", ed->particle_count);
    ccr_editor_undo(ed);
    CHECK(find_stmt(ed, CCR_NODE_CALL, "free") != CCR_NONE, "undo did not bring free(p) back");

    ed->animate = false;
    g_dt = 0.0f;
}

static void test_click_is_not_a_drag(ccr_editor *ed)
{
    ccr_node_id ret = find_stmt(ed, CCR_NODE_RETURN, NULL);
    ccr_node_id parent = N(ed, ret)->parent;
    ccr_vec2 at = top_left(ed, ret);
    frame(ed, grip(at), true, true, false);
    frame(ed, (ccr_vec2){ grip(at).x + 1.0f, grip(at).y }, false, true, false);
    frame(ed, grip(at), false, false, true);
    CHECK(N(ed, ret)->parent == parent, "a click moved a block");
    CHECK(ccr_editor_loose_count(ed) == 0, "a click left a loose stack");
}

static void test_move_whole_function(ccr_editor *ed)
{
    ccr_node_id fn = find_function(ed, "sum");
    ccr_vec2 before = top_left(ed, fn);
    drag(ed, grip(before), grip((ccr_vec2){ before.x + 500.0f, before.y + 40.0f }));
    ccr_vec2 after = top_left(ed, fn);
    CHECK(after.x > before.x + 450.0f, "function did not move (%.0f -> %.0f)", before.x, after.x);
    CHECK(N(ed, fn)->parent == 0, "function left the translation unit");
    CHECK(ccr_editor_loose_count(ed) == 0, "moving a function made a loose stack");
}

/* `continue` out of `if (countdown == 1)`, then back into its empty mouth */
static void test_empty_mouth(ccr_editor *ed)
{
    ccr_node_id cont = find_stmt(ed, CCR_NODE_CONTINUE, NULL);
    ccr_node_id mouth = N(ed, cont)->parent;
    ccr_vec2 at = top_left(ed, cont);

    drag(ed, grip(at), grip((ccr_vec2){ at.x + 2000.0f, at.y + 500.0f }));
    CHECK(N(ed, mouth)->first_child == CCR_NONE, "the if body is not empty");

    /* where the empty mouth's FIRST snap is */
    ccr_vec2 snap = { 0, 0 };
    bool found = false;
    for (uint32_t i = 0; i < ed->script_count && !found; ++i)
        for (uint32_t k = 0; k < ed->scripts[i].blocks.snap_count; ++k) {
            const ccr_snap *s = &ed->scripts[i].blocks.snaps[k];
            if (s->kind == CCR_SNAP_FIRST && s->ref == mouth) {
                snap = (ccr_vec2){ s->at.x + ed->scripts[i].pos.x, s->at.y + ed->scripts[i].pos.y };
                found = true;
                break;
            }
        }
    CHECK(found, "no snap point for the empty mouth");

    at = top_left(ed, cont);
    drag(ed, grip(at), grip((ccr_vec2){ snap.x + 2.0f, snap.y + 2.0f }));
    CHECK(N(ed, cont)->parent == mouth, "continue did not go back into the if");
    CHECK(ccr_editor_loose_count(ed) == 0, "continue is still loose");
}

/* Leave one stack loose and screenshot mid-drag with the drop indicator. */
static void screenshot(ccr_editor *ed, const char *path)
{
    ccr_node_id ret = find_stmt(ed, CCR_NODE_RETURN, "");
    (void)ret;
    ccr_node_id total = find_stmt(ed, CCR_NODE_VAR, "total");
    ccr_vec2 at = top_left(ed, total);
    drag(ed, grip(at), grip((ccr_vec2){ at.x + 760.0f, at.y + 40.0f }));   /* loose */

    ccr_node_id countdown = find_stmt(ed, CCR_NODE_VAR, "countdown");
    ccr_vec2 c = top_left(ed, countdown);
    frame(ed, grip(c), true, true, false);
    frame(ed, grip((ccr_vec2){ c.x + 380.0f, c.y - 300.0f }), false, true, false);
    ccr_rect numbers;
    ccr_editor_node_rect(ed, find_stmt(ed, CCR_NODE_VAR, "numbers"), &numbers);
    ccr_vec2 near = { numbers.x + 14.0f, numbers.y + numbers.h + 10.0f };
    frame(ed, grip(near), false, true, false);
    CHECK(ed->snap_ok, "no drop indicator while hovering under `int numbers`");

    ccr_canvas cv;
    ccr_canvas_init(&cv);
    cv.zoom = 0.62f;
    ccr_rect b = ccr_editor_bounds(ed);
    cv.offset = (ccr_vec2){ 24.0f - b.x * cv.zoom, 64.0f - b.y * cv.zoom };
    for (int i = 0; i < 2; ++i) {
        ccr_gui_begin_frame(0x1E2028FFu);
        ccr_canvas_draw_grid(&cv, (ccr_rect){ 0, 0, 1280, 800 });
        ccr_editor_draw(ed, &cv, (ccr_rect){ 0, 0, 1280, 800 });
        if (i == 1)
            ccr_gui_screenshot(path);
        ccr_gui_end_frame();
    }
    frame(ed, grip(near), false, false, true);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE.c [SCREENSHOT.png]\n", argv[0]);
        return 2;
    }
    if (!ccr_gui_init(1280, 800, "ccratch test")) {
        fprintf(stderr, "no window (run under xvfb-run)\n");
        return 2;
    }

    ccr_ast ast;
    ccr_ast_init(&ast);
    if (!ccr_parse_file(argv[1], NULL, 0, &ast)) {
        fprintf(stderr, "could not parse %s\n", argv[1]);
        return 2;
    }
    ccr_editor ed;
    if (!ccr_editor_init(&ed, &ast)) {
        fprintf(stderr, "editor init failed\n");
        return 2;
    }

    test_tear_off_tail(&ed);
    test_snap_back(&ed);
    test_animated_drop(&ed);
    test_click_is_not_a_drag(&ed);
    test_move_whole_function(&ed);
    test_empty_mouth(&ed);
    if (argc > 2)
        screenshot(&ed, argv[2]);

    ccr_editor_free(&ed);
    ccr_gui_shutdown();

    if (g_failures)
        fprintf(stderr, "%d check(s) failed\n", g_failures);
    else
        printf("editor: all checks passed\n");
    return g_failures ? 1 : 0;
}
