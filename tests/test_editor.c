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

#include <math.h>
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

static ccr_node_id find_top(const ccr_editor *ed, ccr_node_kind kind, const char *name)
{
    for (ccr_node_id c = N(ed, 0)->first_child; c != CCR_NONE; c = N(ed, c)->next_sibling)
        if (N(ed, c)->kind == kind && strcmp(name_of(ed, c), name) == 0)
            return c;
    return CCR_NONE;
}

static bool overlap(ccr_rect a, ccr_rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static ccr_rect script_box(const ccr_editor *ed, uint32_t i)
{
    const ccr_script *s = &ed->scripts[i];
    return (ccr_rect){ s->pos.x + s->blocks.bounds.x, s->pos.y + s->blocks.bounds.y, s->blocks.bounds.w, s->blocks.bounds.h };
}

static uint32_t script_of(const ccr_editor *ed, ccr_node_id root)
{
    for (uint32_t i = 0; i < ed->script_count; ++i)
        if (ed->scripts[i].root == root)
            return i;
    return UINT32_MAX;
}

/* Arranging the canvas: by calls, as a newspaper, back in columns; undo;
 * a stack put down on another makes room; the layout kept and read back. */
static void test_arrange(ccr_editor *ed, const char *path)
{
    const ccr_node_id fmain = find_function(ed, "main"), fsum = find_function(ed, "sum"),
                      fdesc = find_function(ed, "describe");
    const ccr_vec2 sum_was = top_left(ed, fsum);

    CHECK(ccr_editor_arrange(ed, CCR_ARRANGE_CALLS, 1.6f), "by calls: nothing moved");
    const ccr_vec2 m = top_left(ed, fmain), su = top_left(ed, fsum), de = top_left(ed, fdesc);
    CHECK(m.x < su.x && su.x == de.x && su.y != de.y, "main, then what it calls beside it: %.0f %.0f %.0f", m.x, su.x,
          de.x);
    for (uint32_t i = 0; i < ed->script_count; ++i)   /* #includes and types: left of every function */
        if (N(ed, ed->scripts[i].root)->kind != CCR_NODE_FUNCTION && !(N(ed, ed->scripts[i].root)->flags & CCR_FLAG_LOOSE))
            CHECK(script_box(ed, i).x + script_box(ed, i).w < m.x, "a declaration right of main");
    CHECK(ccr_editor_undo(ed) && top_left(ed, fsum).x == sum_was.x && top_left(ed, fsum).y == sum_was.y,
          "undo: back where it was");

    CHECK(ccr_editor_arrange(ed, CCR_ARRANGE_TILES, 1.6f), "newspaper: nothing moved");
    bool clash = false;
    for (uint32_t i = 0; i < ed->script_count; ++i)
        for (uint32_t k = i + 1; k < ed->script_count; ++k)
            clash = clash || overlap(script_box(ed, i), script_box(ed, k));
    CHECK(!clash, "newspaper: stacks on top of each other");
    const ccr_rect all = ccr_editor_bounds(ed);
    CHECK(all.w > all.h * 0.6f, "newspaper: still one tall column (%.0f x %.0f)", all.w, all.h);
    /* in the file's order: down a column, then the next */
    float lx = -1e9f, ly = -1e9f;
    bool in_order = true;
    for (ccr_node_id c = N(ed, 0)->first_child; c != CCR_NONE; c = N(ed, c)->next_sibling) {
        const uint32_t si = script_of(ed, c);
        if (si == UINT32_MAX)
            continue;
        const ccr_rect b = script_box(ed, si);
        in_order = in_order && (b.x > lx + 1.0f || (fabsf(b.x - lx) < 1.0f && b.y > ly));
        lx = b.x;
        ly = b.y;
    }
    CHECK(in_order, "newspaper: not in the file's order");

    /* columns again: as a freshly opened file */
    ccr_ast ast;
    ccr_ast_init(&ast);
    ccr_editor fresh;
    CHECK(ccr_parse_file(path, NULL, 0, &ast) && ccr_editor_init(&fresh, &ast), "reopen");
    CHECK(ccr_editor_arrange(ed, CCR_ARRANGE_COLUMNS, 1.6f), "columns: nothing moved");
    const ccr_vec2 a = top_left(ed, fsum), b = top_left(&fresh, find_function(&fresh, "sum"));
    CHECK(a.x == b.x && a.y == b.y, "columns: not as opened (%.0f,%.0f vs %.0f,%.0f)", a.x, a.y, b.x, b.y);

    /* put a struct down on describe(): describe makes room under it */
    const ccr_node_id pt = find_top(ed, CCR_NODE_STRUCT, "point");
    CHECK(pt != CCR_NONE, "no struct point");
    if (pt != CCR_NONE) {
        const ccr_vec2 at = top_left(ed, pt), d = top_left(ed, fdesc);
        drag(ed, grip(at), grip((ccr_vec2){ d.x + 3.0f, d.y + 20.0f }));
        const uint32_t sp = script_of(ed, pt), sd = script_of(ed, fdesc);
        clash = false;
        for (uint32_t i = 0; i < ed->script_count; ++i)
            for (uint32_t k = i + 1; k < ed->script_count; ++k)
                clash = clash || overlap(script_box(ed, i), script_box(ed, k));
        CHECK(sp != UINT32_MAX && sd != UINT32_MAX && !clash, "put down on describe(): something is on top of something");
        /* and put down on its very top: describe moves down under it */
        const ccr_vec2 d2 = top_left(ed, fdesc);
        drag(ed, grip(top_left(ed, pt)), grip((ccr_vec2){ d2.x, d2.y - 2.0f }));
        const uint32_t sp2 = script_of(ed, pt), sd2 = script_of(ed, fdesc);
        CHECK(sp2 != UINT32_MAX && sd2 != UINT32_MAX && script_box(ed, sd2).y > script_box(ed, sp2).y &&
              !overlap(script_box(ed, sp2), script_box(ed, sd2)), "describe did not make room");
        CHECK(top_left(ed, pt).x == top_left(ed, fdesc).x, "the struct did not line up with describe");
    }

    /* an arrangement of your own: main alone, the other functions by name, the rest */
    {
        ccr_arrangement own;
        char aerr[200];
        CHECK(ccr_arrangement_parse("@name = t\ncolumn = main\ncolumn = funcs | order=name\ncolumn = rest\n", &own,
                                    aerr, sizeof aerr), "arrangement: %s", aerr);
        const uint32_t hist = ed->history_len;
        CHECK(ccr_editor_arrange_custom(ed, &own) && ed->history_len == hist + 1, "own arrangement: one undo step");
        const ccr_vec2 mm = top_left(ed, fmain), ss = top_left(ed, fsum), dd = top_left(ed, fdesc);
        CHECK(mm.x < ss.x && ss.x == dd.x && dd.y < ss.y, "main, then describe above sum: %.0f %.0f %.0f / %.0f %.0f",
              mm.x, ss.x, dd.x, dd.y, ss.y);
        const ccr_node_id inc = N(ed, 0)->first_child;
        CHECK(top_left(ed, inc).x > ss.x, "the rest: right of the functions");
        ccr_rect boxes[256];
        unsigned kinds[256];
        const int np = ccr_editor_arrange_preview(ed, CCR_ARRANGE_COLUMNS, NULL, 1.6f, boxes, kinds, 256);
        CHECK(np == (int)ed->script_count && top_left(ed, fmain).x == mm.x, "a preview moves nothing");
        CHECK(ccr_editor_undo(ed), "undo the own arrangement");
        ccr_editor_arrange_quietly(&fresh, CCR_ARRANGE_COLUMNS, &own, 1.6f);
        CHECK(fresh.history_len == 0 &&
              top_left(&fresh, find_function(&fresh, "main")).x < top_left(&fresh, find_function(&fresh, "sum")).x,
              "quietly, as a file opens: no undo step");
        ccr_editor_free(&fresh);
        ccr_ast_init(&ast);
        CHECK(ccr_parse_file(path, NULL, 0, &ast) && ccr_editor_init(&fresh, &ast), "reopen again");
    }

    /* the layout, kept and read back into the fresh one */
    char *text = ccr_editor_layout_text(ed);
    CHECK(text && ccr_editor_layout_apply(&fresh, text) > 3, "layout text not read back");
    free(text);
    const ccr_node_id fpt = find_top(&fresh, CCR_NODE_STRUCT, "point");
    CHECK(pt == CCR_NONE || (top_left(&fresh, fpt).x == top_left(ed, pt).x && top_left(&fresh, fpt).y == top_left(ed, pt).y),
          "the struct is not where it was left");
    CHECK(top_left(&fresh, find_function(&fresh, "describe")).y == top_left(ed, fdesc).y, "describe moved");
    ccr_editor_free(&fresh);
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
    test_arrange(&ed, argv[1]);
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
